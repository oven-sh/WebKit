/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRemoveUnusedResults.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTFunctionEditing.h"
#include "AOTPassRunner.h"
#include "AOTThreads.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using SiteIndex = ID<struct SiteIndexTag>;

struct ResultRemovalPlan {
    Vector<BitVector> usedResults;
    Vector<TypeID> singleResultTypes;
    BitVector isCandidate;
    Vector<FunctionID> candidates;
    uint64_t numberOfRemovedResults { 0 };
};

} // anonymous namespace

static void setAllResults(BitVector& results, size_t resultCount)
{
    results.ensureSize(resultCount);
    for (size_t index = 0; index < resultCount; ++index)
        results.quickSet(index);
}

static std::optional<ValueID> returnedValueOf(const Terminator& terminator)
{
    switch (terminator.kind) {
    case TerminatorKind::Return:
        return terminator.valueOperand();
    case TerminatorKind::ReturnCall:
        return terminator.callOperand();
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Vector<ResultCallSite> collectResultCallSites(const Program& program, const Function& caller)
{
    Vector<ResultCallSite> sites;
    Vector<SiteIndex> siteByValue;
    for (auto& block : caller.blocks) {
        for (ValueID id : block.values) {
            auto& value = caller.at(id);
            if (value.opcode != Opcode::CallDirect || program.at(value.function()).results.size() <= 1)
                continue;
            if (sites.isEmpty())
                siteByValue.grow(caller.values.size());
            siteByValue[id.index()] = SiteIndex(sites.size());
            sites.append({ id, value.function(), { } });
        }
    }
    if (sites.isEmpty())
        return sites;

    auto siteOf = [&](ValueID producer) -> ResultCallSite* {
        SiteIndex site = siteByValue[producer.index()];
        return site ? &sites[site.index()] : nullptr;
    };
    for (auto& block : caller.blocks) {
        for (ValueID id : block.values) {
            auto& value = caller.at(id);
            if (value.opcode != Opcode::ResultAt)
                continue;
            if (auto* site = siteOf(value.resultsOperand()))
                site->readResults.set(value.index());
        }
        std::optional<ValueID> returned = returnedValueOf(block.terminator);
        if (!returned || !*returned)
            continue;
        if (auto* site = siteOf(*returned))
            setAllResults(site->readResults, program.at(site->callee).results.size());
    }
    return sites;
}

Vector<ResultCallSiteList> collectAllResultCallSites(const Program& program, BuildContext& build)
{
    return collectInParallel<ResultCallSiteList>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<ResultCallSiteList>& lists) {
        bool hasSite = false;
        forEachDirectCall(program, FunctionID(index), [&](BlockID, ValueID, FunctionID callee) {
            hasSite = hasSite || recordFactsOf(program, callee).numberOfResults > 1;
        });
        if (!hasSite)
            return;
        auto sites = collectResultCallSites(program, program.functions[index]);
        if (!sites.isEmpty())
            lists.append({ FunctionID(index), WTF::move(sites) });
    });
}

// The lists of the callers that call some function with a body and several results. A function without a body keeps its results, whoever reads them.
static Vector<ResultCallSiteList> collectResultCallSitesOfBodies(const Program& program, const CallGraph& callGraph, BuildContext& build)
{
    BitVector callsSuchFunction(program.functions.size());
    auto suchFunctions = findIndicesInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned index) {
        RecordFacts callee = recordFactsOf(program, FunctionID(index));
        return callee.hasBody && callee.numberOfResults > 1;
    });
    for (uint32_t index : suchFunctions) {
        for (FunctionID caller : callGraph.callersOf(FunctionID(index)))
            callsSuchFunction.quickSet(caller.index());
    }
    return collectInParallel<ResultCallSiteList>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<ResultCallSiteList>& lists) {
        if (!callsSuchFunction.quickGet(index))
            return;
        auto sites = collectResultCallSites(program, program.functions[index]);
        if (!sites.isEmpty())
            lists.append({ FunctionID(index), WTF::move(sites) });
    });
}

