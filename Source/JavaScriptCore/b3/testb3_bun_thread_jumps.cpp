/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "B3FoldPathConstants.h"
#include "B3SwitchValue.h"
#include "B3ThreadJumps.h"

static Policy policyWithMaxTailSize(unsigned maxSize)
{
    return policyWith([&](Policy& policy) {
        policy.upstream.maxTailDuplicationBlockSize = maxSize;
    });
}

static void testThreadJumpsOnConstantPhi()
{
    Procedure proc(policyWithMaxTailSize(1));
    Graph graph { proc };
    TailWithTwoPredecessors blocks(graph);
    Value* phi = graph.phi(blocks.tail, Int64);
    graph.branch(blocks.root, blocks.first, blocks.left, blocks.right);
    graph.upsilon(blocks.left, graph.int64(blocks.left, 0), phi);
    graph.upsilon(blocks.right, blocks.second, phi);
    graph.branch(blocks.tail, phi, blocks.taken, blocks.notTaken);
    blocks.thread(graph);

    CHECK(blocks.left->last()->opcode() == Jump);
    CHECK(blocks.left->successorBlock(0) == blocks.notTaken);
    CHECK(blocks.right->last()->opcode() == Branch);
    CHECK(blocks.right->successorBlock(0) == blocks.taken);
    CHECK(blocks.right->successorBlock(1) == blocks.notTaken);
    auto code = compileProc(proc);
    int64_t memory[1];
    CHECK_EQ(invoke<int64_t>(*code, 1, 1, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 1, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 0, 0, memory), 20);
}

static void testThreadJumpsDecidedByDominatingBranch()
{
    for (bool testsOpposite : { false, true }) {
        Procedure proc(policyWithMaxTailSize(1));
        Graph graph { proc };
        TailWithTwoPredecessors blocks(graph);
        graph.branch(blocks.root, graph.make(blocks.root, Equal, blocks.first, graph.int64(blocks.root, 5)), blocks.left, blocks.right);
        graph.branch(blocks.tail, graph.make(blocks.tail, testsOpposite ? NotEqual : Equal, blocks.first, graph.int64(blocks.tail, 5)), blocks.taken, blocks.notTaken);
        blocks.thread(graph);

        CHECK(blocks.left->successorBlock(0) == (testsOpposite ? blocks.notTaken : blocks.taken));
        CHECK(blocks.right->successorBlock(0) == (testsOpposite ? blocks.taken : blocks.notTaken));
        CHECK_EQ(numberOfValues(proc, Branch), 1u);
        auto code = compileProc(proc);
        int64_t memory[1];
        CHECK_EQ(invoke<int64_t>(*code, 5, 0, memory), testsOpposite ? 20 : 10);
        CHECK_EQ(invoke<int64_t>(*code, 6, 0, memory), testsOpposite ? 10 : 20);
    }
}

static void testThreadJumpsFollowsTestsOfSameValue()
{
    Procedure proc(policyWithMaxTailSize(1));
    Graph graph { proc };
    TailWithTwoPredecessors blocks(graph);
    BasicBlock* secondTest = proc.addBlock();
    BasicBlock* thirdTest = proc.addBlock();
    Value* phi = graph.phi(blocks.tail, Int64);
    graph.branch(blocks.root, blocks.first, blocks.left, blocks.right);
    graph.upsilon(blocks.left, graph.int64(blocks.left, 8), phi);
    graph.upsilon(blocks.right, blocks.second, phi);
    graph.branch(blocks.tail, graph.make(blocks.tail, BitAnd, phi, graph.int64(blocks.tail, 1)), blocks.taken, secondTest);
    graph.branch(secondTest, graph.make(secondTest, BitAnd, phi, graph.int64(secondTest, 2)), blocks.taken, thirdTest);
    graph.branch(thirdTest, graph.make(thirdTest, BitAnd, phi, graph.int64(thirdTest, 4)), blocks.taken, blocks.notTaken);
    blocks.thread(graph);

    CHECK(blocks.left->last()->opcode() == Jump);
    CHECK(blocks.left->successorBlock(0) == blocks.notTaken);
    auto code = compileProc(proc);
    int64_t memory[1];
    CHECK_EQ(invoke<int64_t>(*code, 1, 7, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 1, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 0, 2, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 0, 4, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 0, 8, memory), 20);
}

static void testThreadJumpsFollowsTestsThroughPhisOnPath()
{
    Procedure proc(policyWithMaxTailSize(1));
    Graph graph { proc };
    TailWithTwoPredecessors blocks(graph);
    BasicBlock* aside = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    Value* phi = graph.phi(blocks.tail, Int64);
    Value* joined = graph.phi(join, Int64);
    graph.branch(blocks.root, blocks.first, blocks.left, blocks.right);
    graph.upsilon(blocks.left, graph.int64(blocks.left, 2), phi);
    graph.upsilon(blocks.right, blocks.second, phi);
    graph.upsilon(blocks.tail, graph.make(blocks.tail, BitXor, phi, graph.int64(blocks.tail, 2)), joined);
    graph.branch(blocks.tail, graph.make(blocks.tail, BitAnd, phi, graph.int64(blocks.tail, 1)), aside, join);
    graph.upsilon(aside, graph.int64(aside, 1), joined);
    graph.store(aside, blocks.second, blocks.pointer, 0, firstHeap);
    graph.jump(aside, join);
    graph.branch(join, joined, blocks.taken, blocks.notTaken);
    blocks.thread(graph);

    CHECK(blocks.left->last()->opcode() == Jump);
    CHECK(blocks.left->successorBlock(0) == blocks.notTaken);
    auto code = compileProc(proc);
    int64_t memory[1];
    CHECK_EQ(invoke<int64_t>(*code, 1, 7, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 1, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 0, 2, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 4, memory), 10);
}

