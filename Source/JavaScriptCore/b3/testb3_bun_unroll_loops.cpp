/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "B3EliminateDeadCode.h"
#include "B3LoopUtilities.h"
#include "B3UnrollLoops.h"

namespace {

enum class TestPlace : uint8_t { Top, Bottom };

enum class Walks : bool { Nothing, Data };

struct CountedSum {
    CountedSum(B3::Opcode relation, int64_t step, int64_t firstIndex, const Policy& policy = loopPolicy(), Walks walks = Walks::Data)
        : proc(policy)
        , loop(graph, relation, step, firstIndex)
        , relation(relation)
        , step(step)
        , firstIndex(firstIndex)
    {
        if (walks == Walks::Nothing)
            return;
        graph.increase(loop.body, loop.sum, zeroReadAt(graph, loop.body, loop.index));
    }

    // Whatever the counter is, the turn reads one of two words, which hold zero.
    static Value* zeroReadAt(Graph& graph, BasicBlock* block, Variable* counter)
    {
        static const int32_t zeros[2] = { 0, 0 };
        Value* distance = graph.make(block, BitAnd, graph.get(block, counter), graph.int64(block, 4));
        Value* word = graph.load(block, Int32, graph.make(block, Add, graph.int64(block, reinterpret_cast<intptr_t>(zeros)), distance), 0, firstHeap);
        return graph.make(block, ZExt32, word);
    }

    void finish(BasicBlock* bodyLastBlock, TestPlace place)
    {
        loop.endBody(bodyLastBlock);
        if (place == TestPlace::Bottom)
            loop.rotate();
    }

    template<typename Functor>
    int64_t expected(int64_t bound, const Functor& summand) const
    {
        uint64_t sum = 0;
        for (int64_t index = firstIndex; holds(relation, index, bound); index = static_cast<int64_t>(static_cast<uint64_t>(index) + static_cast<uint64_t>(step)))
            sum += static_cast<uint64_t>(summand(index));
        return static_cast<int64_t>(sum);
    }

    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop;
    B3::Opcode relation;
    int64_t step;
    int64_t firstIndex;
};

} // anonymous namespace

static bool unrollLoopsAndValidate(Procedure& proc)
{
    bool result = unrollLoops(proc);
    fixSSA(proc);
    validate(proc);
    return result;
}

static int64_t itself(int64_t index) { return index; }
static int64_t one(int64_t) { return 1; }

static void checkUnrolledSum(B3::Opcode relation, int64_t step, int64_t firstIndex, TestPlace place, std::span<const int64_t> bounds)
{
    CountedSum sum(relation, step, firstIndex);
    sum.graph.increase(sum.loop.body, sum.loop.sum, sum.graph.get(sum.loop.body, sum.loop.index));
    sum.finish(sum.loop.body, place);

    CHECK(unrollLoopsAndValidate(sum.proc));
    CHECK(hasRemark(sum.proc, RemarkKind::LoopUnrolled, GaveUpReason::None));
    CHECK_EQ(sum.proc.report().countOf(Counter::LoopsUnrolled), 1u);
    CHECK_EQ(sum.proc.naturalLoops().numLoops(), 2u);
    checkValueCountInEachLoop(sum.proc, Branch, { 1, 1 });
    auto code = compileProc(sum.proc);
    for (int64_t bound : bounds)
        CHECK_EQ(run(*code, bound), sum.expected(bound, itself));
}

static Vector<int64_t> boundsAround(int64_t first, int64_t last)
{
    Vector<int64_t> result;
    for (int64_t bound = first; bound <= last; ++bound)
        result.append(bound);
    return result;
}

static void testUnrollLoopsEveryRelation()
{
    for (TestPlace place : { TestPlace::Top, TestPlace::Bottom }) {
        for (int64_t step : { 1, 3 }) {
            Vector<int64_t> bounds = boundsAround(-3, 70);
            bounds.append(1000);
            checkUnrolledSum(LessThan, step, 0, place, bounds.span());
            checkUnrolledSum(LessEqual, step, 0, place, bounds.span());
            checkUnrolledSum(LessThan, step, -5, place, bounds.span());
            checkUnrolledSum(GreaterThan, -step, 60, place, bounds.span());
            checkUnrolledSum(GreaterEqual, -step, 60, place, bounds.span());
            Vector<int64_t> unsignedBounds = boundsAround(0, 70);
            checkUnrolledSum(Below, step, 0, place, unsignedBounds.span());
            checkUnrolledSum(BelowEqual, step, 0, place, unsignedBounds.span());
            checkUnrolledSum(Above, -step, 60, place, unsignedBounds.span());
            checkUnrolledSum(AboveEqual, -step, 60, place, boundsAround(3, 70).span());
        }
    }
}

