/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "B3EliminateRedundantPhiCycles.h"
#include "B3LoopUtilities.h"
#include "B3RotateLoops.h"
#include "B3SimplifyTerminals.h"
#include "B3SwitchValue.h"

static void testRotateLoopsMovesTestToBottom()
{
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop(graph);
    graph.increase(loop.body, loop.sum, graph.get(loop.body, loop.index));
    loop.endBody(loop.body);

    CHECK(proc.naturalLoops().headerOf(loop.header));
    loop.rotate();

    B3::NaturalLoops& loops = proc.naturalLoops();
    CHECK_EQ(loops.numLoops(), 1u);
    CHECK(loops.loop(0).header() == loop.body);
    CHECK(!loops.belongsTo(loop.header, loops.loop(0)));
    CHECK_EQ(valueCountInLoops(proc, Branch), 1u);
    CHECK_EQ(numberOfValues(proc, Branch), 2u);
    checkSumsIntegersBelowBound(proc);
}

static void testRotateLoopsBottomTestedLoop()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* index = proc.addVariable(Int64);
    Value* bound = cCallArgumentValues<int64_t>(proc, root)[0];
    graph.set(root, index, graph.int64(root, 0));
    graph.jump(root, body);
    graph.increase(body, index, 1);
    graph.branch(body, graph.make(body, LessThan, graph.get(body, index), bound), body, exit);
    graph.ret(exit, graph.get(exit, index));
    proc.resetReachability();
    fixSSA(proc);

    rotateLoops(proc);
    validate(proc);
    CHECK_EQ(valueCountInLoops(proc, Branch), 1u);
    auto code = compileProc(proc);
    CHECK_EQ(invoke<int64_t>(*code, 0), 1);
    CHECK_EQ(invoke<int64_t>(*code, 1), 1);
    CHECK_EQ(invoke<int64_t>(*code, 2), 2);
    CHECK_EQ(invoke<int64_t>(*code, 7), 7);
}

static void testRotateLoopsSkipsLargeTest()
{
    for (unsigned numberOfAdditions : { 10u, 11u }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* header = proc.addBlock();
        BasicBlock* body = proc.addBlock();
        BasicBlock* exit = proc.addBlock();
        Variable* index = proc.addVariable(Int64);
        Value* bound = cCallArgumentValues<int64_t>(proc, root)[0];
        graph.set(root, index, graph.int64(root, 0));
        graph.jump(root, header);
        Value* tested = graph.get(header, index);
        tested->setOrigin(numberedOrigin(1));
        for (unsigned i = 0; i < numberOfAdditions; ++i)
            tested = graph.make(header, Add, tested, bound);
        graph.branch(header, graph.make(header, LessThan, tested, graph.int64(header, 100)), body, exit);
        graph.increase(body, index, 1);
        graph.jump(body, header);
        graph.ret(exit, graph.get(exit, index));
        proc.resetReachability();
        fixSSA(proc);

        CHECK_EQ(rotateLoops(proc), numberOfAdditions == 10);
        CHECK_EQ(!!limitHits(proc, Limit::MaxCopiedValuesPerRotatedLoop), numberOfAdditions == 11);
        CHECK_EQ(hasNumber(proc.report().firstOriginOf(Limit::MaxCopiedValuesPerRotatedLoop), 1), numberOfAdditions == 11);
        fixSSA(proc);
        validate(proc);
        auto code = compileProc(proc);
        CHECK_EQ(invoke<int64_t>(*code, 0), 100);
        CHECK_EQ(invoke<int64_t>(*code, 100), 0);
    }
}

