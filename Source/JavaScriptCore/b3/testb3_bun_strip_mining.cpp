/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "B3CountedLoops.h"
#include "B3IntRange.h"
#include "B3PhiChildren.h"
#include "B3StripMineLoops.h"

static void buildRotatedLoopWithPoll(Graph& graph, TopTestedLoop& loop, PollState& state)
{
    appendPoll(graph, loop.body, state);
    BasicBlock* block = loop.body;
    graph.increase(block, loop.sum, graph.get(block, loop.index));
    loop.endBody(block);
    loop.rotate();
}

static void testStripMineLoopsPollsOncePerStrip()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    PollState state;
    TopTestedLoop loop(graph);
    buildRotatedLoopWithPoll(graph, loop, state);

    CHECK(stripMineLoopsAndValidate(proc));
    CHECK(hasRemark(proc, RemarkKind::PollStripMined, GaveUpReason::None));
    CHECK_EQ(proc.report().countOf(Counter::PollsStripMined), 1u);
    CHECK(!stripMineLoopsAndValidate(proc));
    CHECK(!hasRemark(proc, RemarkKind::PollNotStripMined));
    CHECK_EQ(proc.report().countOf(Counter::PollsStripMined), 1u);
    auto code = compileProc(proc);
    struct Case {
        int64_t bound;
        int64_t numberOfPolls;
    };
    for (Case testCase : { Case { -1, 0 }, Case { 0, 0 }, Case { 1, 0 }, Case { 100, 0 }, Case { maxIterationsPerStrip, 0 }, Case { maxIterationsPerStrip + 1, 1 }, Case { 3 * maxIterationsPerStrip + 100, 3 } }) {
        state = { };
        CHECK_EQ(run(*code, testCase.bound), sumBelow(testCase.bound));
        CHECK_EQ(state.numberOfPolls, testCase.numberOfPolls);
        CHECK_EQ(state.numberOfStops, 0);
    }

    state = { };
    state.limit = -1;
    CHECK_EQ(run(*code, 3 * maxIterationsPerStrip + 100), sumBelow(3 * maxIterationsPerStrip + 100));
    CHECK_EQ(state.numberOfPolls, 3);
    CHECK_EQ(state.numberOfStops, 1);
}

static void checkFirstStripEnd(B3::Opcode relation, bool boundIsZeroExtended, int64_t start, unsigned numberOfSelects)
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    Value* argument = cCallArgumentValues<int64_t>(proc, root)[0];
    Value* bound = boundIsZeroExtended ? graph.make(root, ZExt32, graph.make(root, Trunc, argument)) : argument;
    CountedLoop loop;
    loop.relation = relation;
    loop.step = 1;
    graph.ret(root, appendStripEnd(proc, root, loop, graph.int64(root, start), bound, true));
    CHECK_EQ(numberOfValues(proc, Select), numberOfSelects);
    auto code = compileProc(proc);
    for (int64_t limit : { static_cast<int64_t>(0), static_cast<int64_t>(1), static_cast<int64_t>(5), maxIterationsPerStrip - 1, maxIterationsPerStrip, maxIterationsPerStrip + 1, 20 * maxIterationsPerStrip }) {
        int64_t end = invoke<int64_t>(*code, limit);
        if (holds(relation, start, limit))
            CHECK_EQ(end, std::min(limit, start + maxIterationsPerStrip));
        else
            CHECK(!holds(relation, start + 1, end));
    }
}

static void testFirstStripEndFromZeroToZeroExtendedBound()
{
    checkFirstStripEnd(LessThan, true, 0, 1);
    checkFirstStripEnd(Below, true, 0, 1);
    checkFirstStripEnd(LessThan, true, -3, 1);
    checkFirstStripEnd(LessThan, true, 1, 2);
    checkFirstStripEnd(Below, true, 1, 2);
    checkFirstStripEnd(LessThan, false, 0, 2);
}

