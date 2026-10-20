/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBlockFrequencies.h"

#if ENABLE(AOT)

#include "AOTAnalysisDump.h"
#include "AOTBuildContext.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct FrequencyInputs {
    const Program& program;
    const Function& function;
    const CFG& cfg;
    const BlockCycles& cycles;
    bool ignoresBranchHints;
};

enum class BranchSide : uint8_t {
    Unknown,
    Taken,
    NotTaken,
};

struct BranchCondition {
    ValueID test;
    bool isNegated { false };
};

struct LocalFrequency {
    BlockFrequency applied { BlockFrequency::Normal };
    bool isInherentlyRare { false };

    bool isNormal() const { return applied == BlockFrequency::Normal && !isInherentlyRare; }
    bool isRare() const { return applied == BlockFrequency::Rare || isInherentlyRare; }
};

} // anonymous namespace

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

static BlockFrequency appliedFrequencyOf(const FrequencyInputs& inputs, BlockID id)
{
    switch (recordedFrequencyUseOf(inputs.function, inputs.cfg, id, inputs.ignoresBranchHints)) {
    case RecordedFrequencyUse::NotRecorded:
    case RecordedFrequencyUse::Ignored:
        return BlockFrequency::Normal;
    case RecordedFrequencyUse::Applied:
        return inputs.function.at(id).frequency;
    }
    RELEASE_ASSERT_NOT_REACHED();
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

static BranchSide findUnlikelySide(const FrequencyInputs& inputs, const Vector<LocalFrequency>& blocks, BlockID block)
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

static bool isOnSide(const Terminator& terminator, const Target& target, BranchSide side)
{
    switch (side) {
    case BranchSide::Unknown:
        return false;
    case BranchSide::Taken:
        return &target == &terminator.taken();
    case BranchSide::NotTaken:
        return &target == &terminator.notTaken();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BlockFrequency classOfWeight(double weight)
{
    if (weight <= frequencyConstants.rareEdgeShare)
        return BlockFrequency::Rare;
    if (weight <= frequencyConstants.unlikelyEdgeShare)
        return BlockFrequency::Unlikely;
    return BlockFrequency::Normal;
}

namespace {

class BlockFrequencyGraph {
public:
    using Graph = CFG;
    using Node = BlockID;

    BlockFrequencyGraph(const FrequencyInputs&, CFG&);

    Graph& graph() { return m_cfg; }

    unsigned numSuccessors(Node block)
    {
        // A covered block that returns has its handler edge only, and a node counts as no likelier than its likeliest edge.
        if (isNormalExit(m_inputs.function.at(block).terminator))
            return 0;
        return static_cast<unsigned>(m_cfg.successors(block).size());
    }

    Node successor(Node block, unsigned successorIndex) { return m_cfg.successors(block)[successorIndex]; }
    WTF::EdgeClass edgeClass(Node block, unsigned successorIndex) { return edgeClassOf(m_edges[edgeIndex(block, successorIndex)]); }
    void setEdgeClass(Node block, unsigned successorIndex, WTF::EdgeClass newClass) { m_edges[edgeIndex(block, successorIndex)] = blockFrequencyOf(newClass); }

    double initialWeight(Node block, const WTF::FrequencyEstimatorConstants& constants)
    {
        // An applied Unlikely is the class of the incoming edges, and the weight of an edge is its class times this weight.
        return m_blocks[block.index()].isRare() ? constants.rareEdgeShare : 1;
    }

    double weightCapAfterPropagation(Node, const WTF::FrequencyEstimatorConstants&) { return 1; }
    bool entersEverySuccessor(Node) { return false; }

    template<typename Functor>
    void forEachDecidedPredecessor(Node id, const Functor& functor)
    {
        const Function& function = m_inputs.function;
        const Block& block = function.at(id);
        if (!block.terminator.isConditionalBranch())
            return;
        BranchCondition condition = conditionOf(function, block.terminator);
        size_t parameterIndex = block.parameters.find(condition.test);
        if (parameterIndex == notFound)
            return;
        for (Edge edge : m_cfg.incomingEdges(id)) {
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

    Vector<BlockFrequency> takeEdges() { return WTF::move(m_edges); }
    Vector<uint32_t> takeFirstEdge() { return WTF::move(m_firstEdge); }

private:
    uint32_t edgeIndex(Node block, unsigned successorIndex) const
    {
        uint32_t index = m_firstEdge[block.index()] + successorIndex;
        RELEASE_ASSERT(index < m_firstEdge[block.index() + 1]);
        return index;
    }

    void appendEdgesOf(BlockID);

    const FrequencyInputs& m_inputs;
    CFG& m_cfg;
    Vector<LocalFrequency> m_blocks;
    Vector<BlockFrequency> m_edges;
    Vector<uint32_t> m_firstEdge;
};

BlockFrequencyGraph::BlockFrequencyGraph(const FrequencyInputs& inputs, CFG& cfg)
    : m_inputs(inputs)
    , m_cfg(cfg)
    , m_blocks(inputs.function.blocks.size())
{
    const Function& function = inputs.function;
    for (BlockID block : cfg.reversePostOrder())
        m_blocks[block.index()] = { appliedFrequencyOf(inputs, block), isInherentlyRare(function, function.at(block)) };

    size_t numberOfBlocks = function.blocks.size();
    m_firstEdge.reserveInitialCapacity(numberOfBlocks + 1);
    for (size_t index = 0; index < numberOfBlocks; ++index) {
        BlockID block(index);
        m_firstEdge.append(m_edges.size());
        if (cfg.isReachable(block))
            appendEdgesOf(block);
    }
    m_firstEdge.append(m_edges.size());
}

void BlockFrequencyGraph::appendEdgesOf(BlockID block)
{
    const Terminator& terminator = m_inputs.function.at(block).terminator;
    BranchSide unlikelySide = findUnlikelySide(m_inputs, m_blocks, block);
    forEachOutgoingEdge(m_inputs.function, block, [&](Edge edge, const Target& target) {
        if (edge.isHandlerEdge()) {
            m_edges.append(BlockFrequency::Rare);
            return;
        }
        BlockFrequency side = isOnSide(terminator, target, unlikelySide) ? BlockFrequency::Unlikely : BlockFrequency::Normal;
        m_edges.append(rarerOf(m_blocks[target.block.index()].applied, side));
    });
}

} // anonymous namespace

BlockFrequencies::BlockFrequencies(const AnalysisContext& context, const Function& function, CFG& cfg, const BlockCycles& cycles)
    : m_frequencies(FillWith { }, function.blocks.size(), 0.0)
    , m_blocks(FillWith { }, function.blocks.size(), BlockFrequency::Normal)
    , m_isReachable(function.blocks.size())
{
    bool ignoresBranchHints = context.build.recordedOptions().has(TestingSwitch::IgnoreBranchHints);
    FrequencyInputs inputs { context.program, function, cfg, cycles, ignoresBranchHints };
    BlockFrequencyGraph graph(inputs, cfg);
    WTF::FrequencyEstimator<BlockFrequencyGraph> estimator(graph, cycles.forest(), frequencyConstants);
    estimator.run();

    m_edges = graph.takeEdges();
    m_firstEdge = graph.takeFirstEdge();
    for (BlockID block : cfg.reversePostOrder()) {
        m_isReachable.set(block.index());
        m_frequencies[block.index()] = estimator.frequencyOf(block);
    }

    for (BlockID block : cfg.reversePostOrder().subspan(1)) {
        BlockFrequency entry = BlockFrequency::Rare;
        for (Edge edge : cfg.incomingEdges(block)) {
            if (isForwardEdge(cfg, edge.from, block))
                entry = moreFrequentOf(entry, rarerOf(m_blocks[edge.from.index()], classOf(edge)));
        }
        m_blocks[block.index()] = rarerOf(entry, classOfWeight(estimator.weightOf(block)));
    }
}

double BlockFrequencies::frequencyOf(BlockID block) const
{
    return block.index() < m_frequencies.size() ? m_frequencies[block.index()] : 0;
}

BlockFrequency BlockFrequencies::classOf(BlockID block) const
{
    return block.index() < m_blocks.size() ? m_blocks[block.index()] : BlockFrequency::Normal;
}

BlockFrequency BlockFrequencies::classOf(Edge edge) const
{
    size_t from = edge.from.index();
    if (from >= m_blocks.size())
        return BlockFrequency::Normal;
    uint32_t firstEdge = m_firstEdge[from];
    uint32_t numberOfEdges = m_firstEdge[from + 1] - firstEdge;
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

void BlockFrequencies::dump(PrintStream& out, const Function& function) const
{
    RELEASE_ASSERT(function.blocks.size() == m_blocks.size());
    for (size_t index : m_isReachable)
        out.println("b", index, " frequency: ", nameOf(m_blocks[index]), " count: ", WTF::format("%.6f", m_frequencies[index]));
    for (size_t index : m_isReachable) {
        forEachOutgoingEdge(function, BlockID(index), [&](Edge edge, const Target& target) {
            BlockFrequency frequency = classOf(edge);
            if (rarerOf(classOf(edge.from), classOf(target.block)) >= frequency)
                return;
            dumpEdge(out, edge);
            out.println(" frequency: ", nameOf(frequency));
        });
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
