/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "B3FoldComparisonsWithBounds.h"
#include "B3IntRange.h"
#include "B3SimplifyTerminals.h"

static bool foldInCountedLoop(Procedure& proc)
{
    bool result = foldComparisonsWithBounds(proc);
    validate(proc);
    CHECK(proc.report().countOf(Counter::KnownInductionsUsed));
    return result;
}

static void closeAndRotateLoop(Graph& graph, BasicBlock* latch, BasicBlock* header, Variable* index, int64_t step)
{
    Procedure& proc = graph.proc;
    Value* next = graph.make(latch, Add, graph.get(latch, index), latch->appendIntConstant(proc, Origin(), index->type(), step));
    graph.set(latch, index, next);
    graph.jump(latch, header);
    proc.setKnownInduction(DemotedInduction { index, header, next, header->last()->child(0), step, false });
    proc.resetReachability();
    fixSSA(proc);
    validate(proc);
    CHECK(rotateLoops(proc));
    fixSSA(proc);
    validate(proc);
}

static void testFoldComparisonsWithBounds()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph);
    BasicBlock* block = loop.body;
    addIfComparisonHolds(graph, block, loop.sum, graph.make(block, LessThan, graph.get(block, loop.index), loop.bound), 1, 1000);
    addIfComparisonHolds(graph, block, loop.sum, graph.make(block, GreaterEqual, graph.get(block, loop.index), loop.bound), 2000, 10);
    addIfComparisonHolds(graph, block, loop.sum, graph.make(block, GreaterThan, loop.bound, graph.get(block, loop.index)), 100, 3000);
    loop.endBody(block);
    loop.rotate();

    CHECK(foldInCountedLoop(proc));
    CHECK_EQ(valueCountInLoops(proc, LessThan), 1u);
    CHECK_EQ(valueCountInLoops(proc, GreaterEqual), 0u);
    CHECK_EQ(valueCountInLoops(proc, GreaterThan), 0u);
    reduceStrengthOnce(proc);
    CHECK(!hasInt64Constant(proc, 1000));
    CHECK(!hasInt64Constant(proc, 2000));
    CHECK(!hasInt64Constant(proc, 3000));
    CHECK_EQ(valueCountInLoops(proc, Branch), 1u);
    auto code = compileProc(proc);
    for (int64_t bound : { -1, 0, 1, 7 })
        CHECK_EQ(run(*code, bound), std::max<int64_t>(bound, 0) * 111);
}

static void testFoldComparisonsWithBoundsKeepsUnrelatedComparisons()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph);
    BasicBlock* block = loop.body;
    addIfComparisonHolds(graph, block, loop.sum, graph.make(block, LessThan, graph.get(block, loop.index), loop.extra), 1, 1000);
    addIfComparisonHolds(graph, block, loop.sum, graph.make(block, LessEqual, graph.get(block, loop.index), loop.bound), 10, 2000);
    addIfComparisonHolds(graph, block, loop.sum, graph.make(block, LessThan, graph.make(block, Add, graph.get(block, loop.index), graph.int64(block, 1)), loop.bound), 100, 3000);
    addIfComparisonHolds(graph, block, loop.sum, graph.make(block, Below, graph.get(block, loop.index), loop.bound), 10000, 4000);
    loop.endBody(block);
    loop.rotate();

    CHECK(!foldInCountedLoop(proc));
    auto code = compileProc(proc);
    CHECK_EQ(run(*code, 3, nullptr, 2), (1 + 10 + 100 + 10000) + (1 + 10 + 100 + 10000) + (1000 + 10 + 3000 + 10000));
}

namespace {

struct CountdownLoopCase {
    int64_t startMinusLimit;
    B3::Opcode relation;
    int64_t bound;
    int64_t step;
    bool isFolded;
    bool limitIsWord { true };
    bool startIsDifference { false };
};

} // anonymous namespace

