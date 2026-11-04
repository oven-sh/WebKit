/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "Helpers/Test.h"
#include "TestControlFlowGraph.h"
#include <optional>
#include <vector>
#include <wtf/Dominators.h>

namespace TestWebKitAPI {

using TestDominators = Dominators<TestControlFlowGraph>;
using BlockList = Vector<TestBasicBlock*>;
using BlockIndices = std::vector<size_t>;

static BlockList collectBlocks(const TestControlFlowGraph& graph)
{
    BlockList blocks;
    for (unsigned index = 0; index < graph.numNodes(); ++index) {
        if (TestBasicBlock* block = graph.node(index))
            blocks.append(block);
    }
    return blocks;
}

static BlockIndices collectIndices(const TestControlFlowGraph::Set& blocks)
{
    BlockIndices indices;
    for (size_t index : blocks.indices())
        indices.push_back(index);
    return indices;
}

template<typename Predicate>
static BlockIndices collectIndicesIf(const BlockList& blocks, const Predicate& predicate)
{
    BlockIndices indices;
    for (TestBasicBlock* block : blocks) {
        if (predicate(block))
            indices.push_back(block->index());
    }
    return indices;
}

static TestControlFlowGraph::Set findBlocksReachableAvoiding(TestControlFlowGraph& graph, TestBasicBlock* avoidedBlock)
{
    TestControlFlowGraph::Set reachedBlocks;
    if (graph.root() == avoidedBlock)
        return reachedBlocks;

    BlockList queue;
    reachedBlocks.add(graph.root());
    queue.append(graph.root());
    for (size_t position = 0; position < queue.size(); ++position) {
        TestBasicBlock* block = queue[position];
        for (TestBasicBlock* successor : block->successors) {
            if (successor != avoidedBlock && reachedBlocks.add(successor))
                queue.append(successor);
        }
    }
    return reachedBlocks;
}

namespace {

class DominanceModel {
public:
    explicit DominanceModel(TestControlFlowGraph& graph)
        : m_reachableBlocks(findBlocksReachableAvoiding(graph, nullptr))
        , m_blocksReachableAvoiding(graph.newMap<TestControlFlowGraph::Set>())
    {
        for (TestBasicBlock* block : collectBlocks(graph))
            m_blocksReachableAvoiding[block] = findBlocksReachableAvoiding(graph, block);
    }

    bool strictlyDominates(TestBasicBlock* from, TestBasicBlock* to) const
    {
        return from != to && m_reachableBlocks.contains(to) && !m_blocksReachableAvoiding[from].contains(to);
    }

