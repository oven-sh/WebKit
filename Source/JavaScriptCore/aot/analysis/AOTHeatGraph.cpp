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
#include "AOTLargeVectors.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <algorithm>
#include <atomic>
#include <numeric>
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

// The unknown summary and the summary of native code claim every calling use, which says nothing about the number of calls.
static bool saysNothingAboutCalls(OperandUses uses)
{
    return uses.containsAll(callingUses);
}

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

// The callees of a call are its candidates, and a constructor reads the prototype of new.target without calling it. The code that runs gets every other operand.
static bool isHandedToCodeThatRuns(const Value& user, uint32_t operandIndex)
{
    if (effectRuleOf(user.opcode) != EffectRule::Call)
        return true;
    return !user.operandUses(operandIndex).containsAny(callingUses) && OperandPosition::newTarget(user.opcode) != operandIndex;
}

static bool isHandedToCodeThatRuns(const Terminator&, uint32_t)
{
    return true;
}

namespace {

class HeatEdgeCollector {
public:
    HeatEdgeCollector(const Program& program, const CallGraph& callGraph, const FlowResults& flow, const HeatGraph& graph, const FunctionsCalledAfterReturn& calledAfterReturn, const EntriesThatRunNoCode& entriesThatRunNoCode, FunctionID id, WeightedCallList& calls, Vector<uint8_t>& isCountedByProgram)
        : m_program(program)
        , m_callGraph(callGraph)
        , m_flow(flow)
        , m_graph(graph)
        , m_calledAfterReturn(calledAfterReturn)
        , m_entriesThatRunNoCode(entriesThatRunNoCode)
        , m_function(program.at(id))
        , m_id(id)
        , m_calls(calls)
        , m_isCountedByProgram(isCountedByProgram)
    {
    }

    void collect(const EffectSummaries& effects, BuildContext& build, const ReferencesOfBody* references, const Vector<WeightOfBlock>* keptWeights, Vector<WeightOfBlock>* madeWeights)
    {
        FunctionAnalyses analyses(AnalysisContext { build, m_program, &m_flow, &effects, m_id }, m_function);
        auto visit = [&](BlockID id, double weight) {
            const Block& block = m_function.at(id);
            uint32_t blockSize = block.values.size() + 1;
            m_size += blockSize;
            m_mass += weight * blockSize;
            m_blockGroups.append(HeatBlockGroup { weight, blockSize });
            if (references) {
                addCallsOfNotedSites(analyses, id, *references, weight);
                return;
            }
            for (ValueID value : block.values)
                addCalls(analyses, m_function.at(value), weight);
            addCalls(analyses, block.terminator, weight);
        };
        if (keptWeights) {
            RELEASE_ASSERT(!build.options().verifiesAfterEveryPass || *keptWeights == computeWeights(analyses));
            for (const WeightOfBlock& kept : *keptWeights)
                visit(kept.block, kept.weight);
            if (madeWeights)
                *madeWeights = *keptWeights;
        } else if (madeWeights) {
            *madeWeights = computeWeights(analyses);
            for (const WeightOfBlock& made : *madeWeights)
                visit(made.block, made.weight);
        } else {
            const BlockFrequencies& frequencies = analyses.blockFrequencies();
            for (BlockID id : analyses.cfg().reversePostOrder())
                visit(id, frequencies.frequencyOf(id));
        }
        mergeEdges(m_calls);
        addIndirectCalls();
        mergeBlockGroups();
    }

    uint32_t size() const { return m_size; }
    double mass() const { return m_mass; }
    Vector<HeatBlockGroup, 4> takeBlockGroups() { return WTF::move(m_blockGroups); }

private:
    enum class CallOfHost : uint8_t { StaysUnlessCalleeRunsOnce, IsReplaced };

