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

namespace {

struct BottomTestedLoop {
    explicit BottomTestedLoop(Graph& graph, Type type = Int64)
        : graph(graph)
        , root(graph.proc.addBlock())
        , body(graph.proc.addBlock())
        , latch(graph.proc.addBlock())
        , exit(graph.proc.addBlock())
    {
        auto arguments = cCallArgumentValues<int64_t, int64_t*>(graph.proc, root);
        argument = arguments[0];
        pointer = arguments[1];
        phi = graph.phi(body, type);
    }

    void enterFrom(BasicBlock* block, Value* start)
    {
        graph.upsilon(block, start, phi);
        graph.jump(block, body);
    }

    void close(Value* nextValue, Value* condition)
    {
        next = nextValue;
        test = condition;
        graph.branch(body, test, latch, exit);
        graph.upsilon(latch, next, phi);
        graph.jump(latch, body);
        graph.ret(exit, graph.int64(exit, 0));
        resetAndValidate(graph.proc);
    }

    void publish(int64_t step, bool doesNotWrap = false, bool relationHoldsOnEntry = false)
    {
        graph.proc.setKnownInduction({ phi, next, test, step, doesNotWrap, relationHoldsOnEntry });
        validate(graph.proc);
    }

    void finish(Value* start, Value* nextValue, Value* condition, int64_t step, bool doesNotWrap = false)
    {
        enterFrom(root, start);
        close(nextValue, condition);
        publish(step, doesNotWrap);
    }

    Graph& graph;
    BasicBlock* root;
    BasicBlock* body;
    BasicBlock* latch;
    BasicBlock* exit;
    Value* argument;
    Value* pointer;
    Value* phi;
    Value* next { nullptr };
    Value* test { nullptr };
};

struct PollingLoop {
    PollingLoop(Graph& graph, PollState& state, Type type)
        : graph(graph)
        , root(graph.proc.addBlock())
        , header(graph.proc.addBlock())
        , body(graph.proc.addBlock())
        , exit(graph.proc.addBlock())
        , index(graph.proc.addVariable(type))
        , sum(graph.proc.addVariable(Int64))
    {
        appendPoll(graph, body, state);
        graph.increase(body, sum, 1);
    }

    Value* constant(BasicBlock* block, int64_t value) { return block->appendIntConstant(graph.proc, Origin(), index->type(), value); }

    void finish(Value* condition, Value* next, bool doesNotWrap = false)
    {
        graph.set(root, index, constant(root, 0));
        graph.set(root, sum, graph.int64(root, 0));
        graph.jump(root, header);
        graph.branch(header, condition, body, exit);
        graph.set(body, index, next);
        graph.jump(body, header);
        graph.ret(exit, graph.get(exit, sum));
        graph.proc.resetReachability();
        graph.proc.setKnownInduction(DemotedInduction { index, header, next, condition, 1, doesNotWrap });
        fixSSA(graph.proc);
        validate(graph.proc);
        CHECK(rotateLoops(graph.proc));
        fixSSA(graph.proc);
        validate(graph.proc);
    }

    Graph& graph;
    BasicBlock* root;
    BasicBlock* header;
    BasicBlock* body;
    BasicBlock* exit;
    Variable* index;
    Variable* sum;
};

enum class TopTestForm : uint8_t {
    ContinuesWhenTrue,
    Negated,
    ExitsWhenTrue,
};

struct WordCounterCase {
    B3::Opcode relation;
    std::optional<IntRange> boundRange;
    bool doesNotWrap;
    bool counterRangeIsSafe;
    bool isMined;
};

} // anonymous namespace

static constexpr int64_t manyIterations = 2 * maxIterationsPerStrip + 10;

static std::expected<CountedLoop, GaveUpReason> analyze(Procedure& proc, const B3::NaturalLoop& loop, std::optional<LoopTest> test = std::nullopt)
{
    B3::NaturalLoops& loops = proc.naturalLoops();
    proc.resetValueOwners();
    PhiChildren phiChildren(proc);
    return analyzeCountedLoop({ proc, loops, loop, phiChildren, [&](Value* value) {
        return !loops.belongsTo(value->owner, loop);
    }, test });
}

static std::expected<CountedLoop, GaveUpReason> analyze(Procedure& proc, std::optional<LoopTest> test = std::nullopt)
{
    B3::NaturalLoops& loops = proc.naturalLoops();
    CHECK_EQ(loops.numLoops(), 1u);
    return analyze(proc, loops.loop(0), test);
}

static void checkNotCounted(Procedure& proc, GaveUpReason reason)
{
    auto counted = analyze(proc);
    CHECK(!counted);
    CHECK(counted.error() == reason);
}

static void checkIsCounted(Procedure& proc, bool isCounted)
{
    auto counted = analyze(proc);
    CHECK_EQ(!!counted, isCounted);
    if (!counted)
        CHECK(counted.error() == GaveUpReason::NotCounted);
}