static void testThreadJumpsOnVariableSetOnPath()
{
    Procedure proc(policyWithMaxTailSize(1));
    Graph graph { proc };
    TailWithTwoPredecessors blocks(graph);
    BasicBlock* secondTest = proc.addBlock();
    Variable* flag = proc.addVariable(Int64);
    graph.branch(blocks.root, blocks.first, blocks.left, blocks.right);
    graph.set(blocks.left, flag, graph.int64(blocks.left, 1));
    graph.set(blocks.right, flag, blocks.second);
    graph.store(blocks.tail, blocks.second, blocks.pointer, 8, firstHeap);
    graph.branch(blocks.tail, graph.get(blocks.tail, flag), secondTest, blocks.notTaken);
    graph.branch(secondTest, graph.make(secondTest, BitAnd, graph.get(secondTest, flag), graph.int64(secondTest, 2)), blocks.notTaken, blocks.taken);
    blocks.thread(graph);

    CHECK(blocks.left->last()->opcode() == Jump);
    CHECK(blocks.left->successorBlock(0) == blocks.taken);
    auto code = compileProc(proc);
    int64_t memory[2] = { 0, 0 };
    CHECK_EQ(invoke<int64_t>(*code, 1, 7, memory), 10);
    CHECK_EQ(memory[1], 7);
    CHECK_EQ(invoke<int64_t>(*code, 0, 0, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 1, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 0, 3, memory), 20);
}

static void testThreadJumpsReadsLastSetBeforeGet()
{
    for (bool setsBeforeGet : { false, true }) {
        Procedure proc(policyWithMaxTailSize(1));
        Graph graph { proc };
        TailWithTwoPredecessors blocks(graph);
        BasicBlock* secondTest = proc.addBlock();
        BasicBlock* stillSet = proc.addBlock();
        Variable* flag = proc.addVariable(Int64);
        graph.branch(blocks.root, blocks.first, blocks.left, blocks.right);
        graph.set(blocks.left, flag, graph.int64(blocks.left, 1));
        graph.set(blocks.right, flag, blocks.second);
        Value* read = setsBeforeGet ? nullptr : graph.get(blocks.tail, flag);
        graph.set(blocks.tail, flag, graph.int64(blocks.tail, 0));
        if (setsBeforeGet)
            read = graph.get(blocks.tail, flag);
        graph.branch(blocks.tail, read, secondTest, blocks.notTaken);
        graph.branch(secondTest, graph.get(secondTest, flag), stillSet, blocks.taken);
        graph.ret(stillSet, graph.int64(stillSet, 30));
        blocks.thread(graph);

        CHECK(blocks.left->last()->opcode() == Jump);
        CHECK(blocks.left->successorBlock(0) == (setsBeforeGet ? blocks.notTaken : blocks.taken));
        auto code = compileProc(proc);
        int64_t memory[1];
        CHECK_EQ(invoke<int64_t>(*code, 1, 7, memory), setsBeforeGet ? 20 : 10);
        CHECK_EQ(invoke<int64_t>(*code, 0, 7, memory), setsBeforeGet ? 20 : 10);
        CHECK_EQ(invoke<int64_t>(*code, 0, 0, memory), 20);
    }
}

static void testThreadJumpsKeepsTestOfVariableNotSetOnPath()
{
    Procedure proc(policyWithMaxTailSize(1));
    Graph graph { proc };
    TailWithTwoPredecessors blocks(graph);
    Variable* flag = proc.addVariable(Int64);
    Variable* other = proc.addVariable(Int64);
    graph.set(blocks.root, flag, blocks.second);
    graph.branch(blocks.root, blocks.first, blocks.left, blocks.right);
    graph.set(blocks.left, other, graph.int64(blocks.left, 1));
    graph.set(blocks.right, other, graph.int64(blocks.right, 0));
    graph.branch(blocks.tail, graph.get(blocks.tail, flag), blocks.taken, blocks.notTaken);
    blocks.thread(graph);

    auto code = compileProc(proc);
    int64_t memory[1];
    CHECK_EQ(invoke<int64_t>(*code, 1, 1, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 1, 0, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 1, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 0, 0, memory), 20);
}

static void testThreadJumpsFollowsTestsAfterJumpOnlyJoin()
{
    Procedure proc(policyWithMaxTailSize(1));
    Graph graph { proc };
    TailWithTwoPredecessors blocks(graph);
    BasicBlock* test = proc.addBlock();
    graph.branch(blocks.root, graph.make(blocks.root, Equal, blocks.first, graph.int64(blocks.root, 5)), blocks.left, blocks.right);
    graph.jump(blocks.tail, test);
    graph.branch(test, graph.make(test, Equal, blocks.first, graph.int64(test, 5)), blocks.taken, blocks.notTaken);
    blocks.thread(graph);

    CHECK(blocks.left->successorBlock(0) == blocks.taken);
    CHECK(blocks.right->successorBlock(0) == blocks.notTaken);
    CHECK_EQ(numberOfValues(proc, Branch), 1u);
    auto code = compileProc(proc);
    int64_t memory[1];
    CHECK_EQ(invoke<int64_t>(*code, 5, 0, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 6, 0, memory), 20);
}

static void testThreadJumpsFollowsPhiForwardedByJumpBlock()
{
    Procedure proc(policyWithMaxTailSize(1));
    Graph graph { proc };
    TailWithTwoPredecessors blocks(graph);
    BasicBlock* chooser = proc.addBlock();
    BasicBlock* aside = proc.addBlock();
    BasicBlock* test = proc.addBlock();
    Value* phi = graph.phi(blocks.tail, Int64);
    Value* joined = graph.phi(test, Int64);
    graph.branch(blocks.root, blocks.first, chooser, aside);
    graph.branch(chooser, blocks.second, blocks.left, blocks.right);
    graph.upsilon(blocks.left, graph.int64(blocks.left, 1), phi);
    graph.upsilon(blocks.right, graph.int64(blocks.right, 0), phi);
    graph.upsilon(blocks.tail, phi, joined);
    graph.jump(blocks.tail, test);
    graph.upsilon(aside, blocks.second, joined);
    graph.jump(aside, test);
    graph.branch(test, joined, blocks.taken, blocks.notTaken);
    blocks.thread(graph);

    CHECK(blocks.left->last()->opcode() == Jump);
    CHECK(blocks.left->successorBlock(0) == blocks.taken);
    CHECK(blocks.right->last()->opcode() == Jump);
    CHECK(blocks.right->successorBlock(0) == blocks.notTaken);
    auto code = compileProc(proc);
    int64_t memory[1];
    CHECK_EQ(invoke<int64_t>(*code, 1, 7, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 1, 0, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 3, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 0, 0, memory), 20);
}

