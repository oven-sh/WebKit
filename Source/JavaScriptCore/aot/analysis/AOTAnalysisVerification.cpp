/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFunctionAnalyses.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

template<typename T>
static void checkEqual(std::span<const T> cached, std::span<const T> fresh)
{
    RELEASE_ASSERT(cached.size() == fresh.size());
    for (size_t index = 0; index < cached.size(); ++index)
        RELEASE_ASSERT(cached[index] == fresh[index]);
}

static void checkRequiredAnalyses(AnalysisSet cached)
{
    auto check = [&](FunctionAnalysis analysis, AnalysisSet required) {
        RELEASE_ASSERT(!cached.contains(analysis) || cached.containsAll(required));
    };
    check(FunctionAnalysis::Dominators, { FunctionAnalysis::ControlFlow });
    check(FunctionAnalysis::NaturalLoops, { FunctionAnalysis::ControlFlow, FunctionAnalysis::Dominators });
    check(FunctionAnalysis::Cycles, { FunctionAnalysis::ControlFlow });
    check(FunctionAnalysis::UseLists, { FunctionAnalysis::ControlFlow });
    check(FunctionAnalysis::Identities, { FunctionAnalysis::ControlFlow });
    check(FunctionAnalysis::Liveness, { FunctionAnalysis::ControlFlow, FunctionAnalysis::UseLists });
    check(FunctionAnalysis::Escape, { FunctionAnalysis::ControlFlow, FunctionAnalysis::Cycles, FunctionAnalysis::UseLists });
    check(FunctionAnalysis::EscapeHistory, { FunctionAnalysis::ControlFlow, FunctionAnalysis::Escape });
    check(FunctionAnalysis::Frequencies, { FunctionAnalysis::ControlFlow, FunctionAnalysis::Cycles });
    check(FunctionAnalysis::Guards, { FunctionAnalysis::ControlFlow, FunctionAnalysis::UseLists, FunctionAnalysis::Identities });
    check(FunctionAnalysis::FailedTests, { FunctionAnalysis::ControlFlow });
    check(FunctionAnalysis::Induction, { FunctionAnalysis::ControlFlow, FunctionAnalysis::Dominators, FunctionAnalysis::NaturalLoops });
}

static void checkControlFlow(const CFG& cached, const CFG& fresh)
{
    RELEASE_ASSERT(cached.numNodes() == fresh.numNodes());
    checkEqual(cached.reversePostOrder(), fresh.reversePostOrder());
    for (unsigned blockIndex = 0; blockIndex < fresh.numNodes(); ++blockIndex) {
        BlockID block(blockIndex);
        RELEASE_ASSERT(cached.isReachable(block) == fresh.isReachable(block));
        RELEASE_ASSERT(cached.hasHandler(block) == fresh.hasHandler(block));
        checkEqual(cached.successors(block), fresh.successors(block));
        checkEqual(cached.predecessors(block), fresh.predecessors(block));
        checkEqual(cached.incomingEdges(block), fresh.incomingEdges(block));
    }
}

static void checkDominators(const CFG& graph, const Dominators& cached, const Dominators& fresh, bool selfCheck)
{
    for (BlockID block : graph.reversePostOrder())
        RELEASE_ASSERT(cached.idom(block) == fresh.idom(block));
    if (!selfCheck)
        return;
    for (BlockID from : graph.reversePostOrder()) {
        if (!graph.hasHandler(from))
            continue;
        for (BlockID to : graph.reversePostOrder())
            RELEASE_ASSERT(cached.terminatorDominates(from, to) == fresh.terminatorDominates(from, to));
    }
}

static BlockID innerMostLoopHeaderOf(const NaturalLoops& loops, BlockID block)
{
    const NaturalLoop* loop = loops.innerMostLoopOf(block);
    return loop ? loop->header() : BlockID();
}