static Value* findRecordedPhi(Procedure& proc, BasicBlock* header)
{
    Value* result = nullptr;
    for (Value* value : *header) {
        if (value->opcode() != Phi || !proc.knownInduction(value))
            continue;
        CHECK(!result);
        result = value;
    }
    return result;
}

static Value* wrapped(Graph& graph, BasicBlock* block, Value* value)
{
    return graph.make(block, SExt32, graph.make(block, Trunc, value));
}

static Value* appendRepeatedAdds(Graph& graph, BasicBlock* block, Value* value, Value* amount, int64_t numberOfAdds)
{
    for (int64_t i = 0; i < numberOfAdds; ++i)
        value = graph.make(block, Add, value, amount);
    return value;
}

static void finishWrappedCounter(BottomTestedLoop& loop, Value* start, Value* bound, bool doesNotWrap = false)
{
    Graph& graph = loop.graph;
    Value* one = graph.int64(loop.body, 1);
    Value* next = wrapped(graph, loop.body, graph.make(loop.body, Add, loop.phi, one));
    loop.finish(start, next, graph.make(loop.body, LessThan, next, bound), 1, doesNotWrap);
}

static void closePlainCounter(BottomTestedLoop& loop, B3::Opcode relation = LessThan, int64_t step = 1)
{
    Graph& graph = loop.graph;
    Value* next = graph.make(loop.body, Add, loop.phi, graph.int64(loop.body, step));
    loop.close(next, graph.make(loop.body, relation, next, loop.argument));
}

static void buildPlainCounter(BottomTestedLoop& loop, B3::Opcode relation = LessThan, int64_t step = 1)
{
    loop.enterFrom(loop.root, loop.graph.int64(loop.root, 0));
    closePlainCounter(loop, relation, step);
}

static void testAnalyzeCountedLoop()
{
    struct Case {
        B3::Opcode relation;
        int64_t step;
        bool isCounted;
        std::optional<IntRange> boundRange;
        bool boundIsSafe;
    };
    for (Case testCase : { Case { LessThan, 1, true, std::nullopt, true }, Case { Below, 1, true, std::nullopt, true }, Case { GreaterThan, -1, true, std::nullopt, true },
        Case { LessEqual, 3, true, std::nullopt, false }, Case { AboveEqual, -2, true, std::nullopt, false },
        Case { LessEqual, 3, true, IntRange(-1000000, 1000000), true }, Case { AboveEqual, -2, true, IntRange(2, 1000000), true },
        Case { LessThan, -1, false, std::nullopt, false }, Case { GreaterThan, 1, false, std::nullopt, false }, Case { NotEqual, 1, false, std::nullopt, false }, Case { LessThan, Limits::maxCountedLoopStep + 1, false, std::nullopt, false } }) {
        Procedure proc(remarkPolicy());
        Graph graph { proc };
        TopTestedLoop loop(graph, testCase.relation, testCase.step);
        graph.increase(loop.body, loop.sum, 1);
        loop.endBody(loop.body);
        loop.rotate();
        if (testCase.boundRange)
            proc.setKnownRange(loop.bound, *testCase.boundRange);

        auto counted = analyze(proc);
        CHECK_EQ(!!counted, testCase.isCounted);
        if (!counted) {
            CHECK(counted.error() == GaveUpReason::NotCounted);
            continue;
        }
        CHECK(counted->header == loop.body);
        CHECK(counted->relation == testCase.relation);
        CHECK_EQ(counted->step, testCase.step);
        CHECK(counted->bound == loop.bound);
        CHECK(counted->start->isInt64(0));
        CHECK(counted->inductionVariable->opcode() == Phi);
        CHECK(counted->next->opcode() == Add);
        CHECK_EQ(counted->cannotWrap(), testCase.boundIsSafe);
        CHECK(!counted->isTopTested);
        if (testCase.relation == AboveEqual)
            CHECK(counted->farthestSafeBound == 2);
        CHECK_EQ(counted->increases(), testCase.step > 0);
        CHECK(counted->relationHoldsOnEntry);
    }
}

static void checkCountdownRecurrence()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BottomTestedLoop loop(graph);
    Value* next = graph.make(loop.body, Sub, loop.phi, graph.int64(loop.body, 1));
    loop.finish(graph.int64(loop.root, 100), next, graph.make(loop.body, GreaterThan, next, loop.argument), -1);

    auto counted = analyze(proc);
    CHECK(counted);
    CHECK_EQ(counted->step, -1);
}