static void testRotateLoopsSkipsUncloneableTest()
{
    for (bool isCloningForbidden : { true, false }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* header = proc.addBlock();
        BasicBlock* body = proc.addBlock();
        BasicBlock* exit = proc.addBlock();
        Variable* index = proc.addVariable(Int64);
        Value* bound = cCallArgumentValues<int64_t>(proc, root)[0];
        graph.set(root, index, graph.int64(root, 0));
        graph.jump(root, header);
        PatchpointValue* patchpoint = header->appendNew<PatchpointValue>(proc, Void, Origin(), isCloningForbidden ? cloningForbidden(Patchpoint) : B3::Kind(Patchpoint));
        patchpoint->effects = Effects::none();
        patchpoint->effects.writes = HeapRange::top();
        patchpoint->setGenerator([](CCallHelpers&, const StackmapGenerationParams&) { });
        graph.branch(header, graph.make(header, LessThan, graph.get(header, index), bound), body, exit);
        graph.increase(body, index, 1);
        graph.jump(body, header);
        graph.ret(exit, graph.get(exit, index));
        proc.resetReachability();
        fixSSA(proc);

        CHECK_EQ(rotateLoops(proc), !isCloningForbidden);
        fixSSA(proc);
        validate(proc);
        CHECK_EQ(numberOfValues(proc, Patchpoint), isCloningForbidden ? 1u : 2u);
        CHECK_EQ(valueCountInLoops(proc, Patchpoint), 1u);
        CHECK_EQ(compileAndRun<int64_t>(proc, static_cast<int64_t>(5)), 5);
    }
}

static void testRotateLoopsSkipsRareBackEdge()
{
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop(graph);
    graph.increase(loop.body, loop.sum, graph.get(loop.body, loop.index));
    loop.endBody(loop.body);
    loop.body->setSuccessors(rarely(loop.header));

    CHECK(!rotateLoops(proc));
    checkSumsIntegersBelowBound(proc);
}

static void testRotateLoopsKeepsUnlikelyBackEdge()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* latch = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Value* bound = cCallArgumentValues<int64_t>(proc, root)[0];
    Value* index = graph.phi(header, Int64);
    Value* sum = graph.phi(header, Int64);
    graph.upsilon(root, graph.int64(root, 0), index);
    graph.upsilon(root, graph.int64(root, 0), sum);
    graph.jump(root, header);
    graph.branch(header, graph.make(header, LessThan, index, bound), body, exit);
    Value* next = graph.make(body, Add, index, graph.int64(body, 1));
    graph.upsilon(body, next, index);
    graph.upsilon(body, sum, sum);
    graph.branch(body, graph.make(body, BitAnd, next, graph.int64(body, 1)), unlikely(header), FrequentedBlock(latch));
    graph.upsilon(latch, next, index);
    graph.upsilon(latch, graph.make(latch, Add, sum, next), sum);
    graph.jump(latch, header);
    graph.ret(exit, sum);
    resetAndValidate(proc);

    CHECK(rotateLoops(proc));
    BasicBlock* bottomTest = latch->successorBlock(0);
    CHECK(bottomTest != header);
    CHECK(bottomTest->last()->opcode() == Branch);
    CHECK(bottomTest->successorBlock(0) == body);
    CHECK(body->successor(0) == unlikely(header));
    fixSSA(proc);
    validate(proc);

    B3::NaturalLoops& loops = proc.naturalLoops();
    CHECK_EQ(loops.numLoops(), 2u);
    CHECK(loops.headerOf(header));
    CHECK(loops.headerOf(body));
    CHECK(!loops.belongsTo(header, *loops.headerOf(body)));
    auto code = compileProc(proc);
    for (int64_t argument : { -1, 0, 1, 2, 5, 10 }) {
        int64_t numberOfEvenIntegers = argument > 0 ? argument / 2 : 0;
        CHECK_EQ(invoke<int64_t>(*code, argument), numberOfEvenIntegers * (numberOfEvenIntegers + 1));
    }
}

static void testRotateLoopsHonorsHeaderFilter()
{
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop(graph);
    graph.increase(loop.body, loop.sum, graph.get(loop.body, loop.index));
    loop.endBody(loop.body);

    Vector<BasicBlock*> headers { loop.exit };
    CHECK(!rotateLoops(proc, &headers));
    headers.append(loop.header);
    CHECK(rotateLoops(proc, &headers));
    fixSSA(proc);
    validate(proc);
    headers = { loop.body };
    CHECK(!rotateLoops(proc, &headers));
    checkSumsIntegersBelowBound(proc);
}