static void checkBoundsAtEndOfRange(B3::Opcode relation, int64_t step, int64_t firstIndex, TestPlace place, std::span<const int64_t> bounds)
{
    CountedSum sum(relation, step, firstIndex);
    sum.graph.increase(sum.loop.body, sum.loop.sum, 1);
    sum.finish(sum.loop.body, place);

    CHECK(unrollLoopsAndValidate(sum.proc));
    auto code = compileProc(sum.proc);
    for (int64_t bound : bounds)
        CHECK_EQ(run(*code, bound), sum.expected(bound, one));
}

static void testUnrollLoopsBoundWithoutRoomForOtherTurns()
{
    constexpr int64_t least = std::numeric_limits<int64_t>::min();
    constexpr int64_t greatest = std::numeric_limits<int64_t>::max();
    for (TestPlace place : { TestPlace::Top, TestPlace::Bottom }) {
        checkBoundsAtEndOfRange(LessThan, 1, least, place, boundsAround(least, least + 30).span());
        checkBoundsAtEndOfRange(LessThan, 1, greatest - 30, place, boundsAround(greatest - 40, greatest - 1).span());
        checkBoundsAtEndOfRange(LessThan, 1, greatest - 30, place, std::array<int64_t, 1> { greatest });
        checkBoundsAtEndOfRange(GreaterThan, -1, greatest, place, boundsAround(greatest - 30, greatest - 1).span());
        checkBoundsAtEndOfRange(GreaterThan, -1, least + 30, place, boundsAround(least + 1, least + 40).span());
        checkBoundsAtEndOfRange(GreaterThan, -1, least + 30, place, std::array<int64_t, 1> { least });
        checkBoundsAtEndOfRange(Below, 1, 0, place, boundsAround(0, 30).span());
        checkBoundsAtEndOfRange(Below, 1, -31, place, boundsAround(-40, -1).span());
        checkBoundsAtEndOfRange(Above, -1, -1, place, boundsAround(-31, -2).span());
        checkBoundsAtEndOfRange(Above, -1, 30, place, boundsAround(0, 40).span());
    }
}

static void testUnrollLoopsKeepsOtherWaysOut()
{
    for (TestPlace place : { TestPlace::Top, TestPlace::Bottom }) {
        CountedSum sum(LessThan, 1, 0);
        auto& [proc, graph, loop, relation, step, firstIndex] = sum;
        BasicBlock* out = proc.addBlock();
        BasicBlock* stays = proc.addBlock();
        graph.branch(loop.body, graph.make(loop.body, Equal, graph.get(loop.body, loop.index), loop.extra), rarely(out), FrequentedBlock(stays));
        graph.ret(out, graph.make(out, Sub, graph.int64(out, -1), graph.get(out, loop.sum)));
        graph.increase(stays, loop.sum, graph.get(stays, loop.index));
        sum.finish(stays, place);

        CHECK(unrollLoopsAndValidate(proc));
        auto code = compileProc(proc);
        for (int64_t bound = 0; bound < 40; ++bound) {
            for (int64_t leavesAt = -1; leavesAt < 42; ++leavesAt)
                CHECK_EQ(run(*code, bound, nullptr, leavesAt), leavesAt >= 0 && leavesAt < bound ? -1 - sumBelow(leavesAt) : sumBelow(bound));
        }
    }
}

