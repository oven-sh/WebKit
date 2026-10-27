/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun_region_cloner.h"

#if ENABLE(B3_JIT)

#include "B3RegionCloner.h"

static void checkCloneOrder(BasicBlock* block, BasicBlock* copy)
{
    CHECK_EQ(copy->size(), block->size());
    for (unsigned i = 0; i < block->size(); ++i) {
        Value* original = block->at(i);
        Value* clone = copy->at(i);
        CHECK(clone != original);
        CHECK(clone->kind() == original->kind());
        CHECK(clone->type() == original->type());
        CHECK_EQ(clone->numChildren(), original->numChildren());
        for (unsigned childIndex = 0; childIndex < original->numChildren(); ++childIndex) {
            Value* child = original->child(childIndex);
            size_t indexInBlock = block->values().find(child);
            CHECK(clone->child(childIndex) == (indexInBlock == notFound ? child : copy->at(indexInBlock)));
        }
    }
}

static void testRegionClonerCopiesRegion()
{
    Procedure proc;
    Graph graph { proc };
    Diamond diamond(graph);
    BasicBlock* after = proc.addBlock();
    BasicBlock* outside = proc.addBlock();
    Value* sum = diamond.appendSum();
    graph.branch(diamond.join, diamond.operand, unlikely(after), rarely(outside));
    graph.ret(after, graph.make(after, Mul, sum, graph.int64(after, 2)));
    graph.ret(outside, graph.int64(outside, 0));
    resetAndValidate(proc);

    RegionCloner cloner(proc);
    cloner.addBlock(diamond.join);
    cloner.addBlock(after);
    cloner.demoteEscapingValues();
    BasicBlock* const region[] = { diamond.join, after };
    RegionCloner::BlockMap copies = cloner.cloneBlocks(region);
    BasicBlock* joinCopy = copies.get(diamond.join);
    BasicBlock* afterCopy = copies.get(after);
    checkCloneOrder(diamond.join, joinCopy);
    checkCloneOrder(after, afterCopy);
    CHECK(diamond.right->replaceSuccessor(diamond.join, joinCopy));
    CHECK(cloner.finish());

    CHECK(joinCopy->successor(0) == unlikely(afterCopy));
    CHECK(joinCopy->successor(1) == rarely(outside));
    CHECK(diamond.join->successor(0) == unlikely(after));
    CHECK(diamond.join->successor(1) == rarely(outside));
    CHECK(diamond.left->successorBlock(0) == diamond.join);
    CHECK_EQ(numberOfBlocks(proc), 8u);
    diamond.checkResult(12);
}

static void testRegionClonerDemotesOnlyEscapingValues()
{
    for (bool escapes : { false, true }) {
        Procedure proc;
        Graph graph { proc };
        Diamond diamond(graph);
        if (escapes)
            diamond.returnProductAfterJoin();
        else
            diamond.returnSumFromJoin();

        RegionCloner cloner(proc);
        cloner.addBlock(diamond.join);
        cloner.demoteEscapingValues();
        CHECK_EQ(numberOfValues(proc, B3::Set), escapes ? 1u : 0u);
        CHECK_EQ(cloner.finish(), escapes);
        diamond.checkResult(escapes ? 12 : 6);
    }
}

static void testRegionClonerWithoutCopiesChangesNothing()
{
    Procedure proc;
    Graph graph { proc };
    Diamond diamond(graph);
    diamond.returnSumFromJoin();
    unsigned numberOfBlocksBefore = numberOfBlocks(proc);
    unsigned numberOfValuesBefore = proc.values().size();

    RegionCloner cloner(proc);
    cloner.addBlock(diamond.join);
    cloner.demoteEscapingValues();
    CHECK(!cloner.finish());
    CHECK_EQ(numberOfBlocks(proc), numberOfBlocksBefore);
    CHECK_EQ(proc.values().size(), numberOfValuesBefore);
}

void addBunRegionClonerTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testRegionClonerCopiesRegion());
    RUN(testRegionClonerDemotesOnlyEscapingValues());
    RUN(testRegionClonerWithoutCopiesChangesNothing());
}

#endif // ENABLE(B3_JIT)