static void checkWrappedCounterWithWordBound(std::optional<IntRange> lengthRange, bool cannotWrap)
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BottomTestedLoop loop(graph);
    Value* length = graph.load(loop.root, Int32, loop.pointer, 0, firstHeap);
    if (lengthRange)
        proc.setKnownRange(length, *lengthRange);
    finishWrappedCounter(loop, graph.int64(loop.root, 0), graph.make(loop.root, ZExt32, length));

    auto form = affineFormOf(proc, loop.next, loop.phi, Signedness::Signed);
    CHECK(form);
    CHECK_EQ(form->offset, 1);
    CHECK(form->exactBaseRange == (IntegerInterval { Int128 { std::numeric_limits<int32_t>::min() } - 1, Int128 { std::numeric_limits<int32_t>::max() } - 1 }));

    auto counted = analyze(proc);
    CHECK(counted);
    CHECK_EQ(counted->cannotWrap(), cannotWrap);
    CHECK(counted->farthestSafeBound == std::numeric_limits<int32_t>::max());
}

static void checkWrappedCounterWithRangeOnPhi()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BottomTestedLoop loop(graph);
    proc.setKnownRange(loop.phi, IntRange(0, 1000));
    finishWrappedCounter(loop, graph.int64(loop.root, 0), loop.argument);

    auto counted = analyze(proc);
    CHECK(counted);
    CHECK(counted->cannotWrap());
}

static void checkLongestRecurrence()
{
    constexpr int64_t depth = defaultValueOf(Limit::MaxRangeAnalysisDepth);
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BottomTestedLoop loop(graph);
    Value* next = appendRepeatedAdds(graph, loop.body, loop.phi, graph.int64(loop.body, 1), depth);
    loop.finish(graph.int64(loop.root, 0), next, graph.make(loop.body, LessThan, next, loop.argument), depth);

    auto form = affineFormOf(proc, next, loop.phi, Signedness::Signed);
    CHECK(form);
    CHECK_EQ(form->offset, depth);

    auto counted = analyze(proc);
    CHECK(counted);
    CHECK_EQ(counted->step, depth);
    CHECK_EQ(limitHits(proc, Limit::MaxRangeAnalysisDepth), 0u);
}

static void testAnalyzeCountedLoopReadsRecurrencesAndRanges()
{
    checkCountdownRecurrence();
    checkWrappedCounterWithWordBound(std::nullopt, false);
    checkWrappedCounterWithWordBound(IntRange(0, 1000), true);
    checkWrappedCounterWithRangeOnPhi();
    checkLongestRecurrence();
}

static void checkCounterNeedsMatchingRecord()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BottomTestedLoop loop(graph);
    buildPlainCounter(loop);
    checkNotCounted(proc, GaveUpReason::NotCounted);

    proc.setKnownInduction({ loop.phi, loop.next, nullptr, 1, false, false });
    validate(proc);
    checkNotCounted(proc, GaveUpReason::NotCounted);

    proc.setKnownInduction({ loop.phi, loop.next, loop.test, 2, false, false });
    checkNotCounted(proc, GaveUpReason::NotCounted);
    CHECK_EQ(proc.report().countOf(Counter::KnownInductionsUsed), 0u);

    loop.publish(1);
    CHECK(analyze(proc));
    CHECK_EQ(proc.report().countOf(Counter::KnownInductionsUsed), 1u);
}

static void checkWrappedCounterFromUnknownStart(bool doesNotWrap)
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BottomTestedLoop loop(graph);
    Value* length = graph.load(loop.root, Int32, loop.pointer, 0, firstHeap);
    finishWrappedCounter(loop, loop.argument, graph.make(loop.root, ZExt32, length), doesNotWrap);
    if (!doesNotWrap) {
        checkNotCounted(proc, GaveUpReason::CounterMayWrap);
        return;
    }
    auto counted = analyze(proc);
    CHECK(counted);
    CHECK(counted->counterRangeIsSafe);
}

static void checkTwoBackEdgeValues(int64_t otherStep, bool isCounted)
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BottomTestedLoop loop(graph);
    graph.upsilon(loop.body, graph.make(loop.body, Add, loop.phi, graph.int64(loop.body, otherStep)), loop.phi);
    buildPlainCounter(loop);
    proc.setKnownInduction({ loop.phi, loop.next, loop.test, 1, false, false });
    checkIsCounted(proc, isCounted);
}

static void checkRecurrencePastDepthLimit()
{
    constexpr int64_t depth = defaultValueOf(Limit::MaxRangeAnalysisDepth);
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BottomTestedLoop loop(graph);
    Value* one = graph.int64(loop.body, 1);
    Value* firstAdd = loop.body->appendNew<Value>(proc, Add, numberedOrigin(1), loop.phi, one);
    Value* next = appendRepeatedAdds(graph, loop.body, firstAdd, one, depth);
    loop.finish(graph.int64(loop.root, 0), next, graph.make(loop.body, LessThan, next, loop.argument), depth + 1);

    CHECK(!affineFormOf(proc, next, loop.phi, Signedness::Signed));
    checkNotCounted(proc, GaveUpReason::NotCounted);
    CHECK(limitHits(proc, Limit::MaxRangeAnalysisDepth) > 0);
    CHECK(hasNumber(proc.report().firstOriginOf(Limit::MaxRangeAnalysisDepth), 1));
}