static void testUnrollLoopsLeavesRareBlocksToLoopItself()
{
    for (TestPlace place : { TestPlace::Top, TestPlace::Bottom }) {
        CountedSum sum(LessThan, 1, 0);
        auto& [proc, graph, loop, relation, step, firstIndex] = sum;
        BasicBlock* rare = proc.addBlock();
        BasicBlock* stays = proc.addBlock();
        graph.branch(loop.body, graph.make(loop.body, Equal, graph.get(loop.body, loop.index), loop.extra), rarely(rare), FrequentedBlock(stays));
        graph.callLikePatchpoint(rare);
        graph.increase(rare, loop.sum, 1000);
        graph.jump(rare, stays);
        graph.increase(stays, loop.sum, graph.get(stays, loop.index));
        sum.finish(stays, place);

        CHECK(unrollLoopsAndValidate(proc));
        CHECK_EQ(numberOfValues(proc, Patchpoint), 1u);
        auto code = compileProc(proc);
        for (int64_t bound = 0; bound < 40; ++bound) {
            for (int64_t rareAt = -1; rareAt < 42; ++rareAt)
                CHECK_EQ(run(*code, bound, nullptr, rareAt), sumBelow(bound) + (rareAt >= 0 && rareAt < bound ? 1000 : 0));
        }
    }
}

static void testUnrollLoopsSkipsLoopWithCall()
{
    CountedSum sum(LessThan, 1, 0);
    sum.graph.callLikePatchpoint(sum.loop.body);
    sum.graph.increase(sum.loop.body, sum.loop.sum, sum.graph.get(sum.loop.body, sum.loop.index));
    sum.finish(sum.loop.body, TestPlace::Bottom);
    CHECK(!unrollLoopsAndValidate(sum.proc));
    checkSumsIntegersBelowBound(sum.proc);
}

static void testUnrollLoopsSkipsLoopThatWalksNothing()
{
    CountedSum sum(LessThan, 1, 0, loopPolicy(), Walks::Nothing);
    sum.graph.increase(sum.loop.body, sum.loop.sum, sum.graph.get(sum.loop.body, sum.loop.index));
    sum.graph.increase(sum.loop.body, sum.loop.sum, sum.graph.make(sum.loop.body, ZExt32, sum.graph.load(sum.loop.body, Int32, sum.loop.pointer, 0, firstHeap)));
    sum.finish(sum.loop.body, TestPlace::Bottom);

    CHECK(!unrollLoopsAndValidate(sum.proc));
    CHECK_EQ(sum.proc.naturalLoops().numLoops(), 1u);
}

static void testUnrollLoopsSkipsLoopWithLikelySecondWayOut()
{
    CountedSum sum(LessThan, 1, 0);
    auto& [proc, graph, loop, relation, step, firstIndex] = sum;
    BasicBlock* found = proc.addBlock();
    BasicBlock* stays = proc.addBlock();
    graph.branch(loop.body, graph.make(loop.body, Equal, graph.get(loop.body, loop.index), loop.extra), found, stays);
    graph.ret(found, graph.int64(found, -1));
    graph.increase(stays, loop.sum, graph.get(stays, loop.index));
    sum.finish(stays, TestPlace::Bottom);

    CHECK(!unrollLoopsAndValidate(proc));
    auto code = compileProc(proc);
    CHECK_EQ(run(*code, 20, nullptr, -1), sumBelow(20));
    CHECK_EQ(run(*code, 20, nullptr, 7), -1);
}

static void testUnrollLoopsStoresInOrder()
{
    for (TestPlace place : { TestPlace::Top, TestPlace::Bottom }) {
        CountedSum sum(LessThan, 1, 1);
        auto& [proc, graph, loop, relation, step, firstIndex] = sum;
        Value* address = graph.make(loop.body, Add, loop.pointer, graph.make(loop.body, Shl, graph.get(loop.body, loop.index), graph.int32(loop.body, 2)));
        Value* previous = graph.load(loop.body, Int32, address, -4, firstHeap);
        graph.store(loop.body, graph.make(loop.body, Add, previous, graph.make(loop.body, Trunc, graph.get(loop.body, loop.index))), address, 0, firstHeap);
        sum.finish(loop.body, place);

        CHECK(unrollLoopsAndValidate(proc));
        auto code = compileProc(proc);
        for (int64_t bound = 0; bound < 40; ++bound) {
            int32_t numbers[40] = { };
            CHECK_EQ(run(*code, bound, reinterpret_cast<int64_t*>(numbers)), 0);
            for (int64_t index = 0; index < 40; ++index)
                CHECK_EQ(numbers[index], index < bound ? sumBelow(index + 1) : 0);
        }
    }
}