static Vector<BlockID> sortedBlocksOf(const NaturalLoop& loop)
{
    Vector<BlockID> blocks;
    blocks.reserveInitialCapacity(loop.size());
    for (unsigned index = 0; index < loop.size(); ++index)
        blocks.append(loop.at(index));
    std::ranges::sort(blocks);
    return blocks;
}

static void checkNaturalLoops(const CFG& graph, const NaturalLoops& cached, const NaturalLoops& fresh)
{
    RELEASE_ASSERT(cached.numLoops() == fresh.numLoops());
    for (BlockID block : graph.reversePostOrder())
        RELEASE_ASSERT(innerMostLoopHeaderOf(cached, block) == innerMostLoopHeaderOf(fresh, block));
    for (unsigned loopIndex = 0; loopIndex < fresh.numLoops(); ++loopIndex) {
        const NaturalLoop& freshLoop = fresh.loop(loopIndex);
        const NaturalLoop* cachedLoop = cached.headerOf(freshLoop.header());
        RELEASE_ASSERT(cachedLoop);
        Vector<BlockID> cachedBlocks = sortedBlocksOf(*cachedLoop);
        Vector<BlockID> freshBlocks = sortedBlocksOf(freshLoop);
        checkEqual(cachedBlocks.span(), freshBlocks.span());
    }
}

static void checkBlockCycles(const CFG& graph, const BlockCycles& cached, const BlockCycles& fresh)
{
    BlockID previous;
    for (BlockID block : graph.reversePostOrder()) {
        RELEASE_ASSERT(cached.isInCycle(block) == fresh.isInCycle(block));
        RELEASE_ASSERT(cached.depthOf(block) == fresh.depthOf(block));
        RELEASE_ASSERT(cached.outermostCycleOf(block) == fresh.outermostCycleOf(block));
        RELEASE_ASSERT(cached.innermostCycleOf(block) == fresh.innermostCycleOf(block));
        if (previous)
            RELEASE_ASSERT(cached.areInSameCycle(previous, block) == fresh.areInSameCycle(previous, block));
        previous = block;
    }
}

static void checkUseLists(const Function& function, const UseLists& cached, const UseLists& fresh)
{
    for (uint32_t valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
        ValueID value(valueIndex);
        RELEASE_ASSERT(cached.locationOf(value) == fresh.locationOf(value));
        checkEqual(cached.usesOf(value), fresh.usesOf(value));
    }
}

static void checkIdentities(const Function& function, const ValueIdentities& cached, const ValueIdentities& fresh)
{
    for (uint32_t valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
        ValueID value(valueIndex);
        RELEASE_ASSERT(cached.identityOf(value) == fresh.identityOf(value));
    }
}

static void checkLiveness(const CFG& graph, const Liveness& cached, const Liveness& fresh)
{
    for (BlockID block : graph.reversePostOrder())
        checkEqual(cached.liveAtHead(block), fresh.liveAtHead(block));
}

static void checkEffects(const EffectsCache& cached, FunctionAnalyses& fresh)
{
    cached.forEachCachedValue([&](ValueID value, const EffectsOnEdges& effects) {
        RELEASE_ASSERT(effects == fresh.effectsOf(value));
    });
    cached.forEachCachedTerminator([&](BlockID block, const EffectsOnEdges& effects) {
        RELEASE_ASSERT(effects == fresh.effectsOfTerminator(block));
    });
}

static void checkTrackedObject(const TrackedObject& cached, const TrackedObject& fresh)
{
    RELEASE_ASSERT(cached.definition == fresh.definition);
    RELEASE_ASSERT(cached.kind == fresh.kind);
    RELEASE_ASSERT(cached.state == fresh.state);
    RELEASE_ASSERT(cached.reasons == fresh.reasons);
    RELEASE_ASSERT(cached.firstEscape == fresh.firstEscape);
    RELEASE_ASSERT(cached.uses == fresh.uses);
    RELEASE_ASSERT(cached.liveInstances == fresh.liveInstances);
}