static void checkCountdownLoop(const CountdownLoopCase& testCase)
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* index = proc.addVariable(Int64);
    Variable* sum = proc.addVariable(Int64);
    Value* argument = cCallArgumentValues<int64_t>(proc, root)[0];
    Value* limit = testCase.limitIsWord ? graph.make(root, ZExt32, graph.make(root, Trunc, argument)) : argument;
    Value* boundValue = graph.int64(root, testCase.bound);
    graph.set(root, index, testCase.startIsDifference ? graph.make(root, Sub, limit, graph.int64(root, -testCase.startMinusLimit)) : graph.make(root, Add, limit, graph.int64(root, testCase.startMinusLimit)));
    graph.set(root, sum, graph.int64(root, 0));
    graph.jump(root, header);
    graph.branch(header, graph.make(header, testCase.relation, graph.get(header, index), boundValue), body, exit);
    graph.ret(exit, graph.get(exit, sum));
    BasicBlock* block = body;
    addIfComparisonHolds(graph, block, sum, graph.make(block, Below, graph.get(block, index), limit), 1, 1000);
    addIfComparisonHolds(graph, block, sum, graph.make(block, AboveEqual, graph.get(block, index), limit), 2000, 10);
    addIfComparisonHolds(graph, block, sum, graph.make(block, Above, limit, graph.get(block, index)), 100, 3000);
    addIfComparisonHolds(graph, block, sum, graph.make(block, GreaterEqual, graph.get(block, index), limit), 4000, 0);
    closeAndRotateLoop(graph, block, header, index, testCase.step);

    CHECK_EQ(foldInCountedLoop(proc), testCase.isFolded);
    CHECK_EQ(valueCountInLoops(proc, Below), testCase.isFolded ? 0u : 1u);
    CHECK_EQ(valueCountInLoops(proc, AboveEqual), testCase.isFolded ? 0u : 1u);
    CHECK_EQ(valueCountInLoops(proc, Above), testCase.isFolded ? 0u : 1u);
    auto code = compileProc(proc);
    for (int64_t argumentAtRunTime : { static_cast<int64_t>(-3), static_cast<int64_t>(0), static_cast<int64_t>(1), static_cast<int64_t>(2), static_cast<int64_t>(7), static_cast<int64_t>(100) }) {
        int64_t limitAtRunTime = testCase.limitIsWord ? static_cast<int64_t>(static_cast<uint32_t>(argumentAtRunTime)) : argumentAtRunTime;
        if (limitAtRunTime > 1000)
            continue;
        int64_t expected = 0;
        for (int64_t i = limitAtRunTime + testCase.startMinusLimit; holds(testCase.relation, i, testCase.bound); i += testCase.step) {
            expected += holds(Below, i, limitAtRunTime) ? 1 : 1000;
            expected += holds(AboveEqual, i, limitAtRunTime) ? 2000 : 10;
            expected += holds(Above, limitAtRunTime, i) ? 100 : 3000;
            expected += holds(GreaterEqual, i, limitAtRunTime) ? 4000 : 0;
        }
        CHECK_EQ(invoke<int64_t>(*code, argumentAtRunTime), expected);
    }
}