static void testThreadJumpsStopsFollowingAtUndecidedTest()
{
    Procedure proc(policyWithMaxTailSize(1));
    Graph graph { proc };
    TailWithTwoPredecessors blocks(graph);
    BasicBlock* secondTest = proc.addBlock();
    Value* phi = graph.phi(blocks.tail, Int64);
    graph.branch(blocks.root, blocks.first, blocks.left, blocks.right);
    graph.upsilon(blocks.left, graph.int64(blocks.left, 0), phi);
    graph.upsilon(blocks.right, blocks.second, phi);
    graph.branch(blocks.tail, phi, blocks.taken, secondTest);
    graph.branch(secondTest, graph.make(secondTest, Equal, blocks.second, graph.int64(secondTest, 5)), blocks.taken, blocks.notTaken);
    blocks.thread(graph);

    CHECK(blocks.left->last()->opcode() == Jump);
    CHECK(blocks.left->successorBlock(0) == secondTest);
    auto code = compileProc(proc);
    int64_t memory[1];
    CHECK_EQ(invoke<int64_t>(*code, 1, 5, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 1, 6, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 0, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 6, memory), 10);
}

static BasicBlock* skipJumpOnlyBlocks(BasicBlock* block)
{
    while (block->size() == 1 && block->last()->opcode() == Jump)
        block = block->successorBlock(0);
    return block;
}

static void testThreadJumpsFollowsTestsAfterUndecidedCopiedTest()
{
    Procedure proc(policyWithMaxTailSize(1));
    Graph graph { proc };
    TailWithTwoPredecessors blocks(graph);
    BasicBlock* repeated = proc.addBlock();
    BasicBlock* secondTest = proc.addBlock();
    BasicBlock* other = proc.addBlock();
    Value* phi = graph.phi(blocks.tail, Int32);
    Value* flag = graph.make(blocks.root, BitAnd, blocks.first, graph.int64(blocks.root, 1));
    graph.branch(blocks.root, flag, blocks.right, blocks.left);
    graph.upsilon(blocks.left, graph.make(blocks.left, Equal, blocks.second, graph.int64(blocks.left, 5)), phi);
    graph.upsilon(blocks.right, graph.make(blocks.right, Above, blocks.second, graph.int64(blocks.right, 100)), phi);
    graph.branch(blocks.tail, phi, repeated, secondTest);
    graph.branch(repeated, graph.make(repeated, Equal, blocks.second, graph.int64(repeated, 5)), blocks.taken, other);
    graph.branch(secondTest, flag, other, blocks.notTaken);
    graph.ret(other, graph.int64(other, 30));
    blocks.thread(graph);

    CHECK(blocks.left->last()->opcode() == Branch);
    CHECK(skipJumpOnlyBlocks(blocks.left->successorBlock(0)) == blocks.taken);
    CHECK(skipJumpOnlyBlocks(blocks.left->successorBlock(1)) == blocks.notTaken);
    CHECK(blocks.right->last()->opcode() == Branch);
    CHECK(skipJumpOnlyBlocks(blocks.right->successorBlock(0)) == repeated);
    CHECK(skipJumpOnlyBlocks(blocks.right->successorBlock(1)) == other);
    CHECK_EQ(numberOfValues(proc, Branch), 4u);
    auto code = compileProc(proc);
    int64_t memory[1];
    CHECK_EQ(invoke<int64_t>(*code, 0, 5, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 0, 6, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 200, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 1, 200, memory), 30);
    CHECK_EQ(invoke<int64_t>(*code, 1, 5, memory), 30);
}

static void testThreadJumpsFollowsTestsAfterDecidedAndCopiedTests()
{
    Procedure proc(policyWithMaxTailSize(1));
    Graph graph { proc };
    TailWithTwoPredecessors blocks(graph);
    BasicBlock* join = proc.addBlock();
    BasicBlock* aside = proc.addBlock();
    BasicBlock* repeated = proc.addBlock();
    BasicBlock* other = proc.addBlock();
    Value* phi = graph.phi(blocks.tail, Int64);
    Value* joined = graph.phi(join, Int32);
    graph.branch(blocks.root, blocks.first, blocks.left, blocks.right);
    graph.upsilon(blocks.left, graph.int64(blocks.left, 0), phi);
    graph.upsilon(blocks.right, blocks.second, phi);
    graph.upsilon(blocks.tail, graph.make(blocks.tail, Equal, blocks.second, graph.int64(blocks.tail, 5)), joined);
    graph.branch(blocks.tail, phi, aside, join);
    graph.upsilon(aside, graph.make(aside, Above, blocks.second, graph.int64(aside, 100)), joined);
    graph.jump(aside, join);
    graph.branch(join, joined, repeated, blocks.notTaken);
    graph.branch(repeated, graph.make(repeated, NotEqual, blocks.second, graph.int64(repeated, 5)), other, blocks.taken);
    graph.ret(other, graph.int64(other, 30));
    blocks.thread(graph);

    CHECK(blocks.left->last()->opcode() == Branch);
    CHECK(skipJumpOnlyBlocks(blocks.left->successorBlock(0)) == blocks.taken);
    auto code = compileProc(proc);
    int64_t memory[1];
    CHECK_EQ(invoke<int64_t>(*code, 1, 5, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 1, 6, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 0, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 5, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 200, memory), 30);
}

static bool threadJumpsMakesVariables(Graph& graph, TailWithTwoPredecessors& blocks)
{
    blocks.joinPaths(graph);
    threadJumps(graph.proc);
    validate(graph.proc);
    bool result = !graph.proc.variables().isEmpty();
    fixSSA(graph.proc);
    validate(graph.proc);
    return result;
}

static void testThreadJumpsKeepsBlockLocalPhi()
{
    for (bool leftIsRare : { false, true }) {
        Procedure proc(policyWithMaxTailSize(1));
        Graph graph { proc };
        TailWithTwoPredecessors blocks(graph);
        Value* phi = graph.phi(blocks.tail, Int32);
        graph.branch(blocks.root, blocks.first, leftIsRare ? rarely(blocks.left) : FrequentedBlock(blocks.left), FrequentedBlock(blocks.right));
        graph.upsilon(blocks.left, graph.make(blocks.left, Equal, blocks.second, graph.int64(blocks.left, 5)), phi);
        graph.upsilon(blocks.right, graph.make(blocks.right, Above, blocks.second, graph.int64(blocks.right, 100)), phi);
        graph.branch(blocks.tail, phi, blocks.taken, blocks.notTaken);
        CHECK(!threadJumpsMakesVariables(graph, blocks));

        CHECK(blocks.left->last()->opcode() == (leftIsRare ? Jump : Branch));
        CHECK(blocks.right->last()->opcode() == Branch);
        CHECK_EQ(numberOfValues(proc, Phi), leftIsRare ? 1u : 0u);
        CHECK_EQ(numberOfValues(proc, Upsilon), leftIsRare ? 1u : 0u);
        auto code = compileProc(proc);
        int64_t memory[1];
        CHECK_EQ(invoke<int64_t>(*code, 1, 5, memory), 10);
        CHECK_EQ(invoke<int64_t>(*code, 1, 200, memory), 20);
        CHECK_EQ(invoke<int64_t>(*code, 0, 5, memory), 20);
        CHECK_EQ(invoke<int64_t>(*code, 0, 200, memory), 10);
    }
}