static void checkEscapeAnalysis(const Function& function, const EscapeAnalysis& cached, const EscapeAnalysis& fresh)
{
    RELEASE_ASSERT(cached.gaveUp() == fresh.gaveUp());
    RELEASE_ASSERT(cached.numberOfObjects() == fresh.numberOfObjects());
    for (unsigned objectIndex = 0; objectIndex < fresh.numberOfObjects(); ++objectIndex) {
        ObjectID object(objectIndex);
        checkTrackedObject(cached.at(object), fresh.at(object));
        checkEqual(cached.aliasesOf(object), fresh.aliasesOf(object));
        checkEqual(cached.usesOf(object), fresh.usesOf(object));
        checkEqual(cached.contentsOf(object), fresh.contentsOf(object));
        checkEqual(cached.containersOf(object), fresh.containersOf(object));
    }
    for (uint32_t valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
        PointsToSet cachedSet = cached.pointsTo(ValueID(valueIndex));
        PointsToSet freshSet = fresh.pointsTo(ValueID(valueIndex));
        RELEASE_ASSERT(cachedSet.objects == freshSet.objects);
        RELEASE_ASSERT(cachedSet.mayBeUntracked == freshSet.mayBeUntracked);
    }
}

static void checkUseEffectHistory(const Function& function, const CFG& graph, unsigned numberOfObjects, const UseEffectHistory& cached, const UseEffectHistory& fresh)
{
    RELEASE_ASSERT(cached.gaveUp() == fresh.gaveUp());
    if (fresh.gaveUp())
        return;
    for (BlockID block : graph.reversePostOrder()) {
        ProgramPoint head = ProgramPoint::atHeadOf(block);
        ProgramPoint terminator = pointBeforeTerminatorOf(function, block);
        for (unsigned objectIndex = 0; objectIndex < numberOfObjects; ++objectIndex) {
            ObjectID object(objectIndex);
            RELEASE_ASSERT(cached.occurredBefore(object, head) == fresh.occurredBefore(object, head));
            RELEASE_ASSERT(cached.occurredBefore(object, terminator) == fresh.occurredBefore(object, terminator));
        }
    }
}

static void checkFrequencies(const Function& function, const CFG& graph, const BlockFrequencies& cached, const BlockFrequencies& fresh)
{
    for (BlockID block : graph.reversePostOrder()) {
        RELEASE_ASSERT(cached.frequencyOf(block) == fresh.frequencyOf(block));
        RELEASE_ASSERT(cached.classOf(block) == fresh.classOf(block));
        forEachOutgoingEdge(function, block, [&](Edge edge, const Target&) {
            RELEASE_ASSERT(cached.classOf(edge) == fresh.classOf(edge));
        });
    }
}

static void checkGuards(const Function& function, const CFG& graph, const GuardAvailability& cached, const GuardAvailability& fresh)
{
    RELEASE_ASSERT(cached.gaveUp() == fresh.gaveUp());
    Vector<ValueID> guards;
    for (uint32_t valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
        ValueID value(valueIndex);
        const GuardFacts& facts = fresh.factsOf(value);
        RELEASE_ASSERT(cached.factsOf(value) == facts);
        if (facts.isKnown())
            guards.append(value);
    }
    if (fresh.gaveUp())
        return;
    for (BlockID block : graph.reversePostOrder()) {
        ProgramPoint head = ProgramPoint::atHeadOf(block);
        for (ValueID guard : guards)
            RELEASE_ASSERT(cached.stateAt(guard, head) == fresh.stateAt(guard, head));
    }
}