static void testRotateLoopsNestedLoops()
{
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop outer(graph);
    BasicBlock* innerHeader = proc.addBlock();
    BasicBlock* innerBody = proc.addBlock();
    BasicBlock* outerLatch = proc.addBlock();
    Variable* innerIndex = proc.addVariable(Int64);
    graph.set(outer.body, innerIndex, graph.int64(outer.body, 0));
    graph.jump(outer.body, innerHeader);
    graph.branch(innerHeader, graph.make(innerHeader, LessThan, graph.get(innerHeader, innerIndex), graph.get(innerHeader, outer.index)), innerBody, outerLatch);
    graph.increase(innerBody, outer.sum, 1);
    graph.increase(innerBody, innerIndex, 1);
    graph.jump(innerBody, innerHeader);
    outer.endBody(outerLatch);
    outer.rotate();

    B3::NaturalLoops& loops = proc.naturalLoops();
    CHECK_EQ(loops.numLoops(), 2u);
    CHECK(loops.headerOf(innerBody));
    CHECK(loops.headerOf(outer.body));
    CHECK(!loops.headerOf(innerHeader));
    CHECK(!loops.headerOf(outer.header));
    checkSumsIntegersBelowBound(proc);
}

static unsigned numberOfPollsIn(BasicBlock* block)
{
    unsigned result = 0;
    for (Value* value : *block)
        result += isLoopPoll(value);
    return result;
}

static void testRotateLoopsMovesPollInHeaderToBottomTest()
{
    for (bool hasUnlikelyBackEdge : { false, true }) {
        Procedure proc(remarkPolicy());
        Graph graph { proc };
        PollState state;
        BasicBlock* root = proc.addBlock();
        BasicBlock* header = proc.addBlock();
        BasicBlock* body = proc.addBlock();
        BasicBlock* latch = proc.addBlock();
        BasicBlock* exit = proc.addBlock();
        Variable* index = proc.addVariable(Int64);
        Value* bound = cCallArgumentValues<int64_t>(proc, root)[0];
        graph.set(root, index, graph.int64(root, 0));
        graph.jump(root, header);
        appendPoll(graph, header, state);
        graph.branch(header, graph.make(header, LessThan, graph.get(header, index), bound), body, exit);
        graph.increase(body, index, 1);
        if (hasUnlikelyBackEdge)
            graph.branch(body, graph.make(body, Equal, graph.get(body, index), graph.int64(body, 2)), unlikely(header), FrequentedBlock(latch));
        else
            graph.jump(body, latch);
        graph.jump(latch, header);
        graph.ret(exit, graph.get(exit, index));
        proc.resetReachability();
        fixSSA(proc);

        CHECK(rotateLoops(proc));
        fixSSA(proc);
        validate(proc);
        CHECK_EQ(numberOfValues(proc, Patchpoint), hasUnlikelyBackEdge ? 2u : 1u);
        CHECK_EQ(valueCountInLoops(proc, Patchpoint), hasUnlikelyBackEdge ? 2u : 1u);
        CHECK_EQ(numberOfPollsIn(header), 0u);
        CHECK_EQ(compileAndRun<int64_t>(proc, static_cast<int64_t>(5)), 5);
        CHECK_EQ(state.numberOfPolls, 5);
    }
}

