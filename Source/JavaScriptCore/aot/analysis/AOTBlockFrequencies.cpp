/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBlockFrequencies.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <array>
#include <tuple>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

using BranchSide = FrequencyFacts::BranchSide;

namespace {

struct FrequencyInputs {
    const Program& program;
    const Function& function;
    const CFG& cfg;
    const BlockCycles& cycles;
    bool ignoresBranchHints;
};

struct BranchCondition {
    ValueID test;
    bool isNegated { false };
};

} // anonymous namespace

static bool areBranchHintsIgnored(const AnalysisContext& context)
{
    return context.build.recordedOptions().has(TestingSwitch::IgnoreBranchHints);
}

static BranchSide oppositeOf(BranchSide side)
{
    switch (side) {
    case BranchSide::Unknown:
        return BranchSide::Unknown;
    case BranchSide::Taken:
        return BranchSide::NotTaken;
    case BranchSide::NotTaken:
        return BranchSide::Taken;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isNormalExit(const Terminator& terminator)
{
    return terminator.targets.isEmpty() && wayOutOf(terminator.kind) != WayOut::Abnormal;
}

static bool hasSingleTarget(const Function& function, BlockID block)
{
    return function.at(block).terminator.targets.size() == 1;
}

static bool isForwardEdge(const CFG& cfg, BlockID from, BlockID to)
{
    return cfg.reversePostOrderIndex(from) < cfg.reversePostOrderIndex(to);
}

static bool isEnteredByJumpFromLikelierBlock(const Function& function, const CFG& cfg, BlockID block)
{
    return std::ranges::any_of(cfg.incomingEdges(block), [&](Edge edge) {
        if (edge.isHandlerEdge() || !hasSingleTarget(function, edge.from) || !isForwardEdge(cfg, edge.from, block))
            return false;
        switch (function.at(edge.from).frequency) {
        case BlockFrequency::Normal:
            return true;
        case BlockFrequency::Unlikely:
            return isEnteredByJumpFromLikelierBlock(function, cfg, edge.from);
        case BlockFrequency::Rare:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

RecordedFrequencyUse recordedFrequencyUseOf(const Function& function, const CFG& cfg, BlockID id, bool ignoresBranchHints)
{
    switch (function.at(id).frequency) {
    case BlockFrequency::Normal:
        return RecordedFrequencyUse::NotRecorded;
    case BlockFrequency::Unlikely:
        if (ignoresBranchHints || isEnteredByJumpFromLikelierBlock(function, cfg, id))
            return RecordedFrequencyUse::Ignored;
        return RecordedFrequencyUse::Applied;
    case BlockFrequency::Rare:
        return RecordedFrequencyUse::Applied;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BlockFrequency appliedFrequencyOf(const FrequencyInputs& inputs, std::span<const FrequencyFacts::BlockFacts> blocks, BlockID id)
{
    BlockFrequency recorded = inputs.function.at(id).frequency;
    if (recorded != BlockFrequency::Unlikely)
        return recorded;
    if (inputs.ignoresBranchHints)
        return BlockFrequency::Normal;
    bool isEnteredByJumpFromLikelierBlock = std::ranges::any_of(inputs.cfg.incomingEdges(id), [&](Edge edge) {
        return !edge.isHandlerEdge() && hasSingleTarget(inputs.function, edge.from) && isForwardEdge(inputs.cfg, edge.from, id) && blocks[edge.from.index()].applied == BlockFrequency::Normal;
    });
    return isEnteredByJumpFromLikelierBlock ? BlockFrequency::Normal : BlockFrequency::Unlikely;
}

static bool isInherentlyRare(const Function& function, const Block& block)
{
    if (wayOutOf(block.terminator.kind) == WayOut::Abnormal)
        return true;
    return block.values.containsIf([&](ValueID id) {
        return function.at(id).type == TypeTable::never;
    });
}

static BranchCondition conditionOf(const Function& function, const Terminator& terminator)
{
    BranchCondition condition { terminator.conditionOperand() };
    for (; function.at(condition.test).opcode == Opcode::LogicalNot; condition.test = function.at(condition.test).operand(0))
        condition.isNegated = !condition.isNegated;
    return condition;
}

static BranchSide findNullishSide(const FrequencyInputs& inputs, const Value& test)
{
    auto isReference = [&](ValueID operand, uint16_t otherPrimitives) {
        auto& type = inputs.program.typeOf(inputs.function, operand);
        return type.mayBeUndefinedOrNull() && type.hasCells() && !(type.primitives & ~(Primitive::UndefinedOrNull | otherPrimitives));
    };
    auto isNullishConstant = [&](ValueID operand) {
        return inputs.function.at(operand).opcode == Opcode::Undefined || inputs.function.at(operand).opcode == Opcode::Null;
    };

    if (test.opcode == Opcode::TypeTest) {
        if (!isReference(test.operand(0), cellPrimitives))
            return BranchSide::Unknown;
        auto& tested = inputs.program.at(test.testedType());
        if (tested.isUndefinedOrNull())
            return BranchSide::Taken;
        auto& source = inputs.program.typeOf(inputs.function, test.operand(0));
        if (!tested.mayBeUndefinedOrNull() && source.withoutNullish().isSubsetOf(tested, inputs.program))
            return BranchSide::NotTaken;
        return BranchSide::Unknown;
    }
    if (test.opcode == Opcode::StrictEq || test.opcode == Opcode::Eq) {
        ValueID left = test.operand(0);
        ValueID right = test.operand(1);
        if (!(isNullishConstant(right) && isReference(left, cellPrimitives)) && !(isNullishConstant(left) && isReference(right, cellPrimitives)))
            return BranchSide::Unknown;
        return BranchSide::Taken;
    }
    if (test.opcode == Opcode::ToBoolean && isReference(test.operand(0), 0))
        return BranchSide::NotTaken;
    return BranchSide::Unknown;
}

static BranchSide findDifferentObjectsSide(const FrequencyInputs& inputs, BlockID block, const Value& test)
{
    if (inputs.cycles.isInCycle(block))
        return BranchSide::Unknown;
    if (test.opcode != Opcode::StrictEq)
        return BranchSide::Unknown;
    auto& left = inputs.program.typeOf(inputs.function, test.operand(0));
    auto& right = inputs.program.typeOf(inputs.function, test.operand(1));
    if (!left.hasObjects() || !right.hasObjects() || (!left.isAnything() && !right.isAnything()))
        return BranchSide::Unknown;
    return BranchSide::NotTaken;
}

static std::pair<BranchSide, BranchSide> findRarerSides(const FrequencyInputs& inputs, std::span<const FrequencyFacts::BlockFacts> blocks, BlockID block)
{
    const Function& function = inputs.function;
    const Terminator& terminator = function.at(block).terminator;
    if (!terminator.isConditionalBranch())
        return { };
    if (!blocks[terminator.taken().block.index()].isNormal() || !blocks[terminator.notTaken().block.index()].isNormal())
        return { };

    BranchCondition condition = conditionOf(function, terminator);
    auto& test = function.at(condition.test);

    BranchSide unlikelySide = findNullishSide(inputs, test);
    if (unlikelySide == BranchSide::Unknown)
        unlikelySide = findDifferentObjectsSide(inputs, block, test);

    auto isConstant = [&](ValueID value) {
        return integerConstantThroughCasts(inputs.program, function, value) || inputs.program.typeOf(function, value).singleStringLiteral();
    };
    auto isMaskedByConstant = [&](ValueID value) {
        while (function.at(value).opcode == Opcode::Upcast || function.at(value).opcode == Opcode::AssumeType)
            value = function.at(value).operand(0);
        auto& bits = function.at(value);
        return bits.opcode == Opcode::BitAnd && (integerConstantThroughCasts(inputs.program, function, bits.operand(0)) || integerConstantThroughCasts(inputs.program, function, bits.operand(1)));
    };
    BranchSide sideOffUsualPath = BranchSide::Unknown;
    if (test.opcode == Opcode::IsSlotFilled || (test.opcode == Opcode::TypeTest && !inputs.program.typeOf(function, test.operand(0)).isAnything()))
        sideOffUsualPath = BranchSide::NotTaken;
    else if (test.opcode == Opcode::StrictEq && isConstant(test.operand(0)) != isConstant(test.operand(1)))
        sideOffUsualPath = BranchSide::Taken;
    else if (test.opcode == Opcode::ToBoolean && isMaskedByConstant(test.operand(0)))
        sideOffUsualPath = BranchSide::Taken;
    if (condition.isNegated)
        return { oppositeOf(unlikelySide), oppositeOf(sideOffUsualPath) };
    return { unlikelySide, sideOffUsualPath };
}

static bool isOnSide(uint32_t targetIndex, BranchSide side)
{
    switch (side) {
    case BranchSide::Unknown:
        return false;
    case BranchSide::Taken:
        return !targetIndex;
    case BranchSide::NotTaken:
        return targetIndex == 1;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void forEachDecidedPredecessor(const FrequencyInputs& inputs, BlockID id, NOESCAPE const Invocable<void(BlockID predecessor, uint32_t successorIndex)> auto& functor)
{
    const Function& function = inputs.function;
    const Block& block = function.at(id);
    if (!block.terminator.isConditionalBranch())
        return;
    BranchCondition condition = conditionOf(function, block.terminator);
    size_t parameterIndex = block.parameters.find(condition.test);
    if (parameterIndex == notFound)
        return;
    for (Edge edge : inputs.cfg.incomingEdges(id)) {
        if (edge.isHandlerEdge() || !hasSingleTarget(function, edge.from))
            continue;
        IncomingValue incoming = incomingValueOf(function, edge, static_cast<uint32_t>(parameterIndex));
        if (incoming.kind != IncomingValue::Kind::Argument || !incoming.argument)
            continue;
        const Value& argument = function.at(incoming.argument);
        if (argument.opcode != Opcode::Boolean)
            continue;
        functor(edge.from, argument.boolean() != condition.isNegated ? 0u : 1u);
    }
}

static FrequencyFacts collectFacts(const FrequencyInputs& inputs)
{
    const Function& function = inputs.function;
    const CFG& cfg = inputs.cfg;
    FrequencyFacts facts;
    facts.blocks.grow(function.blocks.size());
    for (BlockID id : cfg.reversePostOrder()) {
        const Block& block = function.at(id);
        FrequencyFacts::BlockFacts& blockFacts = facts.blocks[id.index()];
        blockFacts.recorded = block.frequency;
        blockFacts.applied = appliedFrequencyOf(inputs, facts.blocks.span(), id);
        blockFacts.isInherentlyRare = isInherentlyRare(function, block);
        blockFacts.isNormalExit = isNormalExit(block.terminator);
    }
    for (unsigned index = 0; index < cfg.numNodes(); ++index) {
        BlockID id(index);
        if (!cfg.isReachable(id) || !function.at(id).terminator.isConditionalBranch())
            continue;
        std::tie(facts.blocks[index].unlikelySide, facts.blocks[index].sideOffUsualPath) = findRarerSides(inputs, facts.blocks.span(), id);
        forEachDecidedPredecessor(inputs, id, [&](BlockID predecessor, uint32_t successorIndex) {
            facts.decidedPredecessors.append(FrequencyFacts::DecidedPredecessor { id, predecessor, successorIndex });
        });
    }
    return facts;
}

namespace {

enum WeightLevel : uint8_t { FullWeight, UnlikelyWeight, TwiceUnlikelyWeight, RareWeight };

constexpr std::array<double, 4> weightOfLevel { 1, frequencyConstants.unlikelyEdgeShare, frequencyConstants.unlikelyEdgeShare * frequencyConstants.unlikelyEdgeShare, frequencyConstants.rareEdgeShare };
static_assert(weightOfLevel[TwiceUnlikelyWeight] > weightOfLevel[RareWeight] && weightOfLevel[TwiceUnlikelyWeight] * frequencyConstants.unlikelyEdgeShare <= weightOfLevel[RareWeight]);
static_assert(weightOfLevel[UnlikelyWeight] * 16 == 1 && weightOfLevel[TwiceUnlikelyWeight] * 256 == 1 && weightOfLevel[RareWeight] * 1024 == 1);

} // anonymous namespace

static ALWAYS_INLINE uint8_t weightLevelOfEdge(uint8_t levelOfSuccessor, BlockFrequency edgeClass)
{
    switch (edgeClass) {
    case BlockFrequency::Normal:
        return levelOfSuccessor;
    case BlockFrequency::Unlikely:
        return std::min<uint8_t>(levelOfSuccessor + 1, RareWeight);
    case BlockFrequency::Rare:
        return RareWeight;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BlockFrequency classOfWeightLevel(uint8_t level)
{
    static_assert(weightOfLevel[UnlikelyWeight] <= frequencyConstants.unlikelyEdgeShare && weightOfLevel[TwiceUnlikelyWeight] > frequencyConstants.rareEdgeShare);
    if (level == RareWeight)
        return BlockFrequency::Rare;
    return level == FullWeight ? BlockFrequency::Normal : BlockFrequency::Unlikely;
}

static ALWAYS_INLINE bool isBackEdge(const BlockCycles::Forest& forest, BlockID from, BlockID to)
{
    return forest.isHeader(to) && forest.contains(forest.innermost(to), from);
}

BlockFrequencies::BlockFrequencies(const AnalysisContext& context, const Function& function, CFG& cfg, const BlockCycles& sharedCycles)
    : m_cfg(cfg)
    , m_sharedCycles(sharedCycles)
    , m_blocks(FillWith { }, function.blocks.size(), BlockFrequency::Normal)
{
    if (sharedCycles.hasCycleWithSeveralEntries())
        m_cyclesByLikelihood.emplace(cfg, &function);
    m_facts = collectFacts(FrequencyInputs { context.program, function, cfg, cycles(), areBranchHintsIgnored(context) });

    std::span<const BlockID> order = cfg.reversePostOrder();
    size_t numberOfEdges = cfg.successorsOfAllBlocks().size();
    const BlockID* successors = cfg.successorsOfAllBlocks().data();
    const uint32_t* firstSuccessor = cfg.firstSuccessorOfBlocks().data();
    const FrequencyFacts::BlockFacts* facts = m_facts.blocks.span().data();
    m_edges.grow(numberOfEdges);
    BlockFrequency* edges = m_edges.mutableSpan().data();
    for (size_t edge = 0; edge < numberOfEdges; ++edge)
        edges[edge] = facts[successors[edge].index()].applied;
    m_weights.fill(FullWeight, cfg.numNodes());
    uint8_t* weights = m_weights.mutableSpan().data();
    bool hasHandlers = cfg.hasHandlers();
    for (BlockID block : order) {
        switch (facts[block.index()].unlikelySide) {
        case BranchSide::Unknown:
            break;
        case BranchSide::Taken:
            edges[firstSuccessor[block.index()]] = rarerOf(edges[firstSuccessor[block.index()]], BlockFrequency::Unlikely);
            break;
        case BranchSide::NotTaken:
            edges[firstSuccessor[block.index()] + 1] = rarerOf(edges[firstSuccessor[block.index()] + 1], BlockFrequency::Unlikely);
            break;
        }
        if (hasHandlers && cfg.hasHandler(block))
            edges[firstSuccessor[block.index() + 1] - 1] = BlockFrequency::Rare;
        if (facts[block.index()].isRare())
            weights[block.index()] = RareWeight;
    }

    bool hasCycle = cfg.hasCycle();
    const BlockCycles::Forest* forest = hasCycle ? &cycles().forest() : nullptr;
    std::span<const FrequencyFacts::DecidedPredecessor> decidedPredecessors = m_facts.decidedPredecessors.span();
    auto lowerWeightByEdges = [&](BlockID block) ALWAYS_INLINE_LAMBDA {
        unsigned index = block.index();
        uint32_t first = firstSuccessor[index];
        uint32_t end = firstSuccessor[index + 1];
        if (first == end || facts[index].isNormalExit)
            return;
        uint8_t likeliest = RareWeight;
        for (uint32_t edge = first; edge < end; ++edge)
            likeliest = std::min(likeliest, weightLevelOfEdge(weights[successors[edge].index()], edges[edge]));
        weights[index] = std::max(weights[index], likeliest);
        if (decidedPredecessors.empty()) [[likely]]
            return;
        for (const FrequencyFacts::DecidedPredecessor& decided : std::ranges::equal_range(decidedPredecessors, block, { }, &FrequencyFacts::DecidedPredecessor::block)) {
            if (hasCycle && isBackEdge(*forest, decided.predecessor, block))
                continue;
            RELEASE_ASSERT(decided.successorIndex < end - first);
            uint32_t edge = first + decided.successorIndex;
            uint8_t& weight = weights[decided.predecessor.index()];
            weight = std::max(weight, weightLevelOfEdge(weights[successors[edge].index()], edges[edge]));
        }
    };
    auto markRareEdges = [&](BlockID block) ALWAYS_INLINE_LAMBDA {
        unsigned index = block.index();
        if (weights[index] == RareWeight || facts[index].isNormalExit)
            return;
        for (uint32_t edge = firstSuccessor[index]; edge < firstSuccessor[index + 1]; ++edge) {
            if (weights[successors[edge].index()] == RareWeight && !(hasCycle && isBackEdge(*forest, block, successors[edge])))
                edges[edge] = BlockFrequency::Rare;
        }
    };

    if (!hasCycle) {
        for (size_t index = order.size(); index--;)
            lowerWeightByEdges(order[index]);
        for (BlockID block : order)
            markRareEdges(block);
    } else {
        for (BlockID block : forest->nodesInPostOrder())
            lowerWeightByEdges(block);
        for (BlockID block : forest->nodesInPostOrder())
            markRareEdges(block);
    }
    for (BlockID block : order.subspan(1))
        m_blocks[block.index()] = classOfWeightLevel(weights[block.index()]);
}

BlockFrequencies::~BlockFrequencies() = default;

bool BlockFrequencies::describes(const AnalysisContext& context, const Function& function) const
{
    RELEASE_ASSERT(function.blocks.size() == m_facts.blocks.size());
    for (BlockID id : m_cfg.reversePostOrder()) {
        const Block& block = function.at(id);
        const FrequencyFacts::BlockFacts& known = m_facts.blocks[id.index()];
        if (block.frequency != known.recorded || isNormalExit(block.terminator) != known.isNormalExit || isInherentlyRare(function, block) != known.isInherentlyRare)
            return false;
    }

    FrequencyInputs inputs { context.program, function, m_cfg, cycles(), areBranchHintsIgnored(context) };
    std::span<const FrequencyFacts::DecidedPredecessor> expected = m_facts.decidedPredecessors.span();
    for (unsigned index = 0; index < m_cfg.numNodes(); ++index) {
        BlockID id(index);
        if (!m_cfg.isReachable(id))
            continue;
        if (findRarerSides(inputs, m_facts.blocks.span(), id) != std::pair { m_facts.blocks[index].unlikelySide, m_facts.blocks[index].sideOffUsualPath })
            return false;
        bool isAsExpected = true;
        forEachDecidedPredecessor(inputs, id, [&](BlockID predecessor, uint32_t successorIndex) {
            if (!isAsExpected || expected.empty() || expected.front() != FrequencyFacts::DecidedPredecessor { id, predecessor, successorIndex }) {
                isAsExpected = false;
                return;
            }
            expected = expected.subspan(1);
        });
        if (!isAsExpected)
            return false;
    }
    return expected.empty();
}

void BlockFrequencies::computeClassesOfBlocks() const
{
    m_hasClassesOfBlocks = true;
    std::span<const BlockID> order = m_cfg.reversePostOrder();
    const BlockID* successors = m_cfg.successorsOfAllBlocks().data();
    const uint32_t* firstSuccessor = m_cfg.firstSuccessorOfBlocks().data();
    const BlockFrequency* edges = m_edges.span().data();
    BlockFrequency* classes = m_blocks.mutableSpan().data();
    Vector<BlockFrequency, 128> likeliestWayIn(FillWith { }, m_blocks.size(), BlockFrequency::Rare);
    for (bool hasLikelierWayInFromBehind = true; std::exchange(hasLikelierWayInFromBehind, false);) {
        for (uint32_t orderIndex = 0; orderIndex < order.size(); ++orderIndex) {
            unsigned block = order[orderIndex].index();
            if (orderIndex)
                classes[block] = rarerOf(likeliestWayIn[block], classOfWeightLevel(m_weights[block]));
            BlockFrequency classOfBlock = classes[block];
            for (uint32_t edge = firstSuccessor[block]; edge < firstSuccessor[block + 1]; ++edge) {
                BlockID target = successors[edge];
                BlockFrequency& wayIn = likeliestWayIn[target.index()];
                BlockFrequency wayInFromHere = rarerOf(classOfBlock, edges[edge]);
                if (!isRarerThan(wayIn, wayInFromHere))
                    continue;
                wayIn = wayInFromHere;
                hasLikelierWayInFromBehind |= m_cfg.reversePostOrderIndex(target) <= orderIndex;
            }
        }
    }
}

void BlockFrequencies::estimateCounts() const
{
    using Forest = BlockCycles::Forest;
    constexpr auto& constants = frequencyConstants;
    struct OutgoingEdge {
        BlockID successor;
        double share;
        double weight;
        bool isRare;
    };

    m_hasCounts = true;
    m_frequencies.fill(0.0, m_blocks.size());
    const Forest& forest = cycles().forest();
    std::span<const BlockID> postOrder = forest.nodesInPostOrder().span();
    const BlockID* successors = m_cfg.successorsOfAllBlocks().data();
    const uint32_t* firstSuccessor = m_cfg.firstSuccessorOfBlocks().data();
    const FrequencyFacts::BlockFacts* facts = m_facts.blocks.span().data();
    const BlockFrequency* classOfEdge = m_edges.span().data();
    const uint8_t* weights = m_weights.span().data();
    BlockID root = m_cfg.root();
    size_t numberOfBlocks = m_frequencies.size();
    double* frequencies = m_frequencies.mutableSpan().data();
    Vector<double, 64> incomingStorage(numberOfBlocks);
    double* incoming = incomingStorage.mutableSpan().data();
    Vector<double, 16> iterations(FillWith { }, forest.size(), constants.iterationsPerLoop);
    Vector<double, 16> returningShares(FillWith { }, forest.size(), 1.0);

    auto weightOfEdge = [&](uint32_t edge) {
        return weightOfLevel[weightLevelOfEdge(weights[successors[edge].index()], classOfEdge[edge])];
    };
    auto leavingShare = [&](unsigned cycleOfSource, BlockID to) -> double {
        unsigned cycleOfTarget = forest.innermost(to);
        if (cycleOfSource == cycleOfTarget)
            return 1;
        unsigned common = forest.nearestCommon(cycleOfSource, cycleOfTarget);
        double share = 1;
        for (unsigned cycle = cycleOfSource; cycle != common; cycle = forest[cycle].parent)
            share /= iterations[cycle];
        return share;
    };
    auto splitFlow = [&](auto hasCycles, BlockID block, unsigned cycleOfBlock, NOESCAPE const Invocable<void(BlockID, double)> auto& send) ALWAYS_INLINE_LAMBDA {
        uint32_t first = firstSuccessor[block.index()];
        uint32_t end = firstSuccessor[block.index() + 1];
        if (first == end || facts[block.index()].isNormalExit)
            return;

        if (end - first == 1) {
            BlockID successor = successors[first];
            double share = hasCycles ? leavingShare(cycleOfBlock, successor) : 1;
            if (classOfEdge[first] == BlockFrequency::Rare)
                share *= constants.rareEdgeShare;
            else if (share < 1)
                share *= share > constants.maxUnlikelyEdgeShare ? constants.maxUnlikelyEdgeShare / share : 1;
            else
                share = 1;
            send(successor, share);
            return;
        }

        Vector<OutgoingEdge, 4> edges;
        edges.reserveInitialCapacity(end - first);
        double stayingWeight = 0;
        double leaving = 0;
        double rare = 0;
        for (uint32_t index = first; index < end; ++index) {
            BlockID successor = successors[index];
            OutgoingEdge edge { successor, hasCycles ? leavingShare(cycleOfBlock, successor) : 1, 0, classOfEdge[index] == BlockFrequency::Rare };
            if (edge.isRare)
                rare += edge.share * constants.rareEdgeShare;
            else if (edge.share < 1) {
                edge.share *= weightOfEdge(index);
                leaving += edge.share;
            } else {
                edge.weight = weightOfEdge(index);
                stayingWeight += edge.weight;
            }
            edges.append(edge);
        }
        double leavingScale = leaving > constants.maxUnlikelyEdgeShare ? constants.maxUnlikelyEdgeShare / leaving : 1;
        rare = std::min(rare, constants.maxUnlikelyEdgeShare);
        for (const OutgoingEdge& edge : edges) {
            double share = edge.share;
            if (edge.isRare)
                share *= constants.rareEdgeShare;
            else if (share < 1)
                share *= leavingScale * (1 - rare);
            else
                share = (1 - rare) * (1 - leaving * leavingScale) * edge.weight / stayingWeight;
            send(edge.successor, share);
        }
    };

    if (!forest.size()) {
        std::fill_n(incoming, numberOfBlocks, 0.0);
        incoming[root.index()] = 1;
        for (size_t index = postOrder.size(); index--;) {
            BlockID block = postOrder[index];
            double flow = std::min(incoming[block.index()], constants.maxMultipliedFrequency);
            frequencies[block.index()] = std::clamp(flow, constants.minFrequency, constants.maxFrequency);
            splitFlow(std::false_type { }, block, Forest::none, [&](BlockID successor, double share) ALWAYS_INLINE_LAMBDA {
                incoming[successor.index()] += flow * share;
            });
        }
        return;
    }

    Vector<double, 0> flowStorage(numberOfBlocks);
    double* flows = flowStorage.mutableSpan().data();
    Vector<double, 16> enteringStorage(forest.size());
    double* entering = enteringStorage.mutableSpan().data();
    auto propagate = [&] {
        std::fill_n(incoming, numberOfBlocks, 0.0);
        std::fill_n(flows, numberOfBlocks, 0.0);
        std::fill_n(entering, forest.size(), 0.0);
        incoming[root.index()] = 1;
        for (unsigned cycle = forest.innermost(root); cycle != Forest::none; cycle = forest[cycle].parent)
            entering[cycle] = 1;
        for (size_t index = postOrder.size(); index--;) {
            BlockID block = postOrder[index];
            unsigned cycleOfBlock = forest.innermost(block);
            double flow = std::min(forest.isHeader(block) ? entering[cycleOfBlock] * iterations[cycleOfBlock] : incoming[block.index()], constants.maxMultipliedFrequency);
            frequencies[block.index()] = std::clamp(flow, constants.minFrequency, constants.maxFrequency);
            flows[block.index()] = flow;
            splitFlow(std::true_type { }, block, cycleOfBlock, [&](BlockID successor, double share) ALWAYS_INLINE_LAMBDA {
                double amount = flow * share;
                unsigned cycle = forest.innermost(successor);
                if (cycle == cycleOfBlock) {
                    incoming[successor.index()] += amount;
                    return;
                }
                unsigned common = forest.nearestCommon(cycleOfBlock, cycle);
                if (cycle == common) {
                    incoming[successor.index()] += amount;
                    return;
                }
                for (; cycle != common; cycle = forest[cycle].parent)
                    entering[cycle] += amount;
            });
        }
        for (unsigned cycle = 0; cycle < forest.size(); ++cycle) {
            unsigned header = forest[cycle].header().index();
            returningShares[cycle] = flows[header] > 0 ? incoming[header] / flows[header] : 1;
        }
    };
    auto comesRoundThroughUnlikelyBlocksAlone = [&](unsigned cycle) {
        BlockID header = forest[cycle].header();
        return facts[header.index()].isNormal() && std::ranges::none_of(m_cfg.incomingEdges(header), [&](Edge edge) {
            return forest.contains(cycle, edge.from) && facts[edge.from.index()].isNormal();
        });
    };

    propagate();
    unsigned deepest = 0;
    for (unsigned cycle = 0; cycle < forest.size(); ++cycle)
        deepest = std::max(deepest, forest.depth(cycle));
    for (unsigned depth = deepest; depth; --depth) {
        bool hasCycleSeldomGoneRound = false;
        for (unsigned cycle = 0; cycle < forest.size(); ++cycle) {
            double returning = returningShares[cycle];
            if (forest.depth(cycle) != depth || (returning > constants.unlikelyEdgeShare && !comesRoundThroughUnlikelyBlocksAlone(cycle)))
                continue;
            iterations[cycle] = returning < 1 ? std::min(constants.iterationsPerLoop, 1 / (1 - returning)) : constants.iterationsPerLoop;
            hasCycleSeldomGoneRound = true;
        }
        if (hasCycleSeldomGoneRound)
            propagate();
    }
}

double BlockFrequencies::frequencyOf(BlockID block) const
{
    if (!m_hasCounts) [[unlikely]]
        estimateCounts();
    return block.index() < m_frequencies.size() ? m_frequencies[block.index()] : 0;
}

BlockFrequency BlockFrequencies::classOf(BlockID block) const
{
    if (!m_hasClassesOfBlocks) [[unlikely]]
        computeClassesOfBlocks();
    return block.index() < m_blocks.size() ? m_blocks[block.index()] : BlockFrequency::Normal;
}

BlockFrequency BlockFrequencies::classOf(Edge edge) const
{
    size_t from = edge.from.index();
    if (from >= m_blocks.size())
        return BlockFrequency::Normal;
    std::span<const uint32_t> firstSuccessor = m_cfg.firstSuccessorOfBlocks();
    uint32_t firstEdge = firstSuccessor[from];
    uint32_t numberOfEdges = firstSuccessor[from + 1] - firstEdge;
    if (!numberOfEdges)
        return BlockFrequency::Normal;
    switch (edge.kind) {
    case EdgeKind::Normal:
        return edge.targetIndex < numberOfEdges ? m_edges[firstEdge + edge.targetIndex] : BlockFrequency::Normal;
    case EdgeKind::Handler:
        return m_edges[firstEdge + numberOfEdges - 1];
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BlockFrequency BlockFrequencies::classForBlockOrder(Edge edge) const
{
    BlockFrequency frequency = classOf(edge);
    if (frequency != BlockFrequency::Normal || edge.isHandlerEdge() || edge.from.index() >= m_facts.blocks.size())
        return frequency;
    if (!isOnSide(edge.targetIndex, m_facts.blocks[edge.from.index()].sideOffUsualPath))
        return frequency;
    if (classOf(Edge { edge.from, 1 - edge.targetIndex }) != BlockFrequency::Normal)
        return frequency;
    return BlockFrequency::Unlikely;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
