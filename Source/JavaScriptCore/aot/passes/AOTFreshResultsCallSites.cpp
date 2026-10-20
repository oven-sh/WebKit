/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFreshResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include "AOTSinkAllocations.h"
#include "AOTSummarizeForInlining.h"
#include "AOTThreads.h"
#include <algorithm>
#include <atomic>
#include <iterator>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct LocatedCall {
    WrapperCallSite site;
    ProgramPoint point;
    unsigned siteIndex { 0 };
};

struct TrialExpansion {
    unsigned siteIndex { 0 };
    size_t firstValue { 0 };
    size_t endValue { 0 };
    unsigned numberOfAllocations { 0 };
    unsigned numberOfUnsunkAllocations { 0 };
};

} // anonymous namespace

static Vector<LocatedCall, 4> locateCallsInReverseOrder(FunctionAnalyses& analyses, std::span<const WrapperCallSite> sites)
{
    const UseLists& useLists = analyses.useLists();
    Vector<LocatedCall, 4> calls;
    for (unsigned siteIndex = 0; siteIndex < sites.size(); ++siteIndex) {
        ValueLocation location = useLists.locationOf(sites[siteIndex].call);
        if (location.placement != ValuePlacement::InBlock)
            continue;
        calls.append(LocatedCall { sites[siteIndex], { location.block, location.index }, siteIndex });
    }
    std::ranges::sort(calls, [](const LocatedCall& a, const LocatedCall& b) {
        if (a.point.block != b.point.block)
            return a.point.block > b.point.block;
        return a.point.index > b.point.index;
    });
    return calls;
}

static bool isLegal(InlineDecision legality)
{
    return legality == InlineDecision::Unknown;
}

static bool expandCall(FunctionPassContext& context, IRBuilder& builder, const AnalysisContext& wrapperContext, const LocatedCall& located)
{
    // A cold function's summary has everything that inlining a whole body reads. It skips the head, the conditions and the called parameters.
    InliningSummary summary = summarizeFunctionForInlining(wrapperContext, context.program.at(located.site.wrapper), HeatClass::Cold);
    if (!summary.canInlineWholeBody() || !isLegal(canInline(context, summary, located.site.call)))
        return false;
    copyIn(context, builder, InlineRequest { located.point, located.site.call, InlinedPart::WholeBody, summary });
    return true;
}

static TrialExpansion* findExpansionContaining(std::span<TrialExpansion> expansions, ValueID value)
{
    size_t index = value.index();
    auto next = std::ranges::upper_bound(expansions, index, { }, &TrialExpansion::firstValue);
    if (next == expansions.begin())
        return nullptr;
    TrialExpansion& expansion = *std::prev(next);
    return index < expansion.endValue ? &expansion : nullptr;
}