// A rare edge leaves the loop and leads back in front of its top. Where that cycle has no poll of its own, its turns poll on the way back, and what enters the loop from outside polls nowhere.
static void testRotateLoopsPollsOnWayBackOfCycleWithoutPoll()
{
    for (bool cycleHasPoll : { false, true }) {
        Procedure proc(remarkPolicy());
        Graph graph { proc };
        PollState state;
        BasicBlock* root = proc.addBlock();
        BasicBlock* entry = proc.addBlock();
        BasicBlock* header = proc.addBlock();
        BasicBlock* body = proc.addBlock();
        BasicBlock* latch = proc.addBlock();
        BasicBlock* wayBack = proc.addBlock();
        BasicBlock* exit = proc.addBlock();
        Variable* index = proc.addVariable(Int64);
        Value* bound = cCallArgumentValues<int64_t>(proc, root)[0];
        graph.set(root, index, graph.int64(root, 0));
        graph.jump(root, entry);
        if (cycleHasPoll)
            appendPoll(graph, entry, state);
        graph.jump(entry, header);
        appendPoll(graph, header, state);
        graph.branch(header, graph.make(header, LessThan, graph.get(header, index), bound), body, exit);
        graph.increase(body, index, 1);
        graph.branch(body, graph.make(body, BitAnd, graph.get(body, index), graph.int64(body, 1)), rarely(wayBack), FrequentedBlock(latch));
        graph.jump(latch, header);
        graph.jump(wayBack, entry);
        graph.ret(exit, graph.get(exit, index));
        proc.resetReachability();
        fixSSA(proc);

        CHECK(rotateLoops(proc));
        fixSSA(proc);
        validate(proc);
        CHECK_EQ(numberOfPollsIn(header), 0u);
        CHECK_EQ(numberOfValues(proc, Patchpoint), 2u);
        CHECK_EQ(wayBack->numSuccessors(), 1u);
        CHECK_EQ(numberOfPollsIn(wayBack->successorBlock(0)), 1u);
        CHECK_EQ(wayBack->successorBlock(0) == entry, cycleHasPoll);
        CHECK_EQ(compileAndRun<int64_t>(proc, static_cast<int64_t>(6)), 6);
        CHECK_EQ(state.numberOfPolls, cycleHasPoll ? 7 : 6);
    }
}

// The loop around has no poll, and its turns are likely: they are as many as the entries into the inner loop, so they poll in the test that stays in front of it.
static void testRotateLoopsKeepsPollInFrontOfLoopInsideLikelyCycleWithoutPoll()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    PollState state;
    BasicBlock* root = proc.addBlock();
    BasicBlock* outerHeader = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* outerLatch = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* index = proc.addVariable(Int64);
    Variable* turns = proc.addVariable(Int64);
    Value* bound = cCallArgumentValues<int64_t>(proc, root)[0];
    graph.set(root, index, graph.int64(root, 0));
    graph.set(root, turns, graph.int64(root, 0));
    graph.jump(root, outerHeader);
    graph.jump(outerHeader, header);
    appendPoll(graph, header, state);
    graph.branch(header, graph.make(header, BitAnd, graph.get(header, index), graph.int64(header, 1)), body, outerLatch);
    graph.increase(body, index, 1);
    graph.jump(body, header);
    graph.increase(outerLatch, index, 1);
    graph.increase(outerLatch, turns, 1);
    graph.branch(outerLatch, graph.make(outerLatch, LessThan, graph.get(outerLatch, turns), bound), outerHeader, exit);
    graph.ret(exit, graph.get(exit, turns));
    proc.resetReachability();
    fixSSA(proc);

    CHECK(rotateLoops(proc));
    fixSSA(proc);
    validate(proc);
    CHECK_EQ(numberOfPollsIn(header), 1u);
    CHECK_EQ(numberOfValues(proc, Patchpoint), 2u);
    CHECK_EQ(compileAndRun<int64_t>(proc, static_cast<int64_t>(4)), 4);
    CHECK_EQ(state.numberOfPolls, 7);
}

static void testRotateLoopsWritesRemark()
{
    for (bool hasLikelyBackEdge : { true, false }) {
        Procedure proc(loopPolicy());
        Graph graph { proc };
        TopTestedLoop loop(graph);
        graph.increase(loop.body, loop.sum, graph.get(loop.body, loop.index));
        loop.endBody(loop.body);
        if (!hasLikelyBackEdge)
            loop.body->setSuccessors(rarely(loop.header));

        CHECK_EQ(rotateLoops(proc), hasLikelyBackEdge);
        fixSSA(proc);
        validate(proc);
        CHECK_EQ(hasRemark(proc, RemarkKind::LoopRotated, GaveUpReason::None), hasLikelyBackEdge);
        CHECK_EQ(proc.report().countOf(Counter::LoopsRotated), hasLikelyBackEdge ? 1u : 0u);
        checkSumsIntegersBelowBound(proc);
    }
}