    bool dominates(TestBasicBlock* from, TestBasicBlock* to) const
    {
        return from == to || strictlyDominates(from, to);
    }

private:
    TestControlFlowGraph::Set m_reachableBlocks;
    TestControlFlowGraph::Map<TestControlFlowGraph::Set> m_blocksReachableAvoiding;
};

} // anonymous namespace

static void checkDominance(const BlockList& blocks, const TestDominators& dominators, const DominanceModel& model)
{
    for (TestBasicBlock* from : blocks) {
        for (TestBasicBlock* to : blocks) {
            EXPECT_EQ(model.dominates(from, to), dominators.dominates(from, to)) << "from #" << from->index() << " to #" << to->index();
            EXPECT_EQ(model.strictlyDominates(from, to), dominators.strictlyDominates(from, to)) << "from #" << from->index() << " to #" << to->index();
        }
    }
}

static void checkImmediateDominators(const BlockList& blocks, const TestDominators& dominators, const DominanceModel& model)
{
    for (TestBasicBlock* block : blocks) {
        SCOPED_TRACE(::testing::Message() << "idom of #" << block->index());
        BlockList strictDominators;
        for (TestBasicBlock* other : blocks) {
            if (model.strictlyDominates(other, block))
                strictDominators.append(other);
        }

        TestBasicBlock* idom = dominators.idom(block);
        if (strictDominators.isEmpty()) {
            EXPECT_NULL(idom);
            continue;
        }
        EXPECT_TRUE(strictDominators.contains(idom));
        for (TestBasicBlock* strictDominator : strictDominators)
            EXPECT_TRUE(model.dominates(strictDominator, idom)) << "strict dominator #" << strictDominator->index();
    }
}

static void checkDominatedBlocks(const BlockList& blocks, const TestDominators& dominators, const DominanceModel& model)
{
    for (TestBasicBlock* from : blocks) {
        BlockIndices expected = collectIndicesIf(blocks, [&](TestBasicBlock* to) {
            return model.dominates(from, to);
        });
        EXPECT_EQ(expected, collectIndices(dominators.blocksDominatedBy(from))) << "from #" << from->index();
    }
}

static void checkDominanceFrontiers(const BlockList& blocks, const TestDominators& dominators, const DominanceModel& model)
{
    for (TestBasicBlock* from : blocks) {
        BlockIndices expected = collectIndicesIf(blocks, [&](TestBasicBlock* to) {
            if (model.strictlyDominates(from, to))
                return false;
            return to->predecessors.containsIf([&](TestBasicBlock* predecessor) {
                return model.dominates(from, predecessor);
            });
        });
        EXPECT_EQ(expected, collectIndices(dominators.dominanceFrontierOf(from))) << "from #" << from->index();
    }
}

static std::optional<unsigned> idomIndex(const TestDominators& dominators, TestBasicBlock* block)
{
    if (TestBasicBlock* idom = dominators.idom(block))
        return idom->index();
    return std::nullopt;
}

static void checkSameDominance(const BlockList& blocks, const TestDominators& dominators, const BlockList& otherBlocks, const TestDominators& otherDominators)
{
    ASSERT_EQ(blocks.size(), otherBlocks.size());
    for (size_t to = 0; to < blocks.size(); ++to) {
        EXPECT_EQ(idomIndex(dominators, blocks[to]), idomIndex(otherDominators, otherBlocks[to])) << "idom of #" << to;
        for (size_t from = 0; from < blocks.size(); ++from)
            EXPECT_EQ(dominators.dominates(blocks[from], blocks[to]), otherDominators.dominates(otherBlocks[from], otherBlocks[to])) << "from #" << from << " to #" << to;
    }
}

static void addDiamondEdges(TestControlFlowGraph& graph)
{
    graph.addEdge(0, 1);
    graph.addEdge(0, 2);
    graph.addEdge(1, 3);
    graph.addEdge(2, 3);
}

static void addLoopEdges(TestControlFlowGraph& graph)
{
    graph.addEdge(3, 4);
    graph.addEdge(4, 5);
    graph.addEdge(5, 4);
    graph.addEdge(4, 6);
}

TEST(WTF_Dominators, SingleBlock)
{
    TestControlFlowGraph graph(1);
    TestDominators dominators(graph);
    TestBasicBlock* root = graph.root();

    EXPECT_NULL(dominators.idom(root));
    EXPECT_TRUE(dominators.dominates(root, root));
    EXPECT_FALSE(dominators.strictlyDominates(root, root));
    EXPECT_EQ((BlockIndices { 0 }), collectIndices(dominators.blocksDominatedBy(root)));
    EXPECT_TRUE(dominators.blocksStrictlyDominatedBy(root).isEmpty());
    EXPECT_TRUE(dominators.dominanceFrontierOf(root).isEmpty());
}

TEST(WTF_Dominators, DiamondAndLoop)
{
    TestControlFlowGraph graph(7);
    addDiamondEdges(graph);
    addLoopEdges(graph);

    for (bool selfCheck : { false, true }) {
        SCOPED_TRACE(::testing::Message() << "selfCheck " << selfCheck);
        TestDominators dominators(graph, selfCheck);

        EXPECT_NULL(dominators.idom(graph.node(0)));
        EXPECT_EQ(graph.node(0), dominators.idom(graph.node(1)));
        EXPECT_EQ(graph.node(0), dominators.idom(graph.node(2)));
        EXPECT_EQ(graph.node(0), dominators.idom(graph.node(3)));
        EXPECT_EQ(graph.node(3), dominators.idom(graph.node(4)));
        EXPECT_EQ(graph.node(4), dominators.idom(graph.node(5)));
        EXPECT_EQ(graph.node(4), dominators.idom(graph.node(6)));

        EXPECT_TRUE(dominators.strictlyDominates(graph.node(0), graph.node(6)));
        EXPECT_TRUE(dominators.strictlyDominates(graph.node(3), graph.node(5)));
        EXPECT_FALSE(dominators.dominates(graph.node(1), graph.node(3)));
        EXPECT_FALSE(dominators.dominates(graph.node(2), graph.node(3)));
        EXPECT_FALSE(dominators.dominates(graph.node(5), graph.node(4)));
        EXPECT_FALSE(dominators.dominates(graph.node(5), graph.node(6)));

        EXPECT_EQ((BlockIndices { 4, 5, 6 }), collectIndices(dominators.blocksDominatedBy(graph.node(4))));
        EXPECT_EQ((BlockIndices { 3 }), collectIndices(dominators.dominanceFrontierOf(graph.node(1))));
        EXPECT_EQ((BlockIndices { 3 }), collectIndices(dominators.dominanceFrontierOf(graph.node(2))));
        EXPECT_EQ((BlockIndices { 4 }), collectIndices(dominators.dominanceFrontierOf(graph.node(4))));
        EXPECT_EQ((BlockIndices { 4 }), collectIndices(dominators.dominanceFrontierOf(graph.node(5))));
        EXPECT_TRUE(dominators.dominanceFrontierOf(graph.node(3)).isEmpty());
    }
}

TEST(WTF_Dominators, IrreducibleLoop)
{
    TestControlFlowGraph graph(3);
    graph.addEdge(0, 1);
    graph.addEdge(0, 2);
    graph.addEdge(1, 2);
    graph.addEdge(2, 1);
    TestDominators dominators(graph);

    EXPECT_EQ(graph.root(), dominators.idom(graph.node(1)));
    EXPECT_EQ(graph.root(), dominators.idom(graph.node(2)));
    EXPECT_FALSE(dominators.dominates(graph.node(1), graph.node(2)));
    EXPECT_FALSE(dominators.dominates(graph.node(2), graph.node(1)));
    EXPECT_EQ((BlockIndices { 2 }), collectIndices(dominators.dominanceFrontierOf(graph.node(1))));
    EXPECT_EQ((BlockIndices { 1 }), collectIndices(dominators.dominanceFrontierOf(graph.node(2))));
}

TEST(WTF_Dominators, UnreachableBlocks)
{
    TestControlFlowGraph graph(6);
    addDiamondEdges(graph);
    graph.addEdge(5, 4);
    graph.addEdge(4, 1);
    graph.addEdge(4, 3);
    graph.addEdge(5, 2);
    TestDominators dominators(graph);
    BlockList blocks = collectBlocks(graph);

    for (TestBasicBlock* unreachableBlock : { graph.node(4), graph.node(5) }) {
        SCOPED_TRACE(::testing::Message() << "unreachable block #" << unreachableBlock->index());
        EXPECT_NULL(dominators.idom(unreachableBlock));
        EXPECT_TRUE(dominators.dominates(unreachableBlock, unreachableBlock));
        EXPECT_FALSE(dominators.strictlyDominates(unreachableBlock, unreachableBlock));
        EXPECT_TRUE(dominators.blocksStrictlyDominatedBy(unreachableBlock).isEmpty());
        for (TestBasicBlock* other : blocks) {
            if (other == unreachableBlock)
                continue;
            EXPECT_FALSE(dominators.dominates(unreachableBlock, other)) << "other block #" << other->index();
            EXPECT_FALSE(dominators.dominates(other, unreachableBlock)) << "other block #" << other->index();
        }
    }

    EXPECT_EQ(graph.root(), dominators.idom(graph.node(1)));
    EXPECT_EQ(graph.root(), dominators.idom(graph.node(2)));
    EXPECT_EQ(graph.root(), dominators.idom(graph.node(3)));

    TestControlFlowGraph isolatedGraph(6);
    addDiamondEdges(isolatedGraph);
    isolatedGraph.addEdge(5, 4);
    TestDominators isolatedDominators(isolatedGraph);
    checkSameDominance(blocks, dominators, collectBlocks(isolatedGraph), isolatedDominators);
}

TEST(WTF_Dominators, NullNodesBetweenBlocks)
{
    constexpr unsigned numberOfBlocks = 9;
    for (unsigned numberOfNodes : { numberOfBlocks, TestDominators::maxNodesForSemiNCADominance + 1 }) {
        SCOPED_TRACE(::testing::Message() << numberOfNodes << " nodes");
        TestControlFlowGraph graph(numberOfBlocks, numberOfNodes - numberOfBlocks);
        graph.removeBlock(1);
        graph.removeBlock(5);
        graph.addEdge(0, 2);
        graph.addEdge(0, 3);
        graph.addEdge(2, 4);
        graph.addEdge(3, 4);
        graph.addEdge(4, 6);
        graph.addEdge(6, 7);
        graph.addEdge(7, 6);
        graph.addEdge(8, 4);
        TestDominators dominators(graph);
        DominanceModel model(graph);
        BlockList blocks = collectBlocks(graph);

        EXPECT_EQ(graph.root(), dominators.idom(graph.node(4)));
        EXPECT_EQ(graph.node(4), dominators.idom(graph.node(6)));
        EXPECT_EQ(graph.node(6), dominators.idom(graph.node(7)));
        EXPECT_NULL(dominators.idom(graph.node(8)));
        checkDominance(blocks, dominators, model);
        checkImmediateDominators(blocks, dominators, model);
        checkDominatedBlocks(blocks, dominators, model);
        checkDominanceFrontiers(blocks, dominators, model);
    }
}

TEST(WTF_Dominators, RandomGraphsAgainstModel)
{
    for (unsigned seed = 1; seed <= 200; ++seed) {
        SCOPED_TRACE(::testing::Message() << "seed " << seed);
        WeakRandom random(seed);
        unsigned numberOfBlocks = 2 + random.getUint32(59);
        unsigned numberOfExtraEdges = random.getUint32(2 * numberOfBlocks + 1);
        TestControlFlowGraph graph(numberOfBlocks);
        graph.addRandomEdges(random, numberOfExtraEdges);

        TestDominators dominators(graph);
        DominanceModel model(graph);
        BlockList blocks = collectBlocks(graph);
        checkDominance(blocks, dominators, model);
        checkImmediateDominators(blocks, dominators, model);
        checkDominatedBlocks(blocks, dominators, model);
        checkDominanceFrontiers(blocks, dominators, model);
    }
}

TEST(WTF_Dominators, ChildListsBuiltOnce)
{
    constexpr unsigned numberOfBlocks = 7;
    for (unsigned numberOfNodes : { numberOfBlocks, TestDominators::maxNodesForSemiNCADominance + 1 }) {
        SCOPED_TRACE(::testing::Message() << numberOfNodes << " nodes");
        TestControlFlowGraph graph(numberOfBlocks, numberOfNodes - numberOfBlocks);
        addDiamondEdges(graph);
        addLoopEdges(graph);
        TestDominators dominators(graph);
        DominanceModel model(graph);
        BlockList blocks = collectBlocks(graph);

        for (TestBasicBlock* from : blocks) {
            SCOPED_TRACE(::testing::Message() << "from #" << from->index());
            BlockIndices expected = collectIndicesIf(blocks, [&](TestBasicBlock* to) {
                return model.strictlyDominates(from, to);
            });
            EXPECT_EQ(expected, collectIndices(dominators.blocksStrictlyDominatedBy(from)));
            EXPECT_EQ(expected, collectIndices(dominators.blocksStrictlyDominatedBy(from)));

            size_t numberOfVisits = 0;
            dominators.forAllBlocksStrictlyDominatedBy(from, [&](TestBasicBlock*) {
                ++numberOfVisits;
            });
            EXPECT_EQ(expected.size(), numberOfVisits);
        }
    }
}

TEST(WTF_Dominators, DeepChain)
{
    constexpr unsigned numberOfBlocks = TestDominators::maxNodesForSemiNCADominance;
    TestControlFlowGraph graph(numberOfBlocks);
    for (unsigned index = 1; index < numberOfBlocks; ++index)
        graph.addEdge(index - 1, index);
    TestDominators dominators(graph);

    for (unsigned index = 1; index < numberOfBlocks; ++index)
        EXPECT_EQ(graph.node(index - 1), dominators.idom(graph.node(index))) << "idom of #" << index;

    auto checkPair = [&](unsigned from, unsigned to) {
        EXPECT_EQ(from <= to, dominators.dominates(graph.node(from), graph.node(to))) << "from #" << from << " to #" << to;
        EXPECT_EQ(from < to, dominators.strictlyDominates(graph.node(from), graph.node(to))) << "from #" << from << " to #" << to;
    };
    checkPair(0, numberOfBlocks - 1);
    checkPair(numberOfBlocks - 1, 0);
    checkPair(numberOfBlocks - 2, numberOfBlocks - 1);
    checkPair(numberOfBlocks - 1, numberOfBlocks - 2);
    checkPair(numberOfBlocks - 1, numberOfBlocks - 1);

    WeakRandom random(1);
    for (unsigned numberOfPairs = 10000; numberOfPairs--;) {
        unsigned from = random.getUint32(numberOfBlocks);
        unsigned to = random.getUint32(numberOfBlocks);
        checkPair(from, to);
    }
}

TEST(WTF_Dominators, SemiNCAAndLengauerTarjanAgree)
{
    constexpr unsigned numberOfBlocks = 50;
    constexpr unsigned maxNodesForSemiNCA = TestDominators::maxNodesForSemiNCADominance;
    for (unsigned seed = 1; seed <= 20; ++seed) {
        SCOPED_TRACE(::testing::Message() << "seed " << seed);
        TestControlFlowGraph denseGraph(numberOfBlocks);
        TestControlFlowGraph largestSemiNCAGraph(numberOfBlocks, maxNodesForSemiNCA - numberOfBlocks);
        TestControlFlowGraph lengauerTarjanGraph(numberOfBlocks, maxNodesForSemiNCA + 1 - numberOfBlocks);
        for (TestControlFlowGraph* graph : { &denseGraph, &largestSemiNCAGraph, &lengauerTarjanGraph }) {
            WeakRandom random(seed);
            unsigned numberOfExtraEdges = random.getUint32(2 * numberOfBlocks + 1);
            graph->addRandomEdges(random, numberOfExtraEdges);
        }

        TestDominators denseDominators(denseGraph);
        TestDominators largestSemiNCADominators(largestSemiNCAGraph);
        TestDominators lengauerTarjanDominators(lengauerTarjanGraph);
        BlockList blocks = collectBlocks(denseGraph);
        checkSameDominance(blocks, denseDominators, collectBlocks(largestSemiNCAGraph), largestSemiNCADominators);
        checkSameDominance(blocks, denseDominators, collectBlocks(lengauerTarjanGraph), lengauerTarjanDominators);
    }
}

} // namespace TestWebKitAPI