bool returnsOnlyResults(const Function& function)
{
    return std::ranges::all_of(function.blocks, [&](auto& block) {
        std::optional<ValueID> returned = returnedValueOf(block.terminator);
        if (!returned)
            return true;
        if (!*returned)
            return false;
        auto& value = function.at(*returned);
        return value.opcode == Opcode::Results && value.numberOfVariadicOperands() == function.results.size();
    });
}

static bool readsResultsValue(const Function& function)
{
    return std::ranges::any_of(function.blocks, [&](auto& block) {
        return std::ranges::any_of(block.values, [&](ValueID id) {
            auto& value = function.at(id);
            return value.opcode == Opcode::ResultAt && function.at(value.resultsOperand()).opcode == Opcode::Results;
        });
    });
}

ResultEditing canEditResults(const Program& program, const CallGraph& callGraph, FunctionID id)
{
    auto& function = program.at(id);
    if (!function.hasBody() || function.results.size() <= 1 || !function.mayChangeSignature() || callGraph.entrySet(id) != EntrySet::DirectCallers)
        return ResultEditing::Unknown;
    if (!returnsOnlyResults(function) || readsResultsValue(function))
        return ResultEditing::Unknown;
    return ResultEditing::Allowed;
}

static void retainOperands(Value& results, const BitVector& retained)
{
    std::span<const ValueID> operands = results.valuesOperands();
    Vector<ValueID, 8> retainedOperands;
    for (size_t index : retained)
        retainedOperands.append(operands[index]);
    results.setValuesOperands(retainedOperands.span());
}

void retainResults(Function& function, const BitVector& retained)
{
    ASSERT(returnsOnlyResults(function));
    size_t retainedCount = retained.bitCount();
    RELEASE_ASSERT(retainedCount);
    if (retainedCount == function.results.size())
        return;

    BitVector returnedValues(function.values.size());
    for (auto& block : function.blocks) {
        if (block.terminator.kind == TerminatorKind::Return)
            returnedValues.quickSet(block.terminator.valueOperand().index());
    }
    if (retainedCount > 1) {
        for (size_t index : returnedValues)
            retainOperands(function.edit(ValueID(index)), retained);
    } else {
        size_t retainedIndex = *retained.begin();
        for (auto& block : function.editAllBlocks()) {
            auto& terminator = block.terminator;
            if (terminator.kind == TerminatorKind::Return)
                terminator.setValueOperand(function.at(terminator.valueOperand()).valuesOperands()[retainedIndex]);
        }
        for (auto& block : function.editAllBlocks()) {
            block.values.removeAllMatching([&](ValueID id) {
                return returnedValues.quickGet(id.index());
            });
        }
    }

    size_t newSize = 0;
    for (size_t index : retained)
        function.results[newSize++] = function.results[index];
    function.results.shrink(newSize);
}

static uint32_t countRetainedResultsBelow(const BitVector& retained, uint32_t resultIndex)
{
    uint32_t count = 0;
    for (size_t index : retained) {
        if (index >= resultIndex)
            break;
        ++count;
    }
    return count;
}

void retargetResultCallSites(Function& caller, std::span<const RetargetedCallSite> sites)
{
    if (sites.empty())
        return;
    Vector<SiteIndex> siteByValue(caller.values.size());
    for (size_t index = 0; index < sites.size(); ++index) {
        auto& site = sites[index];
        siteByValue[site.call.index()] = SiteIndex(index);
        auto& call = caller.edit(site.call);
        call.setFunction(site.callee);
        if (site.singleResultType)
            call.type = site.singleResultType;
    }

    std::optional<ValueSubstitution> substitution;
    for (auto& block : caller.blocks) {
        for (ValueID id : block.values) {
            auto& read = caller.edit(id);
            if (read.opcode != Opcode::ResultAt)
                continue;
            SiteIndex index = siteByValue[read.resultsOperand().index()];
            if (!index)
                continue;
            auto& site = sites[index.index()];
            if (!site.singleResultType) {
                read.setIndex(countRetainedResultsBelow(*site.retained, read.index()));
                continue;
            }
            auto& call = caller.edit(site.call);
            if (!call.expected())
                call.setExpected(read.expected());
            if (!substitution)
                substitution.emplace(caller);
            substitution->replace(id, site.call);
        }
    }
    if (!substitution)
        return;
    for (auto& block : caller.editAllBlocks()) {
        block.values.removeAllMatching([&](ValueID id) {
            return substitution->resolve(id) != id;
        });
    }
    substitution->apply(caller, nullptr);
}