static int64_t sumWithRareChain(int64_t bound)
{
    int64_t sum = 0;
    for (int64_t index = 0;;) {
        if ((index & 7) == 7) {
            sum += 100;
            ++index;
            continue;
        }
        sum += index;
        if (++index >= bound)
            return sum;
    }
}

static void testRotateLoopsCountsRareBlockLimit()
{
    for (uint64_t length : { defaultValueOf(Limit::MaxRareBlocksPerRotatedLoop), defaultValueOf(Limit::MaxRareBlocksPerRotatedLoop) + 1 }) {
        bool isWithinLimit = length == defaultValueOf(Limit::MaxRareBlocksPerRotatedLoop);
        Procedure proc(loopPolicy());
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* header = proc.addBlock();
        BasicBlock* body = proc.addBlock();
        BasicBlock* exit = proc.addBlock();
        Vector<BasicBlock*> chain;
        for (uint64_t i = 0; i < length; ++i)
            chain.append(proc.addBlock());
        Variable* index = proc.addVariable(Int64);
        Variable* sum = proc.addVariable(Int64);
        Value* bound = cCallArgumentValues<int64_t>(proc, root)[0];
        graph.set(root, index, graph.int64(root, 0));
        graph.set(root, sum, graph.int64(root, 0));
        graph.jump(root, header);
        Value* lowBits = graph.make(header, BitAnd, graph.get(header, index), graph.int64(header, 7));
        graph.branch(header, graph.make(header, NotEqual, lowBits, graph.int64(header, 7)), FrequentedBlock(body), rarely(chain.first()));
        graph.increase(body, sum, graph.get(body, index));
        graph.increase(body, index, 1);
        graph.branch(body, graph.make(body, LessThan, graph.get(body, index), bound), header, exit);
        graph.increase(chain.first(), sum, 100);
        for (uint64_t i = 0; i + 1 < length; ++i)
            graph.jump(chain[i], chain[i + 1]);
        graph.increase(chain.last(), index, 1);
        graph.jump(chain.last(), header);
        graph.ret(exit, graph.get(exit, sum));
        proc.resetReachability();
        fixSSA(proc);
        validate(proc);

        CHECK_EQ(rotateLoops(proc), isWithinLimit);
        CHECK_EQ(limitHits(proc, Limit::MaxRareBlocksPerRotatedLoop) > 0, !isWithinLimit);
        fixSSA(proc);
        validate(proc);
        CHECK_EQ(compileAndRun<int64_t>(proc, static_cast<int64_t>(20)), sumWithRareChain(20));
    }
}