static void testThreadJumpsDemotesPhiUsedByAnotherBlock()
{
    Procedure proc(policyWithMaxTailSize(1));
    Graph graph { proc };
    TailWithTwoPredecessors blocks(graph);
    BasicBlock* user = proc.addBlock();
    Value* phi = graph.phi(blocks.tail, Int64);
    graph.branch(blocks.root, blocks.first, blocks.left, blocks.right);
    graph.upsilon(blocks.left, blocks.second, phi);
    graph.upsilon(blocks.right, graph.make(blocks.right, Add, blocks.second, graph.int64(blocks.right, 1)), phi);
    graph.branch(blocks.tail, phi, user, blocks.notTaken);
    graph.ret(user, phi);
    CHECK(threadJumpsMakesVariables(graph, blocks));

    auto code = compileProc(proc);
    int64_t memory[1];
    CHECK_EQ(invoke<int64_t>(*code, 1, 7, memory), 7);
    CHECK_EQ(invoke<int64_t>(*code, 1, 0, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 7, memory), 8);
    CHECK_EQ(invoke<int64_t>(*code, 0, -1ll, memory), 20);
}

static void testThreadJumpsDemotesPhiWithUpsilonBeforeBranch()
{
    Procedure proc(policyWithMaxTailSize(1));
    Graph graph { proc };
    TailWithTwoPredecessors blocks(graph);
    BasicBlock* middle = proc.addBlock();
    Value* phi = graph.phi(blocks.tail, Int64);
    graph.branch(blocks.root, blocks.first, blocks.left, middle);
    graph.upsilon(middle, graph.make(middle, Add, blocks.second, graph.int64(middle, 1)), phi);
    graph.branch(middle, graph.make(middle, BitAnd, blocks.second, graph.int64(middle, 1)), blocks.tail, blocks.right);
    graph.upsilon(blocks.left, blocks.second, phi);
    graph.upsilon(blocks.right, graph.make(blocks.right, Sub, blocks.second, graph.int64(blocks.right, 4)), phi);
    graph.branch(blocks.tail, phi, blocks.taken, blocks.notTaken);
    CHECK(threadJumpsMakesVariables(graph, blocks));

    auto code = compileProc(proc);
    int64_t memory[1];
    CHECK_EQ(invoke<int64_t>(*code, 1, 3, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 1, 0, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 1, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 0, -1ll, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 2, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 0, 4, memory), 20);
}

static void testThreadJumpsKeepsUndecidedTest()
{
    Procedure proc(policyWithMaxTailSize(1));
    Graph graph { proc };
    TailWithTwoPredecessors blocks(graph);
    graph.branch(blocks.root, graph.make(blocks.root, Equal, blocks.first, graph.int64(blocks.root, 5)), blocks.left, blocks.right);
    graph.branch(blocks.tail, graph.make(blocks.tail, Equal, blocks.second, graph.int64(blocks.tail, 5)), blocks.taken, blocks.notTaken);
    blocks.thread(graph);

    CHECK(blocks.left->successorBlock(0) == blocks.tail);
    CHECK(blocks.right->successorBlock(0) == blocks.tail);
    CHECK_EQ(numberOfValues(proc, Branch), 2u);
}

static void testThreadJumpsSkipsTailWithUncloneablePatchpoint()
{
    for (bool isCloneable : { false, true }) {
        for (bool testIsDecided : { false, true }) {
            Procedure proc(policyWithMaxTailSize(testIsDecided ? 1 : 2));
            Graph graph { proc };
            TailWithTwoPredecessors blocks(graph);
            graph.branch(blocks.root, blocks.first, blocks.left, blocks.right);
            PatchpointValue* patchpoint = blocks.tail->appendNew<PatchpointValue>(proc, Void, Origin(), isCloneable ? B3::Kind(Patchpoint) : cloningForbidden(Patchpoint));
            patchpoint->setGenerator([](CCallHelpers&, const StackmapGenerationParams&) { });
            graph.branch(blocks.tail, testIsDecided ? blocks.first : blocks.second, blocks.taken, blocks.notTaken);
            blocks.thread(graph);

            for (BasicBlock* predecessor : { blocks.left, blocks.right }) {
                CHECK_EQ(predecessor->successorBlock(0) == blocks.tail, !isCloneable);
                CHECK_EQ(numberOfValues(predecessor, Patchpoint), isCloneable ? 1u : 0u);
            }
            CHECK_EQ(numberOfValues(proc, Patchpoint), isCloneable ? 2u : 1u);
            auto code = compileProc(proc);
            int64_t memory[1];
            CHECK_EQ(invoke<int64_t>(*code, 1, 1, memory), 10);
            CHECK_EQ(invoke<int64_t>(*code, 0, 0, memory), 20);
        }
    }
}

static void testThreadJumpsSkipsLoopHeader()
{
    Procedure proc(policyWithMaxTailSize(1));
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Value* iterationCount = cCallArgumentValues<int64_t>(proc, root)[0];
    Value* shouldContinue = graph.phi(header, Int64);
    Value* index = graph.phi(header, Int64);
    graph.upsilon(root, graph.int64(root, 1), shouldContinue);
    graph.upsilon(root, graph.int64(root, 0), index);
    graph.jump(root, header);
    graph.branch(header, shouldContinue, body, exit);
    Value* next = graph.make(body, Add, index, graph.int64(body, 1));
    graph.upsilon(body, graph.make(body, ZExt32, graph.make(body, LessThan, next, iterationCount)), shouldContinue);
    graph.upsilon(body, next, index);
    graph.jump(body, header);
    graph.ret(exit, index);
    proc.resetReachability();
    threadJumps(proc);
    fixSSA(proc);
    validate(proc);

    CHECK(root->successorBlock(0) == header);
    CHECK(body->successorBlock(0) == header);
    CHECK_EQ(compileAndRun<int64_t>(proc, 4), 4);
}

