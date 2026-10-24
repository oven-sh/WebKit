/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTHeatGraph.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTDiagnostics.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfOperands.h"
#include "AOTEffectsOfProtocols.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGrouping.h"
#include "AOTIR.h"
#include "AOTLargeVectors.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include <algorithm>
#include <atomic>
#include <wtf/Atomics.h>
#include <wtf/BitVector.h>
#include <wtf/CycleForest.h>
#include <wtf/FrequencyEstimator.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(HeatModel::eventsPerRun == frequencyConstants.maxMultiplier);

namespace {

struct WeightedCall {
    uint32_t node;
    double weight;
};

using WeightedCallList = Vector<WeightedCall, 4>;

enum class CallOfHost : uint8_t { StaysUnlessCalleeRunsOnce, IsReplaced };

} // anonymous namespace

static void mergeEdges(WeightedCallList& calls)
{
    AOT::stableSort(calls, [](const WeightedCall& a, const WeightedCall& b) {
        return a.node < b.node;
    });
    size_t mergedSize = 0;
    for (size_t index = 0; index < calls.size(); ++index) {
        WeightedCall call = calls[index];
        if (mergedSize && calls[mergedSize - 1].node == call.node)
            calls[mergedSize - 1].weight += call.weight;
        else
            calls[mergedSize++] = call;
    }
    calls.shrink(mergedSize);
}

static bool saysNothingAboutCalls(OperandUses uses) { return uses.containsAll(callingUses); }

static double estimateCallbackCalls(OperandUses uses)
{
    if (saysNothingAboutCalls(uses))
        return 1;
    if (uses.contains(OperandUse::CallsRepeatedly))
        return HeatModel::callsOfRepeatedCallback;
    if (uses.contains(OperandUse::CallsPerElement))
        return HeatModel::callsPerElement;
    return 1;
}

static bool isHandedToCodeThatRuns(const Value& user, uint32_t operandIndex)
{
    if (effectRuleOf(user.opcode) != EffectRule::Call)
        return true;
    return !user.operandUses(operandIndex).containsAny(callingUses) && OperandPosition::newTarget(user.opcode) != operandIndex;
}

static bool isHandedToCodeThatRuns(const Terminator&, uint32_t) { return true; }

static Vector<WeightOfBlock> computeWeightsOfBlocks(FunctionAnalyses& analyses)
{
    const BlockFrequencies& frequencies = analyses.blockFrequencies();
    Vector<WeightOfBlock> weights;
    weights.reserveInitialCapacity(analyses.cfg().reversePostOrder().size());
    for (BlockID id : analyses.cfg().reversePostOrder())
        weights.append(WeightOfBlock { id, frequencies.frequencyOf(id) });
    return weights;
}