static void testRotateLoopsSkipsGuardWhoseRarePathRejoinsBody()
{
    Procedure proc(loopPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* rare = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* index = proc.addVariable(Int64);
    Variable* sum = proc.addVariable(Int64);
    Value* bound = cCallArgumentValues<int64_t>(proc, root)[0];
    graph.set(root, index, graph.int64(root, 0));
    graph.set(root, sum, graph.int64(root, 0));
    graph.jump(root, header);
    Value* lowBits = graph.make(header, BitAnd, graph.get(header, index), graph.int64(header, 7));
    graph.branch(header, graph.make(header, NotEqual, lowBits, graph.int64(header, 7)), FrequentedBlock(body), rarely(rare));
    graph.increase(rare, sum, 100);
    graph.jump(rare, body);
    graph.increase(body, sum, graph.get(body, index));
    graph.increase(body, index, 1);
    graph.branch(body, graph.make(body, LessThan, graph.get(body, index), bound), header, exit);
    graph.ret(exit, graph.get(exit, sum));
    proc.resetReachability();
    fixSSA(proc);
    validate(proc);
    CHECK(header->successorBlock(0) != body);

    CHECK(!rotateLoops(proc));
    CHECK_EQ(proc.naturalLoops().numLoops(), 1u);
    CHECK_EQ(compileAndRun<int64_t>(proc, static_cast<int64_t>(20)), 190 + 2 * 100);
}

// for (index = 0, flag = 1; index < bound; ) { if (!flag) return sum + 1000; sum += ++index; flag = ..; }
// The block that leads back writes the flag twice. What it writes last is what the next turn reads.
static void testRotateLoopsReadsWhatLatchWritesLast()
{
    enum class Written : uint8_t { OneThenZero, OneThenLowestBitOfIndex, ZeroThenOne };
    for (Written written : { Written::OneThenZero, Written::OneThenLowestBitOfIndex, Written::ZeroThenOne }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* header = proc.addBlock();
        BasicBlock* test = proc.addBlock();
        BasicBlock* body = proc.addBlock();
        BasicBlock* leave = proc.addBlock();
        BasicBlock* exit = proc.addBlock();
        Value* bound = cCallArgumentValues<int64_t>(proc, root)[0];
        Value* index = graph.phi(header, Int64);
        Value* sum = graph.phi(header, Int64);
        Value* flag = graph.phi(header, Int64);
        graph.upsilon(root, graph.int64(root, 0), index);
        graph.upsilon(root, graph.int64(root, 0), sum);
        graph.upsilon(root, graph.int64(root, 1), flag);
        graph.jump(root, header);
        graph.branch(header, graph.make(header, LessThan, index, bound), test, exit);
        graph.branch(test, flag, body, leave);
        Value* next = graph.make(body, Add, index, graph.int64(body, 1));
        graph.upsilon(body, next, index);
        graph.upsilon(body, graph.make(body, Add, sum, next), sum);
        graph.upsilon(body, graph.int64(body, written == Written::ZeroThenOne ? 0 : 1), flag);
        graph.upsilon(body, written == Written::OneThenLowestBitOfIndex ? graph.make(body, BitAnd, next, graph.int64(body, 1)) : graph.int64(body, written == Written::ZeroThenOne ? 1 : 0), flag);
        graph.jump(body, header);
        graph.ret(leave, graph.make(leave, Add, sum, graph.int64(leave, 1000)));
        graph.ret(exit, sum);
        resetAndValidate(proc);

        CHECK(rotateLoops(proc));
        fixSSA(proc);
        validate(proc);
        auto code = compileProc(proc);
        CHECK_EQ(invoke<int64_t>(*code, 0), 0);
        CHECK_EQ(invoke<int64_t>(*code, 1), 1);
        switch (written) {
        case Written::OneThenZero:
            CHECK_EQ(invoke<int64_t>(*code, 2), 1001);
            CHECK_EQ(invoke<int64_t>(*code, 5), 1001);
            break;
        case Written::OneThenLowestBitOfIndex:
            CHECK_EQ(invoke<int64_t>(*code, 2), 3);
            CHECK_EQ(invoke<int64_t>(*code, 5), 1003);
            break;
        case Written::ZeroThenOne:
            CHECK_EQ(invoke<int64_t>(*code, 2), 3);
            CHECK_EQ(invoke<int64_t>(*code, 5), 15);
            break;
        }
    }
}

static void testSimplifyTerminals()
{
    for (int32_t constant : { 0, 1 }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* taken = proc.addBlock();
        BasicBlock* notTaken = proc.addBlock();
        graph.branch(root, graph.int32(root, constant), taken, notTaken);
        graph.ret(taken, graph.int32(taken, 1));
        graph.ret(notTaken, graph.int32(notTaken, 2));
        proc.resetReachability();

        CHECK(simplifyTerminals(proc));
        validate(proc);
        CHECK(root->last()->opcode() == Jump);
        CHECK_EQ(numberOfBlocks(proc), 2u);
        CHECK_EQ(compileAndRun<int32_t>(proc), constant ? 1 : 2);
    }
}

static void testSimplifyTerminalsKeepsNonConstantBranch()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* taken = proc.addBlock();
    BasicBlock* notTaken = proc.addBlock();
    graph.branch(root, cCallArgumentValues<int32_t>(proc, root)[0], taken, notTaken);
    graph.ret(taken, graph.int32(taken, 1));
    graph.ret(notTaken, graph.int32(notTaken, 2));
    proc.resetReachability();

    CHECK(!simplifyTerminals(proc));
    CHECK(root->last()->opcode() == Branch);
    auto code = compileProc(proc);
    CHECK_EQ(invoke<int32_t>(*code, 5), 1);
    CHECK_EQ(invoke<int32_t>(*code, 0), 2);
}