static ResultRemovalPlan planResultRemoval(const Program& program, const CallGraph& callGraph, const BuildContext& build, const Vector<ResultCallSiteList>& callers)
{
    size_t functionCount = program.functions.size();
    ResultRemovalPlan plan;
    plan.usedResults.grow(functionCount);
    for (auto& list : callers) {
        bool canRetargetCaller = build.isEnabled(PassID::RemoveUnusedResults, findFunctionAsRead(&program, list.caller));
        for (auto& site : list.sites) {
            auto& used = plan.usedResults[site.callee.index()];
            if (canRetargetCaller)
                used.merge(site.readResults);
            else
                setAllResults(used, program.at(site.callee).results.size());
        }
    }

    plan.singleResultTypes.grow(functionCount);
    plan.isCandidate.ensureSize(functionCount);
    // Few functions have a result that nobody reads: all threads look for them.
    for (uint32_t index : findIndicesInParallel(build.parallelLoops(), functionCount, [&](unsigned index) {
        FunctionID id(index);
        if (plan.usedResults[index].bitCount() >= program.at(id).results.size())
            return false;
        return canEditResults(program, callGraph, id) == ResultEditing::Allowed && build.isEnabled(PassID::RemoveUnusedResults, findFunctionAsRead(&program, id));
    })) {
        FunctionID id(index);
        auto& results = program.at(id).results;
        auto& used = plan.usedResults[index];
        if (used.isEmpty())
            used.set(0);
        if (used.bitCount() == 1)
            plan.singleResultTypes[index] = program.provenTypeOf(results[*used.begin()]);
        plan.numberOfRemovedResults += results.size() - used.bitCount();
        plan.isCandidate.quickSet(index);
        plan.candidates.append(id);
    }
    return plan;
}

static void retargetCallSites(ProgramEditor& editor, const ResultCallSiteList& list, const ResultRemovalPlan& plan)
{
    Vector<RetargetedCallSite, 8> retargeted;
    for (auto& site : list.sites) {
        size_t callee = site.callee.index();
        if (plan.isCandidate.get(callee))
            retargeted.append({ site.call, site.callee, &plan.usedResults[callee], plan.singleResultTypes[callee] });
    }
    if (!retargeted.isEmpty())
        retargetResultCallSites(editor.at(list.caller), retargeted.span());
}

void removeUnusedResults(ProgramEditor& editor, const OptimizationInputs& inputs, const PassRunner& runner)
{
    BuildContext& build = runner.build();
    const Program& program = editor.program();
    Vector<ResultCallSiteList> callers = collectResultCallSitesOfBodies(program, inputs.callGraph, build);
    ResultRemovalPlan plan = planResultRemoval(program, inputs.callGraph, build, callers);
    if (plan.candidates.isEmpty())
        return;

    const ParallelLoops& loops = build.parallelLoops();
    forEachIndexInParallel(loops, callers.size(), [&](unsigned index) {
        retargetCallSites(editor, callers[index], plan);
    });
    forEachIndexInParallel(loops, plan.candidates.size(), [&](unsigned index) {
        FunctionID id = plan.candidates[index];
        retainResults(editor.at(id), plan.usedResults[id.index()]);
    });
    editFunctionsInParallel(editor, build, plan.candidates.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        simplifyEditedFunction(runner, program, inputs, id, function, arenas);
    });
    build.statistics().count(StatisticID::UnusedResultsRemoved, plan.numberOfRemovedResults);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
