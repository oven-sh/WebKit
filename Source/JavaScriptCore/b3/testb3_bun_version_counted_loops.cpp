/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "B3FixSSA.h"
#include "B3LoopUtilities.h"
#include "B3RotateLoops.h"

static Value* increaseCounter(Graph& graph, BasicBlock* block, Variable* counter, Value* step)
{
    Value* next = graph.make(block, Add, graph.get(block, counter), step);
    graph.set(block, counter, next);
    return next;
}

enum class ValueOfEarlierLoop : uint8_t { Bound, Start };

static int64_t countOfEarlierLoop(int64_t bound)
{
    int64_t count = 0;
    do
        count += 2;
    while (count < bound);
    return count;
}

static void checkCountedLoopAfterEarlierLoop(ValueOfEarlierLoop role)
{
    constexpr int64_t constantBound = 2 * longRun + 100;
    Procedure proc(loopPolicy());
    Graph graph { proc };
    PollState state;
    TopTestedLoop loop(graph);

    BasicBlock* entry = proc.addBlock();
    BasicBlock* earlierHeader = proc.addBlock();
    BasicBlock* earlierRare = proc.addBlock();
    BasicBlock* earlierLatch = proc.addBlock();
    BasicBlock* between = proc.addBlock();
    Variable* count = proc.addVariable(Int64);
    loop.root->setSuccessors(FrequentedBlock(entry));
    graph.set(entry, count, graph.int64(entry, 0));
    Value* constant = graph.int64(entry, constantBound);
    graph.jump(entry, earlierHeader);
    Value* earlierLoaded = graph.load(earlierHeader, Int64, loop.pointer, 0, firstHeap);
    graph.branch(earlierHeader, graph.make(earlierHeader, Equal, earlierLoaded, loop.extra), FrequentedBlock(earlierLatch), rarely(earlierRare));
    graph.jump(earlierRare, earlierLatch);
    Value* counted = graph.make(earlierLatch, Add, graph.get(earlierLatch, count), graph.int64(earlierLatch, 2));
    graph.set(earlierLatch, count, counted);
    graph.branch(earlierLatch, graph.make(earlierLatch, LessThan, counted, loop.bound), earlierHeader, between);
    if (role == ValueOfEarlierLoop::Start) {
        graph.set(between, loop.index, counted);
        loop.start = counted;
    }
    graph.jump(between, loop.header);
    loop.header->last()->child(0)->child(1) = role == ValueOfEarlierLoop::Bound ? counted : constant;

    BasicBlock* rare = proc.addBlock();
    BasicBlock* hot = proc.addBlock();
    appendPoll(graph, loop.body, state);
    Value* loaded = graph.load(loop.body, Int64, loop.pointer, 0, firstHeap);
    graph.branch(loop.body, graph.make(loop.body, Equal, loaded, loop.extra), FrequentedBlock(hot), rarely(rare));
    graph.increase(rare, loop.sum, 1000);
    graph.jump(rare, hot);
    graph.increase(hot, loop.sum, loaded);
    loop.endBody(hot);
    loop.rotate();

    CHECK(versionLoopsAndValidate(proc));
    CHECK_EQ(proc.report().countOf(Counter::LoopsVersioned), 2u);
    CHECK(!hasRemark(proc, RemarkKind::LoopBoundNotClamped, GaveUpReason::BoundNotInvariant));
    auto code = compileProc(proc);
    int64_t memory[1] = { 7 };
    for (int64_t bound : { static_cast<int64_t>(-1), static_cast<int64_t>(1), static_cast<int64_t>(5), static_cast<int64_t>(100), longRun + 101 }) {
        int64_t earlierCount = countOfEarlierLoop(bound);
        int64_t iterations = role == ValueOfEarlierLoop::Bound ? earlierCount : constantBound - earlierCount;
        state = { };
        CHECK_EQ(run(*code, bound, memory, 7), iterations * 7);
        CHECK_EQ(state.numberOfPolls, iterations);
        state = { };
        CHECK_EQ(run(*code, bound, memory, 8), iterations * 1007);
        CHECK_EQ(state.numberOfPolls, iterations);
    }
}

static void testVersionLoopsCountsLoopAfterEarlierLoop()
{
    checkCountedLoopAfterEarlierLoop(ValueOfEarlierLoop::Bound);
    checkCountedLoopAfterEarlierLoop(ValueOfEarlierLoop::Start);
}