static void checkThreadingLimitHits(Procedure& proc, std::optional<Limit> reachedLimit)
{
    for (Limit limit : { Limit::MaxThreadedTestCopyCost, Limit::MaxThreadedPathCost, Limit::MaxBlocksOnThreadedPath, Limit::MaxThreadedTestDepth, Limit::MaxPredecessorsForConditionQueries })
        CHECK_EQ(!!limitHits(proc, limit), limit == reachedLimit);
}

namespace {

struct TestChain {
    unsigned numberOfTests { 1 };
    unsigned storesPerTest { 0 };
    unsigned testDepth { 1 };
    FrequencyClass predecessorClass { FrequencyClass::Normal };
};

} // anonymous namespace

static void checkTestChainThreading(TestChain chain, unsigned numberOfThreadedTests, std::optional<Limit> reachedLimit)
{
    Procedure proc(policyWithMaxTailSize(1));
    Graph graph { proc };
    TailWithTwoPredecessors blocks(graph);
    Vector<BasicBlock*> tests { blocks.tail };
    while (tests.size() < chain.numberOfTests)
        tests.append(proc.addBlock());
    tests.append(blocks.notTaken);
    Value* phi = graph.phi(blocks.tail, Int64);
    graph.branch(blocks.root, blocks.first, FrequentedBlock(blocks.left, chain.predecessorClass), FrequentedBlock(blocks.right));
    graph.upsilon(blocks.left, graph.int64(blocks.left, 0), phi);
    graph.upsilon(blocks.right, blocks.second, phi);
    for (unsigned index = 0; index < chain.numberOfTests; ++index) {
        BasicBlock* test = tests[index];
        for (unsigned i = 0; i < chain.storesPerTest; ++i)
            graph.store(test, blocks.second, blocks.pointer, 0, firstHeap);
        Value* condition = phi;
        for (unsigned depth = 0; depth < chain.testDepth; ++depth)
            condition = graph.make(test, BitAnd, condition, graph.int64(test, 1ll << index));
        graph.branch(test, condition, blocks.taken, tests[index + 1]);
    }
    blocks.thread(graph);

    CHECK(blocks.left->last()->opcode() == Jump);
    CHECK(blocks.left->successorBlock(0) == tests[numberOfThreadedTests]);
    checkThreadingLimitHits(proc, reachedLimit);
    auto code = compileProc(proc);
    int64_t memory[1];
    auto run = [&](int64_t first, int64_t second) {
        return invoke<int64_t>(*code, first, second, memory);
    };
    int64_t testedBits = (1ll << chain.numberOfTests) - 1;
    CHECK_EQ(run(1, 7), 20);
    CHECK_EQ(memory[0], chain.storesPerTest ? 7 : 1);
    for (int64_t second : { 0, 1, 4, 512, 1024 })
        CHECK_EQ(run(0, second), (second & testedBits) ? 10 : 20);
}

static void testThreadJumpsStopsAtPathLimit()
{
    constexpr unsigned limit = defaultValueOf(Limit::MaxBlocksOnThreadedPath);
    checkTestChainThreading({ .numberOfTests = limit / 2 }, limit / 2, std::nullopt);
    checkTestChainThreading({ .numberOfTests = limit }, limit, std::nullopt);
    checkTestChainThreading({ .numberOfTests = limit + 2 }, limit, Limit::MaxBlocksOnThreadedPath);
}

static void testThreadJumpsStopsAtTestCopyCostLimit()
{
    constexpr unsigned limit = defaultValueOf(Limit::MaxThreadedTestCopyCost);
    checkTestChainThreading({ .storesPerTest = limit }, 1, std::nullopt);
    checkTestChainThreading({ .storesPerTest = limit + 1 }, 0, Limit::MaxThreadedTestCopyCost);
}

static void testThreadJumpsStopsAtPathCostLimit()
{
    constexpr unsigned storesPerTest = defaultValueOf(Limit::MaxThreadedTestCopyCost);
    constexpr unsigned numberOfAffordableTests = defaultValueOf(Limit::MaxThreadedPathCost) / storesPerTest;
    checkTestChainThreading({ .numberOfTests = numberOfAffordableTests, .storesPerTest = storesPerTest }, numberOfAffordableTests, std::nullopt);
    checkTestChainThreading({ .numberOfTests = numberOfAffordableTests + 1, .storesPerTest = storesPerTest }, numberOfAffordableTests, Limit::MaxThreadedPathCost);
}

static void testThreadJumpsStopsAtTestDepthLimit()
{
    constexpr unsigned limit = defaultValueOf(Limit::MaxThreadedTestDepth);
    checkTestChainThreading({ .testDepth = limit }, 1, std::nullopt);
    checkTestChainThreading({ .testDepth = limit + 1 }, 0, Limit::MaxThreadedTestDepth);
}

static void testThreadJumpsStopsAtPredecessorLimit()
{
    constexpr unsigned limit = defaultValueOf(Limit::MaxPredecessorsForConditionQueries);
    for (unsigned numberOfPredecessors : { limit, limit + 1 }) {
        bool isWithinLimit = numberOfPredecessors == limit;
        Procedure proc(policyWithMaxTailSize(1));
        Graph graph { proc };
        TailWithTwoPredecessors blocks(graph);
        BasicBlock* chooser = proc.addBlock();
        graph.branch(blocks.root, graph.make(blocks.root, Equal, blocks.first, graph.int64(blocks.root, 5)), blocks.left, chooser);
        SwitchValue* switchValue = chooser->appendNew<SwitchValue>(proc, Origin(), blocks.second);
        switchValue->setFallThrough(FrequentedBlock(blocks.right));
        for (unsigned caseValue = 0; caseValue < numberOfPredecessors - 2; ++caseValue) {
            BasicBlock* predecessor = proc.addBlock();
            switchValue->appendCase(SwitchCase(caseValue, FrequentedBlock(predecessor)));
            graph.jump(predecessor, blocks.tail);
        }
        graph.branch(blocks.tail, graph.make(blocks.tail, Equal, blocks.first, graph.int64(blocks.tail, 5)), blocks.taken, blocks.notTaken);
        blocks.tail->last()->setOrigin(numberedOrigin(1));
        blocks.thread(graph);

        CHECK(blocks.left->successorBlock(0) == (isWithinLimit ? blocks.taken : blocks.tail));
        CHECK(blocks.right->successorBlock(0) == (isWithinLimit ? blocks.notTaken : blocks.tail));
        checkThreadingLimitHits(proc, isWithinLimit ? std::nullopt : std::optional(Limit::MaxPredecessorsForConditionQueries));
        CHECK_EQ(hasNumber(proc.report().firstOriginOf(Limit::MaxPredecessorsForConditionQueries), 1), !isWithinLimit);
        auto code = compileProc(proc);
        int64_t memory[1];
        CHECK_EQ(invoke<int64_t>(*code, 5, 0, memory), 10);
        CHECK_EQ(invoke<int64_t>(*code, 6, 0, memory), 20);
        CHECK_EQ(invoke<int64_t>(*code, 6, 100, memory), 20);
    }
}