    static Vector<WeightOfBlock> computeWeights(FunctionAnalyses& analyses)
    {
        const BlockFrequencies& frequencies = analyses.blockFrequencies();
        Vector<WeightOfBlock> weights;
        weights.reserveInitialCapacity(analyses.cfg().reversePostOrder().size());
        for (BlockID id : analyses.cfg().reversePostOrder())
            weights.append(WeightOfBlock { id, frequencies.frequencyOf(id) });
        return weights;
    }

    // The walk cannot tell a call of a handler from the operand that hands the handler over. Beside the calls of the host neither counts.
    void add(FunctionID callee, double weight, CallOfHost callOfHost = CallOfHost::StaysUnlessCalleeRunsOnce)
    {
        if (!m_program.at(callee).hasBody() || m_calledAfterReturn.perEvent.get(callee.index()))
            return;
        m_calls.append(WeightedCall { m_graph.nodeOf(callee).index(), weight });
        if (callOfHost == CallOfHost::IsReplaced || m_flow.runsAtMostOnce(m_program, callee))
            AtomicRef(m_isCountedByProgram[callee.index()]).store(1, std::memory_order_relaxed);
    }

    void mergeBlockGroups()
    {
        AOT::sort(m_blockGroups, std::ranges::greater { }, &HeatBlockGroup::frequency);
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
        if (m_callGraph.callerSet(m_id) != CallerSet::Complete)
            return false;
        size_t parameter = m_function.at(entryBlock).parameters.find(analyses.identityOf(operand));
        if (parameter == notFound)
            return false;
        return estimateCallbackCalls(analyses.effectContext().summaries.of(m_id).operand(static_cast<uint32_t>(parameter)).uses) > 1;
    }

    // Code that nobody knows is expected to call a closure once each time it is handed the closure.
    void addHandOver(ValueID operand, double weight)
    {
        CalleeList callees = calleesOf(m_program, m_program.typeOf(m_function, operand));
        for (FunctionID callee : callees.functions) {
            if (m_callGraph.callerSet(callee) != CallerSet::Complete && !m_calledAfterReturn.once.get(callee.index()))
                add(callee, weight / callees.functions.size(), CallOfHost::IsReplaced);
        }
    }

    bool runsNoCode(const Value& value) const { return m_entriesThatRunNoCode.opcodes.contains(value.opcode); }
    bool runsNoCode(const Terminator& terminator) const { return m_entriesThatRunNoCode.terminatorKinds.contains(terminator.kind); }

    void addCalls(FunctionAnalyses& analyses, const auto& instruction, double weight)
    {
        if (runsNoCode(instruction))
            return;
        EffectDescription description = describeEffects(m_program, m_function, instruction);
        if (description.functions.isEmpty() && !description.callsUnknownCode)
            return;
        addCallsOfSite(analyses, instruction, weight, description.functions.span(), description.callsUnknownCode);
    }

    // Whoever made the references has described every entry of the body as it is, and has noted those that run code.
    void addCallsOfNotedSites(FunctionAnalyses& analyses, BlockID id, const ReferencesOfBody& references, double weight)
    {
        const Block& block = m_function.at(id);
        std::span<const SiteThatRunsCode> sites = references.sitesThatRunCode.span();
        size_t position = std::ranges::lower_bound(sites, id, { }, &SiteThatRunsCode::block) - sites.begin();
        if (analyses.context().build.options().verifiesAfterEveryPass) [[unlikely]]
            checkNotedSites(id, references, position);
        for (; position < sites.size() && sites[position].block == id; ++position) {
            const SiteThatRunsCode& site = sites[position];
            auto addCallsOfEntry = [&](const auto& instruction) {
                if (site.listsManyFunctions)
                    addCalls(analyses, instruction, weight);
                else
                    addCallsOfSite(analyses, instruction, weight, references.functionsOfSite(position), site.callsUnknownCode);
            };
            if (site.indexInBlock < block.values.size())
                addCallsOfEntry(m_function.at(block.values[site.indexInBlock]));
            else
                addCallsOfEntry(block.terminator);
        }
    }