static void testVersionLoopsCountsLoopWithBoundLoadedBeforeAndAtBottom()
{
    Procedure proc(loopPolicy());
    Graph graph { proc };
    PollState state;
    BasicBlock* root = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* special = proc.addBlock();
    BasicBlock* hot = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* index = proc.addVariable(Int64);
    Variable* sum = proc.addVariable(Int64);
    auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root);
    Value* pointer = arguments[1];
    Value* extra = arguments[2];
    graph.set(root, index, graph.int64(root, 0));
    graph.set(root, sum, graph.int64(root, 0));
    graph.jump(root, header);
    Value* storage = graph.load(header, Int64, pointer, 0, firstHeap);
    Value* continues = graph.make(header, LessThan, graph.get(header, index), graph.load(header, Int64, storage, 8, secondHeap));
    graph.branch(header, continues, body, exit);
    appendPoll(graph, body, state);
    BasicBlock* block = body;
    graph.branch(block, graph.make(block, Equal, graph.get(block, index), extra), rarely(special), FrequentedBlock(hot));
    graph.callLikePatchpoint(special)->effects.writes = secondHeap;
    graph.increase(special, sum, 1000);
    graph.jump(special, hot);
    graph.increase(hot, sum, graph.get(hot, index));
    Value* nextIndex = increaseCounter(graph, hot, index, graph.int64(hot, 1));
    graph.jump(hot, header);
    graph.ret(exit, graph.get(exit, sum));
    proc.resetReachability();
    proc.setKnownInduction(DemotedInduction { index, header, nextIndex, continues, 1, false });
    fixSSA(proc);
    validate(proc);
    CHECK(rotateLoops(proc));
    fixSSA(proc);
    validate(proc);

    CHECK(versionLoopsAndValidate(proc));
    auto code = compileProc(proc);
    constexpr int64_t iterations = 2 * longRun + 100;
    for (int64_t bound : { static_cast<int64_t>(-1), static_cast<int64_t>(0), static_cast<int64_t>(1), static_cast<int64_t>(9), iterations }) {
        int64_t inner[2] = { 0, bound };
        int64_t outer[1] = { reinterpret_cast<int64_t>(inner) };
        state = { };
        CHECK_EQ(run(*code, 0, outer, -1), sumBelow(bound));
        CHECK_EQ(state.numberOfPolls, std::max<int64_t>(bound, 0));
        state = { };
        CHECK_EQ(run(*code, 0, outer, 5), sumBelow(bound) + (bound > 5 ? 1000 : 0));
        CHECK_EQ(state.numberOfPolls, std::max<int64_t>(bound, 0));
    }
}

static void testVersionLoopsTopTestedExcursionStillPolls()
{
    Procedure proc(loopPolicy());
    Graph graph { proc };
    PollState state;
    BasicBlock* root = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* special = proc.addBlock();
    BasicBlock* test = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* unexpected = proc.addBlock();
    BasicBlock* hot = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* index = proc.addVariable(Int64);
    Variable* sum = proc.addVariable(Int64);
    auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root);
    Value* bound = arguments[0];
    Value* pointer = arguments[1];
    Value* extra = arguments[2];
    graph.set(root, index, graph.int64(root, 0));
    graph.set(root, sum, graph.int64(root, 0));
    graph.jump(root, header);
    graph.branch(header, graph.make(header, Below, graph.make(header, BitAnd, graph.get(header, index), graph.int64(header, 3)), extra), rarely(special), FrequentedBlock(test));
    graph.increase(special, sum, 1000);
    graph.jump(special, test);
    Value* continues = graph.make(test, LessThan, graph.get(test, index), bound);
    graph.branch(test, continues, body, exit);
    appendPoll(graph, body, state);
    Value* loaded = graph.load(body, Int64, pointer, 0, firstHeap);
    graph.branch(body, graph.make(body, Below, loaded, graph.int64(body, 100)), FrequentedBlock(hot), rarely(unexpected));
    graph.callLikePatchpoint(unexpected);
    graph.jump(unexpected, hot);
    graph.increase(hot, sum, loaded);
    Value* nextIndex = increaseCounter(graph, hot, index, graph.int64(hot, 1));
    graph.jump(hot, header);
    graph.ret(exit, graph.get(exit, sum));
    proc.resetReachability();
    proc.setKnownInduction(DemotedInduction { index, header, nextIndex, continues, 1, false });
    fixSSA(proc);
    validate(proc);

    versionLoopsAndValidate(proc);
    auto code = compileProc(proc);
    int64_t memory[1] = { 7 };
    constexpr int64_t iterations = 3 * longRun + 100;

    CHECK_EQ(run(*code, iterations, memory, 0), iterations * 7);
    CHECK_EQ(state.numberOfPolls, iterations);

    state = { };
    CHECK_EQ(run(*code, iterations, memory, 4), iterations * 1007 + 1000);
    CHECK_EQ(state.numberOfPolls, iterations);
}