static void checkFailedTests(const Function& function, const CFG& graph, const FailedTests& cached, const FailedTests& fresh)
{
    for (BlockID block : graph.reversePostOrder()) {
        const Terminator& terminator = function.at(block).terminator;
        if (!terminator.isConditionalBranch())
            continue;
        const Value& test = function.at(terminator.conditionOperand());
        if (provesOf(test.opcode) == Proof::None)
            continue;
        for (BlockID successor : graph.successors(block))
            RELEASE_ASSERT(cached.find(test, successor) == fresh.find(test, successor));
    }
}

static void checkInduction(const InductionVariables& cached, const InductionVariables& fresh)
{
    checkEqual(cached.loops(), fresh.loops());
}

void FunctionAnalyses::verifyCached() const
{
    AnalysisSet cached = computed();
    RELEASE_ASSERT(cached.contains(FunctionAnalysis::ControlFlow) || !m_controlFlow.edgesFromUnreachableBlocks);
    if (cached.isEmpty())
        return;
    checkRequiredAnalyses(cached);
    if (cached.contains(FunctionAnalysis::ControlFlow))
        checkBlockCount();
    if (cached.containsAny(valueAnalyses))
        checkValueCount();

    FunctionAnalyses fresh(m_context, m_function);
    CFG& graph = fresh.cfg();

    if (cached.contains(FunctionAnalysis::ControlFlow))
        checkControlFlow(*m_controlFlow.cfg, graph);
    if (m_controlFlow.edgesFromUnreachableBlocks)
        checkEqual(m_controlFlow.edgesFromUnreachableBlocks->span(), fresh.edgesFromUnreachableBlocks());
    if (cached.contains(FunctionAnalysis::Dominators)) {
        bool selfCheck = m_function.blocks.size() <= m_context.build.limit(LimitID::MaxBlocksForDominatorSelfCheck);
        Dominators freshDominators(graph, selfCheck);
        checkDominators(graph, *m_controlFlow.dominators, freshDominators, selfCheck);
        if (cached.contains(FunctionAnalysis::NaturalLoops)) {
            NaturalLoops freshNaturalLoops(graph, freshDominators, selfCheck);
            checkNaturalLoops(graph, *m_controlFlow.naturalLoops, freshNaturalLoops);
        }
    }
    if (cached.contains(FunctionAnalysis::Cycles))
        checkBlockCycles(graph, *m_controlFlow.blockCycles, fresh.blockCycles());

    if (cached.contains(FunctionAnalysis::UseLists))
        checkUseLists(m_function, *m_values.useLists, fresh.useLists());
    if (cached.contains(FunctionAnalysis::Identities))
        checkIdentities(m_function, *m_values.identities, fresh.identities());
    if (cached.contains(FunctionAnalysis::Liveness))
        checkLiveness(graph, *m_values.liveness, fresh.liveness());
    if (cached.contains(FunctionAnalysis::Effects))
        checkEffects(*m_values.effects, fresh);
    if (cached.contains(FunctionAnalysis::Escape))
        checkEscapeAnalysis(m_function, *m_values.escape, fresh.escape());
    if (cached.contains(FunctionAnalysis::EscapeHistory))
        checkUseEffectHistory(m_function, graph, fresh.escape().numberOfObjects(), *m_values.escapeHistory, fresh.escapeHistory());
    if (cached.contains(FunctionAnalysis::Frequencies))
        checkFrequencies(m_function, graph, *m_values.blockFrequencies, fresh.blockFrequencies());
    if (cached.contains(FunctionAnalysis::Guards))
        checkGuards(m_function, graph, *m_values.guards, fresh.guards());
    if (m_values.hasPristineTest && m_values.numberOfValuesSearchedForPristineTest == m_function.values.size())
        RELEASE_ASSERT(*m_values.hasPristineTest == fresh.hasPristineTest());
    if (cached.contains(FunctionAnalysis::FailedTests))
        checkFailedTests(m_function, graph, *m_values.failedTests, fresh.failedTests());
    if (cached.contains(FunctionAnalysis::Induction))
        checkInduction(*m_values.inductionVariables, fresh.inductionVariables());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