static void checkNeighborInLoop(bool countsUp, int64_t start, int64_t bound, int64_t added, bool isFolded)
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* index = proc.addVariable(Int64);
    Variable* sum = proc.addVariable(Int64);
    Value* limit = graph.make(root, ZExt32, graph.make(root, Trunc, cCallArgumentValues<int64_t>(proc, root)[0]));
    Value* boundValue = countsUp ? limit : graph.int64(root, bound);
    Value* addedValue = graph.int64(root, added);
    graph.set(root, index, countsUp ? graph.int64(root, start) : graph.make(root, Add, limit, graph.int64(root, start)));
    graph.set(root, sum, graph.int64(root, 0));
    graph.jump(root, header);
    graph.branch(header, graph.make(header, countsUp ? LessThan : GreaterEqual, graph.get(header, index), boundValue), body, exit);
    graph.ret(exit, graph.get(exit, sum));
    BasicBlock* block = body;
    addIfComparisonHolds(graph, block, sum, graph.make(block, Below, graph.make(block, Add, graph.get(block, index), addedValue), limit), 1, 1000);
    addIfComparisonHolds(graph, block, sum, graph.make(block, BelowEqual, limit, graph.make(block, Add, graph.get(block, index), addedValue)), 2000, 10);
    closeAndRotateLoop(graph, block, header, index, countsUp ? 1 : -1);

    CHECK_EQ(foldInCountedLoop(proc), isFolded);
    CHECK_EQ(valueCountInLoops(proc, Below), isFolded ? 0u : 1u);
    CHECK_EQ(valueCountInLoops(proc, BelowEqual), isFolded ? 0u : 1u);
    auto code = compileProc(proc);
    for (int64_t limitAtRunTime : { static_cast<int64_t>(0), static_cast<int64_t>(1), static_cast<int64_t>(2), static_cast<int64_t>(3), static_cast<int64_t>(9), static_cast<int64_t>(100) }) {
        int64_t expected = 0;
        for (int64_t i = countsUp ? start : limitAtRunTime + start; countsUp ? i < limitAtRunTime : i >= bound; i += countsUp ? 1 : -1) {
            expected += holds(Below, i + added, limitAtRunTime) ? 1 : 1000;
            expected += holds(BelowEqual, limitAtRunTime, i + added) ? 2000 : 10;
        }
        CHECK_EQ(invoke<int64_t>(*code, limitAtRunTime), expected);
    }
}

static void checkNeighborInLoopWithLoadedLimit(int64_t start, int64_t added, bool isFolded, bool limitIsWord = true)
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* inside = proc.addBlock();
    BasicBlock* outside = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* index = proc.addVariable(Int64);
    Variable* sum = proc.addVariable(Int64);
    Value* pointer = cCallArgumentValues<int64_t*>(proc, root)[0];
    Value* addedValue = graph.int64(root, added);
    graph.set(root, index, graph.int64(root, start));
    graph.set(root, sum, graph.int64(root, 0));
    graph.jump(root, header);
    Value* limit = limitIsWord ? graph.make(header, ZExt32, graph.load(header, Int32, pointer, 0, firstHeap)) : graph.load(header, Int64, pointer, 0, firstHeap);
    graph.branch(header, graph.make(header, LessThan, graph.get(header, index), limit), body, exit);
    graph.ret(exit, graph.get(exit, sum));
    graph.increase(body, sum, 7);
    graph.branch(body, graph.make(body, Below, graph.make(body, Add, graph.get(body, index), addedValue), limit), FrequentedBlock(inside), rarely(outside));
    graph.ret(outside, graph.int64(outside, -1));
    graph.store(inside, graph.get(inside, sum), pointer, 8, firstHeap);
    closeAndRotateLoop(graph, inside, header, index, 1);

    CHECK_EQ(foldInCountedLoop(proc), isFolded);
    CHECK_EQ(valueCountInLoops(proc, Below), isFolded ? 0u : 1u);
    auto code = compileProc(proc);
    for (int64_t limitAtRunTime : { static_cast<int64_t>(0), static_cast<int64_t>(1), static_cast<int64_t>(2), static_cast<int64_t>(3), static_cast<int64_t>(9), static_cast<int64_t>(100) }) {
        int64_t memory[2] = { limitAtRunTime, 0 };
        int64_t expected = 0;
        for (int64_t i = start; i < limitAtRunTime; ++i) {
            expected += 7;
            if (!holds(Below, i + added, limitAtRunTime)) {
                expected = -1;
                break;
            }
        }
        CHECK_EQ(invoke<int64_t>(*code, memory), expected);
    }
}

static void testFoldComparisonsOfNeighborWithLimitLoadedAtLoopTest()
{
    checkNeighborInLoopWithLoadedLimit(1, -1, true);
    checkNeighborInLoopWithLoadedLimit(2, -2, true);
    checkNeighborInLoopWithLoadedLimit(2, -1, true);
    checkNeighborInLoopWithLoadedLimit(0, -1, false);
    checkNeighborInLoopWithLoadedLimit(0, 1, false);
    checkNeighborInLoopWithLoadedLimit(1, -2, false);
}