static void testThreadJumpsCopiesOnlyIntoLikelyPredecessors()
{
    for (FrequencyClass leftClass : { FrequencyClass::Normal, FrequencyClass::Unlikely, FrequencyClass::Rare }) {
        bool isLikely = leftClass == FrequencyClass::Normal;
        checkTestChainThreading({ .predecessorClass = leftClass }, 1, std::nullopt);
        checkTestChainThreading({ .storesPerTest = 1, .predecessorClass = leftClass }, isLikely ? 1 : 0, std::nullopt);

        Procedure proc(policyWithMaxTailSize(1));
        Graph graph { proc };
        TailWithTwoPredecessors blocks(graph);
        graph.branch(blocks.root, blocks.first, FrequentedBlock(blocks.left, leftClass), FrequentedBlock(blocks.right));
        graph.branch(blocks.tail, blocks.second, blocks.taken, blocks.notTaken);
        blocks.thread(graph);
        for (BasicBlock* predecessor : { blocks.left, blocks.right })
            CHECK(predecessor->last()->opcode() == (isLikely ? Branch : Jump));
    }
}

static void testThreadJumpsKeepsTailOffHotPathShared()
{
    for (FrequencyClass chooserClass : { FrequencyClass::Normal, FrequencyClass::Unlikely, FrequencyClass::Rare }) {
        Procedure proc(policyWithMaxTailSize(1));
        Graph graph { proc };
        TailWithTwoPredecessors blocks(graph);
        BasicBlock* chooser = proc.addBlock();
        Value* test = graph.make(blocks.root, Equal, blocks.first, graph.int64(blocks.root, 7));
        graph.branch(blocks.root, blocks.first, FrequentedBlock(chooser, chooserClass), FrequentedBlock(blocks.taken));
        graph.branch(chooser, blocks.second, blocks.left, blocks.right);
        graph.branch(blocks.tail, test, blocks.taken, blocks.notTaken);
        blocks.thread(graph);
        for (BasicBlock* predecessor : { blocks.left, blocks.right })
            CHECK(predecessor->last()->opcode() == (chooserClass == FrequencyClass::Normal ? Branch : Jump));

        auto code = compileProc(proc);
        int64_t memory[1];
        CHECK_EQ(invoke<int64_t>(*code, 0, 0, memory), 10);
        CHECK_EQ(invoke<int64_t>(*code, 7, 0, memory), 10);
        CHECK_EQ(invoke<int64_t>(*code, 7, 1, memory), 10);
        CHECK_EQ(invoke<int64_t>(*code, 6, 0, memory), 20);
        CHECK_EQ(invoke<int64_t>(*code, 6, 1, memory), 20);
    }
}

static void checkTailDuplication(const Policy& policy, unsigned tailSize, bool isCopied)
{
    Procedure proc(policy);
    Graph graph { proc };
    TailWithTwoPredecessors blocks(graph);
    graph.branch(blocks.root, blocks.first, blocks.left, blocks.right);
    Value* value = blocks.first;
    for (unsigned size = 1; size < tailSize; ++size)
        value = graph.make(blocks.tail, Add, value, blocks.second);
    graph.branch(blocks.tail, value, blocks.taken, blocks.notTaken);
    blocks.duplicate(graph);

    for (BasicBlock* predecessor : { blocks.left, blocks.right }) {
        if (isCopied) {
            CHECK(predecessor->last()->opcode() == Branch);
            CHECK(skipJumpOnlyBlocks(predecessor->successorBlock(0)) == blocks.taken);
            CHECK(skipJumpOnlyBlocks(predecessor->successorBlock(1)) == blocks.notTaken);
        } else {
            CHECK(predecessor->last()->opcode() == Jump);
            CHECK(predecessor->successorBlock(0) == blocks.tail);
        }
    }
    auto code = compileProc(proc);
    int64_t memory[1];
    auto checkRun = [&](int64_t first, int64_t second) {
        int64_t sum = first + (tailSize - 1) * second;
        CHECK_EQ(invoke<int64_t>(*code, first, second, memory), sum ? 10 : 20);
    };
    checkRun(1, 1);
    checkRun(1, -1);
    checkRun(0, 0);
}

static void testDuplicateTailsUnderDefaultPolicy()
{
    unsigned defaultMaxSize = Options::maxB3TailDupBlockSize();
    checkTailDuplication(Policy::fromProcessOptions(), defaultMaxSize, true);
    checkTailDuplication(Policy::fromProcessOptions(), defaultMaxSize + 1, false);

    Procedure proc;
    Graph graph { proc };
    TailWithTwoPredecessors blocks(graph);
    Value* phi = graph.phi(blocks.tail, Int64);
    graph.branch(blocks.root, blocks.first, blocks.left, blocks.right);
    graph.upsilon(blocks.left, graph.int64(blocks.left, 0), phi);
    graph.upsilon(blocks.right, blocks.second, phi);
    graph.branch(blocks.tail, phi, blocks.taken, blocks.notTaken);
    blocks.duplicate(graph);

    CHECK(blocks.left->last()->opcode() == Branch);
    CHECK(blocks.right->last()->opcode() == Branch);
    auto code = compileProc(proc);
    int64_t memory[1];
    CHECK_EQ(invoke<int64_t>(*code, 1, 1, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 1, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 0, 0, memory), 20);
}

static void testDuplicateTailsSizeFromPolicy()
{
    checkTailDuplication(policyWithMaxTailSize(1), 3, false);
    checkTailDuplication(policyWithMaxTailSize(1), 1, true);
    checkTailDuplication(policyWithMaxTailSize(0), 1, false);
}

static Policy impliedComparisonPolicy()
{
    return policyWith([](Policy& policy) {
        policy.optimization.foldsImpliedComparisons = true;
    });
}