static constexpr int64_t rareIndex = 1000;
static constexpr int64_t rareSummand = 5000;

static void buildTopTestedLoopWithRarePath(Graph& graph, PollState& state, bool resetsCounter)
{
    Procedure& proc = graph.proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* unexpected = proc.addBlock();
    BasicBlock* checked = proc.addBlock();
    BasicBlock* outOfBounds = proc.addBlock();
    BasicBlock* hot = proc.addBlock();
    BasicBlock* rare = proc.addBlock();
    BasicBlock* next = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* index = proc.addVariable(Int64);
    Variable* sum = proc.addVariable(Int64);
    Variable* restarts = proc.addVariable(Int64);
    auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root).eager();
    Value* pointer = arguments[1];
    graph.set(root, index, graph.int64(root, 0));
    graph.set(root, sum, graph.int64(root, 0));
    graph.set(root, restarts, arguments[2]);
    graph.jump(root, header);
    Value* continues = graph.make(header, LessThan, graph.get(header, index), arguments[0]);
    graph.branch(header, continues, body, exit);
    appendPoll(graph, body, state);
    Value* loaded = graph.load(body, Int64, pointer, 0, firstHeap);
    graph.branch(body, graph.make(body, Below, loaded, graph.int64(body, 100)), FrequentedBlock(checked), rarely(unexpected));
    graph.callLikePatchpoint(unexpected);
    graph.jump(unexpected, checked);
    Value* length = graph.make(checked, ZExt32, graph.load(checked, Int32, pointer, 8, secondHeap));
    graph.branch(checked, graph.make(checked, Below, graph.get(checked, index), length), FrequentedBlock(hot), rarely(outOfBounds));
    graph.ret(outOfBounds, graph.int64(outOfBounds, -1));
    graph.increase(hot, sum, loaded);
    graph.branch(hot, graph.make(hot, Equal, graph.get(hot, index), graph.int64(hot, rareIndex)), rarely(rare), FrequentedBlock(next));
    graph.increase(rare, sum, rareSummand);
    if (resetsCounter) {
        graph.increase(rare, restarts, -1);
        Value* restartsAgain = graph.make(rare, GreaterEqual, graph.get(rare, restarts), graph.int64(rare, 0));
        Value* following = graph.make(rare, Add, graph.get(rare, index), graph.int64(rare, 1));
        graph.set(rare, index, graph.select(rare, restartsAgain, graph.int64(rare, 0), following));
        graph.jump(rare, header);
    } else
        graph.jump(rare, next);
    Value* nextIndex = increaseCounter(graph, next, index, graph.int64(next, 1));
    graph.jump(next, header);
    graph.ret(exit, graph.get(exit, sum));
    proc.resetReachability();
    if (!resetsCounter)
        proc.setKnownInduction(DemotedInduction { index, header, nextIndex, continues, 1, false });
    fixSSA(proc);
    validate(proc);
}

static unsigned numberOfRarePathCopies(Procedure& proc)
{
    return numberOfValues(proc, [](Value* value) {
        return value->isInt64(rareSummand);
    });
}

static void testVersionLoopsTopTestedExcursionReturnsToFastLoop()
{
    Procedure proc(loopPolicy());
    Graph graph { proc };
    PollState state;
    buildTopTestedLoopWithRarePath(graph, state, false);

    CHECK(versionLoopsAndValidate(proc));
    CHECK_EQ(proc.report().countOf(Counter::RangeChecksRemoved), 1u);
    CHECK_EQ(numberOfRarePathCopies(proc), 2u);
    auto code = compileProc(proc);
    constexpr int64_t iterations = 3 * longRun + 100;
    int64_t memory[2] = { 7, iterations };
    CHECK_EQ(run(*code, iterations, memory), iterations * 7 + rareSummand);
    CHECK_EQ(state.numberOfPolls, iterations);
    memory[1] = 1500;
    CHECK_EQ(run(*code, 2000, memory), -1);
}

enum class RarePathBeforeTest : uint8_t { OnlyComputes, WritesHoistedLocation };