static void testFoldComparisonsOfLoopVariableNeighborWithLimit()
{
    checkNeighborInLoop(true, 1, 0, -1, true);
    checkNeighborInLoop(true, 2, 0, -1, true);
    checkNeighborInLoop(true, 2, 0, -2, true);
    checkNeighborInLoop(true, 0, 0, 0, true);
    checkNeighborInLoop(false, -2, 0, 1, true);
    checkNeighborInLoop(false, -1, 1, -1, true);
    checkNeighborInLoop(false, -3, 0, 2, true);
}

static void testFoldComparisonsKeepsPossiblyOutOfRangeNeighbor()
{
    checkNeighborInLoop(true, 0, 0, -1, false);
    checkNeighborInLoop(true, 1, 0, -2, false);
    checkNeighborInLoop(true, 0, 0, 1, false);
    checkNeighborInLoop(true, -1, 0, 0, false);
    checkNeighborInLoop(false, -1, 0, 1, false);
    checkNeighborInLoop(false, -1, 0, -1, false);
    checkNeighborInLoop(false, -2, 0, 2, false);
}

static void testFoldComparisonsWithCountdownLoopStartBound()
{
    checkCountdownLoop({ .startMinusLimit = -1, .relation = GreaterEqual, .bound = 0, .step = -1, .isFolded = true });
    checkCountdownLoop({ .startMinusLimit = -1, .relation = GreaterThan, .bound = -1, .step = -1, .isFolded = true });
    checkCountdownLoop({ .startMinusLimit = -2, .relation = GreaterEqual, .bound = 1, .step = -1, .isFolded = true });
    checkCountdownLoop({ .startMinusLimit = -1, .relation = GreaterThan, .bound = 0, .step = -3, .isFolded = true });
}

static void testFoldComparisonsKeepsUnprovenBoundOfCountdownLoop()
{
    checkCountdownLoop({ .startMinusLimit = 0, .relation = GreaterEqual, .bound = 0, .step = -1, .isFolded = false });
    checkCountdownLoop({ .startMinusLimit = 1, .relation = GreaterEqual, .bound = 0, .step = -1, .isFolded = false });
    checkCountdownLoop({ .startMinusLimit = -1, .relation = GreaterEqual, .bound = -1, .step = -1, .isFolded = false });
    checkCountdownLoop({ .startMinusLimit = -1, .relation = GreaterThan, .bound = -2, .step = -1, .isFolded = false });
    checkCountdownLoop({ .startMinusLimit = -1, .relation = GreaterEqual, .bound = 0, .step = -1, .isFolded = false, .limitIsWord = false });
}

static void checkBottomTestedLoop(bool relationHoldsOnEntry)
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* index = proc.addVariable(Int64);
    Variable* sum = proc.addVariable(Int64);
    auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root);
    Value* bound = arguments[0];
    graph.set(root, index, arguments[1]);
    graph.set(root, sum, graph.int64(root, 0));
    graph.jump(root, body);
    BasicBlock* block = body;
    addIfComparisonHolds(graph, block, sum, graph.make(block, LessThan, graph.get(block, index), bound), 1, 1000);
    Value* next = graph.make(block, Add, graph.get(block, index), graph.int64(block, 1));
    graph.set(block, index, next);
    Value* test = graph.make(block, LessThan, next, bound);
    graph.branch(block, test, body, exit);
    graph.ret(exit, graph.get(exit, sum));
    proc.resetReachability();
    fixSSA(proc);
    Value* counter = next->child(0);
    CHECK(counter->opcode() == Phi);
    proc.setKnownInduction({ counter, next, test, 1, false, relationHoldsOnEntry });
    validate(proc);

    CHECK_EQ(foldInCountedLoop(proc), relationHoldsOnEntry);
    CHECK_EQ(valueCountInLoops(proc, LessThan), relationHoldsOnEntry ? 1u : 2u);
    auto code = compileProc(proc);
    CHECK_EQ(invoke<int64_t>(*code, 3, 0), 3);
    if (!relationHoldsOnEntry)
        CHECK_EQ(invoke<int64_t>(*code, 3, 5), 1000);
}