static void testFoldPathConstantsFoldsComparisonImpliedByBranch()
{
    for (bool isImplied : { true, false }) {
        for (bool usesPolicy : { true, false }) {
            Procedure proc(usesPolicy ? impliedComparisonPolicy() : Policy());
            Graph graph { proc };
            BasicBlock* root = proc.addBlock();
            BasicBlock* largeCase = proc.addBlock();
            BasicBlock* smallCase = proc.addBlock();
            BasicBlock* equalCase = proc.addBlock();
            BasicBlock* notEqualCase = proc.addBlock();
            Value* number = cCallArgumentValues<int64_t>(proc, root)[0];
            graph.branch(root, graph.make(root, GreaterEqual, number, graph.int64(root, 100)), largeCase, smallCase);
            graph.ret(largeCase, graph.int64(largeCase, 1));
            graph.branch(smallCase, graph.make(smallCase, Equal, number, graph.int64(smallCase, isImplied ? 200 : 50)), equalCase, notEqualCase);
            graph.ret(equalCase, graph.int64(equalCase, 2));
            graph.ret(notEqualCase, graph.int64(notEqualCase, 3));
            proc.resetReachability();
            validate(proc);

            foldPathConstants(proc);
            validate(proc);
            CHECK_EQ(smallCase->last()->child(0)->isInt(0), isImplied && usesPolicy);
            auto code = compileProc(proc);
            CHECK_EQ(invoke<int64_t>(*code, 200), 1);
            CHECK_EQ(invoke<int64_t>(*code, 100), 1);
            CHECK_EQ(invoke<int64_t>(*code, 99), 3);
            CHECK_EQ(invoke<int64_t>(*code, 50), isImplied ? 3 : 2);
        }
    }
}

static void testFoldPathConstantsBypassesSelectDecidedByBranch()
{
    for (bool usesPolicy : { true, false }) {
        Procedure proc(usesPolicy ? impliedComparisonPolicy() : Policy());
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* largeCase = proc.addBlock();
        BasicBlock* smallCase = proc.addBlock();
        Value* number = cCallArgumentValues<int64_t>(proc, root)[0];
        Value* normalized = root->appendNew<Value>(proc, Select, Origin(), graph.make(root, Equal, number, graph.int64(root, 200)), graph.int64(root, 150), number);
        graph.branch(root, graph.make(root, GreaterEqual, number, graph.int64(root, 100)), largeCase, smallCase);
        graph.ret(largeCase, normalized);
        Value* sum = graph.make(smallCase, Add, normalized, graph.int64(smallCase, 1));
        graph.ret(smallCase, sum);
        proc.resetReachability();
        validate(proc);

        foldPathConstants(proc);
        validate(proc);
        CHECK_EQ(sum->child(0), usesPolicy ? number : normalized);
        CHECK_EQ(largeCase->last()->child(0), normalized);
        auto code = compileProc(proc);
        CHECK_EQ(invoke<int64_t>(*code, 200), 150);
        CHECK_EQ(invoke<int64_t>(*code, 300), 300);
        CHECK_EQ(invoke<int64_t>(*code, 7), 8);
    }
}

static void testFoldPathConstantsFoldsSelect()
{
    for (bool usesPolicy : { true, false }) {
        Procedure proc(usesPolicy ? impliedComparisonPolicy() : Policy());
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* taken = proc.addBlock();
        BasicBlock* notTaken = proc.addBlock();
        auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t>(proc, root);
        Value* condition = arguments[0];
        Value* first = arguments[1];
        Value* second = arguments[2];
        graph.branch(root, condition, taken, notTaken);
        graph.ret(taken, graph.select(taken, condition, first, second));
        graph.ret(notTaken, graph.select(notTaken, first, first, second));
        proc.resetReachability();
        foldPathConstants(proc);
        validate(proc);

        CHECK_EQ(numberOfValues(taken, Select), usesPolicy ? 0u : 1u);
        CHECK_EQ(numberOfValues(notTaken, Select), 1u);
        auto code = compileProc(proc);
        CHECK_EQ(invoke<int64_t>(*code, 1, 2, 3), 2);
        CHECK_EQ(invoke<int64_t>(*code, 0, 2, 3), 2);
        CHECK_EQ(invoke<int64_t>(*code, 0, 0, 3), 3);
    }
}

static void testFoldPathConstantsKeepsComparisonNotImplied()
{
    Procedure proc(impliedComparisonPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* taken = proc.addBlock();
    BasicBlock* notTaken = proc.addBlock();
    Value* x = cCallArgumentValues<int64_t>(proc, root)[0];
    Value* isTiny = graph.make(root, LessThan, x, graph.int64(root, 5));
    graph.branch(root, graph.make(root, LessThan, x, graph.int64(root, 10)), taken, notTaken);
    graph.ret(taken, isTiny);
    graph.ret(notTaken, isTiny);
    proc.resetReachability();
    validate(proc);

    foldPathConstants(proc);
    validate(proc);
    CHECK(taken->last()->child(0) == isTiny);
    CHECK(notTaken->last()->child(0)->isInt(0));
    auto code = compileProc(proc);
    for (int64_t value : { 3, 7, 12 })
        CHECK_EQ(invoke<int32_t>(*code, value), value < 5 ? 1 : 0);
}

static void testFoldPathConstantsManyComparisonsOfOneValue()
{
    constexpr int64_t numberOfComparisons = 2000;
    constexpr int64_t outerConstant = 7;
    constexpr int64_t nestedConstant = 9;
    Procedure proc(impliedComparisonPolicy());
    Graph graph { proc };
    BasicBlock* block = proc.addBlock();
    BasicBlock* outerTaken = nullptr;
    Value* x = cCallArgumentValues<int64_t>(proc, block)[0];
    Variable* sum = proc.addVariable(Int64);
    graph.set(block, sum, graph.int64(block, 0));
    for (int64_t constant = 0; constant < numberOfComparisons; ++constant) {
        BasicBlock* taken = proc.addBlock();
        BasicBlock* next = proc.addBlock();
        graph.branch(block, graph.make(block, Equal, x, graph.int64(block, constant)), taken, next);
        graph.increase(taken, sum, constant);
        if (constant == outerConstant) {
            BasicBlock* nestedTaken = proc.addBlock();
            outerTaken = taken;
            graph.branch(taken, graph.make(taken, Equal, x, graph.int64(taken, nestedConstant)), nestedTaken, next);
            graph.increase(nestedTaken, sum, numberOfComparisons);
            graph.jump(nestedTaken, next);
        } else
            graph.jump(taken, next);
        block = next;
    }
    graph.ret(block, graph.get(block, sum));
    proc.resetReachability();
    fixSSA(proc);
    validate(proc);

    foldPathConstants(proc);
    validate(proc);
    CHECK(outerTaken->last()->child(0)->isInt(0));
    auto code = compileProc(proc);
    constexpr int64_t values[] = { -1, 0, 1, outerConstant, nestedConstant, numberOfComparisons - 1, numberOfComparisons };
    for (int64_t value : values)
        CHECK_EQ(invoke<int64_t>(*code, value), value > 0 && value < numberOfComparisons ? value : 0);
}

static void testFoldPathConstantsIgnoresJoinedSuccessor()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* other = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    Value* x = cCallArgumentValues<int64_t>(proc, root)[0];
    graph.branch(root, x, other, join);
    graph.jump(other, join);
    Value* sum = graph.make(join, Add, x, graph.int64(join, 1));
    graph.ret(join, sum);
    proc.resetReachability();
    validate(proc);

    foldPathConstants(proc);
    validate(proc);
    CHECK(sum->child(0) == x);
    auto code = compileProc(proc);
    for (int64_t value : { 0, 5 })
        CHECK_EQ(invoke<int64_t>(*code, value), value + 1);
}