static void buildTopTestedLoopWithRarePathBeforeTest(Graph& graph, PollState& state, RarePathBeforeTest rarePath)
{
    Procedure& proc = graph.proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* rare = proc.addBlock();
    BasicBlock* test = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* unexpected = proc.addBlock();
    BasicBlock* checked = proc.addBlock();
    BasicBlock* outOfBounds = proc.addBlock();
    BasicBlock* hot = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* index = proc.addVariable(Int64);
    Variable* sum = proc.addVariable(Int64);
    auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root).eager();
    Value* pointer = arguments[1];
    graph.set(root, index, graph.int64(root, 0));
    graph.set(root, sum, graph.int64(root, 0));
    graph.jump(root, header);
    graph.branch(header, graph.make(header, Equal, graph.get(header, index), arguments[2]), rarely(rare), FrequentedBlock(test));
    graph.increase(rare, sum, rareSummand);
    if (rarePath == RarePathBeforeTest::WritesHoistedLocation)
        graph.callLikePatchpoint(rare)->effects.writes = firstHeap;
    graph.jump(rare, test);
    Value* continues = graph.make(test, LessThan, graph.get(test, index), arguments[0]);
    graph.branch(test, continues, body, exit);
    appendPoll(graph, body, state);
    Value* loaded = graph.load(body, Int64, pointer, 0, firstHeap);
    graph.branch(body, graph.make(body, Below, loaded, graph.int64(body, 100)), FrequentedBlock(checked), rarely(unexpected));
    graph.callLikePatchpoint(unexpected);
    graph.jump(unexpected, checked);
    Value* length = graph.make(checked, ZExt32, graph.load(checked, Int32, pointer, 8, secondHeap));
    graph.branch(checked, graph.make(checked, Below, graph.get(checked, index), length), FrequentedBlock(hot), rarely(outOfBounds));
    graph.ret(outOfBounds, graph.int64(outOfBounds, -1));
    graph.increase(hot, sum, loaded);
    Value* nextIndex = increaseCounter(graph, hot, index, graph.int64(hot, 1));
    graph.jump(hot, header);
    graph.ret(exit, graph.get(exit, sum));
    proc.resetReachability();
    proc.setKnownInduction(DemotedInduction { index, header, nextIndex, continues, 1, false });
    fixSSA(proc);
    validate(proc);
}

static void testVersionLoopsExcursionBeforeTopTestReturnsToFastLoop()
{
    constexpr int64_t iterations = 3 * longRun + 100;
    for (RarePathBeforeTest rarePath : { RarePathBeforeTest::OnlyComputes, RarePathBeforeTest::WritesHoistedLocation }) {
        bool returnsToFastLoop = rarePath == RarePathBeforeTest::OnlyComputes;
        Procedure proc(loopPolicy());
        Graph graph { proc };
        PollState state;
        buildTopTestedLoopWithRarePathBeforeTest(graph, state, rarePath);

        CHECK(versionLoopsAndValidate(proc));
        CHECK_EQ(proc.report().countOf(Counter::RangeChecksRemoved), 1u);
        CHECK_EQ(numberOfRarePathCopies(proc), returnsToFastLoop ? 2u : 1u);
        auto code = compileProc(proc);
        int64_t memory[2] = { 7, iterations };

        CHECK_EQ(run(*code, iterations, memory, -1), iterations * 7);
        CHECK_EQ(state.numberOfPolls, iterations);

        state = { };
        CHECK_EQ(run(*code, iterations, memory, rareIndex), iterations * 7 + rareSummand);
        CHECK_EQ(state.numberOfPolls, iterations);

        memory[1] = 1500;
        for (int64_t rareIteration : { 1498, 1499, 1500, 1501 })
            CHECK_EQ(run(*code, 2000, memory, rareIteration), -1);
        for (int64_t rareIteration : { 1, 1498, 1499 })
            CHECK_EQ(run(*code, 1500, memory, rareIteration), 1500 * 7 + rareSummand);
        CHECK_EQ(run(*code, 1500, memory, 1500), 1500 * 7 + rareSummand);
        CHECK_EQ(run(*code, 1500, memory, 1501), 1500 * 7);
    }
}

static void testLoopPhasesKeepFastLoopAfterRarePathBeforeTopTest()
{
    constexpr int64_t iterations = 3 * longRun + 100;
    Procedure proc(optimizingPolicy());
    Graph graph { proc };
    PollState state;
    buildTopTestedLoopWithRarePathBeforeTest(graph, state, RarePathBeforeTest::OnlyComputes);

    auto code = compileProc(proc, 2);
    int64_t memory[2] = { 7, iterations };
    for (int64_t rareIteration : { static_cast<int64_t>(1), rareIndex, longRun - 1, longRun + 1, iterations - 1 }) {
        state = { };
        CHECK_EQ(run(*code, iterations, memory, rareIteration), iterations * 7 + rareSummand);
        CHECK_EQ(state.numberOfPolls, iterations);
    }
    CHECK_EQ(run(*code, iterations, memory, longRun), iterations * 7 + rareSummand);
    CHECK_EQ(run(*code, iterations, memory, iterations), iterations * 7 + rareSummand);
    CHECK_EQ(run(*code, iterations, memory, 0), iterations * 7 + rareSummand);

    memory[1] = 1500;
    for (int64_t rareIteration : { 0, 1498, 1499, 1500, 1501 })
        CHECK_EQ(run(*code, 2000, memory, rareIteration), -1);
}