static void testFoldComparisonsWithBoundsRequiresRelationOnEntry()
{
    checkBottomTestedLoop(false);
    checkBottomTestedLoop(true);
}

static void checkNeighborOfInvariantLimit(Type type, B3::Opcode relation, int64_t start, int64_t added, std::optional<IntRange> limitRange, bool isFolded)
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* index = proc.addVariable(type);
    Variable* sum = proc.addVariable(Int64);
    Value* pointer = cCallArgumentValues<int64_t*>(proc, root)[0];
    Value* limit = graph.load(root, type, pointer, 0, firstHeap);
    if (limitRange)
        proc.setKnownRange(limit, *limitRange);
    Value* addedValue = root->appendIntConstant(proc, Origin(), type, added);
    graph.set(root, index, root->appendIntConstant(proc, Origin(), type, start));
    graph.set(root, sum, graph.int64(root, 0));
    graph.jump(root, header);
    graph.branch(header, graph.make(header, relation, graph.get(header, index), limit), body, exit);
    graph.ret(exit, graph.get(exit, sum));
    BasicBlock* block = body;
    addIfComparisonHolds(graph, block, sum, graph.make(block, Below, graph.make(block, Add, graph.get(block, index), addedValue), limit), 1, 1000);
    closeAndRotateLoop(graph, block, header, index, 1);

    CHECK_EQ(foldInCountedLoop(proc), isFolded);
    CHECK_EQ(valueCountInLoops(proc, Below), isFolded ? 0u : 1u);
    if (isFolded) {
        CHECK(simplifyTerminals(proc));
        validate(proc);
        CHECK_EQ(valueCountInLoops(proc, Branch), 1u);
    }
    auto code = compileProc(proc);
    for (int64_t limitAtRunTime : std::initializer_list<int64_t> { 0, 1, 2, 9, 100, start + 3 }) {
        int64_t memory[1] = { limitAtRunTime };
        int64_t expected = 0;
        for (int64_t i = start; holds(relation, i, limitAtRunTime); ++i)
            expected += holds(Below, i + added, limitAtRunTime) ? 1 : 1000;
        CHECK_EQ(invoke<int64_t>(*code, memory), expected);
    }
}

static void checkCarriedBound(bool isCarriedOnEveryIteration, bool counterIsWrapped, std::optional<IntRange> counterRange, bool isFolded)
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* preHeader = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* inside = proc.addBlock();
    BasicBlock* outside = proc.addBlock();
    BasicBlock* test = proc.addBlock();
    BasicBlock* latch = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Value* pointer = cCallArgumentValues<int64_t*>(proc, root)[0];
    Value* start = graph.int64(root, 1);
    Value* boundOnEntry = graph.make(root, ZExt32, graph.load(root, Int32, pointer, 0, firstHeap));
    graph.branch(root, graph.make(root, LessThan, start, boundOnEntry), preHeader, exit);
    Value* counter = graph.phi(body, Int64);
    Value* carriedBound = graph.phi(body, Int64);
    if (counterRange)
        proc.setKnownRange(counter, *counterRange);
    graph.upsilon(preHeader, start, counter);
    graph.upsilon(preHeader, boundOnEntry, carriedBound);
    graph.jump(preHeader, body);
    Value* length = graph.load(body, Int32, pointer, 0, firstHeap);
    proc.setKnownRange(length, IntRange(0, 1000));
    Value* boundInLoop = graph.make(body, ZExt32, length);
    Value* next = graph.make(body, Add, counter, graph.int64(body, 1));
    if (counterIsWrapped)
        next = graph.make(body, SExt32, graph.make(body, Trunc, next));
    graph.branch(body, graph.make(body, Below, graph.make(body, Add, counter, graph.int64(body, -1)), carriedBound), inside, outside);
    graph.upsilon(isCarriedOnEveryIteration ? latch : inside, boundInLoop, carriedBound);
    graph.jump(inside, test);
    graph.jump(outside, test);
    Value* condition = graph.make(test, LessThan, next, boundInLoop);
    graph.branch(test, condition, latch, exit);
    graph.upsilon(latch, next, counter);
    graph.jump(latch, body);
    graph.ret(exit, graph.int64(exit, 0));
    proc.resetReachability();
    proc.setKnownInduction({ counter, next, condition, 1, false, false });
    validate(proc);

    CHECK_EQ(foldInCountedLoop(proc), isFolded);
    CHECK_EQ(valueCountInLoops(proc, Below), isFolded ? 0u : 1u);
}

