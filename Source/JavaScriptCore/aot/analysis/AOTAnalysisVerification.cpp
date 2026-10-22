/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFunctionAnalyses.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEffectsDeclared.h"
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
    check(FunctionAnalysis::Liveness, { FunctionAnalysis::ControlFlow, FunctionAnalysis::UseLists });
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

static void checkControlFlowAgainstFunction(const Function& function, const CFG& graph)
{
    unsigned numberOfBlocks = function.blocks.size();
    RELEASE_ASSERT(graph.numNodes() == numberOfBlocks);

    BitVector isReachable(numberOfBlocks);
    Vector<BlockID> order;
    isReachable.set(0);
    Vector<std::pair<unsigned, uint32_t>> stack { { 0u, numberOfOutgoingEdges(function.blocks[0]) } };
    while (!stack.isEmpty()) {
        auto [block, numberOfUnvisitedEdges] = stack.last();
        if (!numberOfUnvisitedEdges) {
            order.append(BlockID(block));
            stack.removeLast();
            continue;
        }
        uint32_t next = --stack.last().second;
        unsigned target = targetOf(function, outgoingEdgeAt(function, BlockID(block), next)).block.index();
        if (!isReachable.set(target))
            stack.append({ target, numberOfOutgoingEdges(function.blocks[target]) });
    }
    order.reverse();
    checkEqual(graph.reversePostOrder(), order.span());

    bool hasCycle = false;
    Vector<Vector<Edge>> incomingEdges(numberOfBlocks);
    for (unsigned blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
        BlockID block(blockIndex);
        RELEASE_ASSERT(graph.isReachable(block) == isReachable.get(blockIndex));
        Vector<BlockID> successors;
        if (isReachable.get(blockIndex)) {
            forEachOutgoingEdge(function, block, [&](Edge edge, const Target& target) {
                successors.append(target.block);
                incomingEdges[target.block.index()].append(edge);
                hasCycle |= graph.reversePostOrderIndex(target.block) <= graph.reversePostOrderIndex(block);
            });
        }
        checkEqual(graph.successors(block), successors.span());
        RELEASE_ASSERT(graph.hasHandler(block) == (isReachable.get(blockIndex) && function.blocks[blockIndex].handler));
    }
    RELEASE_ASSERT(graph.hasCycle() == hasCycle);
    for (unsigned blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
        BlockID block(blockIndex);
        checkEqual(graph.incomingEdges(block), incomingEdges[blockIndex].span());
        Vector<BlockID> predecessors;
        for (Edge edge : incomingEdges[blockIndex]) {
            if (predecessors.isEmpty() || predecessors.last() != edge.from)
                predecessors.append(edge.from);
        }
        checkEqual(graph.predecessors(block), predecessors.span());
    }
    for (uint32_t orderIndex = 0; orderIndex < order.size(); ++orderIndex)
        RELEASE_ASSERT(graph.reversePostOrderIndex(order[orderIndex]) == orderIndex);
}