static void testUnrollLoopsSkipsLoopOverWords()
{
    enum class Access : uint8_t { Load, Store };
    for (Access access : { Access::Load, Access::Store }) {
        CountedSum sum(LessThan, 1, 0);
        auto& [proc, graph, loop, relation, step, firstIndex] = sum;
        Value* address = graph.make(loop.body, Add, loop.pointer, graph.make(loop.body, Shl, graph.get(loop.body, loop.index), graph.int32(loop.body, 3)));
        if (access == Access::Load)
            graph.increase(loop.body, loop.sum, graph.load(loop.body, Int64, address, 0, firstHeap));
        else
            graph.store(loop.body, loop.extra, address, 0, firstHeap);
        sum.finish(loop.body, TestPlace::Bottom);

        CHECK(!unrollLoopsAndValidate(proc));
        int64_t words[20];
        std::ranges::fill(words, 3);
        CHECK_EQ(compileAndRun<int64_t>(proc, static_cast<int64_t>(20), words, static_cast<int64_t>(5)), access == Access::Load ? 60 : 0);
        CHECK_EQ(words[19], access == Access::Load ? 3 : 5);
    }
}

static void testUnrollLoopsPollsOnceForAllTurns()
{
    for (TestPlace place : { TestPlace::Top, TestPlace::Bottom }) {
        CountedSum sum(LessThan, 1, 0);
        PollState state;
        appendPoll(sum.graph, sum.loop.body, state);
        sum.graph.increase(sum.loop.body, sum.loop.sum, sum.graph.get(sum.loop.body, sum.loop.index));
        sum.finish(sum.loop.body, place);

        CHECK(unrollLoopsAndValidate(sum.proc));
        checkValueCountInEachLoop(sum.proc, isLoopPoll, "polls"_s, { 1, 1 });
        auto code = compileProc(sum.proc);
        for (int64_t bound : { 0, 1, 7, 8, 9, 64, 67 }) {
            state = { };
            CHECK_EQ(run(*code, bound), sumBelow(bound));
            CHECK_EQ(state.numberOfPolls, bound / Limits::maxTurnsPerUnrolledLoop + bound % Limits::maxTurnsPerUnrolledLoop);
        }
    }
}

static void testUnrollLoopsWritesFewerTurnsOfLargerLoop()
{
    struct Case {
        unsigned numberOfAdditions;
        unsigned expectedTurns;
    };
    for (Case testCase : { Case { 0, 8 }, Case { 1, 8 }, Case { 2, 4 }, Case { 7, 4 }, Case { 8, 0 }, Case { 30, 0 } }) {
        CountedSum sum(LessThan, 1, 0);
        auto& [proc, graph, loop, relation, step, firstIndex] = sum;
        Value* summand = graph.get(loop.body, loop.index);
        for (unsigned i = 0; i < testCase.numberOfAdditions; ++i)
            summand = graph.make(loop.body, BitXor, graph.make(loop.body, Add, summand, loop.extra), loop.bound);
        graph.increase(loop.body, loop.sum, summand);
        sum.finish(loop.body, TestPlace::Bottom);
        unsigned numberOfOperations = numberOfValues(proc, BitXor);

        CHECK_EQ(unrollLoopsAndValidate(proc), !!testCase.expectedTurns);
        CHECK_EQ(numberOfValues(proc, BitXor), numberOfOperations * (1 + testCase.expectedTurns));
        CHECK_EQ(limitHits(proc, Limit::MaxValuesPerUnrolledLoop) > 0, testCase.expectedTurns < Limits::maxTurnsPerUnrolledLoop);
    }
}

static void testUnrollLoopsSkipsLoopThatEndsBeforeAllTurns()
{
    for (int64_t bound : { 3, 7, 8, 100 }) {
        CountedSum sum(LessThan, 1, 0);
        auto& [proc, graph, loop, relation, step, firstIndex] = sum;
        Value* constant = proc.add<Const64Value>(Origin(), bound);
        loop.root->appendNonTerminal(constant);
        loop.header->last()->child(0)->child(1) = constant;
        graph.increase(loop.body, loop.sum, graph.get(loop.body, loop.index));
        sum.finish(loop.body, TestPlace::Bottom);

        CHECK_EQ(unrollLoopsAndValidate(proc), bound >= 8);
        CHECK_EQ(compileAndRun<int64_t>(proc, static_cast<int64_t>(0), nullptr, static_cast<int64_t>(0)), sumBelow(bound));
    }
}