static void testFoldComparisonsWithCarriedBound()
{
    checkCarriedBound(true, false, std::nullopt, true);
    checkCarriedBound(false, false, std::nullopt, false);
    checkCarriedBound(true, true, IntRange(0, 1000), true);
    checkCarriedBound(true, true, std::nullopt, false);
}

static void testFoldComparisonsReadsFormsAndRanges()
{
    checkCountdownLoop({ .startMinusLimit = -1, .relation = GreaterEqual, .bound = 0, .step = -1, .isFolded = true, .startIsDifference = true });
    checkNeighborInLoopWithLoadedLimit(1, -1, false, false);

    checkNeighborOfInvariantLimit(Int32, LessThan, 1, -1, IntRange(0, 1000), true);
    checkNeighborOfInvariantLimit(Int64, LessThan, 1, -1, IntRange(0, 1000), true);
    checkNeighborOfInvariantLimit(Int64, LessEqual, 1, -1, IntRange(0, 1000), true);
    checkNeighborOfInvariantLimit(Int64, LessThan, 1ll << 32, -((1ll << 31) + 1), IntRange(0, 1ll << 40), true);

    checkNeighborOfInvariantLimit(Int32, LessThan, 1, -1, std::nullopt, false);
    checkNeighborOfInvariantLimit(Int64, LessThan, 1, -1, std::nullopt, false);
    checkNeighborOfInvariantLimit(Int64, LessEqual, 1, -1, IntRange(0, std::numeric_limits<int64_t>::max()), false);
}

namespace {

struct TopTestedLoopCase {
    std::optional<int64_t> publishedStep { 1 };
    bool latchHasSecondPredecessor { false };
    bool boundIsLoadedInLoop { false };
    bool isFolded;
};

} // anonymous namespace

