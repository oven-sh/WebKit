/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCalleeVersions.h"
#include "AOTCodeSizeEstimate.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include "AOTIRBuilder.h"
#include "AOTInlineCopy.h"
#include "AOTInlineCostModel.h"
#include "AOTSummarizeForInlining.h"
#include "AOTTypePropagation.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class FollowUp : uint8_t { None, MarkNeverInlined, ReportDecline, Rewrite };

struct DecisionHandling {
    FollowUp followUp;
    LimitID limit;
};

struct InlineSite {
    BlockID block;
    uint32_t index { 0 };
    ValueID call;
    const InliningSummary* summary { nullptr };
    double heat { 0 };
    InlineVerdict verdict;
};

struct CallerBudget {
    uint64_t frameCount { 0 };
    std::optional<uint64_t> bytes;
};

} // anonymous namespace

static constexpr DecisionHandling handlingOf(InlineDecision decision)
{
    switch (decision) {
    case InlineDecision::Unknown:
        return { FollowUp::None, LimitID::None };
    case InlineDecision::Inline:
        return { FollowUp::Rewrite, LimitID::None };
    case InlineDecision::InlineHead:
        return { FollowUp::Rewrite, LimitID::None };
    case InlineDecision::Barrier:
        return { FollowUp::ReportDecline, LimitID::None };
    case InlineDecision::SignatureMismatch:
        return { FollowUp::None, LimitID::None };
    case InlineDecision::ClosesCycle:
        return { FollowUp::MarkNeverInlined, LimitID::None };
    case InlineDecision::DepthLimit:
        return { FollowUp::ReportDecline, LimitID::MaxInlineDepth };
    case InlineDecision::CallerSizeLimit:
        return { FollowUp::ReportDecline, LimitID::MaxInlinedCallerBytes };
    case InlineDecision::CostExceedsBenefit:
        return { FollowUp::ReportDecline, LimitID::None };
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static constexpr bool isAccepted(InlineDecision decision)
{
    return handlingOf(decision).followUp == FollowUp::Rewrite;
}

static bool isAccepted(const InlineSite& site)
{
    return isAccepted(site.verdict.decision);
}

static constexpr bool canCallCalleeVersion(InlineDecision decision)
{
    return decision != InlineDecision::Inline && decision != InlineDecision::Barrier && decision != InlineDecision::SignatureMismatch;
}

static InlinedPart partToCopy(InlineDecision decision)
{
    RELEASE_ASSERT(isAccepted(decision));
    return decision == InlineDecision::InlineHead ? InlinedPart::Head : InlinedPart::WholeBody;
}

static bool isCandidate(const Value& value)
{
    return value.opcode == Opcode::CallDirect && !value.isNeverInlined() && !value.isAfterInlinedHead();
}

namespace {

class CallInliner {
public:
    explicit CallInliner(FunctionPassContext&);

    PassOutcome run();

private:
    Vector<InlineSite> findCandidates(uint32_t firstFrame) const;
    PassOutcome markNoReturnCallBlocks();

    bool canSummarize(FunctionID callee) const;
    const InliningSummary& summaryOf(FunctionID callee);
    InlineVerdict computeVerdict(const InlineSite&);

    uint64_t computeInlinedBytes();
    bool fitsInCaller(const InlineSite&, CallerBudget&);
    void spendCallerBudget(Vector<InlineSite>&);

    PassOutcome markCallsClosingCycles(std::span<const InlineSite>);
    PassOutcome callCalleeVersions(std::span<const InlineSite>);
    bool wouldHaveTooManyCopies(const InlineSite&, uint32_t entryParameter);
    void countVerdicts(std::span<const InlineSite>);
    void reportDeclines(std::span<const InlineSite>);

    void copyAcceptedSites(std::span<const InlineSite>);

    FunctionPassContext& m_context;
    const Program& m_program;
    Function& m_caller;
    IRBuilder m_builder;
};

CallInliner::CallInliner(FunctionPassContext& context)
    : m_context(context)
    , m_program(context.program)
    , m_caller(context.function)
    , m_builder(context.analyses.context(), context.function, context.types, &context.analyses)
{
}

// A copied body has calls of its own. They are decided in the same run, so that the passes that follow see a call's code together with what it calls:
// an object that the caller allocates is still untested when the last of the code that fills it arrives.
PassOutcome CallInliner::run()
{
    PassOutcome outcome = PassOutcome::Unchanged;
    // The frames of a copy are new, and they keep their numbers where typing cuts the copy and numbers the values again.
    uint32_t firstUndecidedFrame = 0;
    while (true) {
        Vector<InlineSite> sites = findCandidates(firstUndecidedFrame);
        outcome |= markNoReturnCallBlocks();
        if (sites.isEmpty())
            return outcome;

        for (InlineSite& site : sites) {
            double heat = m_context.heatOf(site.block);
            site.summary = &summaryOf(m_caller.at(site.call).function());
            site.heat = heat > 0 ? heat : 0;
            site.verdict = computeVerdict(site);
        }
        spendCallerBudget(sites);
        outcome |= markCallsClosingCycles(sites.span());
        outcome |= callCalleeVersions(sites.span());
        countVerdicts(sites.span());

        bool hasAcceptedSite = sites.containsIf([](const InlineSite& site) {
            return isAccepted(site);
        });
        if (!hasAcceptedSite) {
            if (!firstUndecidedFrame)
                reportDeclines(sites.span());
            return outcome;
        }
        firstUndecidedFrame = static_cast<uint32_t>(inlineFrameCountOf(m_caller));
        copyAcceptedSites(sites.span());
        outcome = PassOutcome::Changed;
    }
}

Vector<InlineSite> CallInliner::findCandidates(uint32_t firstFrame) const
{
    const CFG& cfg = m_context.analyses.cfg();
    Vector<InlineSite> sites;
    for (uint32_t blockIndex = 0; blockIndex < m_caller.blocks.size(); ++blockIndex) {
        BlockID blockID(blockIndex);
        if (!cfg.isReachable(blockID) || isRecordedAsRare(m_caller, blockID))
            continue;
        const Block& block = m_caller.at(blockID);
        for (uint32_t index = 0; index < block.values.size(); ++index) {
            ValueID id = block.values[index];
            if (m_caller.at(id).inlineFrame >= firstFrame && isCandidate(m_caller.at(id)))
                sites.append(InlineSite { .block = blockID, .index = index, .call = id });
        }
    }
    if (sites.isEmpty())
        return sites;

    const BlockFrequencies& frequencies = m_context.analyses.blockFrequencies();
    sites.removeAllMatching([&](const InlineSite& site) {
        return frequencies.classOf(site.block) == BlockFrequency::Rare;
    });
    return sites;
}

PassOutcome CallInliner::markNoReturnCallBlocks()
{
    const InliningSummaries& inlining = m_context.inputs.inlining;
    PassOutcome outcome = PassOutcome::Unchanged;
    for (BlockID blockID : m_context.analyses.cfg().reversePostOrder()) {
        const Block& block = m_caller.at(blockID);
        if (block.frequency == BlockFrequency::Rare)
            continue;
        bool callsNoReturnFunction = block.values.containsIf([&](ValueID id) {
            const Value& value = m_caller.at(id);
            return value.opcode == Opcode::CallDirect && inlining.neverReturns(m_program, value.function());
        });
        if (!callsNoReturnFunction)
            continue;
        m_caller.edit(blockID, BlockEdit::Rest).frequency = BlockFrequency::Rare;
        outcome = PassOutcome::Changed;
    }
    if (outcome == PassOutcome::Changed)
        m_context.analyses.invalidate(FunctionAnalysis::Frequencies);
    return outcome;
}

bool CallInliner::canSummarize(FunctionID callee) const
{
    while (ProvisionalFunctions::contains(callee))
        callee = m_program.at(callee).derivation.source;
    return m_context.build.isEnabled(PassID::SummarizeForInlining, findFunctionAsRead(&m_program, callee));
}

const InliningSummary& CallInliner::summaryOf(FunctionID callee)
{
    const InliningSummaries& inlining = m_context.inputs.inlining;
    if (inlining.contains(callee) || !canSummarize(callee))
        return inlining.of(m_program, callee);

    // Nothing of the caller goes into it.
    return m_context.inputs.derivedFunctions->inliningSummaryOf(callee, [&] {
        AnalysisContext context { m_context.build, m_program, &m_context.inputs.flow, &m_context.inputs.effects, callee };
        return summarizeFunctionForInlining(context, m_program.at(callee), HeatClass::Warm);
    });
}

InlineVerdict CallInliner::computeVerdict(const InlineSite& site)
{
    InlineDecision legality = canInline(m_context, *site.summary, site.call);
    if (legality != InlineDecision::Unknown)
        return { .decision = legality };
    return computeInlineVerdict(m_context, *site.summary, site.call);
}

// The limit is on what inlining adds. A caller that is large by itself takes in as much as a small one.
uint64_t CallInliner::computeInlinedBytes()
{
    const BlockFrequencies& frequencies = m_context.analyses.blockFrequencies();
    uint64_t bytes = 0;
    for (BlockID block : m_context.analyses.cfg().reversePostOrder()) {
        if (frequencies.classOf(block) == BlockFrequency::Rare)
            continue;
        for (ValueID id : m_caller.at(block).values) {
            const Value& value = m_caller.at(id);
            if (value.inlineFrame)
                bytes += estimatedCodeBytes(m_program, m_caller, value);
        }
    }
    return bytes;
}

bool CallInliner::fitsInCaller(const InlineSite& site, CallerBudget& budget)
{
    const Value& call = m_caller.at(site.call);
    uint64_t frameCount = budget.frameCount + inlineFrameCountOf(m_program.at(call.function()));
    if (m_context.exceeds(LimitID::InlineFramesPerFunction, frameCount, site.call))
        return false;

    uint32_t costBytes = site.verdict.costBytes;
    if (costBytes && !site.verdict.benefits.contains(InlineBenefit::Wrapper)) {
        if (!budget.bytes)
            budget.bytes = computeInlinedBytes();
        if (m_context.exceeds(LimitID::MaxInlinedCallerBytes, *budget.bytes + costBytes, site.call))
            return false;
        *budget.bytes += costBytes;
    }
    budget.frameCount = frameCount;
    return true;
}

void CallInliner::spendCallerBudget(Vector<InlineSite>& sites)
{
    Vector<uint32_t, 8> order;
    for (uint32_t index = 0; index < sites.size(); ++index) {
        if (isAccepted(sites[index]))
            order.append(index);
    }
    std::ranges::sort(order, [&](uint32_t a, uint32_t b) {
        const InlineSite& first = sites[a];
        const InlineSite& second = sites[b];
        if (first.heat != second.heat)
            return first.heat > second.heat;
        uint64_t scaledBenefitOfFirst = uint64_t { first.verdict.benefitBytes } * (uint64_t { second.verdict.costBytes } + 1);
        uint64_t scaledBenefitOfSecond = uint64_t { second.verdict.benefitBytes } * (uint64_t { first.verdict.costBytes } + 1);
        if (scaledBenefitOfFirst != scaledBenefitOfSecond)
            return scaledBenefitOfFirst > scaledBenefitOfSecond;
        return a < b;
    });

    CallerBudget budget { .frameCount = inlineFrameCountOf(m_caller) };
    for (uint32_t index : order) {
        if (!fitsInCaller(sites[index], budget))
            sites[index].verdict.decision = InlineDecision::CallerSizeLimit;
    }
}

PassOutcome CallInliner::markCallsClosingCycles(std::span<const InlineSite> sites)
{
    PassOutcome outcome = PassOutcome::Unchanged;
    for (const InlineSite& site : sites) {
        if (handlingOf(site.verdict.decision).followUp != FollowUp::MarkNeverInlined)
            continue;
        m_caller.edit(site.call).setIsNeverInlined(true);
        outcome = PassOutcome::Changed;
    }
    return outcome;
}

// The type of the parameter lists the functions that it is seen to receive, and each may get a copy of the callee. A helper that the whole program hands its callbacks to
// would be there a hundred times, and most of the copies would never run: one copy serves all of its callers.
bool CallInliner::wouldHaveTooManyCopies(const InlineSite& site, uint32_t entryParameter)
{
    const Function& callee = m_program.at(m_caller.at(site.call).function());
    const Type& parameter = m_program.typeOf(callee, callee.at(entryBlock).parameters[entryParameter]);
    return m_context.exceeds(LimitID::MaxBytesOfCalleeVersions, uint64_t { parameter.functions.size() } * site.summary->codeBytes, site.call);
}

PassOutcome CallInliner::callCalleeVersions(std::span<const InlineSite> sites)
{
    PassOutcome outcome = PassOutcome::Unchanged;
    for (const InlineSite& site : sites) {
        if (!canCallCalleeVersion(site.verdict.decision))
            continue;
        const Value& call = m_caller.at(site.call);
        for (size_t index : site.summary->calledParameters) {
            if (index >= call.numberOfOperands())
                break;
            uint32_t entryParameter = static_cast<uint32_t>(index);
            const Type& passed = m_program.typeOf(m_caller, call.operand(entryParameter));
            FunctionID argument = passed.singleFunction();
            BuiltinID builtinArgument = singleBuiltinFunction(m_program, passed);
            if ((!argument && !builtinArgument) || ProvisionalFunctions::contains(argument) || !canHaveCalleeVersion(m_program, call.function(), entryParameter))
                continue;
            if (m_context.exceeds(LimitID::MaxValuesInCalleeVersion, m_program.at(call.function()).values.size(), site.call))
                break;
            bool mayBeNullish = passed.mayBeUndefinedOrNull();
            bool sharesCopy = isLibraryCode(m_program, m_program.at(call.function())) || isInColderPartOfProgram(m_context.inputs.heat, m_program, m_context.id);
            // All who pass one built-in function share the copy for it, and few built-in functions are passed at all.
            bool isForCalleeAlone = builtinArgument || ((isHot(heatClassOf(m_context, site.block)) || !sharesCopy) && !wouldHaveTooManyCopies(site, entryParameter));
            m_caller.edit(site.call, ValueEdit::Definition).setFunction(m_context.derivedFunction(!isForCalleeAlone
                ? calleeVersionKeyForAnyFunction(call.function(), entryParameter, mayBeNullish)
                : argument ? calleeVersionKey(call.function(), entryParameter, argument, mayBeNullish)
                : calleeVersionKeyForBuiltin(call.function(), entryParameter, builtinArgument, mayBeNullish)));
            outcome = PassOutcome::Changed;
            break;
        }
    }
    // The call names another function. Its operands and its place are as they were.
    if (outcome == PassOutcome::Changed)
        m_context.analyses.invalidate(valueAnalyses - analysesThatReadNoTypes);
    return outcome;
}

void CallInliner::countVerdicts(std::span<const InlineSite> sites)
{
    Statistics& statistics = m_context.build.statistics();
    for (const InlineSite& site : sites) {
        statistics.count(statisticFor(site.verdict.decision));
        if (!isAccepted(site))
            continue;
        for (InlineBenefit benefit : site.verdict.benefits)
            statistics.count(statisticFor(benefit));
    }
}

void CallInliner::reportDeclines(std::span<const InlineSite> sites)
{
    if (!m_context.build.options().collectsDeclines())
        return;

    for (const InlineSite& site : sites) {
        DecisionHandling handling = handlingOf(site.verdict.decision);
        if (handling.followUp != FollowUp::ReportDecline || !isHot(heatClassOf(m_context, site.block)))
            continue;

        uint64_t allowedBytes = uint64_t { site.verdict.budgetBytes } + site.verdict.benefitBytes;
        uint64_t excessBytes = site.verdict.costBytes > allowedBytes ? site.verdict.costBytes - allowedBytes : 0;
        m_context.addDecline(Wire::DeclineKind::HotCallNotInlined, site.call, { }, handling.limit, static_cast<uint32_t>(excessBytes));
    }
}

void CallInliner::copyAcceptedSites(std::span<const InlineSite> sites)
{
    uint32_t firstCopiedValue = static_cast<uint32_t>(m_caller.values.size());
    Vector<ValueID> changedValues;
    for (size_t index = sites.size(); index--;) {
        if (isAccepted(sites[index]))
            changedValues.append(sites[index].call);
    }

    // From the last site to the first: what stands in front of a site stays where it is.
    IRBuilder::DeferredResultReads resultReads(m_builder, changedValues.span());
    for (size_t index = sites.size(); index--;) {
        const InlineSite& site = sites[index];
        if (isAccepted(site))
            copyIn(m_context, m_builder, InlineRequest { { site.block, site.index }, site.call, partToCopy(site.verdict.decision), *site.summary, &resultReads });
    }
    resultReads.finish();

    for (uint32_t index = static_cast<uint32_t>(m_caller.values.size()); index-- > firstCopiedValue;)
        changedValues.append(ValueID(index));
    narrowValueTypes(TypePropagationInputs { m_program, m_context.id, m_caller, m_context.analyses, m_context.types, m_context.build, &m_context.typingMemo }, changedValues.span());
}

} // anonymous namespace

PassOutcome inlineCalls(FunctionPassContext& context)
{
    PassOutcome outcome = CallInliner(context).run();
    // Every analysis is dropped behind a copied body, those that look into a call behind the choice of a callee version, the frequencies behind the mark on a block. No analysis reads the mark on a call that closes a cycle.
    // The effects begin a new round all the same, and what is made from them goes with them: a later pass that asks for effects in the middle of its own edits goes by the answers that the round holds.
    if (outcome == PassOutcome::Changed)
        context.keptAnalyses = allAnalyses - FunctionAnalysis::Effects;
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
