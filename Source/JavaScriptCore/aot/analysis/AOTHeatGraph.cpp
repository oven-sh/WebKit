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
#include "AOTIR.h"
#include "AOTThreads.h"
#include <algorithm>
#include <numeric>
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

struct FunctionsCalledAfterReturn {
    BitVector once;
    BitVector perEvent;
};

} // anonymous namespace

static FunctionsCalledAfterReturn findFunctionsCalledAfterReturn(const FlowResults& flow)
{
    FunctionsCalledAfterReturn functions;
    for (const FlowIndirectCall& call : flow.indirectCalls()) {
        if (call.times == BuiltinCallbackTimes::Later)
            functions.once.set(call.callee.index());
        if (call.times == BuiltinCallbackTimes::PerEvent)
            functions.perEvent.set(call.callee.index());
    }
    return functions;
}

static void mergeEdges(WeightedCallList& calls)
{
    std::stable_sort(calls.begin(), calls.end(), [](const WeightedCall& a, const WeightedCall& b) {
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

static double estimateCallbackCalls(OperandUses uses)
{
    // The unknown summary and the summary of native code claim every calling use, which says nothing about the number of calls.
    if (uses.containsAll(callingUses))
        return 1;
    if (uses.contains(OperandUse::CallsRepeatedly))
        return HeatModel::callsOfRepeatedCallback;
    if (uses.contains(OperandUse::CallsPerElement))
        return HeatModel::callsPerElement;
    return 1;
}

namespace {

class HeatEdgeCollector {
public:
    HeatEdgeCollector(const Program& program, const FlowResults& flow, const HeatGraph& graph, const FunctionsCalledAfterReturn& calledAfterReturn, FunctionID id, WeightedCallList& calls)
        : m_program(program)
        , m_flow(flow)
        , m_graph(graph)
        , m_calledAfterReturn(calledAfterReturn)
        , m_function(program.at(id))
        , m_id(id)
        , m_calls(calls)
    {
    }

    void collect(const CallGraph& callGraph, const EffectSummaries& effects, BuildContext& build)
    {
        FunctionAnalyses analyses(AnalysisContext { build, m_program, &m_flow, &effects, m_id }, m_function);
        m_callerSet = callGraph.callerSet(m_id);
        const BlockFrequencies& frequencies = analyses.blockFrequencies();
        for (BlockID id : analyses.cfg().reversePostOrder()) {
            const Block& block = m_function.at(id);
            uint32_t blockSize = block.values.size() + 1;
            double weight = frequencies.frequencyOf(id);
            m_size += blockSize;
            m_mass += weight * blockSize;
            m_blockGroups.append(HeatBlockGroup { weight, blockSize });
            for (ValueID value : block.values)
                addCalls(analyses, m_function.at(value), weight);
            addCalls(analyses, block.terminator, weight);
        }
        mergeEdges(m_calls);
        addIndirectCalls();
        mergeBlockGroups();
    }

    uint32_t size() const { return m_size; }
    double mass() const { return m_mass; }
    Vector<HeatBlockGroup, 4> takeBlockGroups() { return WTF::move(m_blockGroups); }

private:
    // The walk cannot tell a call of a handler from the operand that hands the handler over. Beside the calls of the host neither counts.
    void add(FunctionID callee, double weight)
    {
        if (m_program.at(callee).hasBody() && !m_calledAfterReturn.perEvent.get(callee.index()))
            m_calls.append(WeightedCall { m_graph.nodeOf(callee).index(), weight });
    }

    void mergeBlockGroups()
    {
        std::ranges::sort(m_blockGroups, std::ranges::greater { }, &HeatBlockGroup::frequency);
        size_t mergedSize = 0;
        for (const HeatBlockGroup& group : m_blockGroups) {
            if (mergedSize && m_blockGroups[mergedSize - 1].frequency == group.frequency)
                m_blockGroups[mergedSize - 1].size += group.size;
            else
                m_blockGroups[mergedSize++] = group;
        }
        m_blockGroups.shrink(mergedSize);
    }

    void addUnknownCode(double weight)
    {
        m_calls.append(WeightedCall { m_graph.unknownCodeNode().index(), weight });
    }

    void addCandidates(std::span<const FunctionID> candidates, double weight)
    {
        for (FunctionID candidate : candidates)
            add(candidate, weight / candidates.size());
    }

    // The summary of a parameter covers the callees that the parameter is handed on to, so the call that hands a callback to this function has counted these calls.
    bool isCountedByCallers(FunctionAnalyses& analyses, ValueID operand) const
    {
        if (m_callerSet != CallerSet::Complete)
            return false;
        size_t parameter = m_function.at(entryBlock).parameters.find(analyses.identityOf(operand));
        if (parameter == notFound)
            return false;
        return estimateCallbackCalls(analyses.effectContext().summaries.of(m_id).parameter(static_cast<uint32_t>(parameter)).operand.uses) > 1;
    }

    void addCalls(FunctionAnalyses& analyses, const auto& instruction, double weight)
    {
        EffectDescription description = describeEffects(m_program, m_function, instruction);
        if (description.functions.isEmpty() && !description.callsUnknownCode)
            return;
        addCandidates(description.functions.span(), weight);
        if (description.callsUnknownCode)
            addUnknownCode(weight);
        instruction.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            double additionalCalls = estimateCallbackCalls(summarizeOperand(analyses.effectContext(), m_function, instruction, operandIndex).uses) - 1;
            if (!additionalCalls || isCountedByCallers(analyses, operand))
                return;
            CalleeList callees = calleesOf(m_program, m_program.typeOf(m_function, operand));
            // An operand that is called once after the return has the uses of one that is called repeatedly.
            for (FunctionID callee : callees.functions) {
                if (!m_calledAfterReturn.once.get(callee.index()))
                    add(callee, weight * additionalCalls / callees.functions.size());
            }
            if (!callees.isComplete)
                addUnknownCode(weight * additionalCalls);
        });
    }

    void addIndirectCalls()
    {
        FunctionID idAsRead = findFunctionAsRead(&m_program, m_id);
        if (!idAsRead)
            return;
        size_t mergedSize = m_calls.size();
        auto hasEdgeTo = [&](FunctionID callee) {
            return std::ranges::binary_search(m_calls.span().first(mergedSize), m_graph.nodeOf(callee).index(), { }, &WeightedCall::node);
        };
        for (const FlowIndirectCall& call : m_flow.indirectCallsFrom(idAsRead)) {
            if (!hasEdgeTo(call.callee))
                addIndirectCall(call);
        }
        if (m_calls.size() > mergedSize)
            mergeEdges(m_calls);
    }

    void addIndirectCall(const FlowIndirectCall& call)
    {
        // The flag of a callback is about the site that calls the built-in. On the other kinds flow also sets it for a call that the operation itself repeats.
        bool describesSite = call.kind == FlowIndirectCallKind::BuiltinCallback || call.times == BuiltinCallbackTimes::Once;
        double runsOfSite = call.isInCycle && describesSite ? frequencyConstants.iterationsPerLoop : 1;
        switch (call.times) {
        case BuiltinCallbackTimes::PerEvent:
            return;
        case BuiltinCallbackTimes::Repeatedly:
            add(call.callee, runsOfSite * HeatModel::callsOfRepeatedCallback);
            return;
        case BuiltinCallbackTimes::PerElement:
            add(call.callee, runsOfSite * HeatModel::callsPerElement);
            return;
        case BuiltinCallbackTimes::Later:
        case BuiltinCallbackTimes::Once:
            add(call.callee, runsOfSite);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    const Program& m_program;
    const FlowResults& m_flow;
    const HeatGraph& m_graph;
    const FunctionsCalledAfterReturn& m_calledAfterReturn;
    const Function& m_function;
    FunctionID m_id;
    WeightedCallList& m_calls;
    CallerSet m_callerSet { CallerSet::Unknown };
    uint32_t m_size { 0 };
    double m_mass { 0 };
    Vector<HeatBlockGroup, 4> m_blockGroups;
};

} // anonymous namespace

static BitVector findFunctionsCalledOnceByProgram(const Program& program, const FlowResults& flow, const HeatGraph& graph, const Vector<WeightedCallList>& callsOfFunctions)
{
    BitVector functions(graph.numberOfFunctions());
    for (const WeightedCallList& calls : callsOfFunctions) {
        for (const WeightedCall& call : calls) {
            FunctionID callee = graph.functionOf(HeatNodeID(call.node));
            if (callee && flow.runsAtMostOnce(program, callee))
                functions.quickSet(callee.index());
        }
    }
    return functions;
}

static WeightedCallList collectHostCalls(const Program& program, const CallGraph& callGraph, const HeatGraph& graph, const BitVector& isCalledPerEvent, const BitVector& isCalledOnceByProgram, const ParallelLoops& loops)
{
    Vector<FunctionID> callees;
    callees.append(callGraph.roots());
    callees.append(callGraph.functionsCalledByUnknownCode());
    for (size_t index : isCalledPerEvent)
        callees.append(FunctionID(index));
    sortAndRemoveDuplicates(loops, callees);

    WeightedCallList calls;
    calls.reserveInitialCapacity(callees.size());
    for (FunctionID callee : callees) {
        if (!program.at(callee).hasBody() || isCalledOnceByProgram.get(callee.index()))
            continue;
        calls.append(WeightedCall { graph.nodeOf(callee).index(), isCalledPerEvent.get(callee.index()) ? HeatModel::eventsPerRun : 1.0 });
    }
    return calls;
}

static WeightedCallList collectUnknownCodeCalls(const Program& program, const CallGraph& callGraph, const FlowResults& flow, const HeatGraph& graph, const BitVector& isCalledPerEvent)
{
    WeightedCallList calls;
    // Where no unseen code runs, native code alone stands behind the node, and flow lists what native code calls.
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

void HeatGraph::computePredecessors()
{
    m_firstPredecessor.fill(0u, m_firstSuccessor.size());
    for (Node successor : m_successors)
        ++m_firstPredecessor[successor.index() + 1];
    std::partial_sum(m_firstPredecessor.begin(), m_firstPredecessor.end(), m_firstPredecessor.begin());
    Vector<uint32_t> nextPositions = m_firstPredecessor;
    m_predecessors.grow(m_successors.size());
    for (unsigned index = 0; index < numNodes(); ++index) {
        for (Node successor : successors(Node(index)))
            m_predecessors[nextPositions[successor.index()]++] = Node(index);
    }
}

HeatGraph buildHeatGraph(const Program& program, const CallGraph& callGraph, const FlowResults& flow, const EffectSummaries& effects, BuildContext& build)
{
    unsigned count = program.functions.size();
    HeatGraph graph;
    graph.m_sizes.fill(0u, count);
    graph.m_masses.fill(0.0, count);
    graph.m_blockGroups.grow(count);
    unsigned numberOfNodes = graph.numNodes();
    Vector<WeightedCallList> calls(numberOfNodes);
    FunctionsCalledAfterReturn calledAfterReturn = findFunctionsCalledAfterReturn(flow);
    forEachIndexInParallel(build.parallelLoops(), count, [&](unsigned index) {
        FunctionID id(index);
        if (!program.at(id).hasBody())
            return;
        HeatEdgeCollector collector(program, flow, graph, calledAfterReturn, id, calls[index]);
        collector.collect(callGraph, effects, build);
        graph.m_sizes[index] = collector.size();
        graph.m_masses[index] = collector.mass();
        graph.m_blockGroups[index] = collector.takeBlockGroups();
    });
    BitVector isCalledOnceByProgram = findFunctionsCalledOnceByProgram(program, flow, graph, calls);
    calls[graph.hostNode().index()] = collectHostCalls(program, callGraph, graph, calledAfterReturn.perEvent, isCalledOnceByProgram, build.parallelLoops());
    calls[graph.unknownCodeNode().index()] = collectUnknownCodeCalls(program, callGraph, flow, graph, calledAfterReturn.perEvent);

    graph.m_isPresent.fill(true, numberOfNodes);
    for (unsigned index = 0; index < count; ++index)
        graph.m_isPresent[index] = program.functions[index].hasBody();

    graph.m_firstSuccessor.fill(0u, numberOfNodes + 1);
    for (unsigned index = 0; index < numberOfNodes; ++index)
        graph.m_firstSuccessor[index + 1] = graph.m_firstSuccessor[index] + calls[index].size();
    graph.m_successors.reserveInitialCapacity(graph.m_firstSuccessor[numberOfNodes]);
    graph.m_multipliers.reserveInitialCapacity(graph.m_firstSuccessor[numberOfNodes]);
    for (const WeightedCallList& list : calls) {
        for (const WeightedCall& call : list) {
            graph.m_successors.append(HeatNodeID(call.node));
            graph.m_multipliers.append(call.weight);
        }
    }
    graph.computePredecessors();
    return graph;
}

static HeatNodeID chooseCycleHeader(const HeatGraph& graph, std::span<const HeatNodeID> members, const auto& isInCycle)
{
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
}

Vector<double> estimateActivations(HeatGraph& graph)
{
    WTF::CycleForest<HeatGraph> cycles(graph, [&](std::span<const HeatNodeID> members, const Vector<HeatNodeID, 2>&, const auto& isInCycle) {
        return chooseCycleHeader(graph, members, isInCycle);
    });
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