static void testUnrollLoopsSkipsLoopThatRunsRarely()
{
    for (double minFrequency : { 1.0, 2.0 }) {
        Policy policy = loopPolicy();
        policy.loops.minFrequencyOfUnrolledLoop = minFrequency;
        CountedSum sum(LessThan, 1, 0, policy);
        sum.graph.increase(sum.loop.body, sum.loop.sum, sum.graph.get(sum.loop.body, sum.loop.index));
        sum.finish(sum.loop.body, TestPlace::Bottom);
        CHECK_EQ(unrollLoopsAndValidate(sum.proc), minFrequency == 1.0);
        checkSumsIntegersBelowBound(sum.proc);
    }
}

static void testUnrollLoopsSkipsBoundComputedInLoop()
{
    CountedSum sum(LessThan, 1, 0);
    auto& [proc, graph, loop, relation, step, firstIndex] = sum;
    Value* loaded = proc.add<MemoryValue>(Load, Int64, Origin(), loop.pointer, 0, firstHeap);
    loop.header->values().insert(0, loaded);
    loop.header->last()->child(0)->child(1) = loaded;
    graph.increase(loop.body, loop.sum, graph.get(loop.body, loop.index));
    graph.store(loop.body, graph.make(loop.body, Sub, loaded, graph.int64(loop.body, 1)), loop.pointer, 0, firstHeap);
    loop.endBody(loop.body);

    CHECK(!unrollLoopsAndValidate(proc));
    int64_t word = 20;
    CHECK_EQ(compileAndRun<int64_t>(proc, static_cast<int64_t>(0), &word, static_cast<int64_t>(0)), sumBelow(10));
}