static void testStripMineLoopsAllRelationsAndSteps()
{
    struct Case {
        B3::Opcode relation;
        int64_t firstIndex;
        int64_t step;
        int64_t bound;
    };
    constexpr int64_t many = 2 * maxIterationsPerStrip + 10;
    for (Case testCase : { Case { LessThan, -many, 1, 5 }, Case { LessEqual, 0, 3, many }, Case { LessEqual, 0, 1, many }, Case { GreaterThan, 5, -1, -many }, Case { GreaterEqual, many, -7, -many },
        Case { Below, 0, 1, many }, Case { BelowEqual, 1, 2, many }, Case { Above, many, -1, 5 }, Case { AboveEqual, many, -3, 5 },
        Case { LessThan, 10, 1, 5 }, Case { GreaterThan, 0, -1, 5 }, Case { Below, 10, 1, 5 }, Case { Above, 1, -1, 5 },
        Case { LessThan, std::numeric_limits<int64_t>::max() - many, 1, std::numeric_limits<int64_t>::max() }, Case { GreaterThan, std::numeric_limits<int64_t>::min() + many, -1, std::numeric_limits<int64_t>::min() } }) {
        Procedure proc(remarkPolicy());
        Graph graph { proc };
        PollState state;
        TopTestedLoop loop(graph, testCase.relation, testCase.step, testCase.firstIndex);
        buildRotatedLoopWithPoll(graph, loop, state);
        proc.setKnownRange(loop.bound, IntRange(testCase.bound, testCase.bound));

        CHECK(stripMineLoopsAndValidate(proc));
        uint64_t expected = 0;
        int64_t iterations = 0;
        for (int64_t index = testCase.firstIndex; holds(testCase.relation, index, testCase.bound); index += testCase.step) {
            expected += static_cast<uint64_t>(index);
            ++iterations;
        }
        CHECK_EQ(static_cast<uint64_t>(run(*compileProc(proc), testCase.bound)), expected);
        CHECK(state.numberOfPolls <= iterations / 1000);
        CHECK(state.numberOfPolls >= iterations * std::abs(testCase.step) / maxIterationsPerStrip - 1);
    }
}

static void testStripMineLoopsNeedsPoll()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    PollState state;
    TopTestedLoop loop(graph);
    appendPoll(graph, loop.body, state)->isLoopPoll = false;
    graph.increase(loop.body, loop.sum, graph.get(loop.body, loop.index));
    loop.endBody(loop.body);
    loop.rotate();

    CHECK(!stripMineLoopsAndValidate(proc));
    CHECK(!hasRemark(proc, RemarkKind::PollNotStripMined));
    CHECK_EQ(run(*compileProc(proc), 10), 45);
    CHECK_EQ(state.numberOfPolls, 10);
}

static void testStripMineLoopsNeedsRecord()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    PollState state;
    TopTestedLoop loop(graph);
    appendPoll(graph, loop.body, state);
    graph.increase(loop.body, loop.sum, graph.get(loop.body, loop.index));
    loop.endBody(loop.body);
    proc.knownInductions().remove(loop.counter());
    loop.rotate();

    CHECK(!stripMineLoopsAndValidate(proc));
    CHECK(hasRemark(proc, RemarkKind::PollNotStripMined, GaveUpReason::NotCounted));
    CHECK_EQ(proc.report().countOf(Counter::KnownInductionsUsed), 0u);
    CHECK_EQ(run(*compileProc(proc), 10), 45);
    CHECK_EQ(state.numberOfPolls, 10);
}

static void testStripMineLoopsSkipsLoopWithTwoPolls()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    PollState state;
    TopTestedLoop loop(graph);
    appendPoll(graph, loop.body, state);
    appendPoll(graph, loop.body, state);
    BasicBlock* block = loop.body;
    graph.increase(block, loop.sum, graph.get(block, loop.index));
    loop.endBody(block);
    loop.rotate();

    CHECK(!stripMineLoopsAndValidate(proc));
    CHECK(hasRemark(proc, RemarkKind::PollNotStripMined, GaveUpReason::SeveralPolls));
    CHECK_EQ(run(*compileProc(proc), 10), 45);
    CHECK_EQ(state.numberOfPolls, 20);
}