static double estimateIndirectCalls(const FlowIndirectCall& call)
{
    bool describesSite = call.kind == FlowIndirectCallKind::BuiltinCallback || call.times == BuiltinCallbackTimes::Once;
    double runsOfSite = call.isInCycle && describesSite ? frequencyConstants.iterationsPerLoop : 1;
    switch (call.times) {
    case BuiltinCallbackTimes::PerEvent:
        return 0;
    case BuiltinCallbackTimes::Repeatedly:
        return runsOfSite * HeatModel::callsOfRepeatedCallback;
    case BuiltinCallbackTimes::PerElement:
        return runsOfSite * HeatModel::callsPerElement;
    case BuiltinCallbackTimes::Later:
    case BuiltinCallbackTimes::Once:
        return runsOfSite;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static WeightedCallList collectHostCalls(const Program& program, const CallGraph& callGraph, const HeatGraph& graph, const ParallelArray<WeightedCallList>& callsOfProgram, const BitVector& isCalledPerEvent, const Vector<uint8_t>& isCountedByProgram, const ParallelLoops& loops)
{
    Vector<FunctionID> callees;
    callees.append(callGraph.roots());
    callees.append(callGraph.functionsCalledByUnknownCode());
    for (size_t index : isCalledPerEvent)
        callees.append(FunctionID(index));
    sortAndRemoveDuplicates(loops, callees);

    auto callOf = [&](FunctionID callee) {
        return WeightedCall { graph.nodeOf(callee).index(), isCalledPerEvent.get(callee.index()) ? HeatModel::eventsPerRun : 1.0 };
    };
    WeightedCallList calls;
    calls.reserveInitialCapacity(callees.size());
    Vector<FunctionID> calleesCountedByProgram;
    for (FunctionID callee : callees) {
        if (!program.at(callee).hasBody())
            continue;
        if (isCountedByProgram[callee.index()])
            calleesCountedByProgram.append(callee);
        else
            calls.append(callOf(callee));
    }
    if (calleesCountedByProgram.isEmpty())
        return calls;

    BitVector isReached(callsOfProgram.size());
    Vector<uint32_t> worklist;
    auto reachWithCallees = [&](uint32_t node) {
        if (isReached.quickSet(node))
            return;
        worklist.append(node);
        while (!worklist.isEmpty()) {
            for (const WeightedCall& call : callsOfProgram[worklist.takeLast()]) {
                if (!isReached.quickSet(call.node))
                    worklist.append(call.node);
            }
        }
    };
    for (const WeightedCall& call : calls)
        reachWithCallees(call.node);
    size_t numberOfSortedCalls = calls.size();
    for (FunctionID callee : calleesCountedByProgram) {
        if (isReached.quickGet(graph.nodeOf(callee).index()))
            continue;
        calls.append(callOf(callee));
        reachWithCallees(graph.nodeOf(callee).index());
    }
    if (calls.size() > numberOfSortedCalls)
        AOT::sort(calls, { }, &WeightedCall::node);
    return calls;
}

static WeightedCallList collectUnknownCodeCalls(const Program& program, const CallGraph& callGraph, const FlowResults& flow, const HeatGraph& graph, const BitVector& isCalledPerEvent)
{
    WeightedCallList calls;
    if (!flow.mayRunUnseenCode())
        return calls;
    for (FunctionID callee : callGraph.functionsCalledByUnknownCode()) {
        if (!flow.runsAtMostOnce(program, callee) && !isCalledPerEvent.get(callee.index()))
            calls.append(WeightedCall { graph.nodeOf(callee).index(), 1.0 });
    }
    for (WeightedCall& call : calls)
        call.weight /= calls.size();
    return calls;
}

void BodyWeights::didChange(std::span<const FunctionID> functions)
{
    for (FunctionID id : functions) {
        if (id.index() < m_blocks.size())
            m_blocks[id.index()].clear();
    }
}

HeatGraph buildHeatGraph(const Program& program, const CallGraph& callGraph, const FlowResults& flow, const EffectSummaries& effects, BuildContext& build, const BodyReferences* references, const BodyWeights* keptWeights, BodyWeights* madeWeights)
{
    const ParallelLoops& loops = build.parallelLoops();
    unsigned count = program.functions.size();
    RELEASE_ASSERT(!references || references->size() == count);
    RELEASE_ASSERT(!keptWeights || keptWeights->m_blocks.size() <= count);
    if (madeWeights) {
        RELEASE_ASSERT(madeWeights->m_blocks.isEmpty());
        madeWeights->m_blocks.grow(count);
    }
    HeatGraph graph;
    graph.m_sizes.fill(0u, count);
    graph.m_masses.fill(0.0, count);
    unsigned numberOfNodes = graph.numNodes();
    graph.m_isPresent.fill(true, numberOfNodes);
    ParallelArray<WeightedCallList> calls(loops, numberOfNodes, [](unsigned) {
        return WeightedCallList();
    });
    Vector<uint8_t> isCountedByProgram(FillWith { }, count, static_cast<uint8_t>(0));
    BitVector isCalledOnceAfterReturn;
    BitVector isCalledPerEvent;
    for (const FlowIndirectCall& call : flow.indirectCalls()) {
        if (call.times == BuiltinCallbackTimes::Later)
            isCalledOnceAfterReturn.set(call.callee.index());
        if (call.times == BuiltinCallbackTimes::PerEvent)
            isCalledPerEvent.set(call.callee.index());
    }
    const bool verifies = build.options().verifiesAfterEveryPass;
    const EntriesThatRunNoCode entriesThatRunNoCode = findEntriesThatRunNoCode();
    graph.m_blockGroups = ParallelArray<Vector<HeatBlockGroup, 4>>(loops, count, largeFunctions(program, loops).span(), [&](unsigned index) -> Vector<HeatBlockGroup, 4> {
        FunctionID id(index);
        const Function& function = program.at(id);
        if (!function.hasBody()) {
            graph.m_isPresent[index] = false;
            return { };
        }
        WeightedCallList& callsOfBody = calls[index];
        const ReferencesOfBody* referencesOfBody = references ? &references->of(id) : nullptr;
        FunctionAnalyses analyses(AnalysisContext { build, program, &flow, &effects, id }, function);

        auto add = [&](FunctionID callee, double weight, CallOfHost callOfHost = CallOfHost::StaysUnlessCalleeRunsOnce) {
            if (!program.at(callee).hasBody() || isCalledPerEvent.get(callee.index()))
                return;
            callsOfBody.append(WeightedCall { graph.nodeOf(callee).index(), weight });
            if (callOfHost == CallOfHost::IsReplaced || flow.runsAtMostOnce(program, callee))
                AtomicRef(isCountedByProgram[callee.index()]).store(1, std::memory_order_relaxed);
        };
        auto addUnknownCode = [&](double weight) {
            callsOfBody.append(WeightedCall { graph.unknownCodeNode().index(), weight });
        };
        auto isCountedByCallers = [&](ValueID operand) {
            if (callGraph.callerSet(id) != CallerSet::Complete)
                return false;
            size_t parameter = function.at(entryBlock).parameters.find(analyses.identityOf(operand));
            if (parameter == notFound)
                return false;
            return estimateCallbackCalls(analyses.effectContext().summaries.of(id).operand(static_cast<uint32_t>(parameter)).uses) > 1;
        };
        auto addHandOver = [&](ValueID operand, double weight) {
            CalleeList callees = calleesOf(program, program.typeOf(function, operand));
            for (FunctionID callee : callees.functions) {
                if (callGraph.callerSet(callee) != CallerSet::Complete && !isCalledOnceAfterReturn.get(callee.index()))
                    add(callee, weight / callees.functions.size(), CallOfHost::IsReplaced);
            }
        };
        auto addCallsOfSite = [&](const auto& instruction, double weight, std::span<const FunctionID> candidates, bool callsUnknownCode) {
            for (FunctionID candidate : candidates)
                add(candidate, weight / candidates.size());
            if (callsUnknownCode)
                addUnknownCode(weight);
            instruction.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                OperandUses uses = summarizeOperand(analyses.effectContext(), function, instruction, operandIndex, UsesAskedFor::AllButPassesToUserCode).uses;
                if (verifies) [[unlikely]]
                    RELEASE_ASSERT((uses | OperandUse::PassesToUserCode) == (summarizeOperand(analyses.effectContext(), function, instruction, operandIndex).uses | OperandUse::PassesToUserCode));
                if (saysNothingAboutCalls(uses)) {
                    if (callsUnknownCode && isHandedToCodeThatRuns(instruction, operandIndex))
                        addHandOver(operand, weight);
                    return;
                }
                double additionalCalls = estimateCallbackCalls(uses) - 1;
                if (!additionalCalls || isCountedByCallers(operand))
                    return;
                CalleeList callees = calleesOf(program, program.typeOf(function, operand));
                for (FunctionID callee : callees.functions) {
                    if (!isCalledOnceAfterReturn.get(callee.index()))
                        add(callee, weight * additionalCalls / callees.functions.size());
                }
                if (!callees.isComplete)
                    addUnknownCode(weight * additionalCalls);
            });
        };
        auto addCalls = [&](const auto& instruction, double weight) {
            if (entriesThatRunNoCode.contains(instruction))
                return;
            EffectDescription description = describeEffects(program, function, instruction);
            if (description.functions.isEmpty() && !description.callsUnknownCode)
                return;
            addCallsOfSite(instruction, weight, description.functions.span(), description.callsUnknownCode);
        };
        auto checkNotedSites = [&](BlockID blockID, size_t position) {
            const Block& block = function.at(blockID);
            std::span<const SiteThatRunsCode> sites = referencesOfBody->sitesThatRunCode.span();
            auto check = [&](const auto& instruction, uint32_t indexInBlock) {
                EffectDescription description = describeEffects(program, function, instruction);
                if (description.functions.isEmpty() && !description.callsUnknownCode)
                    return;
                RELEASE_ASSERT(position < sites.size());
                const SiteThatRunsCode& site = sites[position];
                RELEASE_ASSERT(site.block == blockID && site.indexInBlock == indexInBlock && !!site.callsUnknownCode == description.callsUnknownCode);
                RELEASE_ASSERT(site.listsManyFunctions || std::ranges::equal(referencesOfBody->functionsOfSite(position), description.functions));
                ++position;
            };
            for (uint32_t indexInBlock = 0; indexInBlock < block.values.size(); ++indexInBlock)
                check(function.at(block.values[indexInBlock]), indexInBlock);
            check(block.terminator, block.values.size());
            RELEASE_ASSERT(position == sites.size() || sites[position].block != blockID);
        };
        auto addCallsOfNotedSites = [&](BlockID blockID, double weight) {
            const Block& block = function.at(blockID);
            std::span<const SiteThatRunsCode> sites = referencesOfBody->sitesThatRunCode.span();
            size_t position = std::ranges::lower_bound(sites, blockID, { }, &SiteThatRunsCode::block) - sites.begin();
            if (verifies) [[unlikely]]
                checkNotedSites(blockID, position);
            for (; position < sites.size() && sites[position].block == blockID; ++position) {
                const SiteThatRunsCode& site = sites[position];
                auto addCallsOfEntry = [&](const auto& instruction) {
                    if (site.listsManyFunctions)
                        addCalls(instruction, weight);
                    else
                        addCallsOfSite(instruction, weight, referencesOfBody->functionsOfSite(position), site.callsUnknownCode);
                };
                if (site.indexInBlock < block.values.size())
                    addCallsOfEntry(function.at(block.values[site.indexInBlock]));
                else
                    addCallsOfEntry(block.terminator);
            }
        };

        uint32_t size = 0;
        double mass = 0;
        Vector<HeatBlockGroup, 4> blockGroups;
        auto visit = [&](BlockID blockID, double weight) {
            const Block& block = function.at(blockID);
            uint32_t blockSize = block.values.size() + 1;
            size += blockSize;
            mass += weight * blockSize;
            blockGroups.append(HeatBlockGroup { weight, blockSize });
            if (referencesOfBody) {
                addCallsOfNotedSites(blockID, weight);
                return;
            }
            for (ValueID value : block.values)
                addCalls(function.at(value), weight);
            addCalls(block.terminator, weight);
        };
        bool hasKeptWeights = keptWeights && index < keptWeights->m_blocks.size() && !keptWeights->m_blocks[index].isEmpty();
        Vector<WeightOfBlock>* made = madeWeights ? &madeWeights->m_blocks[index] : nullptr;
        if (hasKeptWeights) {
            const Vector<WeightOfBlock>& kept = keptWeights->m_blocks[index];
            RELEASE_ASSERT(!verifies || kept == computeWeightsOfBlocks(analyses));
            for (const WeightOfBlock& entry : kept)
                visit(entry.block, entry.weight);
            if (made)
                *made = kept;
        } else if (made) {
            *made = computeWeightsOfBlocks(analyses);
            for (const WeightOfBlock& entry : *made)
                visit(entry.block, entry.weight);
        } else {
            const BlockFrequencies& frequencies = analyses.blockFrequencies();
            for (BlockID blockID : analyses.cfg().reversePostOrder())
                visit(blockID, frequencies.frequencyOf(blockID));
        }
        mergeEdges(callsOfBody);

        if (FunctionID idAsRead = findFunctionAsRead(&program, id)) {
            Vector<std::pair<FunctionID, double>, 8> countedByFlow;
            for (const FlowIndirectCall& call : flow.indirectCallsFrom(idAsRead)) {
                if (double numberOfCalls = estimateIndirectCalls(call))
                    countedByFlow.append({ call.callee, numberOfCalls });
            }
            AOT::stableSort(countedByFlow, { }, [](const auto& entry) {
                return entry.first.index();
            });

            size_t mergedSize = callsOfBody.size();
            auto callsCountedByWalk = [&](FunctionID callee) {
                std::span<const WeightedCall> edges = callsOfBody.span().first(mergedSize);
                auto edge = std::ranges::lower_bound(edges, graph.nodeOf(callee).index(), { }, &WeightedCall::node);
                return edge != edges.end() && edge->node == graph.nodeOf(callee).index() ? edge->weight : 0;
            };
            for (size_t position = 0; position < countedByFlow.size();) {
                FunctionID callee = countedByFlow[position].first;
                double numberOfCalls = 0;
                for (; position < countedByFlow.size() && countedByFlow[position].first == callee; ++position)
                    numberOfCalls += countedByFlow[position].second;
                double countedByWalk = callsCountedByWalk(callee);
                if (numberOfCalls > countedByWalk)
                    add(callee, numberOfCalls - countedByWalk);
            }
            if (callsOfBody.size() > mergedSize)
                mergeEdges(callsOfBody);
        }

        AOT::sort(blockGroups, std::ranges::greater { }, &HeatBlockGroup::frequency);
        size_t numberOfGroups = 0;
        for (const HeatBlockGroup& group : blockGroups) {
            if (numberOfGroups && blockGroups[numberOfGroups - 1].frequency == group.frequency)
                blockGroups[numberOfGroups - 1].size += group.size;
            else
                blockGroups[numberOfGroups++] = group;
        }
        blockGroups.shrink(numberOfGroups);
        graph.m_sizes[index] = size;
        graph.m_masses[index] = mass;
        return blockGroups;
    });
    calls[graph.unknownCodeNode().index()] = collectUnknownCodeCalls(program, callGraph, flow, graph, isCalledPerEvent);
    calls[graph.hostNode().index()] = collectHostCalls(program, callGraph, graph, calls, isCalledPerEvent, isCountedByProgram, loops);

    graph.m_firstSuccessor.fill(0u, numberOfNodes + 1);
    for (unsigned index = 0; index < numberOfNodes; ++index)
        graph.m_firstSuccessor[index + 1] = graph.m_firstSuccessor[index] + calls[index].size();
    growInParallel(loops, graph.m_successors, graph.m_firstSuccessor[numberOfNodes]);
    growInParallel(loops, graph.m_multipliers, graph.m_firstSuccessor[numberOfNodes]);
    forEachIndexInParallel(loops, numberOfNodes, [&](unsigned index) {
        uint32_t position = graph.m_firstSuccessor[index];
        for (const WeightedCall& call : calls[index]) {
            graph.m_successors[position] = HeatNodeID(call.node);
            graph.m_multipliers[position++] = call.weight;
        }
    });
    build.destroyLater(WTF::move(calls));
    groupByIndex(numberOfNodes, graph.m_firstPredecessor, graph.m_predecessors, [&](const auto& add) {
        for (unsigned index = 0; index < numberOfNodes; ++index) {
            for (HeatNodeID successor : graph.successors(HeatNodeID(index)))
                add(successor.index(), HeatNodeID(index));
        }
    });
    return graph;
}