static void testUnrollLoopsOnlyInnermostLoop()
{
    Procedure proc(loopPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* outer = proc.addBlock();
    BasicBlock* inner = proc.addBlock();
    BasicBlock* outerLatch = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* row = proc.addVariable(Int64);
    Variable* column = proc.addVariable(Int64);
    Variable* sum = proc.addVariable(Int64);
    Value* bound = cCallArgumentValues<int64_t>(proc, root)[0];
    graph.set(root, row, graph.int64(root, 0));
    graph.set(root, sum, graph.int64(root, 0));
    graph.jump(root, outer);
    graph.set(outer, column, graph.int64(outer, 0));
    graph.jump(outer, inner);
    graph.increase(inner, sum, graph.make(inner, Mul, graph.get(inner, row), graph.get(inner, column)));
    graph.increase(inner, sum, CountedSum::zeroReadAt(graph, inner, column));
    graph.increase(inner, column, 1);
    graph.branch(inner, graph.make(inner, LessThan, graph.get(inner, column), bound), inner, outerLatch);
    graph.increase(outerLatch, row, 1);
    graph.branch(outerLatch, graph.make(outerLatch, LessThan, graph.get(outerLatch, row), bound), outer, exit);
    graph.ret(exit, graph.get(exit, sum));
    proc.resetReachability();
    fixSSA(proc);

    CHECK(unrollLoopsAndValidate(proc));
    CHECK_EQ(proc.naturalLoops().numLoops(), 3u);
    auto code = compileProc(proc);
    for (int64_t size : { 1, 2, 7, 8, 9, 20 })
        CHECK_EQ(invoke<int64_t>(*code, size), sumBelow(size) * sumBelow(size));
}

static void checkPipelineUnrollsLoopOverElements(const Policy& policy)
{
    unsigned numberOfLoads = 0;
    Procedure proc(policyWithInspection(policy, [&](Procedure& optimized) {
        numberOfLoads = numberOfValues(optimized, [](Value* value) {
            return value->opcode() == Load && value->type() == Double;
        });
    }));
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* index = proc.addVariable(Int64);
    Variable* sum = proc.addVariable(Double);
    auto arguments = cCallArgumentValues<int64_t, double*>(proc, root);
    Value* length = arguments[0];
    Value* pointer = arguments[1];
    graph.set(root, index, graph.int64(root, 0));
    graph.set(root, sum, root->appendNew<ConstDoubleValue>(proc, Origin(), 0.0));
    graph.jump(root, header);
    graph.branch(header, graph.make(header, LessThan, graph.get(header, index), length), body, exit);
    Value* address = graph.make(body, Add, pointer, graph.make(body, Shl, graph.get(body, index), graph.int32(body, 3)));
    graph.set(body, sum, graph.make(body, Add, graph.get(body, sum), graph.load(body, Double, address, 0, firstHeap)));
    graph.increase(body, index, 1);
    graph.jump(body, header);
    graph.ret(exit, graph.get(exit, sum));

    auto code = compileProc(proc, 2);
    CHECK_EQ(numberOfLoads, 1 + Limits::maxTurnsPerUnrolledLoop);
    double elements[40];
    for (unsigned i = 0; i < 40; ++i)
        elements[i] = 1.0 / (i + 3);
    for (int64_t count = 0; count <= 40; ++count) {
        double expected = 0;
        for (int64_t i = 0; i < count; ++i)
            expected += elements[i];
        CHECK_EQ(invoke<double>(*code, count, elements), expected);
    }
}

static void testPipelineUnrollsLoopOverElements()
{
    Policy policy = optimizingPolicy();
    policy.loops.unrolls = true;
    checkPipelineUnrollsLoopOverElements(policy);
}

static void testPipelineUnrollsWhereNothingElseIsDoneToLoops()
{
    Policy policy = optimizingPolicy();
    policy.loops = { };
    policy.loops.unrolls = true;
    checkPipelineUnrollsLoopOverElements(policy);
}

namespace {

// double sum = start; do { sum = sum OP words[index]; } while (++index < bound), entered if 0 < bound. The start comes and the sum goes as bits.
struct SumOfIntegersLoop {
    explicit SumOfIntegersLoop(B3::Opcode operation, bool storesEverySum = false)
        : operation(operation)
    {
        BasicBlock* root = proc.addBlock();
        BasicBlock* body = proc.addBlock();
        BasicBlock* exit = proc.addBlock();
        Variable* index = proc.addVariable(Int64);
        Variable* sum = proc.addVariable(Double);
        auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root).eager();
        graph.set(root, index, graph.int64(root, 0));
        graph.set(root, sum, graph.make(root, BitwiseCast, arguments[2]));
        graph.branch(root, graph.make(root, LessThan, graph.int64(root, 0), arguments[0]), body, exit);

        Value* address = graph.make(body, Add, arguments[1], graph.make(body, Shl, graph.get(body, index), graph.int32(body, 2)));
        Value* word = graph.load(body, Int32, address, 8, firstHeap);
        Value* next = graph.make(body, operation, graph.get(body, sum), graph.make(body, IToD, word));
        graph.set(body, sum, next);
        if (storesEverySum)
            graph.store(body, next, arguments[1], 0, secondHeap);
        graph.increase(body, index, 1);
        graph.branch(body, graph.make(body, LessThan, graph.get(body, index), arguments[0]), body, exit);
        graph.ret(exit, graph.make(exit, BitwiseCast, graph.get(exit, sum)));
        proc.resetReachability();
        fixSSA(proc);
        validate(proc);
    }

    int64_t expected(std::span<const int32_t> words, int64_t bound, double start) const
    {
        double sum = start;
        for (int64_t index = 0; index < bound; ++index)
            sum = operation == Add ? sum + words[2 + index] : sum - words[2 + index];
        return std::bit_cast<int64_t>(sum);
    }

    void check(Compilation& code, Vector<int32_t>& words, int64_t bound, double start) const
    {
        CHECK_EQ(run(code, bound, reinterpret_cast<int64_t*>(words.mutableSpan().data()), std::bit_cast<int64_t>(start)), expected(words.span(), bound, start));
    }

    Procedure proc { loopPolicy() };
    Graph graph { proc };
    B3::Opcode operation;
};

} // anonymous namespace