static void checkTestOnWidenedCounter()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BottomTestedLoop loop(graph, Int32);
    Value* next = graph.make(loop.body, Add, loop.phi, graph.int32(loop.body, 1));
    Value* widened = graph.make(loop.body, SExt32, next);
    loop.finish(graph.int32(loop.root, 0), next, graph.make(loop.body, LessThan, widened, loop.argument), 1);
    checkNotCounted(proc, GaveUpReason::NotCounted);
}

static void checkNextThroughPhi()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* odd = proc.addBlock();
    BasicBlock* even = proc.addBlock();
    BasicBlock* test = proc.addBlock();
    BasicBlock* latch = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Value* argument = cCallArgumentValues<int64_t>(proc, root)[0];
    Value* phi = graph.phi(body, Int64);
    Value* merged = graph.phi(test, Int64);
    graph.upsilon(root, graph.int64(root, 0), phi);
    graph.jump(root, body);
    Value* one = graph.int64(body, 1);
    Value* sum = graph.make(body, Add, phi, one);
    graph.branch(body, graph.make(body, BitAnd, sum, one), odd, even);
    graph.upsilon(odd, sum, merged);
    graph.jump(odd, test);
    graph.upsilon(even, graph.int64(even, 5), merged);
    graph.jump(even, test);
    Value* condition = graph.make(test, LessThan, merged, argument);
    graph.branch(test, condition, latch, exit);
    graph.upsilon(latch, merged, phi);
    graph.jump(latch, body);
    graph.ret(exit, graph.int64(exit, 0));
    resetAndValidate(proc);
    proc.setKnownInduction({ phi, merged, condition, 1, false, false });
    validate(proc);
    checkNotCounted(proc, GaveUpReason::NotCounted);
}

static void checkTwoBranchesOnTest()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BottomTestedLoop loop(graph);
    Value* next = graph.make(loop.body, Add, loop.phi, graph.int64(loop.body, 1));
    Value* test = graph.make(loop.body, LessThan, next, loop.argument);
    loop.enterFrom(loop.root, graph.int64(loop.root, 0));
    graph.branch(loop.body, test, loop.latch, loop.exit);
    graph.upsilon(loop.latch, next, loop.phi);
    graph.branch(loop.latch, test, loop.body, loop.exit);
    graph.ret(loop.exit, graph.int64(loop.exit, 0));
    resetAndValidate(proc);
    proc.setKnownInduction({ loop.phi, next, test, 1, false, false });
    validate(proc);

    checkNotCounted(proc, GaveUpReason::NotCounted);
    auto counted = analyze(proc, LoopTest { loop.body, test });
    CHECK(counted);
    CHECK(counted->test == loop.body);
}

static void checkSecondRecordIsTried()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BottomTestedLoop loop(graph);
    Value* counter = graph.phi(loop.body, Int64);
    Value* zero = graph.int64(loop.root, 0);
    graph.upsilon(loop.root, zero, counter);
    Value* nextCounter = graph.make(loop.body, Add, counter, graph.int64(loop.body, 1));
    Value* nextOther = graph.make(loop.body, Add, loop.phi, graph.int64(loop.body, 2));
    Value* test = graph.make(loop.body, LessThan, nextCounter, loop.argument);
    graph.upsilon(loop.latch, nextCounter, counter);
    loop.finish(zero, nextOther, test, 2);
    checkNotCounted(proc, GaveUpReason::NotCounted);

    proc.setKnownInduction({ counter, nextCounter, test, 1, false, false });
    validate(proc);
    auto counted = analyze(proc);
    CHECK(counted);
    CHECK(counted->inductionVariable == counter);
    CHECK_EQ(counted->step, 1);
}

static void checkPlainCounter(B3::Opcode relation, int64_t step, bool isCounted)
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BottomTestedLoop loop(graph);
    buildPlainCounter(loop, relation, step);
    loop.publish(step);
    checkIsCounted(proc, isCounted);
}

static void checkRelationsAndSteps()
{
    checkPlainCounter(LessThan, 1, true);
    checkPlainCounter(GreaterThan, -1, true);
    checkPlainCounter(Equal, 1, false);
    checkPlainCounter(NotEqual, 1, false);
    checkPlainCounter(LessThan, Limits::maxCountedLoopStep, true);
    checkPlainCounter(LessThan, Limits::maxCountedLoopStep + 1, false);
    checkPlainCounter(GreaterThan, -Limits::maxCountedLoopStep, true);
    checkPlainCounter(GreaterThan, -Limits::maxCountedLoopStep - 1, false);
    checkPlainCounter(LessThan, -1, false);
    checkPlainCounter(GreaterThan, 1, false);
}