static void testStripMineLoopsSkipsVariantBound()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    PollState state;
    BasicBlock* root = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* index = proc.addVariable(Int64);
    Value* pointer = cCallArgumentValues<int64_t*>(proc, root)[0];
    graph.set(root, index, graph.int64(root, 0));
    graph.jump(root, header);
    Value* test = graph.make(header, LessThan, graph.get(header, index), graph.load(header, Int64, pointer, 0, firstHeap));
    graph.branch(header, test, body, exit);
    appendPoll(graph, body, state);
    Value* next = graph.make(body, Add, graph.get(body, index), graph.int64(body, 1));
    graph.set(body, index, next);
    graph.jump(body, header);
    graph.ret(exit, graph.get(exit, index));
    proc.resetReachability();
    proc.setKnownInduction(DemotedInduction { index, header, next, test, 1, false });
    fixSSA(proc);
    CHECK(rotateLoops(proc));
    fixSSA(proc);
    validate(proc);

    CHECK(!stripMineLoopsAndValidate(proc));
    CHECK(hasRemark(proc, RemarkKind::PollNotStripMined, GaveUpReason::BoundNotInvariant));
    int64_t memory[1] = { 10 };
    CHECK_EQ(invoke<int64_t>(*compileProc(proc), memory), 10);
    CHECK_EQ(state.numberOfPolls, 10);
}

static void testStripMineLoopsSkipsPollOnOnePath()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    PollState state;
    TopTestedLoop loop(graph);
    BasicBlock* polling = proc.addBlock();
    BasicBlock* next = proc.addBlock();
    graph.branch(loop.body, graph.make(loop.body, BitAnd, graph.get(loop.body, loop.index), graph.int64(loop.body, 1)), polling, next);
    appendPoll(graph, polling, state);
    graph.jump(polling, next);
    graph.increase(next, loop.sum, graph.get(next, loop.index));
    loop.endBody(next);
    loop.rotate();

    CHECK(!stripMineLoopsAndValidate(proc));
    CHECK(hasRemark(proc, RemarkKind::PollNotStripMined, GaveUpReason::PollNotOnEveryIteration));
    CHECK_EQ(run(*compileProc(proc), 10), 45);
    CHECK_EQ(state.numberOfPolls, 5);
}

namespace {

struct BottomTestedLoop {
    BottomTestedLoop(Graph& graph, PollState& state, int64_t maxLimit)
    {
        Procedure& proc = graph.proc;
        BasicBlock* root = proc.addBlock();
        BasicBlock* preHeader = proc.addBlock();
        body = proc.addBlock();
        BasicBlock* exit = proc.addBlock();
        Variable* index = proc.addVariable(Int64);
        Variable* sum = proc.addVariable(Int64);
        bound = cCallArgumentValues<int64_t>(proc, root)[0];
        Value* start = graph.int64(root, 0);
        Value* farthestLimit = graph.int64(root, maxLimit);
        limit = graph.select(root, graph.make(root, LessThan, bound, farthestLimit), bound, farthestLimit);
        graph.set(root, index, start);
        graph.set(root, sum, start);
        graph.branch(root, graph.make(root, LessThan, start, bound), preHeader, exit);
        graph.jump(preHeader, body);
        appendPoll(graph, body, state);
        nextSum = graph.make(body, Add, graph.get(body, sum), graph.get(body, index));
        graph.set(body, sum, nextSum);
        Value* next = graph.make(body, Add, graph.get(body, index), graph.int64(body, 1));
        graph.set(body, index, next);
        Value* test = graph.make(body, LessThan, next, bound);
        graph.branch(body, test, body, exit);
        graph.ret(exit, graph.get(exit, sum));
        proc.resetReachability();
        proc.setKnownInduction(DemotedInduction { index, body, next, test, 1, false });
        fixSSA(proc);
        validate(proc);
    }

    BasicBlock* body;
    Value* bound;
    Value* limit;
    Value* nextSum;
};

} // anonymous namespace