static void testFoldPathConstantsSwitchCases()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* shared = proc.addBlock();
    BasicBlock* alone = proc.addBlock();
    BasicBlock* otherwise = proc.addBlock();
    Value* x = cCallArgumentValues<int64_t>(proc, root)[0];
    SwitchValue* switchValue = root->appendNew<SwitchValue>(proc, Origin(), x);
    switchValue->setFallThrough(FrequentedBlock(otherwise));
    switchValue->appendCase(SwitchCase(1, FrequentedBlock(shared)));
    switchValue->appendCase(SwitchCase(2, FrequentedBlock(shared)));
    switchValue->appendCase(SwitchCase(3, FrequentedBlock(alone)));
    auto appendReturnedSum = [&](BasicBlock* block) {
        Value* sum = graph.make(block, Add, x, graph.int64(block, 10));
        graph.ret(block, sum);
        return sum;
    };
    Value* sharedSum = appendReturnedSum(shared);
    Value* aloneSum = appendReturnedSum(alone);
    Value* otherwiseSum = appendReturnedSum(otherwise);
    proc.resetReachability();
    validate(proc);

    foldPathConstants(proc);
    validate(proc);
    CHECK(sharedSum->child(0) == x);
    CHECK(aloneSum->child(0)->isInt(3));
    CHECK(otherwiseSum->child(0) == x);
    auto code = compileProc(proc);
    for (int64_t value : { 1, 2, 3, 9 })
        CHECK_EQ(invoke<int64_t>(*code, value), value + 10);
}

static void testFoldPathConstantsKeepsUsesOfConstantPredicate()
{
    Procedure proc(impliedComparisonPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* taken = proc.addBlock();
    BasicBlock* neverEntered = proc.addBlock();
    BasicBlock* loop = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Value* bound = cCallArgumentValues<int64_t>(proc, root)[0];
    Value* one = graph.int64(root, 1);
    graph.branch(root, one, taken, neverEntered);
    graph.ret(taken, bound);
    Value* index = graph.phi(loop, Int64);
    graph.upsilon(neverEntered, graph.int64(neverEntered, 0), index);
    graph.jump(neverEntered, loop);
    Value* next = graph.make(loop, Add, index, one);
    Value* test = graph.make(loop, LessThan, next, bound);
    graph.upsilon(loop, next, index);
    graph.branch(loop, test, loop, exit);
    graph.ret(exit, next);
    proc.setKnownInduction({ index, next, test, 1, false, false });
    proc.resetReachability();
    validate(proc);

    foldPathConstants(proc);
    validate(proc);
    CHECK(next->child(1) == one);
    CHECK(proc.knownInduction(index));
    CHECK_EQ(invoke<int64_t>(*compileProc(proc), 7), 7);
}

void addBunJumpThreadingTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testThreadJumpsOnConstantPhi());
    RUN(testThreadJumpsDecidedByDominatingBranch());
    RUN(testThreadJumpsFollowsTestsOfSameValue());
    RUN(testThreadJumpsFollowsTestsThroughPhisOnPath());
    RUN(testThreadJumpsOnVariableSetOnPath());
    RUN(testThreadJumpsReadsLastSetBeforeGet());
    RUN(testThreadJumpsKeepsTestOfVariableNotSetOnPath());
    RUN(testThreadJumpsFollowsTestsAfterJumpOnlyJoin());
    RUN(testThreadJumpsFollowsPhiForwardedByJumpBlock());
    RUN(testThreadJumpsStopsFollowingAtUndecidedTest());
    RUN(testThreadJumpsFollowsTestsAfterUndecidedCopiedTest());
    RUN(testThreadJumpsFollowsTestsAfterDecidedAndCopiedTests());
    RUN(testThreadJumpsKeepsBlockLocalPhi());
    RUN(testThreadJumpsDemotesPhiUsedByAnotherBlock());
    RUN(testThreadJumpsDemotesPhiWithUpsilonBeforeBranch());
    RUN(testThreadJumpsKeepsUndecidedTest());
    RUN(testThreadJumpsSkipsTailWithUncloneablePatchpoint());
    RUN(testThreadJumpsSkipsLoopHeader());
    RUN(testThreadJumpsStopsAtPathLimit());
    RUN(testThreadJumpsStopsAtTestCopyCostLimit());
    RUN(testThreadJumpsStopsAtPathCostLimit());
    RUN(testThreadJumpsStopsAtTestDepthLimit());
    RUN(testThreadJumpsStopsAtPredecessorLimit());
    RUN(testThreadJumpsCopiesOnlyIntoLikelyPredecessors());
    RUN(testThreadJumpsKeepsTailOffHotPathShared());
    RUN(testDuplicateTailsUnderDefaultPolicy());
    RUN(testDuplicateTailsSizeFromPolicy());
    RUN(testFoldPathConstantsFoldsComparisonImpliedByBranch());
    RUN(testFoldPathConstantsBypassesSelectDecidedByBranch());
    RUN(testFoldPathConstantsFoldsSelect());
    RUN(testFoldPathConstantsKeepsComparisonNotImplied());
    RUN(testFoldPathConstantsManyComparisonsOfOneValue());
    RUN(testFoldPathConstantsIgnoresJoinedSuccessor());
    RUN(testFoldPathConstantsSwitchCases());
    RUN(testFoldPathConstantsKeepsUsesOfConstantPredicate());
}

#endif // ENABLE(B3_JIT)