static void checkSingleBlockLoop(bool testsNext)
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Value* bound = cCallArgumentValues<int64_t>(proc, root)[0];
    Value* phi = graph.phi(body, Int64);
    graph.upsilon(root, graph.int64(root, 0), phi);
    graph.jump(root, body);
    Value* next = graph.make(body, Add, phi, graph.int64(body, 1));
    Value* test = graph.make(body, LessThan, testsNext ? next : phi, bound);
    graph.upsilon(body, next, phi);
    graph.branch(body, test, body, exit);
    graph.ret(exit, graph.int64(exit, 0));
    resetAndValidate(proc);
    proc.setKnownInduction({ phi, next, test, 1, false, false });
    validate(proc);
    checkIsCounted(proc, testsNext);
}

static void checkLoopWithoutPreHeader(bool hasTwoEntries)
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BottomTestedLoop loop(graph);
    if (hasTwoEntries) {
        BasicBlock* first = proc.addBlock();
        BasicBlock* second = proc.addBlock();
        graph.branch(loop.root, loop.argument, first, second);
        loop.enterFrom(first, graph.int64(first, 0));
        loop.enterFrom(second, graph.int64(second, 0));
    } else {
        graph.upsilon(loop.root, graph.int64(loop.root, 0), loop.phi);
        graph.branch(loop.root, loop.argument, loop.body, loop.exit);
    }
    closePlainCounter(loop);
    loop.publish(1);
    checkNotCounted(proc, GaveUpReason::SeveralEntries);
}

static void checkVariantBound()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BottomTestedLoop loop(graph);
    Value* next = graph.make(loop.body, Add, loop.phi, graph.int64(loop.body, 1));
    Value* bound = graph.load(loop.body, Int64, loop.pointer, 0, firstHeap);
    loop.finish(graph.int64(loop.root, 0), next, graph.make(loop.body, LessThan, next, bound), 1);
    checkNotCounted(proc, GaveUpReason::BoundNotInvariant);
}

static void testAnalyzeCountedLoopDeclinesOtherShapes()
{
    checkCounterNeedsMatchingRecord();
    checkWrappedCounterFromUnknownStart(false);
    checkWrappedCounterFromUnknownStart(true);
    checkTwoBackEdgeValues(1, true);
    checkTwoBackEdgeValues(2, false);
    checkRecurrencePastDepthLimit();
    checkTestOnWidenedCounter();
    checkNextThroughPhi();
    checkTwoBranchesOnTest();
    checkSecondRecordIsTried();
    checkRelationsAndSteps();
    checkSingleBlockLoop(true);
    checkSingleBlockLoop(false);
    checkLoopWithoutPreHeader(true);
    checkLoopWithoutPreHeader(false);
    checkVariantBound();
}

static void checkTopTestedLoop(TopTestForm form, bool counterIsWrapped, bool rareBlockPassesArgument)
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* skips = proc.addBlock();
    BasicBlock* test = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t*>(proc, root);
    Value* argument = arguments[0];
    Value* length = graph.load(root, Int32, arguments[1], 0, firstHeap);
    proc.setKnownRange(length, IntRange(0, 1000));
    Value* bound = graph.make(root, ZExt32, length);
    Value* phi = graph.phi(header, Int64);
    graph.upsilon(root, graph.int64(root, 0), phi);
    graph.jump(root, header);
    Value* next = graph.make(header, Add, phi, graph.int64(header, 1));
    if (counterIsWrapped)
        next = wrapped(graph, header, next);
    Value* skipCondition = graph.make(header, BitAnd, phi, graph.int64(header, 64));
    graph.branch(header, skipCondition, rarely(skips), FrequentedBlock(test));
    graph.upsilon(skips, rareBlockPassesArgument ? argument : next, phi);
    graph.jump(skips, header);
    Value* comparison = graph.make(test, form == TopTestForm::ExitsWhenTrue ? GreaterEqual : LessThan, phi, bound);
    switch (form) {
    case TopTestForm::ContinuesWhenTrue:
        graph.branch(test, comparison, body, exit);
        break;
    case TopTestForm::Negated:
        graph.branch(test, graph.make(test, Equal, comparison, graph.int32(test, 0)), exit, body);
        break;
    case TopTestForm::ExitsWhenTrue:
        graph.branch(test, comparison, exit, body);
        break;
    }
    graph.upsilon(body, next, phi);
    graph.jump(body, header);
    graph.ret(exit, graph.int64(exit, 0));
    resetAndValidate(proc);
    proc.setKnownInduction({ phi, next, comparison, 1, false, false });
    validate(proc);

    bool isCounted = !rareBlockPassesArgument;
    CHECK(!analyze(proc, LoopTest { test, skipCondition }));
    CHECK_EQ(!!analyze(proc, LoopTest { test, comparison }), isCounted && form != TopTestForm::Negated);
    auto counted = analyze(proc);
    CHECK_EQ(!!counted, isCounted);
    if (!counted) {
        CHECK(counted.error() == GaveUpReason::NotCounted);
        return;
    }
    CHECK(counted->isTopTested);
    CHECK(counted->relation == LessThan);
    CHECK_EQ(counted->step, 1);
    CHECK(counted->test == test);
    CHECK(counted->latch == body);
    CHECK_EQ(counted->latchSuccessorIndex, form == TopTestForm::ContinuesWhenTrue ? 0u : 1u);
    CHECK(counted->testedValue() == phi);
    CHECK(counted->bound == bound);
    CHECK(counted->cannotWrap());
    CHECK(!counted->relationHoldsOnEntry);
}