static bool isRemovedOrSunk(SunkAllocationOutcome outcome)
{
    switch (outcome) {
    case SunkAllocationOutcome::Removed:
    case SunkAllocationOutcome::Sunk:
        return true;
    case SunkAllocationOutcome::Unknown:
    case SunkAllocationOutcome::NotACandidate:
    case SunkAllocationOutcome::Abandoned:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Vector<WrapperCallSite, 4> FreshResultSplitter::findWrapperCalls(FunctionID id, const Function& caller) const
{
    Vector<WrapperCallSite, 4> sites;
    for (const Block& block : caller.blocks) {
        for (ValueID call : block.values) {
            const Value& value = caller.at(call);
            if (value.opcode != Opcode::CallDirect || value.isNeverInlined() || !isWrapper(value.function()))
                continue;
            sites.append(WrapperCallSite { call, value.function() });
        }
    }
    if (sites.isEmpty())
        return sites;

    FunctionAnalyses analyses(contextFor(id), caller);
    sites.removeAllMatching([&](const WrapperCallSite& site) {
        ValueLocation location = analyses.useLists().locationOf(site.call);
        return location.placement != ValuePlacement::InBlock || isCold(heatClassOf(m_inputs.heat, analyses, location.block));
    });
    return sites;
}

bool FreshResultSplitter::expandCalls(FunctionPassContext& context, std::span<const WrapperCallSite> sites) const
{
    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    bool didExpand = false;
    for (const LocatedCall& located : locateCallsInReverseOrder(context.analyses, sites))
        didExpand |= expandCall(context, builder, contextFor(located.site.wrapper), located);
    return didExpand;
}

Vector<WrapperCallSite, 4> FreshResultSplitter::chooseProfitableCalls(FunctionID caller, const Function& function, std::span<const WrapperCallSite> sites, TaskArenas& arenas) const
{
    Function copy = function;
    Vector<TrialExpansion, 4> expansions;
    {
        FunctionAnalyses analyses(contextFor(caller), copy);
        FunctionPassContext context { program(), m_inputs, caller, copy, analyses, arenas.types, arenas.strings, build() };
        IRBuilder builder(analyses.context(), copy, arenas.types, &analyses);
        for (const LocatedCall& located : locateCallsInReverseOrder(analyses, sites)) {
            size_t firstValue = copy.values.size();
            if (expandCall(context, builder, contextFor(located.site.wrapper), located))
                expansions.append(TrialExpansion { located.siteIndex, firstValue, copy.values.size() });
        }
    }
    if (expansions.isEmpty())
        return { };

    FunctionAnalyses analyses(contextFor(caller), copy);
    for (const SunkAllocation& allocation : predictSinking(analyses)) {
        TrialExpansion* expansion = findExpansionContaining(expansions.mutableSpan(), allocation.allocation);
        if (!expansion)
            continue;
        ++expansion->numberOfAllocations;
        if (!isRemovedOrSunk(allocation.outcome))
            ++expansion->numberOfUnsunkAllocations;
    }

    BitVector isProfitable(sites.size());
    for (const TrialExpansion& expansion : expansions) {
        if (expansion.numberOfAllocations && !expansion.numberOfUnsunkAllocations)
            isProfitable.set(expansion.siteIndex);
    }

    Vector<WrapperCallSite, 4> chosen;
    for (size_t siteIndex : isProfitable)
        chosen.append(sites[siteIndex]);
    return chosen;
}

unsigned FreshResultSplitter::expandProfitableCalls(FunctionID caller, Function& function, TaskArenas& arenas) const
{
    Vector<WrapperCallSite, 4> sites = findWrapperCalls(caller, function);
    if (sites.isEmpty())
        return 0;
    Vector<WrapperCallSite, 4> chosen = chooseProfitableCalls(caller, function, sites.span(), arenas);
    if (chosen.isEmpty())
        return 0;

    {
        FunctionAnalyses analyses(contextFor(caller), function);
        FunctionPassContext context { program(), m_inputs, caller, function, analyses, arenas.types, arenas.strings, build() };
        expandCalls(context, chosen.span());
        m_runner.runFunctionPass(PassID::SinkAllocations, context);
    }
    simplifyEditedFunction(m_runner, program(), m_inputs, caller, function, arenas);
    return chosen.size();
}

void FreshResultSplitter::expandCallSites()
{
    if (m_wrappers.isEmpty())
        return;

    Vector<FunctionID> callers = collectInParallel<FunctionID>(build().parallelLoops(), program().functions.size(), [&](unsigned index, Vector<FunctionID>& found) {
        FunctionID id(index);
        const Function& function = program().at(id);
        if (!function.hasBody() || isWrapper(id) || !build().isEnabled(PassID::SplitFreshResults, id))
            return;
        if (!findWrapperCalls(id, function).isEmpty())
            found.append(id);
    });

    std::atomic<uint64_t> numberOfExpandedCalls { 0 };
    editFunctionsInParallel(m_editor, build(), callers.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        numberOfExpandedCalls.fetch_add(expandProfitableCalls(id, function, arenas), std::memory_order_relaxed);
    });
    build().statistics().count(StatisticID::ResultsReturnedInRegisters, numberOfExpandedCalls.load(std::memory_order_relaxed));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