static void testSimplifyTerminalsLooksThroughIdentity()
{
    for (int32_t constant : { 0, 1 }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* taken = proc.addBlock();
        BasicBlock* notTaken = proc.addBlock();
        graph.branch(root, graph.make(root, Identity, graph.int32(root, constant)), taken, notTaken);
        graph.ret(taken, graph.int32(taken, 1));
        graph.ret(notTaken, graph.int32(notTaken, 2));
        proc.resetReachability();

        CHECK(simplifyTerminals(proc));
        validate(proc);
        CHECK(root->last()->opcode() == Jump);
        CHECK_EQ(numberOfBlocks(proc), 2u);
        CHECK_EQ(compileAndRun<int32_t>(proc), constant ? 1 : 2);
    }
}

struct SwitchTargets {
    BasicBlock* three;
    BasicBlock* seven;
    BasicBlock* otherwise;
};

static SwitchTargets appendSwitch(Graph& graph, BasicBlock* root, Value* value)
{
    Procedure& proc = graph.proc;
    SwitchTargets targets { proc.addBlock(), proc.addBlock(), proc.addBlock() };
    SwitchValue* switchValue = root->appendNew<SwitchValue>(proc, Origin(), value);
    switchValue->setFallThrough(FrequentedBlock(targets.otherwise));
    switchValue->appendCase(SwitchCase(3, FrequentedBlock(targets.three)));
    switchValue->appendCase(SwitchCase(7, FrequentedBlock(targets.seven)));
    graph.ret(targets.three, graph.int32(targets.three, 30));
    graph.ret(targets.seven, graph.int32(targets.seven, 70));
    graph.ret(targets.otherwise, graph.int32(targets.otherwise, 0));
    proc.resetReachability();
    return targets;
}

static void testSimplifyTerminalsOfSwitch()
{
    for (bool matchesCase : { true, false }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        SwitchTargets targets = appendSwitch(graph, root, graph.int32(root, matchesCase ? 7 : 5));

        CHECK(simplifyTerminals(proc));
        validate(proc);
        CHECK(root->last()->opcode() == Jump);
        CHECK(root->successorBlock(0) == (matchesCase ? targets.seven : targets.otherwise));
        CHECK_EQ(numberOfBlocks(proc), 2u);
        CHECK_EQ(compileAndRun<int32_t>(proc), matchesCase ? 70 : 0);
    }

    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    appendSwitch(graph, root, cCallArgumentValues<int32_t>(proc, root)[0]);

    CHECK(!simplifyTerminals(proc));
    CHECK(root->last()->opcode() == Switch);
    auto code = compileProc(proc);
    CHECK_EQ(invoke<int32_t>(*code, 3), 30);
    CHECK_EQ(invoke<int32_t>(*code, 7), 70);
    CHECK_EQ(invoke<int32_t>(*code, 5), 0);
}