static void testAnalyzeTopTestedCountedLoop()
{
    for (TopTestForm form : { TopTestForm::ContinuesWhenTrue, TopTestForm::Negated, TopTestForm::ExitsWhenTrue }) {
        for (bool counterIsWrapped : { false, true }) {
            for (bool rareBlockPassesArgument : { false, true })
                checkTopTestedLoop(form, counterIsWrapped, rareBlockPassesArgument);
        }
    }
}

static void checkRelationHoldsOnEntry(std::optional<int64_t> constantBound, std::optional<IntRange> argumentRange, bool isProven)
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BottomTestedLoop loop(graph);
    if (argumentRange)
        proc.setKnownRange(loop.argument, *argumentRange);
    Value* bound = constantBound ? graph.int64(loop.root, *constantBound) : loop.argument;
    Value* next = graph.make(loop.body, Add, loop.phi, graph.int64(loop.body, 1));
    loop.finish(graph.int64(loop.root, 0), next, graph.make(loop.body, LessThan, next, bound), 1);

    auto counted = analyze(proc);
    CHECK(counted);
    CHECK_EQ(counted->relationHoldsOnEntry, isProven);
    CHECK_EQ(relationHoldsOnEntry(proc, *counted, counted->bound), isProven);
}

static void checkRelationHoldsByRecord()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BottomTestedLoop loop(graph);
    Value* ten = graph.int64(loop.root, 10);
    buildPlainCounter(loop);
    loop.publish(1, false, true);

    auto counted = analyze(proc);
    CHECK(counted);
    CHECK(counted->relationHoldsOnEntry);
    CHECK(!relationHoldsOnEntry(proc, *counted, counted->bound));
    CHECK(relationHoldsOnEntry(proc, *counted, ten));
}

static void checkRelationHoldsBelowGuard(unsigned numberOfBlocksBelowGuard, bool loopIsOnTakenEdge)
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    BottomTestedLoop loop(graph);
    BasicBlock* block = proc.addBlock();
    Value* guard = graph.make(loop.root, LessThan, graph.int64(loop.root, 0), loop.argument);
    if (loopIsOnTakenEdge)
        graph.branch(loop.root, guard, block, loop.exit);
    else
        graph.branch(loop.root, guard, loop.exit, block);
    for (unsigned i = 0; i < numberOfBlocksBelowGuard; ++i) {
        BasicBlock* nextBlock = proc.addBlock();
        graph.jump(block, nextBlock);
        block = nextBlock;
    }
    loop.enterFrom(block, graph.int64(block, 0));
    closePlainCounter(loop);
    loop.publish(1);

    auto counted = analyze(proc);
    CHECK(counted);
    CHECK(counted->preHeader == block);
    CHECK_EQ(counted->relationHoldsOnEntry, loopIsOnTakenEdge);
}

static void testRelationHoldsOnEntry()
{
    checkRelationHoldsOnEntry(std::nullopt, std::nullopt, false);
    checkRelationHoldsOnEntry(10, std::nullopt, true);
    checkRelationHoldsOnEntry(std::nullopt, IntRange(1, 50), true);
    checkRelationHoldsOnEntry(std::nullopt, IntRange(0, 50), false);
    checkRelationHoldsByRecord();
    for (unsigned numberOfBlocksBelowGuard : { 0u, 1u, 6u }) {
        checkRelationHoldsBelowGuard(numberOfBlocksBelowGuard, true);
        checkRelationHoldsBelowGuard(numberOfBlocksBelowGuard, false);
    }
}

static void checkRotatedLoopKeepsRecord()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph, LessThan, 3);
    graph.increase(loop.body, loop.sum, 1);
    loop.endBody(loop.body);
    loop.publishInduction(true, true);
    Value* next = loop.nextCounter();
    Value* topTest = loop.header->last()->child(0);
    loop.rotate();

    Value* phi = findRecordedPhi(proc, loop.body);
    CHECK(phi);
    std::optional<InductionRecord> record = proc.knownInduction(phi);
    CHECK(record->next == next);
    CHECK(record->test && record->test != topTest);
    CHECK(record->test->opcode() == LessThan);
    CHECK_EQ(record->step, 3);
    CHECK(record->doesNotWrap);
    CHECK(!record->relationHoldsOnEntry);

    auto counted = analyze(proc);
    CHECK(counted);
    CHECK(!counted->isTopTested);
    CHECK(counted->header == loop.body);
    CHECK(counted->inductionVariable == phi);
    CHECK_EQ(counted->step, 3);
    CHECK(counted->counterRangeIsSafe);
    CHECK(counted->relationHoldsOnEntry);
    CHECK(relationHoldsOnEntry(proc, *counted, counted->bound));
}