Vector<double> estimateActivations(HeatGraph& graph)
{
    AOT_THRESHOLD(unsigned, maxDepthOfCallCycles, 16);
    WTF::CycleForest<HeatGraph> cycles(graph, [&](std::span<const HeatNodeID> members, const Vector<HeatNodeID, 2>&, const auto& isInCycle) {
        HeatNodeID header;
        int64_t mostPaths = 0;
        for (HeatNodeID member : members) {
            int64_t paths = static_cast<int64_t>(std::ranges::count_if(graph.predecessors(member), isInCycle)) * std::ranges::count_if(graph.successors(member), isInCycle);
            if (header && (paths < mostPaths || (paths == mostPaths && header < member)))
                continue;
            header = member;
            mostPaths = paths;
        }
        return header;
    }, maxDepthOfCallCycles);
    Vector<double> result = WTF::estimateMultipliedFrequencies(graph, cycles, [&](HeatNodeID node, unsigned successorIndex) {
        return graph.multiplierOf(node, successorIndex);
    }, frequencyConstants);
    result.shrink(graph.numberOfFunctions());
    return result;
}

void HeatGraph::dump(PrintStream& out) const
{
    auto printSuccessors = [&](Node node) {
        std::span<const Node> callees = successors(node);
        for (unsigned index = 0; index < callees.size(); ++index) {
            if (FunctionID function = functionOf(callees[index]))
                out.print(" f", function.index());
            else
                out.print(" unknown");
            out.print("*", multiplierOf(node, index));
        }
        out.println();
    };
    for (unsigned index = 0; index < numberOfFunctions(); ++index) {
        if (!m_isPresent[index])
            continue;
        FunctionID function(index);
        out.print("f", index, " size ", sizeOf(function), " mass ", massOf(function), ":");
        printSuccessors(nodeOf(function));
    }
    out.print("host:");
    printSuccessors(hostNode());
    out.print("unknown:");
    printSuccessors(unknownCodeNode());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