    // The call graph has compared the references with the bodies. This finds a body that has changed since, of which nobody was told.
    void checkNotedSites(BlockID id, const ReferencesOfBody& references, size_t position) const
    {
        const Block& block = m_function.at(id);
        std::span<const SiteThatRunsCode> sites = references.sitesThatRunCode.span();
        auto check = [&](const auto& instruction, uint32_t indexInBlock) {
            EffectDescription description = describeEffects(m_program, m_function, instruction);
            if (description.functions.isEmpty() && !description.callsUnknownCode)
                return;
            RELEASE_ASSERT(position < sites.size());
            const SiteThatRunsCode& site = sites[position];
            RELEASE_ASSERT(site.block == id && site.indexInBlock == indexInBlock && !!site.callsUnknownCode == description.callsUnknownCode);
            RELEASE_ASSERT(site.listsManyFunctions || std::ranges::equal(references.functionsOfSite(position), description.functions));
            ++position;
        };
        for (uint32_t index = 0; index < block.values.size(); ++index)
            check(m_function.at(block.values[index]), index);
        check(block.terminator, block.values.size());
        RELEASE_ASSERT(position == sites.size() || sites[position].block != id);
    }

    void addCallsOfSite(FunctionAnalyses& analyses, const auto& instruction, double weight, std::span<const FunctionID> functions, bool callsUnknownCode)
    {
        addCandidates(functions, weight);
        if (callsUnknownCode)
            addUnknownCode(weight);
        instruction.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            // Only the calling uses are read.
            OperandUses uses = summarizeOperand(analyses.effectContext(), m_function, instruction, operandIndex, UsesAskedFor::AllButPassesToUserCode).uses;
            if (analyses.context().build.options().verifiesAfterEveryPass) [[unlikely]]
                RELEASE_ASSERT((uses | OperandUse::PassesToUserCode) == (summarizeOperand(analyses.effectContext(), m_function, instruction, operandIndex).uses | OperandUse::PassesToUserCode));
            if (saysNothingAboutCalls(uses)) {
                if (callsUnknownCode && isHandedToCodeThatRuns(instruction, operandIndex))
                    addHandOver(operand, weight);
                return;
            }
            double additionalCalls = estimateCallbackCalls(uses) - 1;
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

    // The walk and flow count the same calls, each by what it knows: the walk by how often the block of the site runs, flow by how often a built-in calls what it is handed.
    void addIndirectCalls()
    {
        FunctionID idAsRead = findFunctionAsRead(&m_program, m_id);
        if (!idAsRead)
            return;
        Vector<std::pair<FunctionID, double>, 8> countedByFlow;
        for (const FlowIndirectCall& call : m_flow.indirectCallsFrom(idAsRead)) {
            if (double calls = estimateIndirectCalls(call))
                countedByFlow.append({ call.callee, calls });
        }
        AOT::stableSort(countedByFlow, { }, [](const auto& entry) {
            return entry.first.index();
        });

        size_t mergedSize = m_calls.size();
        auto callsCountedByWalk = [&](FunctionID callee) {
            std::span<const WeightedCall> edges = m_calls.span().first(mergedSize);
            auto edge = std::ranges::lower_bound(edges, m_graph.nodeOf(callee).index(), { }, &WeightedCall::node);
            return edge != edges.end() && edge->node == m_graph.nodeOf(callee).index() ? edge->weight : 0;
        };
        for (size_t index = 0; index < countedByFlow.size();) {
            FunctionID callee = countedByFlow[index].first;
            double calls = 0;
            for (; index < countedByFlow.size() && countedByFlow[index].first == callee; ++index)
                calls += countedByFlow[index].second;
            double countedByWalk = callsCountedByWalk(callee);
            if (calls > countedByWalk)
                add(callee, calls - countedByWalk);
        }
        if (m_calls.size() > mergedSize)
            mergeEdges(m_calls);
    }

    static double estimateIndirectCalls(const FlowIndirectCall& call)
    {
        // The flag of a callback is about the site that calls the built-in. On the other kinds flow also sets it for a call that the operation itself repeats.
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

    const Program& m_program;
    const CallGraph& m_callGraph;
    const FlowResults& m_flow;
    const HeatGraph& m_graph;
    const FunctionsCalledAfterReturn& m_calledAfterReturn;
    const EntriesThatRunNoCode& m_entriesThatRunNoCode;
    const Function& m_function;
    FunctionID m_id;
    WeightedCallList& m_calls;
    Vector<uint8_t>& m_isCountedByProgram;
    uint32_t m_size { 0 };
    double m_mass { 0 };
    Vector<HeatBlockGroup, 4> m_blockGroups;
};

class ReachedNodes {
public:
    explicit ReachedNodes(const ParallelArray<WeightedCallList>& calls)
        : m_calls(calls)
        , m_isReached(calls.size())
    {
    }

    bool contains(uint32_t node) const { return m_isReached.quickGet(node); }

    void addWithCallees(uint32_t node)
    {
        if (m_isReached.quickSet(node))
            return;
        m_worklist.append(node);
        while (!m_worklist.isEmpty()) {
            for (const WeightedCall& call : m_calls[m_worklist.takeLast()]) {
                if (!m_isReached.quickSet(call.node))
                    m_worklist.append(call.node);
            }
        }
    }

private:
    const ParallelArray<WeightedCallList>& m_calls;
    BitVector m_isReached;
    Vector<uint32_t> m_worklist;
};

} // anonymous namespace

// The one call of the host stands for calls that nobody sees.
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