enum class LoopTestPosition : uint8_t { Top, Bottom };

static void buildLoopWithBoundReplacedOnRarePath(Graph& graph, PollState& state, LoopTestPosition position)
{
    Procedure& proc = graph.proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* rare = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    BasicBlock* body = position == LoopTestPosition::Top ? proc.addBlock() : join;
    BasicBlock* unexpected = proc.addBlock();
    BasicBlock* checked = proc.addBlock();
    BasicBlock* outOfBounds = proc.addBlock();
    BasicBlock* hot = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* index = proc.addVariable(Int64);
    Variable* sum = proc.addVariable(Int64);
    Variable* bound = proc.addVariable(Int64);
    auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root).eager();
    graph.set(root, index, graph.int64(root, 0));
    graph.set(root, sum, graph.int64(root, 0));
    graph.jump(root, header);
    graph.set(header, bound, arguments[0]);
    graph.branch(header, graph.make(header, Equal, graph.get(header, index), arguments[2]), rarely(rare), FrequentedBlock(join));
    graph.increase(rare, sum, rareSummand);
    graph.set(rare, bound, graph.int64(rare, 0));
    graph.jump(rare, join);
    Value* continues = nullptr;
    if (position == LoopTestPosition::Top) {
        continues = graph.make(join, LessThan, graph.get(join, index), graph.get(join, bound));
        graph.branch(join, continues, body, exit);
    }
    appendPoll(graph, body, state);
    Value* loaded = graph.load(body, Int64, arguments[1], 0, firstHeap);
    graph.branch(body, graph.make(body, Below, loaded, graph.int64(body, 100)), FrequentedBlock(checked), rarely(unexpected));
    graph.callLikePatchpoint(unexpected);
    graph.jump(unexpected, checked);
    Value* length = graph.make(checked, ZExt32, graph.load(checked, Int32, arguments[1], 8, secondHeap));
    graph.branch(checked, graph.make(checked, Below, graph.get(checked, index), length), FrequentedBlock(hot), rarely(outOfBounds));
    graph.ret(outOfBounds, graph.int64(outOfBounds, -1));
    graph.increase(hot, sum, loaded);
    Value* nextIndex = increaseCounter(graph, hot, index, graph.int64(hot, 1));
    if (position == LoopTestPosition::Top)
        graph.jump(hot, header);
    else {
        continues = graph.make(hot, LessThan, nextIndex, graph.get(hot, bound));
        graph.branch(hot, continues, header, exit);
    }
    graph.ret(exit, graph.get(exit, sum));
    proc.resetReachability();
    proc.setKnownInduction(DemotedInduction { index, header, nextIndex, continues, 1, false });
    fixSSA(proc);
    validate(proc);
}

static void testVersionLoopsRarePathWithOtherBoundLeavesFastLoop()
{
    constexpr int64_t iterations = 3 * longRun + 100;
    for (LoopTestPosition position : { LoopTestPosition::Top, LoopTestPosition::Bottom }) {
        Procedure proc(loopPolicy());
        Graph graph { proc };
        PollState state;
        buildLoopWithBoundReplacedOnRarePath(graph, state, position);

        CHECK(versionLoopsAndValidate(proc));
        CHECK_EQ(proc.report().countOf(Counter::LoopBoundsClamped), 1u);
        CHECK_EQ(numberOfRarePathCopies(proc), 1u);
        auto code = compileProc(proc);
        int64_t memory[2] = { 7, iterations };

        CHECK_EQ(run(*code, iterations, memory, -1), iterations * 7);
        CHECK_EQ(state.numberOfPolls, iterations);
        int64_t bodyRunsOnRareIteration = position == LoopTestPosition::Top ? 0 : 1;
        for (int64_t rareIteration : { static_cast<int64_t>(1), rareIndex, longRun - 1, longRun, longRun + 1 })
            CHECK_EQ(run(*code, iterations, memory, rareIteration), (rareIteration + bodyRunsOnRareIteration) * 7 + rareSummand);
    }
}