static void checkTopTestedLoop(const TopTestedLoopCase& testCase)
{
    constexpr int64_t step = 1;
    bool isCounted = testCase.publishedStep == step;
    bool passesValidation = isCounted || !testCase.publishedStep;
    Policy policy = remarkPolicy();
    if (!passesValidation)
        policy.validation.level = ValidationLevel::AtBoundaries;
    Procedure proc(policy);
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* last = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t*>(proc, root);
    Value* boundArgument = arguments[0];
    Value* pointer = arguments[1];
    Value* counter = graph.phi(header, Int64);
    graph.upsilon(root, graph.int64(root, 0), counter);
    graph.jump(root, header);

    Value* bound = testCase.boundIsLoadedInLoop ? graph.load(header, Int64, pointer, 16, secondHeap) : boundArgument;
    graph.store(header, graph.make(header, LessThan, counter, bound), pointer, 0, firstHeap);
    Value* test = graph.make(header, LessThan, counter, bound);
    graph.branch(header, test, body, exit);
    graph.ret(exit, counter);

    Value* comparisonInBody = graph.make(body, LessThan, counter, bound);
    Value* numberOfPasses = graph.make(body, Add, graph.load(body, Int64, pointer, 8, firstHeap), graph.make(body, ZExt32, comparisonInBody));
    graph.store(body, numberOfPasses, pointer, 8, firstHeap);
    if (testCase.latchHasSecondPredecessor)
        graph.branch(body, graph.make(body, BitAnd, numberOfPasses, graph.int64(body, 1)), body, last);
    else
        graph.jump(body, last);

    Value* next = graph.make(last, Add, counter, graph.int64(last, step));
    graph.upsilon(last, next, counter);
    graph.jump(last, header);
    resetAndValidate(proc);
    if (testCase.publishedStep)
        proc.setKnownInduction({ counter, next, test, *testCase.publishedStep, false, false });

    CHECK_EQ(foldComparisonsWithBounds(proc), testCase.isFolded);
    CHECK_EQ(proc.report().countOf(Counter::KnownInductionsUsed), isCounted ? 1u : 0u);
    CHECK_EQ(numberOfValues(header, LessThan), 2u);
    CHECK_EQ(numberOfValues(body, LessThan), testCase.isFolded ? 0u : 1u);
    if (!passesValidation)
        return;

    validate(proc);
    auto code = compileProc(proc);
    for (int64_t boundAtRunTime : { -1, 0, 1, 7 }) {
        int64_t memory[3] = { 7, 0, boundAtRunTime };
        int64_t numberOfIterations = std::max<int64_t>(boundAtRunTime, 0);
        CHECK_EQ(invoke<int64_t>(*code, boundAtRunTime, memory), numberOfIterations);
        CHECK_EQ(memory[0], 0);
        CHECK_EQ(memory[1], numberOfIterations * (testCase.latchHasSecondPredecessor ? 2 : 1));
    }
}

static void testFoldComparisonsInTopTestedLoop()
{
    for (bool boundIsLoadedInLoop : { false, true }) {
        checkTopTestedLoop({ .boundIsLoadedInLoop = boundIsLoadedInLoop, .isFolded = true });
        checkTopTestedLoop({ .latchHasSecondPredecessor = true, .boundIsLoadedInLoop = boundIsLoadedInLoop, .isFolded = false });
    }
}

static void testFoldComparisonsNeedsRecord()
{
    checkTopTestedLoop({ .publishedStep = std::nullopt, .isFolded = false });
    checkTopTestedLoop({ .publishedStep = 2, .isFolded = false });

    Procedure proc(policyWith([](Policy& policy) {
        policy.knownProperties.trustsKnownInductions = false;
    }));
    Graph graph { proc };
    TopTestedLoop loop(graph);
    BasicBlock* block = loop.body;
    addIfComparisonHolds(graph, block, loop.sum, graph.make(block, LessThan, graph.get(block, loop.index), loop.bound), 1, 1000);
    loop.endBody(block);
    loop.rotate();

    CHECK(!foldComparisonsWithBounds(proc));
    CHECK(!proc.report().countOf(Counter::KnownInductionsUsed));
    auto code = compileProc(proc);
    for (int64_t bound : { -1, 0, 1, 7 })
        CHECK_EQ(run(*code, bound), std::max<int64_t>(bound, 0));
}

void addBunComparisonFoldingTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testFoldComparisonsWithBounds());
    RUN(testFoldComparisonsWithBoundsKeepsUnrelatedComparisons());
    RUN(testFoldComparisonsWithBoundsRequiresRelationOnEntry());
    RUN(testFoldComparisonsWithCountdownLoopStartBound());
    RUN(testFoldComparisonsOfLoopVariableNeighborWithLimit());
    RUN(testFoldComparisonsOfNeighborWithLimitLoadedAtLoopTest());
    RUN(testFoldComparisonsKeepsPossiblyOutOfRangeNeighbor());
    RUN(testFoldComparisonsKeepsUnprovenBoundOfCountdownLoop());
    RUN(testFoldComparisonsReadsFormsAndRanges());
    RUN(testFoldComparisonsWithCarriedBound());
    RUN(testFoldComparisonsInTopTestedLoop());
    RUN(testFoldComparisonsNeedsRecord());
}

#endif // ENABLE(B3_JIT)