    ReachedNodes reached(callsOfProgram);
    for (const WeightedCall& call : calls)
        reached.addWithCallees(call.node);
    size_t numberOfSortedCalls = calls.size();
    for (FunctionID callee : calleesCountedByProgram) {
        if (reached.contains(graph.nodeOf(callee).index()))
            continue;
        calls.append(callOf(callee));
        reached.addWithCallees(graph.nodeOf(callee).index());
    }
    if (calls.size() > numberOfSortedCalls)
        AOT::sort(calls, { }, &WeightedCall::node);
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
    FunctionsCalledAfterReturn calledAfterReturn = findFunctionsCalledAfterReturn(flow);
    const EntriesThatRunNoCode entriesThatRunNoCode = findEntriesThatRunNoCode();
    graph.m_blockGroups = ParallelArray<Vector<HeatBlockGroup, 4>>(loops, count, largeFunctions(program, loops).span(), [&](unsigned index) -> Vector<HeatBlockGroup, 4> {
        FunctionID id(index);
        if (!program.at(id).hasBody()) {
            graph.m_isPresent[index] = false;
            return { };
        }
        HeatEdgeCollector collector(program, callGraph, flow, graph, calledAfterReturn, entriesThatRunNoCode, id, calls[index], isCountedByProgram);
        bool hasKeptWeights = keptWeights && index < keptWeights->m_blocks.size() && !keptWeights->m_blocks[index].isEmpty();
        collector.collect(effects, build, references ? &references->of(id) : nullptr, hasKeptWeights ? &keptWeights->m_blocks[index] : nullptr, madeWeights ? &madeWeights->m_blocks[index] : nullptr);
        graph.m_sizes[index] = collector.size();
        graph.m_masses[index] = collector.mass();
        return collector.takeBlockGroups();
    });
    calls[graph.unknownCodeNode().index()] = collectUnknownCodeCalls(program, callGraph, flow, graph, calledAfterReturn.perEvent);
    calls[graph.hostNode().index()] = collectHostCalls(program, callGraph, graph, calls, calledAfterReturn.perEvent, isCountedByProgram, loops);

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
    AOT_THRESHOLD(unsigned, maxDepthOfCallCycles, 16);
    WTF::CycleForest<HeatGraph> cycles(graph, [&](std::span<const HeatNodeID> members, const Vector<HeatNodeID, 2>&, const auto& isInCycle) {
        return chooseCycleHeader(graph, members, isInCycle);
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