static void testClampLoopBound()
{
    constexpr int64_t maxLimit = 100;
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    PollState state;
    BottomTestedLoop loop(graph, state, maxLimit);

    B3::NaturalLoops& loops = proc.naturalLoops();
    CHECK_EQ(loops.numLoops(), 1u);
    proc.resetValueOwners();
    PhiChildren phiChildren(proc);
    auto counted = analyzeCountedLoop({ proc, loops, loops.loop(0), phiChildren, [&](Value* value) {
        return !loops.belongsTo(value->owner, loops.loop(0));
    }, std::nullopt });
    CHECK(counted);
    CHECK(!counted->isTopTested);
    CHECK(counted->test == loop.body);
    CHECK(counted->bound == loop.bound);

    BasicBlock* beyondLimit = proc.addBlock();
    graph.ret(beyondLimit, graph.make(beyondLimit, Neg, loop.nextSum));
    StripTargets targets;
    targets.limit = loop.limit;
    targets.beyondLimit = beyondLimit;
    clampLoopBound(proc, *counted, targets);
    proc.resetReachability();
    proc.invalidateCFG();
    fixSSA(proc);
    validate(proc);

    CHECK_EQ(numberOfValues(proc, Patchpoint), 1u);
    CHECK_EQ(numberOfValues(loop.body, Patchpoint), 1u);
    CHECK(!hasRemark(proc, RemarkKind::PollStripMined));
    CHECK_EQ(valueCountInLoops(proc, Branch), 1u);
    CHECK(loop.body->last()->child(0)->child(1) == loop.limit);
    CHECK_EQ(beyondLimit->numPredecessors(), 1u);
    BasicBlock* boundTest = beyondLimit->predecessor(0);
    CHECK(boundTest->last()->child(0)->child(1) == loop.bound);
    CHECK(boundTest->taken().block() == beyondLimit);
    CHECK(boundTest->taken().isRare());
    CHECK(!boundTest->notTaken().isRare());

    auto code = compileProc(proc);
    for (int64_t bound : { -1, 0, 1, 2, 99, 100, 101, 5000 }) {
        state = { };
        CHECK_EQ(invoke<int64_t>(*code, bound), bound > maxLimit ? -sumBelow(maxLimit) : sumBelow(bound));
        CHECK_EQ(state.numberOfPolls, std::clamp<int64_t>(bound, 0, maxLimit));
    }
}

static void testLoopPhasesArraySum()
{
    Vector<unsigned> numberOfLoads;
    auto inspect = [&](Procedure& optimized) {
        numberOfLoads = valueCountInEachLoop(optimized, Load);
    };
    Procedure proc(policyWithInspection(optimizingPolicy(), inspect));
    Graph graph { proc };
    PollState state;
    TopTestedLoop loop(graph);
    BasicBlock* rare = proc.addBlock();
    BasicBlock* block = proc.addBlock();
    BasicBlock* outOfBounds = proc.addBlock();
    BasicBlock* inBounds = proc.addBlock();
    appendPoll(graph, loop.body, state);
    graph.branch(loop.body, loop.extra, rarely(rare), FrequentedBlock(block));
    graph.emptyPatchpoint(rare)->effects.writes = firstHeap;
    graph.jump(rare, block);
    Value* length = graph.make(block, ZExt32, graph.load(block, Int32, loop.pointer, 0, firstHeap));
    graph.branch(block, graph.make(block, AboveEqual, graph.get(block, loop.index), length), rarely(outOfBounds), FrequentedBlock(inBounds));
    graph.ret(outOfBounds, graph.int64(outOfBounds, -1));
    Value* address = graph.make(inBounds, Add, loop.pointer, graph.make(inBounds, Shl, graph.get(inBounds, loop.index), graph.int32(inBounds, 3)));
    graph.increase(inBounds, loop.sum, graph.load(inBounds, Int64, address, 8, secondHeap));
    loop.endBody(inBounds);

    auto code = compileProc(proc, 2);
    CHECK_EQ(proc.report().countOf(Counter::LoopsVersioned), 1u);
    CHECK_EQ(proc.report().countOf(Counter::RangeChecksRemoved), 1u);
    CHECK(numberOfLoads.size() >= 2);
    CHECK_EQ(numberOfLoads.first(), 1u);
    CHECK_EQ(numberOfLoads.last(), 2u);

    constexpr int64_t size = 3 * maxIterationsPerStrip + 100;
    Vector<int64_t> memory(size + 1);
    memory[0] = size;
    int64_t expected = 0;
    for (int64_t i = 0; i < size; ++i) {
        memory[i + 1] = i * 7 - 3;
        expected += memory[i + 1];
    }
    CHECK_EQ(run(*code, size, memory.mutableSpan().data()), expected);
    CHECK_EQ(state.numberOfPolls, 3);
    CHECK_EQ(run(*code, 0, memory.mutableSpan().data()), 0);
    CHECK_EQ(run(*code, 2, memory.mutableSpan().data()), -3 + 4);
    CHECK_EQ(run(*code, 2, memory.mutableSpan().data(), 1), -3 + 4);
    CHECK_EQ(run(*code, size + 1, memory.mutableSpan().data()), -1);

    state = { };
    state.limit = -1;
    CHECK_EQ(run(*code, size, memory.mutableSpan().data()), expected);
    CHECK_EQ(state.numberOfStops, 1);
}