static void buildTopTestedLoopWithConditionReplacedOnRarePath(Graph& graph, PollState& state)
{
    Procedure& proc = graph.proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* rare = proc.addBlock();
    BasicBlock* test = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* unexpected = proc.addBlock();
    BasicBlock* checked = proc.addBlock();
    BasicBlock* outOfBounds = proc.addBlock();
    BasicBlock* hot = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* index = proc.addVariable(Int64);
    Variable* sum = proc.addVariable(Int64);
    Variable* condition = proc.addVariable(Int32);
    auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root).eager();
    Value* pointer = arguments[1];
    // With a load on each side of them, the loads in the loop are hoisted without peeling the loop test, whose condition is a Phi.
    graph.load(root, Int64, pointer, 0, firstHeap);
    graph.load(root, Int64, pointer, 16, firstHeap);
    graph.load(root, Int64, pointer, 32, firstHeap);
    graph.set(root, index, graph.int64(root, 0));
    graph.set(root, sum, graph.int64(root, 0));
    graph.jump(root, header);
    Value* continues = graph.make(header, LessThan, graph.get(header, index), arguments[0]);
    graph.set(header, condition, continues);
    graph.branch(header, graph.make(header, Equal, graph.get(header, index), arguments[2]), rarely(rare), FrequentedBlock(test));
    graph.increase(rare, sum, rareSummand);
    graph.set(rare, condition, graph.int32(rare, 0));
    graph.jump(rare, test);
    graph.branch(test, graph.get(test, condition), body, exit);
    appendPoll(graph, body, state);
    Value* loaded = graph.load(body, Int64, pointer, 8, firstHeap);
    graph.branch(body, graph.make(body, Below, loaded, graph.int64(body, 100)), FrequentedBlock(checked), rarely(unexpected));
    graph.callLikePatchpoint(unexpected);
    graph.jump(unexpected, checked);
    Value* length = graph.make(checked, ZExt32, graph.load(checked, Int32, pointer, 24, secondHeap));
    graph.branch(checked, graph.make(checked, Below, graph.get(checked, index), length), FrequentedBlock(hot), rarely(outOfBounds));
    graph.ret(outOfBounds, graph.int64(outOfBounds, -1));
    graph.increase(hot, sum, loaded);
    Value* nextIndex = increaseCounter(graph, hot, index, graph.int64(hot, 1));
    graph.jump(hot, header);
    graph.ret(exit, graph.get(exit, sum));
    proc.resetReachability();
    proc.setKnownInduction(DemotedInduction { index, header, nextIndex, continues, 1, false });
    fixSSA(proc);
    validate(proc);
}

static void testVersionLoopsRarePathWithOtherConditionLeavesFastLoop()
{
    constexpr int64_t iterations = 3 * longRun + 100;
    Procedure proc(loopPolicy());
    Graph graph { proc };
    PollState state;
    buildTopTestedLoopWithConditionReplacedOnRarePath(graph, state);

    CHECK(versionLoopsAndValidate(proc));
    CHECK_EQ(proc.report().countOf(Counter::LoopBoundsClamped), 1u);
    CHECK_EQ(numberOfRarePathCopies(proc), 1u);
    auto code = compileProc(proc);
    int64_t memory[5] = { 0, 7, 0, iterations, 0 };

    CHECK_EQ(run(*code, iterations, memory, -1), iterations * 7);
    CHECK_EQ(state.numberOfPolls, iterations);
    for (int64_t rareIteration : { static_cast<int64_t>(1), rareIndex, longRun - 1, longRun, longRun + 1 })
        CHECK_EQ(run(*code, iterations, memory, rareIteration), rareIteration * 7 + rareSummand);
}

static void testVersionLoopsDoesNotCountLoopWithoutRecord()
{
    Procedure proc(loopPolicy());
    Graph graph { proc };
    PollState state;
    buildTopTestedLoopWithRarePath(graph, state, true);

    CHECK(versionLoopsAndValidate(proc));
    CHECK(hasRemark(proc, RemarkKind::LoopBoundNotClamped, GaveUpReason::NotCounted));
    CHECK_EQ(proc.report().countOf(Counter::KnownInductionsUsed), 0u);
    CHECK_EQ(proc.report().countOf(Counter::RangeChecksRemoved), 0u);
    CHECK_EQ(numberOfRarePathCopies(proc), 2u);
    checkValueCountInEachLoop(proc, isLoopPoll, "polls"_s, { 1, 1 });
    checkValueCountInEachLoop(proc, Below, { 1, 2 });
    auto code = compileProc(proc);
    constexpr int64_t bound = 2000;
    constexpr int64_t restarts = 200;
    constexpr int64_t iterations = restarts * (rareIndex + 1) + bound;
    int64_t memory[2] = { 7, 3000 };
    CHECK_EQ(run(*code, bound, memory, restarts), iterations * 7 + (restarts + 1) * rareSummand);
    CHECK_EQ(state.numberOfPolls, iterations);
    CHECK_EQ(run(*code, bound, memory, 0), bound * 7 + rareSummand);
    memory[1] = 1500;
    CHECK_EQ(run(*code, bound, memory, 0), -1);
}