static void testUnrollLoopsAddsSumOfIntegersAsIntegers()
{
    Vector<int32_t> words(2 + 1000);
    for (size_t index = 0; index < words.size(); ++index)
        words[index] = static_cast<int32_t>(index * 2654435761u);
    words[5] = std::numeric_limits<int32_t>::min();
    words[6] = std::numeric_limits<int32_t>::max();
    constexpr double infinity = std::numeric_limits<double>::infinity();
    for (B3::Opcode operation : { Add, Sub }) {
        SumOfIntegersLoop loop(operation);
        CHECK(unrollLoopsAndValidate(loop.proc));
        eliminateDeadCode(loop.proc);
        checkValueCountInEachLoop(loop.proc, IToD, { 0, 1 });
        checkValueCountInEachLoop(loop.proc, SExt32, { 0, 8 });
        auto code = compileProc(loop.proc);
        for (double start : { 0.0, -0.0, 1.0, -7.0, 0.5, 0.1, -1e-300, 1e15, -1e15, 2251799813685247.0, 2251799813685248.0, -2251799813685248.5, 4503599627370497.0, 9007199254740992.0, -9007199254740994.0, 1e300, infinity, -infinity, std::numeric_limits<double>::quiet_NaN() }) {
            for (int64_t bound = -1; bound <= 40; ++bound)
                loop.check(*code, words, bound, start);
            loop.check(*code, words, 1000, start);
        }
        Vector<int32_t> zeros;
        zeros.fill(0, 2 + 40);
        for (int64_t bound = 0; bound <= 40; ++bound)
            loop.check(*code, zeros, bound, -0.0);
        zeros[2 + 3] = 1;
        zeros[2 + 5] = -1;
        for (int64_t bound = 0; bound <= 40; ++bound) {
            for (double start : { 0.0, -0.0, 1.0, -1.0 })
                loop.check(*code, zeros, bound, start);
        }
    }
}

static void testUnrollLoopsGoesOnWithDoublesWhereSumOfIntegersGrowsLarge()
{
    constexpr int64_t bound = (1ll << 22) + 1001;
    for (int32_t word : { std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::min() + 1 }) {
        Vector<int32_t> words;
        words.fill(word, 2 + bound);
        for (B3::Opcode operation : { Add, Sub }) {
            SumOfIntegersLoop loop(operation);
            CHECK(unrollLoopsAndValidate(loop.proc));
            auto code = compileProc(loop.proc);
            for (double start : { 0.0, 1.0, 2251799813685247.0, -2251799813685247.0 }) {
                loop.check(*code, words, bound, start);
                loop.check(*code, words, (1ll << 21) + 3, start);
            }
        }
    }
}

static void testUnrollLoopsKeepsSumThatLoopLooksAt()
{
    Vector<int32_t> words;
    words.fill(3, 2 + 100);
    SumOfIntegersLoop loop(Add, true);
    CHECK(unrollLoopsAndValidate(loop.proc));
    eliminateDeadCode(loop.proc);
    checkValueCountInEachLoop(loop.proc, IToD, { 1, 8 });
    checkValueCountInEachLoop(loop.proc, SExt32, { 0, 0 });
    auto code = compileProc(loop.proc);
    for (int64_t bound : { 0, 1, 7, 8, 9, 100 }) {
        loop.check(*code, words, bound, 0.5);
        if (bound)
            CHECK_EQ(*reinterpret_cast<int64_t*>(words.mutableSpan().data()), loop.expected(words.span(), bound, 0.5));
    }
}

void addBunLoopUnrollingTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testUnrollLoopsEveryRelation());
    RUN(testUnrollLoopsBoundWithoutRoomForOtherTurns());
    RUN(testUnrollLoopsKeepsOtherWaysOut());
    RUN(testUnrollLoopsLeavesRareBlocksToLoopItself());
    RUN(testUnrollLoopsSkipsLoopWithCall());
    RUN(testUnrollLoopsSkipsLoopThatWalksNothing());
    RUN(testUnrollLoopsSkipsLoopWithLikelySecondWayOut());
    RUN(testUnrollLoopsStoresInOrder());
    RUN(testUnrollLoopsSkipsLoopOverWords());
    RUN(testUnrollLoopsPollsOnceForAllTurns());
    RUN(testUnrollLoopsWritesFewerTurnsOfLargerLoop());
    RUN(testUnrollLoopsSkipsLoopThatEndsBeforeAllTurns());
    RUN(testUnrollLoopsSkipsLoopThatRunsRarely());
    RUN(testUnrollLoopsSkipsBoundComputedInLoop());
    RUN(testUnrollLoopsOnlyInnermostLoop());
    RUN(testPipelineUnrollsLoopOverElements());
    RUN(testPipelineUnrollsWhereNothingElseIsDoneToLoops());
    RUN(testUnrollLoopsAddsSumOfIntegersAsIntegers());
    RUN(testUnrollLoopsGoesOnWithDoublesWhereSumOfIntegersGrowsLarge());
    RUN(testUnrollLoopsKeepsSumThatLoopLooksAt());
}

#endif // ENABLE(B3_JIT)