static void checkDominators(const CFG& graph, const Dominators& cached, const Dominators& fresh, bool selfCheck)
{
    for (BlockID block : graph.reversePostOrder()) {
        RELEASE_ASSERT(cached.idom(block) == fresh.idom(block));
        RELEASE_ASSERT(cached.idom(block) == fresh.treeForChecking().idom(block));
    }
    // What the graph answers in place of the tree.
    for (unsigned blockIndex = 0; blockIndex < graph.numNodes(); ++blockIndex)
        RELEASE_ASSERT(cached.dominates(graph.root(), BlockID(blockIndex)) == fresh.treeForChecking().dominates(graph.root(), BlockID(blockIndex)));
    if (!selfCheck)
        return;
    for (BlockID from : graph.reversePostOrder()) {
        if (!graph.hasHandler(from))
            continue;
        for (BlockID to : graph.reversePostOrder())
            RELEASE_ASSERT(cached.terminatorDominates(from, to) == fresh.terminatorDominatesInSplitGraph(from, to));
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

static void checkUseListsAgainstFunction(const Function& function, const CFG& graph, const UseLists& useLists)
{
    Vector<uint32_t> numberOfUsesSeen(FillWith { }, function.values.size(), 0u);
    Vector<ValueLocation> locations(function.values.size());
    auto checkNextUse = [&](ValueID used, const Use& use) {
        std::span<const Use> uses = useLists.usesOf(used);
        uint32_t position = numberOfUsesSeen[used.index()]++;
        RELEASE_ASSERT(position < uses.size() && uses[position] == use);
        RELEASE_ASSERT(usedValueOf(function, use) == used);
    };
    for (BlockID id : graph.reversePostOrder()) {
        const Block& block = function.at(id);
        auto checkArguments = [&](Use::Kind kind, const Target& target, uint32_t targetIndex) {
            for (uint32_t argumentIndex = 0; argumentIndex < target.arguments.size(); ++argumentIndex) {
                if (ValueID argument = target.arguments[argumentIndex])
                    checkNextUse(argument, Use { kind, id, { }, argumentIndex, targetIndex });
            }
        };
        for (uint32_t index = 0; index < block.parameters.size(); ++index)
            locations[block.parameters[index].index()] = { ValuePlacement::Parameter, id, index };
        for (uint32_t index = 0; index < block.values.size(); ++index)
            locations[block.values[index].index()] = { ValuePlacement::InBlock, id, index };
        if (block.subject)
            checkNextUse(block.subject, Use { Use::Kind::BlockSubject, id, { }, 0, 0 });
        if (block.handler)
            checkArguments(Use::Kind::HandlerArgument, *block.handler, 0);
        for (ValueID user : block.values) {
            function.at(user).forEachOperand([&](ValueID used, uint32_t operandIndex) {
                checkNextUse(used, Use { Use::Kind::Operand, id, user, operandIndex, 0 });
            });
        }
        block.terminator.forEachOperand([&](ValueID used, uint32_t operandIndex) {
            checkNextUse(used, Use { Use::Kind::TerminatorOperand, id, { }, operandIndex, 0 });
        });
        for (uint32_t targetIndex = 0; targetIndex < block.terminator.targets.size(); ++targetIndex)
            checkArguments(Use::Kind::EdgeArgument, block.terminator.targets[targetIndex], targetIndex);
    }
    for (uint32_t valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
        RELEASE_ASSERT(useLists.useCount(ValueID(valueIndex)) == numberOfUsesSeen[valueIndex]);
        RELEASE_ASSERT(useLists.locationOf(ValueID(valueIndex)) == locations[valueIndex]);
    }
}

static void checkIdentities(const Function& function, const ValueIdentities& cached, const ValueIdentities& fresh)
{
    for (uint32_t valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
        ValueID value(valueIndex);
        RELEASE_ASSERT(cached.identityOf(value) == fresh.identityOf(value));
    }
}

static void checkIdentitiesOnDemand(const Function& function, const CFG& graph, const ValueIdentities& fresh)
{
    ValueIdentities onDemand(function, graph, ValueIdentities::RepeatedWalks::StartAtFirstChangedBlock, nullptr, ValueIdentities::Evaluation::OnDemand);
    for (uint32_t valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
        ValueID value(valueIndex);
        if (ValueID identity = onDemand.tryResolve(value))
            RELEASE_ASSERT(identity == fresh.identityOf(value));
    }
}

static void checkLiveness(const CFG& graph, const Liveness& cached, const Liveness& fresh)
{
    for (BlockID block : graph.reversePostOrder())
        checkEqual(cached.liveAtHead(block), fresh.liveAtHead(block));
}

static void checkEffectFacts(Opcode opcode, const Effects& fresh)
{
    const OpcodeEffectFacts& facts = effectFactsOf(opcode);
    RELEASE_ASSERT(EffectTraits(fresh).containsAll(facts.traits.atLeast) && facts.traits.atMost.containsAll(EffectTraits(fresh)));
    RELEASE_ASSERT(!facts.staysWithinDeclaredBound || fresh.isSubsetOf(declaredBoundOf(opcode)));
    RELEASE_ASSERT(!facts.mayExitSideways || fresh.mayExitSideways());
    RELEASE_ASSERT(fresh.writes.heaps().containsAll(facts.writtenHeaps));
}

// An answer outlives a change of the function, so there may be answers where the effects do not count as computed.
static void checkEffects(const EffectContext& context, const Function& function, const EffectsCache& cached)
{
    auto checkValue = [&](ValueID id) {
        const EffectsOnEdges* kept = cached.findKept(id);
        if (!kept)
            return;
        const Value& value = function.at(id);
        EffectsOnEdges fresh = effectsOf(context, function, value);
        RELEASE_ASSERT(*kept == fresh);
        checkEffectFacts(value.opcode, fresh.onAnyEdge());
        checkEffectFacts(value.opcode, fresh.normal);
    };

    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const Block& block = function.blocks[blockIndex];
        for (ValueID parameter : block.parameters)
            checkValue(parameter);
        for (ValueID value : block.values)
            checkValue(value);
        if (const EffectsOnEdges* kept = cached.findKeptOfTerminator(BlockID(blockIndex)))
            RELEASE_ASSERT(*kept == effectsOf(context, function, block.terminator));
    }
}

static void checkTrackedObject(const TrackedObject& cached, const TrackedObject& fresh)
{
    RELEASE_ASSERT(cached.definition == fresh.definition);
    RELEASE_ASSERT(cached.kind == fresh.kind);
    RELEASE_ASSERT(cached.state == fresh.state);
    RELEASE_ASSERT(cached.reasons == fresh.reasons);
    RELEASE_ASSERT(cached.firstEscape == fresh.firstEscape);
    RELEASE_ASSERT(cached.uses == fresh.uses);
}

static void checkEscapeAnalysis(const Function& function, const EscapeAnalysis& cached, const EscapeAnalysis& fresh)
{
    RELEASE_ASSERT(cached.gaveUp() == fresh.gaveUp());
    RELEASE_ASSERT(cached.numberOfObjects() == fresh.numberOfObjects());
    for (unsigned objectIndex = 0; objectIndex < fresh.numberOfObjects(); ++objectIndex) {
        ObjectID object(objectIndex);
        checkTrackedObject(cached.at(object), fresh.at(object));
        if (cached.hasLiveInstances()) {
            RELEASE_ASSERT(cached.liveInstancesOf(object) == fresh.liveInstancesOf(object));
            RELEASE_ASSERT(cached.objectsRenewedAtMakersOf(object) == fresh.objectsRenewedAtMakersOf(object));
        }
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

// What comesFromOutside() sees from the definitions is what the sets say.
static void checkValuesFromOutside(FunctionAnalyses& fresh)
{
    const Function& function = fresh.function();
    const EscapeAnalysis& escape = fresh.escape();
    std::optional<ValueID> freshEntryParameter = findFreshEntryParameter(function, fresh.cfg());
    for (uint32_t valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
        ValueID value(valueIndex);
        if (!comesFromOutside(fresh, value))
            continue;
        PointsToSet set = escape.pointsTo(value);
        bool mayBeObjectFromOutside = set.mayBeUntracked;
        for (ObjectID object : set.objects)
            mayBeObjectFromOutside |= escape.at(object).kind == ObjectKind::EntryParameter && escape.at(object).definition != freshEntryParameter;
        RELEASE_ASSERT(mayBeObjectFromOutside);
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

void FunctionAnalyses::verifyUseLists() const
{
    const CFG& graph = *m_controlFlow.cfg;
    UseLists fresh(m_function, graph);
    checkUseLists(m_function, *m_values.useLists, fresh);
    checkUseListsAgainstFunction(m_function, graph, *m_values.useLists);
}

void FunctionAnalyses::verifyIdentities(const CFG& graph) const
{
    checkIdentities(m_function, *m_values.identities, ValueIdentities(m_function, graph, ValueIdentities::RepeatedWalks::StartAtEntry));
}

void FunctionAnalyses::verifyLiveness() const
{
    const CFG& graph = *m_controlFlow.cfg;
    LivenessOptions options;
    options.solver = LivenessOptions::Solver::PerValue;
    checkLiveness(graph, *m_values.liveness, Liveness(m_function, graph, *m_values.useLists, options));
}

void FunctionAnalyses::verifyFrequencies()
{
    CFG& graph = *m_controlFlow.cfg;
    BlockFrequencies fresh(m_context, m_function, graph, *m_controlFlow.blockCycles);
    checkFrequencies(m_function, graph, *m_values.blockFrequencies, fresh);
}

void FunctionAnalyses::verifyCached()
{
    // Lists that have been told of changes are compared as the next pass that asks for them will see them.
    if (m_values.useLists)
        m_values.useLists->endSilence();
    if (m_values.useLists && !m_values.useLists->isUpToDate() && !m_values.useLists->mustBeMadeAnew())
        useLists();

    AnalysisSet cached = computed();
    if (m_controlFlow.mayDescribeFormerGraph && m_controlFlow.cfg->describes(m_function))
        cached.add(heldControlFlowAnalyses());
    // Frequencies that the next who asks would be handed are compared, too.
    bool wouldKeepFrequencies = m_values.frequenciesMayDescribeFormerFunction && cached.contains(FunctionAnalysis::ControlFlow) && m_values.blockFrequencies->describes(m_context, m_function);
    RELEASE_ASSERT(cached.contains(FunctionAnalysis::ControlFlow) || !m_controlFlow.edgesFromUnreachableBlocks);
    checkEffects(m_effectContext, m_function, m_effects);
    if (cached.isEmpty())
        return;
    checkRequiredAnalyses(cached);
    if (cached.contains(FunctionAnalysis::ControlFlow))
        checkBlockCount();
    if (cached.containsAny(valueAnalyses))
        checkValueCount();

    FunctionAnalyses fresh(m_context, m_function);
    CFG& graph = fresh.cfg();

    if (cached.contains(FunctionAnalysis::ControlFlow)) {
        checkControlFlow(*m_controlFlow.cfg, graph);
        checkControlFlowAgainstFunction(m_function, *m_controlFlow.cfg);
    }
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

    if (cached.contains(FunctionAnalysis::UseLists)) {
        checkUseLists(m_function, *m_values.useLists, fresh.useLists());
        checkUseListsAgainstFunction(m_function, graph, *m_values.useLists);
    }
    if (cached.contains(FunctionAnalysis::Identities)) {
        ValueIdentities freshIdentities(m_function, graph, ValueIdentities::RepeatedWalks::StartAtEntry);
        m_values.identities->setGraph(&graph);
        checkIdentities(m_function, *m_values.identities, freshIdentities);
        m_values.identities->setGraph(nullptr);
        checkIdentitiesOnDemand(m_function, graph, freshIdentities);
    }
    if (cached.contains(FunctionAnalysis::Liveness)) {
        LivenessOptions options;
        options.solver = LivenessOptions::Solver::PerValue;
        checkLiveness(graph, *m_values.liveness, Liveness(m_function, graph, fresh.useLists(), options));
    }
    if (cached.contains(FunctionAnalysis::Escape))
        checkEscapeAnalysis(m_function, *m_values.escape, fresh.escapeWithLiveInstances());
    if (cached.contains(FunctionAnalysis::EscapeHistory))
        checkUseEffectHistory(m_function, graph, fresh.escape().numberOfObjects(), *m_values.escapeHistory, fresh.escapeHistory());
    if (cached.containsAny({ FunctionAnalysis::Escape, FunctionAnalysis::Guards }))
        checkValuesFromOutside(fresh);
    if (cached.contains(FunctionAnalysis::Frequencies) || wouldKeepFrequencies)
        checkFrequencies(m_function, graph, *m_values.blockFrequencies, fresh.blockFrequencies());
    if (cached.contains(FunctionAnalysis::Guards))
        checkGuards(m_function, graph, *m_values.guards, fresh.guards());
    if (m_values.opcodes && m_values.numberOfValuesWithCollectedOpcodes == m_function.values.size())
        RELEASE_ASSERT(*m_values.opcodes == fresh.opcodesOfValues());
    if (cached.contains(FunctionAnalysis::FailedTests))
        checkFailedTests(m_function, graph, *m_values.failedTests, fresh.failedTests());
    if (cached.contains(FunctionAnalysis::Induction))
        checkInduction(*m_values.inductionVariables, fresh.inductionVariables());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