static void testVersionLoopsKeepsPollWithRedundantLoopTest()
{
    Procedure proc(loopPolicy());
    Graph graph { proc };
    PollState state;
    BasicBlock* root = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* rare = proc.addBlock();
    BasicBlock* hot = proc.addBlock();
    BasicBlock* test = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* index = proc.addVariable(Int64);
    Variable* sum = proc.addVariable(Int64);
    auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root).eager();
    graph.set(root, index, graph.int64(root, 0));
    graph.set(root, sum, graph.int64(root, 0));
    graph.jump(root, body);
    appendPoll(graph, body, state);
    Value* loaded = graph.load(body, Int64, arguments[1], 0, firstHeap);
    graph.branch(body, graph.make(body, Equal, loaded, arguments[2]), FrequentedBlock(hot), rarely(rare));
    graph.increase(rare, sum, 1000);
    graph.jump(rare, hot);
    graph.increase(hot, sum, loaded);
    Value* nextIndex = increaseCounter(graph, hot, index, graph.int64(hot, 1));
    Value* continues = graph.make(hot, LessThan, graph.get(hot, index), arguments[0]);
    graph.branch(hot, continues, FrequentedBlock(test), rarely(exit));
    graph.branch(test, continues, body, exit);
    graph.ret(exit, graph.get(exit, sum));
    proc.resetReachability();
    proc.setKnownInduction(DemotedInduction { index, body, nextIndex, continues, 1, false });
    fixSSA(proc);
    validate(proc);

    CHECK(versionLoopsAndValidate(proc));
    checkValueCountInEachLoop(proc, Patchpoint, { 1, 1 });
    auto code = compileProc(proc);
    int64_t memory[1] = { 7 };
    CHECK_EQ(run(*code, 100, memory, 7), 700);
    CHECK_EQ(state.numberOfPolls, 100);
    CHECK_EQ(run(*code, 100, memory, 8), 100 * 1007);
    CHECK_EQ(run(*code, 0, memory, 7), 7);
}

namespace {

enum class Induction : uint8_t { Published, PublishedNoWrap, NotPublished, SkipsAhead };
enum class CounterProof : uint8_t { None, CannotWrap, EntryGuard };

struct LoopCounter {
    Type type;
    B3::Opcode relation;
    Induction induction;
};

} // anonymous namespace

static int64_t sumWithRangeChecks(const LoopCounter& counter, int64_t bound, const int32_t* limits)
{
    int64_t index = 0;
    int64_t sum = 0;
    if (!holds(counter.relation, index, bound))
        return sum;
    while (true) {
        if (index >= limits[0] || index + 1 >= limits[1])
            return -1;
        if (counter.induction == Induction::SkipsAhead && index == 40) {
            index += 2;
            continue;
        }
        ++sum;
        ++index;
        if (!holds(counter.relation, index, bound))
            return sum;
    }
}

static void buildLoopWithRangeChecks(Graph& graph, PollState& state, const LoopCounter& counter)
{
    Procedure& proc = graph.proc;
    BasicBlock* root = proc.addBlock();
    BasicBlock* preHeader = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* bottom = proc.addBlock();
    BasicBlock* outOfBounds = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* index = proc.addVariable(counter.type);
    Variable* sum = proc.addVariable(Int64);
    auto arguments = cCallArgumentValues<int64_t, int32_t*>(proc, root);
    Value* argument = arguments[0];
    Value* limits = arguments[1];
    bool isInt32 = counter.type == Int32;
    Value* bound = isInt32 ? graph.make(root, Trunc, argument) : argument;
    auto constant = [&](BasicBlock* block, int32_t value) {
        return isInt32 ? graph.int32(block, value) : graph.int64(block, value);
    };

    graph.set(root, index, constant(root, 0));
    graph.set(root, sum, graph.int64(root, 0));
    graph.branch(root, graph.make(root, counter.relation, graph.get(root, index), bound), preHeader, exit);
    graph.jump(preHeader, body);
    graph.ret(outOfBounds, graph.int64(outOfBounds, -1));
    graph.ret(exit, graph.get(exit, sum));
    graph.increase(bottom, sum, 1);
    Value* nextIndex = increaseCounter(graph, bottom, index, constant(bottom, 1));
    Value* continues = graph.make(bottom, counter.relation, graph.get(bottom, index), bound);
    graph.branch(bottom, continues, body, exit);

    appendPoll(graph, body, state);
    BasicBlock* block = body;
    for (unsigned k : { 0u, 1u }) {
        MemoryValue* word = graph.load(block, Int32, limits, static_cast<int32_t>(4 * k), HeapRange(40 + k, 41 + k));
        word->setControlDependent(false);
        Value* limit = isInt32 ? word : graph.make(block, ZExt32, word);
        Value* checked = graph.get(block, index);
        if (k)
            checked = graph.make(block, Add, checked, constant(block, 1));
        BasicBlock* next = proc.addBlock();
        graph.branch(block, graph.make(block, AboveEqual, checked, limit), rarely(outOfBounds), FrequentedBlock(next));
        block = next;
    }
    if (counter.induction == Induction::SkipsAhead) {
        BasicBlock* rare = proc.addBlock();
        graph.branch(block, graph.make(block, Equal, graph.get(block, index), constant(block, 40)), rarely(rare), FrequentedBlock(bottom));
        graph.increase(rare, index, constant(rare, 2));
        graph.jump(rare, body);
    } else
        graph.jump(block, bottom);
    proc.resetReachability();
    switch (counter.induction) {
    case Induction::Published:
    case Induction::PublishedNoWrap:
        proc.setKnownInduction(DemotedInduction { index, body, nextIndex, continues, 1, counter.induction == Induction::PublishedNoWrap });
        break;
    case Induction::NotPublished:
    case Induction::SkipsAhead:
        break;
    }
    fixSSA(proc);
    validate(proc);
}