static void testLoopPhasesFoldTestsImpliedByLoopCondition()
{
    unsigned numberOfBranchesInLoops = 0;
    bool hasDeadTargets = true;
    auto inspect = [&](Procedure& optimized) {
        numberOfBranchesInLoops = valueCountInLoops(optimized, Branch);
        hasDeadTargets = hasInt64Constant(optimized, 1000) || hasInt64Constant(optimized, 2000);
    };
    Procedure proc(policyWithInspection(optimizingPolicy(), inspect));
    Graph graph { proc };
    TopTestedLoop loop(graph);
    BasicBlock* block = loop.body;
    addIfComparisonHolds(graph, block, loop.sum, graph.make(block, LessThan, graph.get(block, loop.index), loop.bound), 1, 1000);
    addIfComparisonHolds(graph, block, loop.sum, graph.make(block, GreaterEqual, graph.get(block, loop.index), loop.bound), 2000, 10);
    loop.endBody(block);

    auto code = compileProc(proc, 2);
    CHECK_EQ(numberOfBranchesInLoops, 1u);
    CHECK(!hasDeadTargets);
    for (int64_t bound : { -1, 0, 1, 7 })
        CHECK_EQ(run(*code, bound), std::max<int64_t>(bound, 0) * 11);
}

static bool hasEnabledLoopStep(Procedure& proc)
{
    Vector<ASCIILiteral> enabledSteps = listEnabledSteps(proc);
    for (ASCIILiteral loopStep : { "findLoops"_s, "simplifyTerminals"_s, "eliminateRedundantPhiCycles"_s, "rotateLoops"_s, "cleanUpRotatedLoops"_s, "foldComparisonsWithBounds"_s, "versionLoops"_s, "stripMineLoops"_s }) {
        if (enabledSteps.contains(loopStep))
            return true;
    }
    return false;
}

static void testLoopPhasesAreOptIn()
{
    Procedure defaultProcedure;
    defaultProcedure.setOptLevel(2);
    CHECK(!hasEnabledLoopStep(defaultProcedure));
    for (bool LoopPolicy::* field : { &LoopPolicy::rotates, &LoopPolicy::foldsComparisonsWithBounds, &LoopPolicy::versions, &LoopPolicy::stripMinesPolls }) {
        Procedure optedIn(policyWith([&](Policy& policy) {
            policy.loops.*field = true;
        }));
        CHECK(hasEnabledLoopStep(optedIn));
    }

    unsigned numberOfBranches = 0;
    auto inspect = [&](Procedure& optimized) {
        CHECK_EQ(optimized.naturalLoops().numLoops(), 1u);
        numberOfBranches = numberOfValues(optimized, Branch);
    };
    Procedure proc(policyWithInspection(Policy(), inspect));
    Graph graph { proc };
    TopTestedLoop loop(graph);
    graph.increase(loop.body, loop.sum, graph.get(loop.body, loop.index));
    loop.endBody(loop.body);

    auto code = compileProc(proc, 2);
    CHECK_EQ(numberOfBranches, 1u);
    CHECK_EQ(run(*code, 10), 45);
}

void addBunStripMiningTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testStripMineLoopsPollsOncePerStrip());
    RUN(testFirstStripEndFromZeroToZeroExtendedBound());
    RUN(testStripMineLoopsAllRelationsAndSteps());
    RUN(testStripMineLoopsNeedsPoll());
    RUN(testStripMineLoopsNeedsRecord());
    RUN(testStripMineLoopsSkipsLoopWithTwoPolls());
    RUN(testStripMineLoopsSkipsVariantBound());
    RUN(testStripMineLoopsSkipsPollOnOnePath());
    RUN(testClampLoopBound());
    RUN(testLoopPhasesArraySum());
    RUN(testLoopPhasesFoldTestsImpliedByLoopCondition());
    RUN(testLoopPhasesAreOptIn());
}

#endif // ENABLE(B3_JIT)
