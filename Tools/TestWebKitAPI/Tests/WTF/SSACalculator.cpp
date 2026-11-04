/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "Helpers/Test.h"
#include "TestControlFlowGraph.h"
#include <algorithm>
#include <utility>
#include <vector>
#include <wtf/SSACalculator.h>

namespace TestWebKitAPI {

class TestSSACalculatorAdapter {
public:
    using CFG = TestControlFlowGraph;
    using Value = unsigned;

    explicit TestSSACalculatorAdapter(CFG& graph)
        : m_graph(graph)
        , m_dominators(graph)
    {
    }

    CFG& cfg() const { return m_graph; }
    Dominators<CFG>& dominators() { return m_dominators; }

    static void dumpValue(PrintStream& out, Value value) { out.print(value); }

private:
    CFG& m_graph;
    Dominators<CFG> m_dominators;
};

using TestSSACalculator = WTF::SSACalculator<TestSSACalculatorAdapter>;
using Variable = TestSSACalculator::Variable;
using PhiLocations = std::vector<std::pair<unsigned, unsigned>>;

constexpr unsigned storedValue = 1000;

static unsigned phiValueFor(TestBasicBlock* block)
{
    return 1 + block->index();
}

static void addTwoDiamondEdges(TestControlFlowGraph& graph)
{
    graph.addEdge(0, 1);
    graph.addEdge(0, 2);
    graph.addEdge(1, 3);
    graph.addEdge(2, 3);
    graph.addEdge(3, 4);
    graph.addEdge(3, 5);
    graph.addEdge(4, 6);
    graph.addEdge(5, 6);
}

static Variable* createVariableWithDefs(TestSSACalculator& calculator, const Vector<unsigned>& blocksWithDefs)
{
    Variable* variable = calculator.newVariable();
    for (unsigned blockIndex : blocksWithDefs)
        calculator.newDef(variable, calculator.cfg().node(blockIndex), storedValue);
    return variable;
}

static PhiLocations collectPhiLocations(TestSSACalculator& calculator)
{
    PhiLocations locations;
    TestControlFlowGraph& graph = calculator.cfg();
    for (unsigned blockIndex = 0; blockIndex < graph.numNodes(); ++blockIndex) {
        TestBasicBlock* block = graph.node(blockIndex);
        if (!block)
            continue;

        for (TestSSACalculator::Def* phi : calculator.phisForBlock(block)) {
            EXPECT_EQ(block, phi->block());
            EXPECT_EQ(phiValueFor(block), phi->value());
            locations.emplace_back(phi->variable()->index(), blockIndex);
        }
    }
    std::sort(locations.begin(), locations.end());
    return locations;
}

TEST(WTF_SSACalculator, UnfilteredInsertsAtMergePoints)
{
    TestControlFlowGraph graph(7);
    addTwoDiamondEdges(graph);
    TestSSACalculator calculator(graph);
    Variable* variable = createVariableWithDefs(calculator, { 1, 2, 4 });
    createVariableWithDefs(calculator, { 0 });

    calculator.computePhis([](Variable*, TestBasicBlock* block) {
        return phiValueFor(block);
    });

    EXPECT_EQ((PhiLocations { { variable->index(), 3u }, { variable->index(), 6u } }), collectPhiLocations(calculator));
    EXPECT_EQ(phiValueFor(graph.node(3)), calculator.reachingDefAtHead(graph.node(5), variable)->value());
    EXPECT_EQ(storedValue, calculator.reachingDefAtTail(graph.node(4), variable)->value());
    EXPECT_EQ(phiValueFor(graph.node(6)), calculator.reachingDefAtTail(graph.node(6), variable)->value());
}

TEST(WTF_SSACalculator, NullNodeBetweenBlocks)
{
    TestControlFlowGraph graph(5);
    graph.removeBlock(2);
    graph.addEdge(0, 1);
    graph.addEdge(0, 3);
    graph.addEdge(1, 4);
    graph.addEdge(3, 4);
    TestSSACalculator calculator(graph);
    Variable* variable = createVariableWithDefs(calculator, { 1, 3 });

    calculator.computePhis([](Variable*, TestBasicBlock* block) {
        return phiValueFor(block);
    });

    EXPECT_EQ((PhiLocations { { variable->index(), 4u } }), collectPhiLocations(calculator));
}

TEST(WTF_SSACalculator, BeforeVariableRunsBeforeFunctor)
{
    TestControlFlowGraph graph(7);
    addTwoDiamondEdges(graph);
    TestSSACalculator calculator(graph);
    createVariableWithDefs(calculator, { 1, 2, 4 });
    createVariableWithDefs(calculator, { 0 });
    createVariableWithDefs(calculator, { });
    createVariableWithDefs(calculator, { 4, 5 });

    std::vector<unsigned> startedVariables;
    Variable* currentVariable = nullptr;
    calculator.computePhis(
        [&](Variable* variable) {
            startedVariables.push_back(variable->index());
            currentVariable = variable;
        },
        [&](Variable* variable, TestBasicBlock* block) {
            EXPECT_EQ(currentVariable, variable);
            return phiValueFor(block);
        });

    EXPECT_EQ((std::vector<unsigned> { 0, 1, 2, 3 }), startedVariables);
    EXPECT_EQ((PhiLocations { { 0u, 3u }, { 0u, 6u }, { 3u, 6u } }), collectPhiLocations(calculator));
}

TEST(WTF_SSACalculator, FilteredThenUnfiltered)
{
    TestControlFlowGraph graph(7);
    addTwoDiamondEdges(graph);
    TestSSACalculator calculator(graph);
    Variable* variable = createVariableWithDefs(calculator, { 1, 2, 4 });

    unsigned numberOfFunctorCalls = 0;
    calculator.computePhis(
        [](TestBasicBlock*) {
            return false;
        },
        [](Variable*) { },
        [&](Variable*, TestBasicBlock*) {
            ++numberOfFunctorCalls;
            return 0u;
        });
    EXPECT_EQ(0u, numberOfFunctorCalls);
    EXPECT_TRUE(collectPhiLocations(calculator).empty());

    calculator.computePhis([](Variable*, TestBasicBlock* block) {
        return phiValueFor(block);
    });
    EXPECT_EQ((PhiLocations { { variable->index(), 3u }, { variable->index(), 6u } }), collectPhiLocations(calculator));
}

TEST(WTF_SSACalculator, FilteredThenOtherFilter)
{
    TestControlFlowGraph graph(7);
    addTwoDiamondEdges(graph);
    TestSSACalculator calculator(graph);
    createVariableWithDefs(calculator, { 1, 2, 4 });

    auto computePhisOnlyAt = [&](TestBasicBlock* acceptedBlock) {
        calculator.computePhis(
            [&](TestBasicBlock* block) {
                return block == acceptedBlock;
            },
            [](Variable*) { },
            [&](Variable*, TestBasicBlock* block) {
                return block == acceptedBlock ? phiValueFor(block) : 0;
            });
    };

    computePhisOnlyAt(graph.node(3));
    EXPECT_EQ(1u, calculator.phisForBlock(graph.node(3)).size());
    EXPECT_TRUE(calculator.phisForBlock(graph.node(6)).isEmpty());

    computePhisOnlyAt(graph.node(6));
    EXPECT_EQ(1u, calculator.phisForBlock(graph.node(3)).size());
    EXPECT_EQ(1u, calculator.phisForBlock(graph.node(6)).size());
}

TEST(WTF_SSACalculator, UnfilteredCacheIsReused)
{
    TestControlFlowGraph graph(7);
    addTwoDiamondEdges(graph);
    TestSSACalculator calculator(graph);

    calculator.computePhis([](Variable*, TestBasicBlock* block) {
        return phiValueFor(block);
    });
    Variable* variable = createVariableWithDefs(calculator, { 1, 2, 4 });

    TestBasicBlock* prunedBlock = graph.node(6);
    unsigned numberOfFilterCalls = 0;
    calculator.computePhis(
        [&](TestBasicBlock* block) {
            ++numberOfFilterCalls;
            return block != prunedBlock;
        },
        [](Variable*) { },
        [&](Variable*, TestBasicBlock* block) {
            return block != prunedBlock ? phiValueFor(block) : 0;
        });

    EXPECT_EQ(0u, numberOfFilterCalls);
    EXPECT_EQ((PhiLocations { { variable->index(), 3u } }), collectPhiLocations(calculator));
}

TEST(WTF_SSACalculator, ResetDropsCache)
{
    TestControlFlowGraph graph(7);
    addTwoDiamondEdges(graph);
    TestSSACalculator calculator(graph);
    auto insertPhi = [](Variable*, TestBasicBlock* block) {
        return phiValueFor(block);
    };

    calculator.computePhis(insertPhi);
    calculator.reset();
    Variable* variable = createVariableWithDefs(calculator, { 1, 2, 4 });

    unsigned numberOfFilterCalls = 0;
    calculator.computePhis(
        [&](TestBasicBlock*) {
            ++numberOfFilterCalls;
            return true;
        },
        [](Variable*) { },
        insertPhi);

    EXPECT_GT(numberOfFilterCalls, 0u);
    EXPECT_EQ((PhiLocations { { variable->index(), 3u }, { variable->index(), 6u } }), collectPhiLocations(calculator));
}

TEST(WTF_SSACalculator, FilterMatchesPruning)
{
    constexpr unsigned numberOfVariables = 4;
    size_t numberOfPhis = 0;
    for (unsigned seed = 1; seed <= 100; ++seed) {
        SCOPED_TRACE(::testing::Message() << "seed " << seed);
        WeakRandom random(seed);
        unsigned numberOfBlocks = 2 + random.getUint32(39);
        unsigned numberOfExtraEdges = random.getUint32(2 * numberOfBlocks + 1);
        TestControlFlowGraph graph(numberOfBlocks);
        graph.addRandomEdges(random, numberOfExtraEdges);

        TestSSACalculator filteredCalculator(graph);
        TestSSACalculator unfilteredCalculator(graph);
        for (unsigned variableIndex = 0; variableIndex < numberOfVariables; ++variableIndex) {
            Vector<unsigned> blocksWithDefs;
            for (unsigned blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
                if (!random.getUint32(4))
                    blocksWithDefs.append(blockIndex);
            }
            createVariableWithDefs(filteredCalculator, blocksWithDefs);
            createVariableWithDefs(unfilteredCalculator, blocksWithDefs);
        }

        TestControlFlowGraph::Set acceptedBlocks;
        for (unsigned blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
            if (random.getUint32(2))
                acceptedBlocks.add(graph.node(blockIndex));
        }
        auto insertPhi = [&](Variable*, TestBasicBlock* block) {
            return acceptedBlocks.contains(block) ? phiValueFor(block) : 0;
        };

        filteredCalculator.computePhis(
            [&](TestBasicBlock* block) {
                return acceptedBlocks.contains(block);
            },
            [](Variable*) { },
            insertPhi);
        unfilteredCalculator.computePhis(insertPhi);

        PhiLocations expected = collectPhiLocations(unfilteredCalculator);
        EXPECT_EQ(expected, collectPhiLocations(filteredCalculator));
        numberOfPhis += expected.size();
    }
    EXPECT_GT(numberOfPhis, 0u);
}

} // namespace TestWebKitAPI