static void checkRangeChecks(const LoopCounter& counter, CounterProof proof)
{
    Procedure proc(loopPolicy());
    Graph graph { proc };
    PollState state;
    buildLoopWithRangeChecks(graph, state, counter);

    CHECK(versionLoopsAndValidate(proc));
    bool isProven = proof != CounterProof::None;
    unsigned checksRemoved = isProven ? 2 : 0;
    Vector<unsigned> checksInEachLoop = valueCountInEachLoop(proc, AboveEqual);
    CHECK_EQ(checksInEachLoop.first(), 2 - checksRemoved);
    CHECK_EQ(checksInEachLoop.last(), 2u);
    CHECK_EQ(proc.report().countOf(Counter::RangeChecksRemoved), checksRemoved);
    CHECK_EQ(hasRemark(proc, RemarkKind::RangeChecksRemoved, GaveUpReason::None), isProven);
    CHECK_EQ(hasRemark(proc, RemarkKind::LoopBoundNotClamped, GaveUpReason::NotCounted), !isProven);
    CHECK_EQ(hasInt64Constant(proc, std::numeric_limits<int64_t>::max() - 1), proof == CounterProof::EntryGuard);

    auto code = compileProc(proc);
    struct LimitWords {
        int32_t first;
        int32_t second;
    };
    for (int64_t testBound : { 0, 10, 60 }) {
        for (LimitWords testLimits : { LimitWords { 100, 100 }, LimitWords { 10, 10 }, LimitWords { 11, 11 }, LimitWords { 100, 5 } }) {
            int32_t words[2] = { testLimits.first, testLimits.second };
            CHECK_EQ(invoke<int64_t>(*code, testBound, words), sumWithRangeChecks(counter, testBound, words));
        }
    }
}

static void testVersionLoopsRemovesRangeChecksOnlyFromProvenCounter()
{
    checkRangeChecks({ Int64, LessThan, Induction::Published }, CounterProof::CannotWrap);
    checkRangeChecks({ Int32, LessThan, Induction::Published }, CounterProof::CannotWrap);
    checkRangeChecks({ Int64, LessEqual, Induction::PublishedNoWrap }, CounterProof::CannotWrap);
    checkRangeChecks({ Int64, LessEqual, Induction::Published }, CounterProof::EntryGuard);
    checkRangeChecks({ Int64, LessThan, Induction::NotPublished }, CounterProof::None);
    checkRangeChecks({ Int64, LessThan, Induction::SkipsAhead }, CounterProof::None);
}

void addBunCountedLoopVersioningTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testVersionLoopsCountsLoopAfterEarlierLoop());
    RUN(testVersionLoopsCountsLoopWithBoundLoadedBeforeAndAtBottom());
    RUN(testVersionLoopsTopTestedExcursionStillPolls());
    RUN(testVersionLoopsTopTestedExcursionReturnsToFastLoop());
    RUN(testVersionLoopsExcursionBeforeTopTestReturnsToFastLoop());
    RUN(testLoopPhasesKeepFastLoopAfterRarePathBeforeTopTest());
    RUN(testVersionLoopsRarePathWithOtherBoundLeavesFastLoop());
    RUN(testVersionLoopsRarePathWithOtherConditionLeavesFastLoop());
    RUN(testVersionLoopsDoesNotCountLoopWithoutRecord());
    RUN(testVersionLoopsKeepsPollWithRedundantLoopTest());
    RUN(testVersionLoopsRemovesRangeChecksOnlyFromProvenCounter());
}

#endif // ENABLE(B3_JIT)