static void testEliminateRedundantPhiCycles()
{
    for (bool isRedundant : { true, false }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* header = proc.addBlock();
        BasicBlock* left = proc.addBlock();
        BasicBlock* right = proc.addBlock();
        BasicBlock* join = proc.addBlock();
        BasicBlock* exit = proc.addBlock();
        auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t>(proc, root);
        Value* carriedValue = arguments[0];
        Value* other = arguments[1];
        Value* bound = arguments[2];

        Value* atHeader = graph.phi(header, Int64);
        Value* index = graph.phi(header, Int64);
        Value* atJoin = graph.phi(join, Int64);
        graph.upsilon(root, carriedValue, atHeader);
        graph.upsilon(root, graph.int64(root, 0), index);
        graph.jump(root, header);
        graph.branch(header, graph.make(header, BitAnd, index, graph.int64(header, 1)), left, right);
        graph.upsilon(left, atHeader, atJoin);
        graph.jump(left, join);
        graph.upsilon(right, isRedundant ? carriedValue : other, atJoin);
        graph.jump(right, join);
        Value* next = graph.make(join, Add, index, graph.int64(join, 1));
        graph.upsilon(join, atJoin, atHeader);
        graph.upsilon(join, next, index);
        graph.branch(join, graph.make(join, LessThan, next, bound), header, exit);
        graph.ret(exit, atJoin);
        proc.resetReachability();
        validate(proc);

        CHECK_EQ(eliminateRedundantPhiCycles(proc), isRedundant);
        validate(proc);
        CHECK_EQ(numberOfValues(proc, Phi), isRedundant ? 1u : 3u);
        auto code = compileProc(proc);
        CHECK_EQ(invoke<int64_t>(*code, 42, 7, 1), isRedundant ? 42 : 7);
        CHECK_EQ(invoke<int64_t>(*code, 42, 7, 2), isRedundant ? 42 : 7);
        CHECK_EQ(invoke<int64_t>(*code, 42, 7, 5), isRedundant ? 42 : 7);
    }
}

static void testEliminateRedundantPhiCyclesKeepsPhiOfValueInSameBlock()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    Value* x = cCallArgumentValues<int64_t>(proc, root)[0];
    UpsilonValue* upsilon = root->appendNew<UpsilonValue>(proc, Origin(), graph.make(root, Add, x, graph.int64(root, 1)));
    Value* phi = graph.phi(root, Int64);
    upsilon->setPhi(phi);
    graph.ret(root, phi);
    proc.resetReachability();
    validate(proc);

    CHECK(!eliminateRedundantPhiCycles(proc));
    validate(proc);
    CHECK_EQ(numberOfValues(proc, Phi), 1u);
    CHECK_EQ(compileAndRun<int64_t>(proc, static_cast<int64_t>(41)), 42);
}

void addBunLoopTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testRotateLoopsMovesTestToBottom());
    RUN(testRotateLoopsBottomTestedLoop());
    RUN(testRotateLoopsSkipsLargeTest());
    RUN(testRotateLoopsSkipsUncloneableTest());
    RUN(testRotateLoopsSkipsRareBackEdge());
    RUN(testRotateLoopsKeepsUnlikelyBackEdge());
    RUN(testRotateLoopsHonorsHeaderFilter());
    RUN(testRotateLoopsNestedLoops());
    RUN(testRotateLoopsMovesPollInHeaderToBottomTest());
    RUN(testRotateLoopsPollsOnWayBackOfCycleWithoutPoll());
    RUN(testRotateLoopsKeepsPollInFrontOfLoopInsideLikelyCycleWithoutPoll());
    RUN(testRotateLoopsWritesRemark());
    RUN(testRotateLoopsCountsRareBlockLimit());
    RUN(testRotateLoopsSkipsGuardWhoseRarePathRejoinsBody());
    RUN(testRotateLoopsReadsWhatLatchWritesLast());
    RUN(testSimplifyTerminals());
    RUN(testSimplifyTerminalsKeepsNonConstantBranch());
    RUN(testSimplifyTerminalsLooksThroughIdentity());
    RUN(testSimplifyTerminalsOfSwitch());
    RUN(testEliminateRedundantPhiCycles());
    RUN(testEliminateRedundantPhiCyclesKeepsPhiOfValueInSameBlock());
}

#endif // ENABLE(B3_JIT)