static void checkRotatedLoopWithoutRecord()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph);
    graph.increase(loop.body, loop.sum, 1);
    loop.endBody(loop.body);
    proc.knownInductions().remove(loop.counter());
    loop.rotate();

    CHECK(!findRecordedPhi(proc, loop.body));
    checkNotCounted(proc, GaveUpReason::NotCounted);
}

static void checkRotatedLoopWithTwoLatches()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph);
    BasicBlock* left = proc.addBlock();
    BasicBlock* right = proc.addBlock();
    graph.branch(loop.body, graph.make(loop.body, BitAnd, loop.extra, graph.int64(loop.body, 1)), left, right);
    graph.increase(right, loop.index, loop.step);
    graph.jump(right, loop.header);
    loop.endBody(left);
    CHECK(analyze(proc));
    loop.rotate();

    CHECK(findRecordedPhi(proc, loop.body));
    checkNotCounted(proc, GaveUpReason::NotCounted);
}

static void testRotationCarriesInductionRecord()
{
    checkRotatedLoopKeepsRecord();
    checkRotatedLoopWithoutRecord();
    checkRotatedLoopWithTwoLatches();
}

static void testOuterLoopIsCounted()
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    TopTestedLoop outer(graph);
    BasicBlock* innerHeader = proc.addBlock();
    BasicBlock* innerBody = proc.addBlock();
    BasicBlock* afterInner = proc.addBlock();
    Variable* innerIndex = proc.addVariable(Int64);
    graph.set(outer.body, innerIndex, graph.int64(outer.body, 0));
    graph.jump(outer.body, innerHeader);
    graph.branch(innerHeader, graph.make(innerHeader, LessThan, graph.get(innerHeader, innerIndex), outer.extra), innerBody, afterInner);
    graph.increase(innerBody, innerIndex, 1);
    graph.increase(innerBody, outer.sum, 1);
    graph.jump(innerBody, innerHeader);
    outer.endBody(afterInner);

    B3::NaturalLoops& loops = proc.naturalLoops();
    CHECK_EQ(loops.numLoops(), 2u);
    const B3::NaturalLoop* outerLoop = loops.headerOf(outer.header);
    const B3::NaturalLoop* innerLoop = loops.headerOf(innerHeader);
    CHECK(outerLoop && innerLoop);
    CHECK(loops.innerMostOuterLoop(*innerLoop) == outerLoop);

    auto counted = analyze(proc, *outerLoop);
    CHECK(counted);
    CHECK(counted->isTopTested);
    CHECK(counted->header == outer.header);
    CHECK(counted->latch == outer.body);
    CHECK_EQ(counted->step, 1);

    auto inner = analyze(proc, *innerLoop);
    CHECK(!inner);
    CHECK(inner.error() == GaveUpReason::NotCounted);
}

static void buildCountedLoop(Graph& graph, PollState& state, Type type, bool constantIsFirst, bool boundIsFirst)
{
    PollingLoop loop(graph, state, type);
    Value* bound = type == Int32 ? cCallArgumentValues<int32_t>(graph.proc, loop.root)[0] : cCallArgumentValues<int64_t>(graph.proc, loop.root)[0];
    Value* indexAtHeader = graph.get(loop.header, loop.index);
    Value* condition = boundIsFirst ? graph.make(loop.header, GreaterThan, bound, indexAtHeader) : graph.make(loop.header, LessThan, indexAtHeader, bound);
    Value* one = loop.constant(loop.body, 1);
    Value* indexInBody = graph.get(loop.body, loop.index);
    loop.finish(condition, constantIsFirst ? graph.make(loop.body, Add, one, indexInBody) : graph.make(loop.body, Add, indexInBody, one));
}

static void checkPolls(const PollState& state, int64_t numberOfIterations, bool isMined)
{
    int64_t numberOfPollsBetweenStrips = numberOfIterations > 0 ? (numberOfIterations - 1) / maxIterationsPerStrip : 0;
    CHECK_EQ(state.numberOfPolls, isMined ? numberOfPollsBetweenStrips : numberOfIterations);
}

static void checkCountedLoopOfWidthAndOperandOrder(Type type, bool constantIsFirst, bool boundIsFirst)
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    PollState state;
    buildCountedLoop(graph, state, type, constantIsFirst, boundIsFirst);

    CHECK(stripMineLoopsAndValidate(proc));
    auto code = compileProc(proc);
    for (int64_t bound : std::initializer_list<int64_t> { -1, 0, 1, 100, manyIterations }) {
        state = { };
        int64_t result = type == Int32 ? invoke<int64_t>(*code, static_cast<int32_t>(bound)) : invoke<int64_t>(*code, bound);
        CHECK_EQ(result, std::max<int64_t>(bound, 0));
        checkPolls(state, std::max<int64_t>(bound, 0), true);
    }
}

