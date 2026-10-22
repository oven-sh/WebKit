/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBlockFrequencies.h"

#if ENABLE(AOT)

#include "AOTAnalysisDump.h"
#include "AOTBuildContext.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <array>
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

static bool isAbnormalExit(TerminatorKind kind)
{
    switch (kind) {
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
        return true;
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isNormalExit(const Terminator& terminator)
{
    return terminator.targets.isEmpty() && !isAbnormalExit(terminator.kind);
}

static bool hasSingleTarget(const Function& function, BlockID block)
{
    return function.at(block).terminator.targets.size() == 1;
}

static bool isForwardEdge(const CFG& cfg, BlockID from, BlockID to)
{
    return cfg.reversePostOrderIndex(from) < cfg.reversePostOrderIndex(to);
}

// A jump is always taken: a block is as likely as a block that jumps to it.
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
    if (isAbnormalExit(block.terminator.kind))
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

static BranchSide findUnlikelySide(const FrequencyInputs& inputs, std::span<const FrequencyFacts::BlockFacts> blocks, BlockID block)
{
    const Function& function = inputs.function;
    const Terminator& terminator = function.at(block).terminator;
    if (!terminator.isConditionalBranch())
        return BranchSide::Unknown;
    if (!blocks[terminator.taken().block.index()].isNormal() || !blocks[terminator.notTaken().block.index()].isNormal())
        return BranchSide::Unknown;

    BranchCondition condition = conditionOf(function, terminator);
    auto& test = function.at(condition.test);

    BranchSide side = findNullishSide(inputs, test);
    if (side == BranchSide::Unknown)
        side = findDifferentObjectsSide(inputs, block, test);
    return condition.isNegated ? oppositeOf(side) : side;
}

static BranchSide findSideOffUsualPath(const FrequencyInputs& inputs, std::span<const FrequencyFacts::BlockFacts> blocks, BlockID block)
{
    const Function& function = inputs.function;
    const Terminator& terminator = function.at(block).terminator;
    if (!terminator.isConditionalBranch())
        return BranchSide::Unknown;
    if (!blocks[terminator.taken().block.index()].isNormal() || !blocks[terminator.notTaken().block.index()].isNormal())
        return BranchSide::Unknown;

    auto isConstant = [&](ValueID value) {
        return integerConstantThroughCasts(inputs.program, function, value) || inputs.program.typeOf(function, value).singleStringLiteral();
    };
    auto isMaskedByConstant = [&](ValueID value) {
        while (function.at(value).opcode == Opcode::Upcast || function.at(value).opcode == Opcode::AssumeType)
            value = function.at(value).operand(0);
        auto& bits = function.at(value);
        return bits.opcode == Opcode::BitAnd && (integerConstantThroughCasts(inputs.program, function, bits.operand(0)) || integerConstantThroughCasts(inputs.program, function, bits.operand(1)));
    };
    BranchCondition condition = conditionOf(function, terminator);
    auto& test = function.at(condition.test);
    BranchSide side = BranchSide::Unknown;
    // A test of a value that may be anything asks what it is, and every answer is usual.
    if (test.opcode == Opcode::IsSlotFilled || (test.opcode == Opcode::TypeTest && !inputs.program.typeOf(function, test.operand(0)).isAnything()))
        side = BranchSide::NotTaken;
    else if (test.opcode == Opcode::StrictEq && isConstant(test.operand(0)) != isConstant(test.operand(1)))
        side = BranchSide::Taken;
    else if (test.opcode == Opcode::ToBoolean && isMaskedByConstant(test.operand(0)))
        side = BranchSide::Taken;
    return condition.isNegated ? oppositeOf(side) : side;
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
        facts.blocks[index].unlikelySide = findUnlikelySide(inputs, facts.blocks.span(), id);
        facts.blocks[index].sideOffUsualPath = findSideOffUsualPath(inputs, facts.blocks.span(), id);
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
    // All that enters a cycle counts as entering at its header. A hot loop and its rare copy that lead into each other are one cycle with two entries.
    if (sharedCycles.hasCycleWithSeveralEntries())
        m_cyclesByLikelihood.emplace(cfg, function);
    m_facts = collectFacts(FrequencyInputs { context.program, function, cfg, cycles(), areBranchHintsIgnored(context) });

    // From here on nothing is read of the function.
    classifyEdges();
    estimateWeights();
    for (BlockID block : cfg.reversePostOrder().subspan(1))
        m_blocks[block.index()] = classOfWeightLevel(m_weights[block.index()]);
}

BlockFrequencies::~BlockFrequencies() = default;

// An unreachable block has no successor in the graph.
void BlockFrequencies::classifyEdges()
{
    std::span<const BlockID> successors = m_cfg.successorsOfAllBlocks();
    const uint32_t* firstSuccessor = m_cfg.firstSuccessorOfBlocks().data();
    const FrequencyFacts::BlockFacts* facts = m_facts.blocks.span().data();
    m_edges.grow(successors.size());
    BlockFrequency* edges = m_edges.mutableSpan().data();
    for (size_t edge = 0; edge < successors.size(); ++edge)
        edges[edge] = facts[successors[edge].index()].applied;
    bool hasHandlers = m_cfg.hasHandlers();
    for (BlockID block : m_cfg.reversePostOrder()) {
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
        if (hasHandlers && m_cfg.hasHandler(block))
            edges[firstSuccessor[block.index() + 1] - 1] = BlockFrequency::Rare;
    }
}

void BlockFrequencies::estimateWeights()
{
    const BlockID* successors = m_cfg.successorsOfAllBlocks().data();
    const uint32_t* firstSuccessor = m_cfg.firstSuccessorOfBlocks().data();
    const FrequencyFacts::BlockFacts* facts = m_facts.blocks.span().data();
    BlockFrequency* edges = m_edges.mutableSpan().data();
    m_weights.fill(FullWeight, m_cfg.numNodes());
    uint8_t* weights = m_weights.mutableSpan().data();
    for (BlockID block : m_cfg.reversePostOrder()) {
        if (facts[block.index()].isRare())
            weights[block.index()] = RareWeight;
    }

    bool hasCycle = m_cfg.hasCycle();
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
        std::span<const BlockID> order = m_cfg.reversePostOrder();
        for (size_t index = order.size(); index--;)
            lowerWeightByEdges(order[index]);
        for (BlockID block : order)
            markRareEdges(block);
        return;
    }
    for (BlockID block : forest->nodesInPostOrder())
        lowerWeightByEdges(block);
    for (BlockID block : forest->nodesInPostOrder())
        markRareEdges(block);
}

bool BlockFrequencies::describes(const AnalysisContext& context, const Function& function) const
{
    RELEASE_ASSERT(function.blocks.size() == m_facts.blocks.size());
    for (BlockID id : m_cfg.reversePostOrder()) {
        const Block& block = function.at(id);
        const FrequencyFacts::BlockFacts& known = m_facts.blocks[id.index()];
        if (block.frequency != known.recorded || isNormalExit(block.terminator) != known.isNormalExit || isInherentlyRare(function, block) != known.isInherentlyRare)
            return false;
    }

    // With the graph and all that is recorded as they were, so is what is applied, and so are the headers of the cycles with several entries.
    FrequencyInputs inputs { context.program, function, m_cfg, cycles(), areBranchHintsIgnored(context) };
    std::span<const FrequencyFacts::DecidedPredecessor> expected = m_facts.decidedPredecessors.span();
    for (unsigned index = 0; index < m_cfg.numNodes(); ++index) {
        BlockID id(index);
        if (!m_cfg.isReachable(id))
            continue;
        if (findUnlikelySide(inputs, m_facts.blocks.span(), id) != m_facts.blocks[index].unlikelySide)
            return false;
        if (findSideOffUsualPath(inputs, m_facts.blocks.span(), id) != m_facts.blocks[index].sideOffUsualPath)
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
    for (uint32_t orderIndex = 0; orderIndex < order.size(); ++orderIndex) {
        unsigned block = order[orderIndex].index();
        if (orderIndex)
            classes[block] = rarerOf(likeliestWayIn[block], classes[block]);
        BlockFrequency classOfBlock = classes[block];
        for (uint32_t edge = firstSuccessor[block]; edge < firstSuccessor[block + 1]; ++edge) {
            BlockID target = successors[edge];
            if (m_cfg.reversePostOrderIndex(target) <= orderIndex)
                continue;
            BlockFrequency& wayIn = likeliestWayIn[target.index()];
            wayIn = moreFrequentOf(wayIn, rarerOf(classOfBlock, edges[edge]));
        }
    }
}

class BlockFrequencies::Counts {
public:
    using Forest = BlockCycles::Forest;

    Counts(const BlockFrequencies& owner, std::span<double> frequencies)
        : m_forest(owner.cycles().forest())
        , m_postOrder(m_forest.nodesInPostOrder().span())
        , m_successors(owner.m_cfg.successorsOfAllBlocks().data())
        , m_firstSuccessor(owner.m_cfg.firstSuccessorOfBlocks().data())
        , m_facts(owner.m_facts.blocks.span().data())
        , m_edges(owner.m_edges.span().data())
        , m_weights(owner.m_weights.span().data())
        , m_root(owner.m_cfg.root())
        , m_frequencies(frequencies.data())
        , m_incoming(frequencies.size())
        , m_iterations(FillWith { }, m_forest.size(), frequencyConstants.iterationsPerLoop)
        , m_returningShares(FillWith { }, m_forest.size(), 1.0)
    {
    }

    void run()
    {
        if (!m_forest.size()) {
            propagateWithoutCycles();
            return;
        }
        m_flows.grow(m_incoming.size());
        m_entering.grow(m_forest.size());
        propagate();
        unsigned deepest = 0;
        for (unsigned cycle = 0; cycle < m_forest.size(); ++cycle)
            deepest = std::max(deepest, m_forest.depth(cycle));
        for (unsigned depth = deepest; depth; --depth) {
            bool hasCycleSeldomGoneRound = false;
            for (unsigned cycle = 0; cycle < m_forest.size(); ++cycle) {
                double returning = m_returningShares[cycle];
                if (m_forest.depth(cycle) != depth || returning > frequencyConstants.unlikelyEdgeShare)
                    continue;
                m_iterations[cycle] = 1 / (1 - returning);
                hasCycleSeldomGoneRound = true;
            }
            if (hasCycleSeldomGoneRound)
                propagate();
        }
    }

private:
    struct OutgoingEdge {
        BlockID successor;
        double share;
        double weight;
        bool isRare;
    };

    double weightOfEdge(uint32_t edge) const { return weightOfLevel[weightLevelOfEdge(m_weights[m_successors[edge].index()], m_edges[edge])]; }

    double leavingShare(unsigned cycleOfSource, BlockID to) const
    {
        unsigned cycleOfTarget = m_forest.innermost(to);
        if (cycleOfSource == cycleOfTarget)
            return 1;
        unsigned common = m_forest.nearestCommon(cycleOfSource, cycleOfTarget);
        double share = 1;
        for (unsigned cycle = cycleOfSource; cycle != common; cycle = m_forest[cycle].parent)
            share /= m_iterations[cycle];
        return share;
    }

    template<bool hasCycles>
    ALWAYS_INLINE void splitFlow(BlockID block, unsigned cycleOfBlock, NOESCAPE const Invocable<void(BlockID, double)> auto& send) const
    {
        constexpr auto& constants = frequencyConstants;
        uint32_t first = m_firstSuccessor[block.index()];
        uint32_t end = m_firstSuccessor[block.index() + 1];
        if (first == end || m_facts[block.index()].isNormalExit)
            return;

        if (end - first == 1) {
            BlockID successor = m_successors[first];
            double share = hasCycles ? leavingShare(cycleOfBlock, successor) : 1;
            if (m_edges[first] == BlockFrequency::Rare)
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
            BlockID successor = m_successors[index];
            OutgoingEdge edge { successor, hasCycles ? leavingShare(cycleOfBlock, successor) : 1, 0, m_edges[index] == BlockFrequency::Rare };
            if (edge.isRare)
                rare += edge.share * constants.rareEdgeShare;
            else if (edge.share < 1)
                leaving += edge.share;
            else {
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
    }

    void propagateWithoutCycles()
    {
        constexpr auto& constants = frequencyConstants;
        double* incoming = m_incoming.mutableSpan().data();
        std::fill_n(incoming, m_incoming.size(), 0.0);
        incoming[m_root.index()] = 1;
        for (size_t index = m_postOrder.size(); index--;) {
            BlockID block = m_postOrder[index];
            double flow = std::min(incoming[block.index()], constants.maxMultipliedFrequency);
            m_frequencies[block.index()] = std::clamp(flow, constants.minFrequency, constants.maxFrequency);
            splitFlow<false>(block, Forest::none, [&](BlockID successor, double share) ALWAYS_INLINE_LAMBDA {
                incoming[successor.index()] += flow * share;
            });
        }
    }

    void propagate()
    {
        constexpr auto& constants = frequencyConstants;
        double* incoming = m_incoming.mutableSpan().data();
        double* flows = m_flows.mutableSpan().data();
        double* entering = m_entering.mutableSpan().data();
        std::fill_n(incoming, m_incoming.size(), 0.0);
        std::fill_n(flows, m_flows.size(), 0.0);
        std::fill_n(entering, m_entering.size(), 0.0);
        incoming[m_root.index()] = 1;
        for (unsigned cycle = m_forest.innermost(m_root); cycle != Forest::none; cycle = m_forest[cycle].parent)
            entering[cycle] = 1;
        for (size_t index = m_postOrder.size(); index--;) {
            BlockID block = m_postOrder[index];
            unsigned cycleOfBlock = m_forest.innermost(block);
            double flow = std::min(m_forest.isHeader(block) ? entering[cycleOfBlock] * m_iterations[cycleOfBlock] : incoming[block.index()], constants.maxMultipliedFrequency);
            m_frequencies[block.index()] = std::clamp(flow, constants.minFrequency, constants.maxFrequency);
            flows[block.index()] = flow;
            splitFlow<true>(block, cycleOfBlock, [&](BlockID successor, double share) ALWAYS_INLINE_LAMBDA {
                double amount = flow * share;
                unsigned cycle = m_forest.innermost(successor);
                if (cycle == cycleOfBlock) {
                    incoming[successor.index()] += amount;
                    return;
                }
                unsigned common = m_forest.nearestCommon(cycleOfBlock, cycle);
                if (cycle == common) {
                    incoming[successor.index()] += amount;
                    return;
                }
                for (; cycle != common; cycle = m_forest[cycle].parent)
                    entering[cycle] += amount;
            });
        }
        for (unsigned cycle = 0; cycle < m_forest.size(); ++cycle) {
            unsigned header = m_forest[cycle].header().index();
            m_returningShares[cycle] = flows[header] > 0 ? incoming[header] / flows[header] : 1;
        }
    }

    const Forest& m_forest;
    std::span<const BlockID> m_postOrder;
    const BlockID* m_successors;
    const uint32_t* m_firstSuccessor;
    const FrequencyFacts::BlockFacts* m_facts;
    const BlockFrequency* m_edges;
    const uint8_t* m_weights;
    BlockID m_root;
    double* m_frequencies;
    Vector<double, 64> m_incoming;
    Vector<double, 0> m_flows;
    Vector<double, 16> m_entering;
    Vector<double, 16> m_iterations;
    Vector<double, 16> m_returningShares;
};

void BlockFrequencies::estimateCounts() const
{
    m_hasCounts = true;
    m_frequencies.fill(0.0, m_blocks.size());
    Counts(*this, m_frequencies.mutableSpan()).run();
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

void BlockFrequencies::dump(PrintStream& out, const Function& function) const
{
    RELEASE_ASSERT(function.blocks.size() == m_blocks.size());
    for (size_t index = 0; index < m_blocks.size(); ++index) {
        if (m_cfg.isReachable(BlockID(index)))
            out.println("b", index, " frequency: ", nameOf(classOf(BlockID(index))), " count: ", WTF::format("%.6f", frequencyOf(BlockID(index))));
    }
    for (size_t index = 0; index < m_blocks.size(); ++index) {
        if (!m_cfg.isReachable(BlockID(index)))
            continue;
        forEachOutgoingEdge(function, BlockID(index), [&](Edge edge, const Target& target) {
            BlockFrequency frequency = classOf(edge);
            if (classForBlockOrder(edge) != frequency) {
                dumpEdge(out, edge);
                out.println(" off the usual path");
            }
            if (rarerOf(classOf(edge.from), classOf(target.block)) >= frequency)
                return;
            dumpEdge(out, edge);
            out.println(" frequency: ", nameOf(frequency));
        });
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