static void testCountedLoopsOfEitherWidthAndOperandOrder()
{
    for (Type type : { Int32, Int64 }) {
        for (bool constantIsFirst : { false, true }) {
            for (bool boundIsFirst : { false, true })
                checkCountedLoopOfWidthAndOperandOrder(type, constantIsFirst, boundIsFirst);
        }
    }
}

static void checkStripMiningNeedsProof(Procedure& proc, bool isMined)
{
    CHECK_EQ(stripMineLoopsAndValidate(proc), isMined);
    CHECK_EQ(hasRemark(proc, RemarkKind::PollNotStripMined, GaveUpReason::CounterMayWrap), !isMined);
}

static void checkWordCounterToLoadedBound(const WordCounterCase& testCase)
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    PollState state;
    PollingLoop loop(graph, state, Int32);
    Value* pointer = cCallArgumentValues<int64_t*>(proc, loop.root)[0];
    Value* bound = graph.load(loop.root, Int32, pointer, 0, firstHeap);
    if (testCase.boundRange)
        proc.setKnownRange(bound, *testCase.boundRange);
    Value* condition = graph.make(loop.header, testCase.relation, graph.get(loop.header, loop.index), bound);
    loop.finish(condition, graph.make(loop.body, Add, graph.get(loop.body, loop.index), loop.constant(loop.body, 1)), testCase.doesNotWrap);

    auto counted = analyze(proc);
    CHECK(counted);
    CHECK(counted->relation == testCase.relation);
    CHECK_EQ(counted->counterRangeIsSafe, testCase.counterRangeIsSafe);
    CHECK_EQ(counted->cannotWrap(), testCase.isMined);

    checkStripMiningNeedsProof(proc, testCase.isMined);
    auto code = compileProc(proc);
    for (int64_t boundAtRunTime : std::initializer_list<int64_t> { 0, 5, manyIterations }) {
        state = { };
        int64_t memory[1] = { boundAtRunTime };
        CHECK_EQ(invoke<int64_t>(*code, memory), boundAtRunTime + 1);
        checkPolls(state, boundAtRunTime + 1, testCase.isMined);
    }
}

static void checkWrappedCounterToArgument(bool boundIsWrapped, bool doesNotWrap, bool isMined)
{
    Procedure proc(remarkPolicy());
    Graph graph { proc };
    PollState state;
    PollingLoop loop(graph, state, Int64);
    Value* argument = cCallArgumentValues<int64_t>(proc, loop.root)[0];
    Value* bound = boundIsWrapped ? wrapped(graph, loop.root, argument) : argument;
    Value* condition = graph.make(loop.header, LessThan, graph.get(loop.header, loop.index), bound);
    loop.finish(condition, wrapped(graph, loop.body, graph.make(loop.body, Add, graph.get(loop.body, loop.index), graph.int64(loop.body, 1))), doesNotWrap);

    checkStripMiningNeedsProof(proc, isMined);
    auto code = compileProc(proc);
    for (int64_t boundAtRunTime : std::initializer_list<int64_t> { 0, 5, manyIterations }) {
        state = { };
        CHECK_EQ(invoke<int64_t>(*code, boundAtRunTime), boundAtRunTime);
        checkPolls(state, boundAtRunTime, isMined);
    }
}

static void testCountedLoopRequiresNoWrapProof()
{
    checkWordCounterToLoadedBound({ LessEqual, std::nullopt, false, false, false });
    checkWordCounterToLoadedBound({ LessEqual, IntRange(0, 1000000), false, false, true });
    checkWordCounterToLoadedBound({ LessEqual, std::nullopt, true, true, true });
    checkWordCounterToLoadedBound({ BelowEqual, std::nullopt, true, false, false });
    checkWordCounterToLoadedBound({ BelowEqual, IntRange(0, 1000000), false, false, true });
    checkWrappedCounterToArgument(false, false, false);
    checkWrappedCounterToArgument(true, false, true);
    checkWrappedCounterToArgument(false, true, true);
}

void addBunCountedLoopTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testAnalyzeCountedLoop());
    RUN(testAnalyzeCountedLoopReadsRecurrencesAndRanges());
    RUN(testAnalyzeCountedLoopDeclinesOtherShapes());
    RUN(testAnalyzeTopTestedCountedLoop());
    RUN(testRelationHoldsOnEntry());
    RUN(testRotationCarriesInductionRecord());
    RUN(testOuterLoopIsCounted());
    RUN(testCountedLoopsOfEitherWidthAndOperandOrder());
    RUN(testCountedLoopRequiresNoWrapProof());
}

#endif // ENABLE(B3_JIT)
