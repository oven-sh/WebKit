/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3.h"

#if ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)

#include "B3BasicBlockUtils.h"
#include "B3CountedLoops.h"
#include "B3CycleForest.h"
#include "B3DuplicateTails.h"
#include "B3EliminateDeadCode.h"
#include "B3FoldPathConstants.h"
#include "B3HoistLoopInvariantValues.h"
#include "B3InferSwitches.h"
#include "B3NaturalLoops.h"
#include "B3RotateLoops.h"
#include "B3SinkValues.h"
#include "B3StripMineLoops.h"
#include "B3VersionLoops.h"

namespace {

struct Graph {
    Value* int32(BasicBlock* block, int32_t value) { return block->appendNew<Const32Value>(proc, Origin(), value); }
    Value* int64(BasicBlock* block, int64_t value) { return block->appendNew<Const64Value>(proc, Origin(), value); }
    Value* number(BasicBlock* block, double value) { return block->appendNew<ConstDoubleValue>(proc, Origin(), value); }
    Value* make(BasicBlock* block, B3::Opcode opcode, Value* child) { return block->appendNew<Value>(proc, opcode, Origin(), child); }
    Value* make(BasicBlock* block, B3::Opcode opcode, Value* left, Value* right) { return block->appendNew<Value>(proc, opcode, Origin(), left, right); }
    Value* select(BasicBlock* block, Value* condition, Value* ifTrue, Value* ifFalse) { return block->appendNew<Value>(proc, Select, Origin(), condition, ifTrue, ifFalse); }

    Value* get(BasicBlock* block, Variable* variable) { return block->appendNew<VariableValue>(proc, B3::Get, Origin(), variable); }
    void set(BasicBlock* block, Variable* variable, Value* value) { block->appendNew<VariableValue>(proc, B3::Set, Origin(), variable, value); }
    void increase(BasicBlock* block, Variable* variable, Value* by) { set(block, variable, make(block, Add, get(block, variable), by)); }
    void increase(BasicBlock* block, Variable* variable, int64_t by) { increase(block, variable, int64(block, by)); }

    Value* phi(BasicBlock* block, Type type) { return block->appendNew<Value>(proc, Phi, type, Origin()); }
    void upsilon(BasicBlock* block, Value* value, Value* phi) { block->appendNew<UpsilonValue>(proc, Origin(), value, phi); }

    MemoryValue* load(BasicBlock* block, Type type, Value* pointer, int32_t offset, HeapRange range)
    {
        return block->appendNew<MemoryValue>(proc, Load, type, Origin(), pointer, offset, range);
    }

    void store(BasicBlock* block, Value* value, Value* pointer, int32_t offset, HeapRange range)
    {
        block->appendNew<MemoryValue>(proc, Store, Origin(), value, pointer, offset, range);
    }

    PatchpointValue* emptyPatchpoint(BasicBlock* block, Type type = Void)
    {
        PatchpointValue* result = block->appendNew<PatchpointValue>(proc, type, Origin());
        result->effects = Effects::none();
        result->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
            if (params.size())
                jit.move(CCallHelpers::TrustedImm32(0), params[0].gpr());
        });
        return result;
    }

    PatchpointValue* callLikePatchpoint(BasicBlock* block)
    {
        PatchpointValue* result = emptyPatchpoint(block);
        result->effects.writesLocalState = true;
        result->clobberLate(RegisterSet::argumentGPRs());
        return result;
    }

    void jump(BasicBlock* from, BasicBlock* to)
    {
        from->appendNew<Value>(proc, Jump, Origin());
        from->setSuccessors(FrequentedBlock(to));
    }

    void branch(BasicBlock* from, Value* condition, FrequentedBlock taken, FrequentedBlock notTaken)
    {
        from->appendNew<Value>(proc, Branch, Origin(), condition);
        from->setSuccessors(taken, notTaken);
    }

    void branch(BasicBlock* from, Value* condition, BasicBlock* taken, BasicBlock* notTaken)
    {
        branch(from, condition, FrequentedBlock(taken), FrequentedBlock(notTaken));
    }

    void ret(BasicBlock* block, Value* value) { block->appendNew<Value>(proc, Return, Origin(), value); }

    Procedure& proc;
};

FrequentedBlock rarely(BasicBlock* block) { return FrequentedBlock(block, FrequencyClass::Rare); }

constexpr HeapRange firstHeap { 10, 11 };
constexpr HeapRange secondHeap { 11, 12 };
constexpr HeapRange bothHeaps { 10, 12 };

template<typename Functor>
unsigned numberOfValues(Procedure& proc, const Functor& functor)
{
    unsigned result = 0;
    for (BasicBlock* block : proc) {
        for (Value* value : *block)
            result += functor(value);
    }
    return result;
}

unsigned numberOfValues(Procedure& proc, B3::Opcode opcode)
{
    return numberOfValues(proc, [&](Value* value) {
        return value->opcode() == opcode;
    });
}

unsigned numberOfValues(BasicBlock* block, B3::Opcode opcode)
{
    unsigned result = 0;
    for (Value* value : *block)
        result += value->opcode() == opcode;
    return result;
}

unsigned numberOfBlocks(Procedure& proc)
{
    unsigned result = 0;
    for (BasicBlock* block : proc) {
        UNUSED_PARAM(block);
        ++result;
    }
    return result;
}

bool hasInt64Constant(Procedure& proc, int64_t constant)
{
    return numberOfValues(proc, [&](Value* value) {
        return value->isInt64(constant);
    });
}

unsigned numberOfValuesInLoops(Procedure& proc, B3::Opcode opcode)
{
    B3::NaturalLoops& loops = proc.naturalLoops();
    unsigned result = 0;
    for (BasicBlock* block : proc) {
        if (loops.innerMostLoopOf(block))
            result += numberOfValues(block, opcode);
    }
    return result;
}

Vector<unsigned> numberOfValuesInEachLoop(Procedure& proc, B3::Opcode opcode)
{
    B3::NaturalLoops& loops = proc.naturalLoops();
    Vector<unsigned> result;
    for (unsigned loopIndex = 0; loopIndex < loops.numLoops(); ++loopIndex) {
        const B3::NaturalLoop& loop = loops.loop(loopIndex);
        unsigned number = 0;
        for (unsigned i = 0; i < loop.size(); ++i)
            number += numberOfValues(loop.at(i), opcode);
        result.append(number);
    }
    std::ranges::sort(result);
    return result;
}

void checkNumberOfValuesInEachLoop(Procedure& proc, B3::Opcode opcode, const Vector<unsigned>& expected)
{
    Vector<unsigned> found = numberOfValuesInEachLoop(proc, opcode);
    if (found == expected)
        return;
    crashLock.lock();
    dataLogLn("Expected ", listDump(expected), " of ", opcode, " in the loops but found ", listDump(found), ":\n", proc);
    CRASH();
}

struct TopTestedLoop {
    explicit TopTestedLoop(Graph& graph, B3::Opcode relation = LessThan, int64_t step = 1, int64_t firstIndex = 0)
        : graph(graph)
        , step(step)
        , root(graph.proc.addBlock())
        , header(graph.proc.addBlock())
        , body(graph.proc.addBlock())
        , exit(graph.proc.addBlock())
        , index(graph.proc.addVariable(Int64))
        , sum(graph.proc.addVariable(Int64))
    {
        auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(graph.proc, root);
        bound = arguments[0];
        pointer = arguments[1];
        extra = arguments[2];
        start = graph.int64(root, firstIndex);
        graph.set(root, index, start);
        graph.set(root, sum, graph.int64(root, 0));
        graph.jump(root, header);
        graph.branch(header, graph.make(header, relation, graph.get(header, index), bound), body, exit);
        graph.ret(exit, graph.get(exit, sum));
    }

    void endBody(BasicBlock* lastBlockOfBody)
    {
        graph.increase(lastBlockOfBody, index, step);
        graph.jump(lastBlockOfBody, header);
        graph.proc.resetReachability();
        fixSSA(graph.proc);
        validate(graph.proc);
    }

    void rotate()
    {
        CHECK(rotateLoops(graph.proc));
        fixSSA(graph.proc);
        validate(graph.proc);
    }

    Graph& graph;
    int64_t step;
    BasicBlock* root;
    BasicBlock* header;
    BasicBlock* body;
    BasicBlock* exit;
    Variable* index;
    Variable* sum;
    Value* bound;
    Value* pointer;
    Value* extra;
    Value* start;
};

int64_t run(Compilation& code, int64_t bound, int64_t* pointer = nullptr, int64_t extra = 0)
{
    return invoke<int64_t>(code, bound, pointer, extra);
}

int64_t sumOfIntegersBelow(int64_t bound) { return bound <= 0 ? 0 : bound * (bound - 1) / 2; }

void checkSumsIntegersBelowBound(Procedure& proc)
{
    auto code = compileProc(proc);
    for (int64_t bound : { -1, 0, 1, 2, 10, 1000 })
        CHECK_EQ(run(*code, bound), sumOfIntegersBelow(bound));
}

bool reduceStrengthOnce(Procedure& proc, bool isPositionIndependent)
{
    proc.resetReachability();
    proc.setOptLevel(2);
    proc.setPositionIndependent(isPositionIndependent);
    bool result = reduceStrength(proc, ReduceStrengthPass::Initial);
    proc.setPositionIndependent(false);
    validate(proc);
    return result;
}

bool rotateAndVersionLoops(Procedure& proc)
{
    Vector<BasicBlock*> headersOfVersionsTestedAtTheTop;
    bool result = versionLoops(proc, headersOfVersionsTestedAtTheTop);
    fixSSA(proc);
    validate(proc);
    return result;
}

struct PollState {
    int64_t limit { 0 };
    int64_t numberOfPolls { 0 };
    int64_t numberOfStops { 0 };
};

PatchpointValue* appendPollLimit(Graph& graph, BasicBlock* block, PollState& state)
{
    Procedure& proc = graph.proc;
    if (!proc.loopPollGenerator()) {
        proc.setLoopPollGenerator(createSharedTask<StackmapGeneratorFunction>([&state](CCallHelpers& jit, const StackmapGenerationParams& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            GPRReg result = params[0].gpr();
            jit.move(CCallHelpers::TrustedImmPtr(&state), result);
            jit.add64(CCallHelpers::TrustedImm32(1), CCallHelpers::Address(result, OBJECT_OFFSETOF(PollState, numberOfPolls)));
            jit.load64(CCallHelpers::Address(result, OBJECT_OFFSETOF(PollState, limit)), result);
        }));
    }
    PatchpointValue* limit = block->appendNew<PatchpointValue>(proc, Int64, Origin());
    limit->effects = Effects::none();
    limit->effects.reads = HeapRange::top();
    limit->effects.writesLocalState = true;
    limit->clobber(RegisterSet::macroClobberedGPRs());
    limit->setGenerator(RefPtr<StackmapGenerator>(proc.loopPollGenerator()));
    return limit;
}

BasicBlock* appendPoll(Graph& graph, BasicBlock* block, PollState& state)
{
    Procedure& proc = graph.proc;
    BasicBlock* slowPath = proc.addBlock();
    BasicBlock* continuation = proc.addBlock();
    PatchpointValue* limit = appendPollLimit(graph, block, state);
    graph.branch(block, graph.make(block, LessThan, limit, graph.int64(block, 0)), rarely(slowPath), FrequentedBlock(continuation));
    PatchpointValue* handler = slowPath->appendNew<PatchpointValue>(proc, Void, Origin());
    handler->effects = Effects::none();
    handler->effects.reads = HeapRange::top();
    handler->effects.writes = HeapRange::top();
    handler->numGPScratchRegisters = 1;
    handler->clobber(RegisterSet::macroClobberedGPRs());
    handler->setGenerator([&state](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        GPRReg scratch = params.gpScratch(0);
        jit.move(CCallHelpers::TrustedImmPtr(&state), scratch);
        jit.add64(CCallHelpers::TrustedImm32(1), CCallHelpers::Address(scratch, OBJECT_OFFSETOF(PollState, numberOfStops)));
        jit.store64(CCallHelpers::TrustedImm32(0), CCallHelpers::Address(scratch, OBJECT_OFFSETOF(PollState, limit)));
    });
    graph.jump(slowPath, continuation);
    return continuation;
}

bool holds(B3::Opcode relation, int64_t left, int64_t right)
{
    switch (relation) {
    case LessThan:
        return left < right;
    case LessEqual:
        return left <= right;
    case GreaterThan:
        return left > right;
    case GreaterEqual:
        return left >= right;
    case Below:
        return static_cast<uint64_t>(left) < static_cast<uint64_t>(right);
    case BelowEqual:
        return static_cast<uint64_t>(left) <= static_cast<uint64_t>(right);
    case Above:
        return static_cast<uint64_t>(left) > static_cast<uint64_t>(right);
    case AboveEqual:
        return static_cast<uint64_t>(left) >= static_cast<uint64_t>(right);
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return false;
    }
}

} // anonymous namespace

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
    CHECK_EQ(numberOfValuesInLoops(proc, Branch), 1u);
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
    CHECK_EQ(numberOfValuesInLoops(proc, Branch), 1u);
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
        for (unsigned i = 0; i < numberOfAdditions; ++i)
            tested = graph.make(header, Add, tested, bound);
        graph.branch(header, graph.make(header, LessThan, tested, graph.int64(header, 100)), body, exit);
        graph.increase(body, index, 1);
        graph.jump(body, header);
        graph.ret(exit, graph.get(exit, index));
        proc.resetReachability();
        fixSSA(proc);

        CHECK_EQ(rotateLoops(proc), numberOfAdditions == 10);
        fixSSA(proc);
        validate(proc);
        auto code = compileProc(proc);
        CHECK_EQ(invoke<int64_t>(*code, 0), 100);
        CHECK_EQ(invoke<int64_t>(*code, 100), 0);
    }
}

static void testRotateLoopsSkipsUncloneableTest()
{
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
    graph.emptyPatchpoint(header)->effects.writes = HeapRange::top();
    graph.branch(header, graph.make(header, LessThan, graph.get(header, index), bound), body, exit);
    graph.increase(body, index, 1);
    graph.jump(body, header);
    graph.ret(exit, graph.get(exit, index));
    proc.resetReachability();
    fixSSA(proc);

    CHECK(!rotateLoops(proc));
    CHECK_EQ(compileAndRun<int64_t>(proc, 5), 5);
}

static void testRotateLoopsSkipsRareBackEdge()
{
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop(graph);
    loop.body->setFrequency(0.01);
    graph.increase(loop.body, loop.sum, graph.get(loop.body, loop.index));
    loop.endBody(loop.body);

    CHECK(!rotateLoops(proc));
    checkSumsIntegersBelowBound(proc);
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

static void addIfComparisonHolds(Graph& graph, BasicBlock*& block, Variable* sum, Value* comparison, int64_t ifTrue, int64_t ifFalse)
{
    BasicBlock* holds = graph.proc.addBlock();
    BasicBlock* fails = graph.proc.addBlock();
    BasicBlock* next = graph.proc.addBlock();
    graph.branch(block, comparison, holds, fails);
    graph.increase(holds, sum, ifTrue);
    graph.jump(holds, next);
    graph.increase(fails, sum, ifFalse);
    graph.jump(fails, next);
    block = next;
}

static void testFoldComparisonsWithBounds()
{
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop(graph);
    BasicBlock* block = loop.body;
    addIfComparisonHolds(graph, block, loop.sum, graph.make(block, LessThan, graph.get(block, loop.index), loop.bound), 1, 1000);
    addIfComparisonHolds(graph, block, loop.sum, graph.make(block, GreaterEqual, graph.get(block, loop.index), loop.bound), 2000, 10);
    addIfComparisonHolds(graph, block, loop.sum, graph.make(block, GreaterThan, loop.bound, graph.get(block, loop.index)), 100, 3000);
    loop.endBody(block);
    loop.rotate();

    CHECK(foldComparisonsWithBounds(proc));
    validate(proc);
    CHECK_EQ(numberOfValuesInLoops(proc, LessThan), 1u);
    CHECK_EQ(numberOfValuesInLoops(proc, GreaterEqual), 0u);
    CHECK_EQ(numberOfValuesInLoops(proc, GreaterThan), 0u);
    reduceStrengthOnce(proc, false);
    CHECK(!hasInt64Constant(proc, 1000));
    CHECK(!hasInt64Constant(proc, 2000));
    CHECK(!hasInt64Constant(proc, 3000));
    CHECK_EQ(numberOfValuesInLoops(proc, Branch), 1u);
    auto code = compileProc(proc);
    for (int64_t bound : { -1, 0, 1, 7 })
        CHECK_EQ(run(*code, bound), std::max<int64_t>(bound, 0) * 111);
}

static void testFoldComparisonsWithBoundsKeepsUnrelatedComparisons()
{
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop(graph);
    BasicBlock* block = loop.body;
    addIfComparisonHolds(graph, block, loop.sum, graph.make(block, LessThan, graph.get(block, loop.index), loop.extra), 1, 1000);
    addIfComparisonHolds(graph, block, loop.sum, graph.make(block, LessEqual, graph.get(block, loop.index), loop.bound), 10, 2000);
    addIfComparisonHolds(graph, block, loop.sum, graph.make(block, LessThan, graph.make(block, Add, graph.get(block, loop.index), graph.int64(block, 1)), loop.bound), 100, 3000);
    addIfComparisonHolds(graph, block, loop.sum, graph.make(block, Below, graph.get(block, loop.index), loop.bound), 10000, 4000);
    loop.endBody(block);
    loop.rotate();

    CHECK(!foldComparisonsWithBounds(proc));
    auto code = compileProc(proc);
    CHECK_EQ(run(*code, 3, nullptr, 2), (1 + 10 + 100 + 10000) + (1 + 10 + 100 + 10000) + (1000 + 10 + 3000 + 10000));
}

static void testFoldComparisonsWithBoundsRequiresRelationOnEntry()
{
    Procedure proc;
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
    graph.increase(block, index, 1);
    graph.branch(block, graph.make(block, LessThan, graph.get(block, index), bound), body, exit);
    graph.ret(exit, graph.get(exit, sum));
    proc.resetReachability();
    fixSSA(proc);
    validate(proc);

    CHECK(!foldComparisonsWithBounds(proc));
    auto code = compileProc(proc);
    CHECK_EQ(invoke<int64_t>(*code, 3, 0), 3);
    CHECK_EQ(invoke<int64_t>(*code, 3, 5), 1000);
}

static void testAnalyzeCountedLoop()
{
    struct Case {
        B3::Opcode relation;
        int64_t step;
        bool isCounted;
    };
    for (Case testCase : { Case { LessThan, 1, true }, Case { LessEqual, 3, true }, Case { Below, 1, true }, Case { GreaterThan, -1, true }, Case { AboveEqual, -2, true },
        Case { LessThan, -1, false }, Case { GreaterThan, 1, false }, Case { NotEqual, 1, false }, Case { LessThan, maxIterationsPerStrip + 1, false } }) {
        Procedure proc;
        Graph graph { proc };
        TopTestedLoop loop(graph, testCase.relation, testCase.step);
        graph.increase(loop.body, loop.sum, 1);
        loop.endBody(loop.body);
        loop.rotate();

        B3::NaturalLoops& loops = proc.naturalLoops();
        CHECK_EQ(loops.numLoops(), 1u);
        proc.resetValueOwners();
        UpsilonsByPhi upsilonsByPhi(proc);
        auto counted = analyzeCountedLoop(loops, loops.loop(0), upsilonsByPhi, [&](Value* value) {
            return !loops.belongsTo(value->owner, loops.loop(0));
        });
        CHECK_EQ(!!counted, testCase.isCounted);
        if (!counted)
            continue;
        CHECK(counted->header == loop.body);
        CHECK(counted->relation == testCase.relation);
        CHECK_EQ(counted->step, testCase.step);
        CHECK(counted->bound == loop.bound);
        CHECK(counted->start->isInt64(0));
        CHECK(counted->inductionVariable->opcode() == Phi);
        CHECK(counted->next->opcode() == Add);
        CHECK(!counted->wrapsAroundInt32);
        CHECK(!counted->saturation);
        CHECK_EQ(counted->increases(), testCase.step > 0);
        CHECK(relationHoldsOnEntry(*counted));
    }
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
        Value* handedAround = arguments[0];
        Value* other = arguments[1];
        Value* bound = arguments[2];

        Value* atHeader = graph.phi(header, Int64);
        Value* index = graph.phi(header, Int64);
        Value* atJoin = graph.phi(join, Int64);
        graph.upsilon(root, handedAround, atHeader);
        graph.upsilon(root, graph.int64(root, 0), index);
        graph.jump(root, header);
        graph.branch(header, graph.make(header, BitAnd, index, graph.int64(header, 1)), left, right);
        graph.upsilon(left, atHeader, atJoin);
        graph.jump(left, join);
        graph.upsilon(right, isRedundant ? handedAround : other, atJoin);
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

template<typename MakeLeft, typename MakeRight>
static bool narrowsPhiOf(const MakeLeft& makeLeft, const MakeRight& makeRight, int64_t expectedOnLeft, int64_t expectedOnRight)
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* left = proc.addBlock();
    BasicBlock* right = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t, int32_t, int64_t>(proc, root);
    Value* condition = arguments[0];
    Value* word = arguments[1];
    Value* wide = arguments[2];
    Value* phi = graph.phi(join, Int64);
    graph.branch(root, condition, left, right);
    graph.upsilon(left, makeLeft(graph, left, word, wide), phi);
    graph.jump(left, join);
    graph.upsilon(right, makeRight(graph, right, word, wide), phi);
    graph.jump(right, join);
    graph.ret(join, phi);
    proc.resetReachability();

    bool result = narrowPhisOfExtendedWords(proc);
    validate(proc);
    CHECK_EQ(numberOfValues(proc, [](Value* value) {
        return value->opcode() == Phi && value->type() == Int32;
    }), result ? 1u : 0u);
    auto code = compileProc(proc);
    CHECK_EQ(invoke<int64_t>(*code, 1, -2, 1ll << 40), expectedOnLeft);
    CHECK_EQ(invoke<int64_t>(*code, 0, -2, 1ll << 40), expectedOnRight);
    return result;
}

static void testNarrowPhisOfExtendedWords()
{
    auto signExtended = [](Graph& graph, BasicBlock* block, Value* word, Value*) {
        return graph.make(block, SExt32, word);
    };
    auto zeroExtended = [](Graph& graph, BasicBlock* block, Value* word, Value*) {
        return graph.make(block, ZExt32, word);
    };
    auto wideValue = [](Graph&, BasicBlock*, Value*, Value* wide) {
        return wide;
    };
    auto constant = [](int64_t value) {
        return [value](Graph& graph, BasicBlock* block, Value*, Value*) {
            return graph.int64(block, value);
        };
    };
    constexpr int64_t zeroExtendedWord = 0xfffffffell;

    CHECK(narrowsPhiOf(signExtended, signExtended, -2, -2));
    CHECK(narrowsPhiOf(zeroExtended, zeroExtended, zeroExtendedWord, zeroExtendedWord));
    CHECK(narrowsPhiOf(signExtended, constant(-5), -2, -5));
    CHECK(narrowsPhiOf(signExtended, constant(std::numeric_limits<int32_t>::min()), -2, std::numeric_limits<int32_t>::min()));
    CHECK(narrowsPhiOf(zeroExtended, constant(0xffffffffll), zeroExtendedWord, 0xffffffffll));

    CHECK(!narrowsPhiOf(signExtended, zeroExtended, -2, zeroExtendedWord));
    CHECK(!narrowsPhiOf(signExtended, wideValue, -2, 1ll << 40));
    CHECK(!narrowsPhiOf(signExtended, constant(0x80000000ll), -2, 0x80000000ll));
    CHECK(!narrowsPhiOf(zeroExtended, constant(-1), zeroExtendedWord, -1));
    CHECK(!narrowsPhiOf(zeroExtended, constant(1ll << 32), zeroExtendedWord, 1ll << 32));
    CHECK(!narrowsPhiOf(constant(1), constant(2), 1, 2));
}

static bool foldsComparisonWithConstant(B3::Opcode comparison, B3::Opcode extension, int64_t constant, bool constantIsFirst)
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    Value* extended = graph.make(root, extension, cCallArgumentValues<int32_t>(proc, root)[0]);
    Value* other = graph.int64(root, constant);
    graph.ret(root, constantIsFirst ? graph.make(root, comparison, other, extended) : graph.make(root, comparison, extended, other));
    proc.resetReachability();

    bool result = foldComparisonsWithWhatIsOutOfRange(proc);
    validate(proc);
    auto code = compileProc(proc);
    for (int32_t word : { 0, 1, -1, 5, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max() }) {
        int64_t wide = extension == SExt32 ? static_cast<int64_t>(word) : static_cast<int64_t>(static_cast<uint32_t>(word));
        CHECK_EQ(!!invoke<int32_t>(*code, word), comparison == Equal ? wide == constant : wide != constant);
    }
    return result;
}

static void testFoldComparisonsWithWhatIsOutOfRange()
{
    for (B3::Opcode comparison : { Equal, NotEqual }) {
        for (bool constantIsFirst : { false, true }) {
            CHECK(foldsComparisonWithConstant(comparison, SExt32, 1ll << 40, constantIsFirst));
            CHECK(foldsComparisonWithConstant(comparison, SExt32, 0x80000000ll, constantIsFirst));
            CHECK(foldsComparisonWithConstant(comparison, SExt32, static_cast<int64_t>(std::numeric_limits<int32_t>::min()) - 1, constantIsFirst));
            CHECK(foldsComparisonWithConstant(comparison, ZExt32, -1, constantIsFirst));
            CHECK(foldsComparisonWithConstant(comparison, ZExt32, 1ll << 32, constantIsFirst));

            CHECK(!foldsComparisonWithConstant(comparison, SExt32, 5, constantIsFirst));
            CHECK(!foldsComparisonWithConstant(comparison, SExt32, -1, constantIsFirst));
            CHECK(!foldsComparisonWithConstant(comparison, SExt32, std::numeric_limits<int32_t>::min(), constantIsFirst));
            CHECK(!foldsComparisonWithConstant(comparison, ZExt32, 0xffffffffll, constantIsFirst));
            CHECK(!foldsComparisonWithConstant(comparison, ZExt32, 0, constantIsFirst));
        }
    }
}


template<typename Functor>
static void buildRotatedLoopWithInvariantGuard(Graph& graph, TopTestedLoop& loop, const Functor& fillHotBlock)
{
    BasicBlock* rare = graph.proc.addBlock();
    BasicBlock* hot = graph.proc.addBlock();
    Value* loaded = graph.load(loop.body, Int64, loop.pointer, 0, firstHeap);
    graph.branch(loop.body, graph.make(loop.body, Equal, loaded, loop.extra), FrequentedBlock(hot), rarely(rare));
    graph.increase(rare, loop.sum, 1000);
    graph.jump(rare, hot);
    graph.increase(hot, loop.sum, loaded);
    fillHotBlock(hot);
    loop.endBody(hot);
    loop.rotate();
}

static void checkLoopWithInvariantGuard(Procedure& proc)
{
    auto code = compileProc(proc);
    for (int64_t bound : { -1, 0, 1, 2, 9 }) {
        int64_t turns = std::max<int64_t>(bound, 0);
        int64_t memory[2] = { 7, 0 };
        CHECK_EQ(run(*code, bound, memory, 7), turns * 7);
        CHECK_EQ(run(*code, bound, memory, 8), turns * 1007);
    }
}

static void testVersionLoopsHoistsLoadAndGuard()
{
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop(graph);
    buildRotatedLoopWithInvariantGuard(graph, loop, [](BasicBlock*) { });
    checkNumberOfValuesInEachLoop(proc, Load, { 1 });

    CHECK(rotateAndVersionLoops(proc));
    checkNumberOfValuesInEachLoop(proc, Load, { 0, 1 });
    checkNumberOfValuesInEachLoop(proc, Branch, { 1, 2 });
    checkLoopWithInvariantGuard(proc);
}

static void testVersionLoopsHoistsLoadPastStoreToDisjointHeap()
{
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop(graph);
    buildRotatedLoopWithInvariantGuard(graph, loop, [&](BasicBlock* block) {
        graph.store(block, graph.get(block, loop.sum), loop.pointer, 8, secondHeap);
    });

    CHECK(rotateAndVersionLoops(proc));
    checkNumberOfValuesInEachLoop(proc, Load, { 0, 1 });
    checkNumberOfValuesInEachLoop(proc, Store, { 1, 1 });
    checkLoopWithInvariantGuard(proc);
}

static void testVersionLoopsKeepsLoadClobberedByLoop()
{
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop(graph);
    buildRotatedLoopWithInvariantGuard(graph, loop, [&](BasicBlock* block) {
        graph.store(block, graph.get(block, loop.sum), loop.pointer, 8, bothHeaps);
    });

    CHECK(!rotateAndVersionLoops(proc));
    checkLoopWithInvariantGuard(proc);
}

static void testVersionLoopsSkipsLoopWithCall()
{
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop(graph);
    buildRotatedLoopWithInvariantGuard(graph, loop, [&](BasicBlock* block) {
        graph.callLikePatchpoint(block);
    });

    CHECK(!rotateAndVersionLoops(proc));
    checkLoopWithInvariantGuard(proc);
}

static void testVersionLoopsSkipsLargeLoop()
{
    for (unsigned numberOfOperations : { 10u, 60u }) {
        Procedure proc;
        Graph graph { proc };
        TopTestedLoop loop(graph);
        buildRotatedLoopWithInvariantGuard(graph, loop, [&](BasicBlock* block) {
            Value* value = graph.get(block, loop.index);
            for (unsigned i = 0; i < numberOfOperations; ++i)
                value = graph.make(block, BitXor, value, graph.get(block, loop.index));
            graph.increase(block, loop.sum, graph.make(block, BitAnd, value, graph.int64(block, 0)));
        });

        CHECK_EQ(rotateAndVersionLoops(proc), numberOfOperations == 10);
        checkLoopWithInvariantGuard(proc);
    }
}

static void testVersionLoopsSkipsLoopWithNothingToHoist()
{
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop(graph);
    graph.increase(loop.body, loop.sum, graph.get(loop.body, loop.index));
    loop.endBody(loop.body);
    loop.rotate();

    CHECK(!rotateAndVersionLoops(proc));
    checkSumsIntegersBelowBound(proc);
}

static void testVersionLoopsOnlyInnermostLoops()
{
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop outer(graph);
    BasicBlock* innerHeader = proc.addBlock();
    BasicBlock* innerBody = proc.addBlock();
    BasicBlock* outerLatch = proc.addBlock();
    Variable* innerIndex = proc.addVariable(Int64);
    Value* loaded = graph.load(outer.body, Int64, outer.pointer, 0, firstHeap);
    graph.increase(outer.body, outer.sum, graph.make(outer.body, Mul, loaded, outer.extra));
    graph.set(outer.body, innerIndex, graph.int64(outer.body, 0));
    graph.jump(outer.body, innerHeader);
    graph.branch(innerHeader, graph.make(innerHeader, LessThan, graph.get(innerHeader, innerIndex), graph.int64(innerHeader, 2)), innerBody, outerLatch);
    graph.callLikePatchpoint(innerBody);
    graph.increase(innerBody, innerIndex, 1);
    graph.jump(innerBody, innerHeader);
    outer.endBody(outerLatch);
    outer.rotate();

    CHECK(!rotateAndVersionLoops(proc));
    int64_t memory[1] = { 5 };
    CHECK_EQ(run(*compileProc(proc), 3, memory, 2), 30);
}

static void testVersionLoopsRareEdgeContinuesInOriginalLoop()
{
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop(graph);
    BasicBlock* special = proc.addBlock();
    BasicBlock* hot = proc.addBlock();
    Value* increment = graph.make(loop.body, Add, graph.load(loop.body, Int64, loop.pointer, 0, firstHeap), graph.int64(loop.body, 1));
    graph.branch(loop.body, graph.make(loop.body, Equal, graph.get(loop.body, loop.index), loop.extra), rarely(special), FrequentedBlock(hot));
    graph.increase(special, loop.sum, 1000);
    graph.jump(special, hot);
    graph.increase(hot, loop.sum, increment);
    loop.endBody(hot);
    loop.rotate();

    CHECK(rotateAndVersionLoops(proc));
    CHECK_EQ(numberOfValuesInEachLoop(proc, Load).first(), 0u);
    CHECK_EQ(numberOfValues(proc, Load), 2u);
    auto code = compileProc(proc);
    int64_t memory[1] = { 5 };
    for (int64_t special : { -1, 0, 4, 9, 10 })
        CHECK_EQ(run(*code, 10, memory, special), 60 + (special >= 0 && special < 10 ? 1000 : 0));
}

static void testVersionLoopsUnswitchesInvariantBranch()
{
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop(graph);
    BasicBlock* odd = proc.addBlock();
    BasicBlock* even = proc.addBlock();
    BasicBlock* next = proc.addBlock();
    Value* isOdd = graph.make(loop.body, NotEqual, graph.make(loop.body, BitAnd, loop.extra, graph.int64(loop.body, 1)), graph.int64(loop.body, 0));
    graph.branch(loop.body, isOdd, odd, even);
    graph.increase(odd, loop.sum, graph.get(odd, loop.index));
    graph.jump(odd, next);
    graph.increase(even, loop.sum, 1);
    graph.jump(even, next);
    loop.endBody(next);
    loop.rotate();

    CHECK(rotateAndVersionLoops(proc));
    checkNumberOfValuesInEachLoop(proc, Branch, { 1, 1, 2 });
    auto code = compileProc(proc);
    for (int64_t bound : { 0, 1, 10 }) {
        CHECK_EQ(run(*code, bound, nullptr, 3), sumOfIntegersBelow(bound));
        CHECK_EQ(run(*code, bound, nullptr, 4), bound);
    }
}

static void testVersionLoopsKeepsVariantBranch()
{
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop(graph);
    BasicBlock* odd = proc.addBlock();
    BasicBlock* even = proc.addBlock();
    BasicBlock* next = proc.addBlock();
    Value* isOdd = graph.make(loop.body, NotEqual, graph.make(loop.body, BitAnd, graph.get(loop.body, loop.index), graph.int64(loop.body, 1)), graph.int64(loop.body, 0));
    graph.branch(loop.body, isOdd, odd, even);
    graph.increase(odd, loop.sum, 100);
    graph.jump(odd, next);
    graph.increase(even, loop.sum, 1);
    graph.jump(even, next);
    loop.endBody(next);
    loop.rotate();

    CHECK(!rotateAndVersionLoops(proc));
    CHECK_EQ(run(*compileProc(proc), 5), 203);
}

static void testVersionLoopsStripMinesPollInFastVersion()
{
    Procedure proc;
    Graph graph { proc };
    PollState state;
    TopTestedLoop loop(graph);
    BasicBlock* rare = proc.addBlock();
    BasicBlock* hot = proc.addBlock();
    BasicBlock* block = appendPoll(graph, loop.body, state);
    Value* loaded = graph.load(block, Int64, loop.pointer, 0, firstHeap);
    graph.branch(block, graph.make(block, Equal, loaded, loop.extra), FrequentedBlock(hot), rarely(rare));
    graph.increase(rare, loop.sum, 1000);
    graph.jump(rare, hot);
    graph.increase(hot, loop.sum, loaded);
    loop.endBody(hot);
    loop.rotate();

    CHECK(rotateAndVersionLoops(proc));
    CHECK(!stripMineLoops(proc));
    auto code = compileProc(proc);
    int64_t memory[1] = { 7 };
    constexpr int64_t turns = 3 * maxIterationsPerStrip + 100;

    CHECK_EQ(run(*code, turns, memory, 7), turns * 7);
    CHECK_EQ(state.numberOfPolls, 3);
    CHECK_EQ(state.numberOfStops, 0);

    state = { };
    CHECK_EQ(run(*code, 100, memory, 7), 700);
    CHECK_EQ(state.numberOfPolls, 0);

    state = { };
    CHECK_EQ(run(*code, 100, memory, 8), 100 * 1007);
    CHECK_EQ(state.numberOfPolls, 100);

    state = { };
    state.limit = -1;
    CHECK_EQ(run(*code, turns, memory, 7), turns * 7);
    CHECK_EQ(state.numberOfStops, 1);
    CHECK_EQ(state.limit, 0);
    CHECK_EQ(state.numberOfPolls, 1 + turns - maxIterationsPerStrip);
}

static void buildRotatedLoopWithPoll(Graph& graph, TopTestedLoop& loop, PollState& state)
{
    BasicBlock* block = appendPoll(graph, loop.body, state);
    graph.increase(block, loop.sum, graph.get(block, loop.index));
    loop.endBody(block);
    loop.rotate();
}

static void testStripMineLoopsPollsOncePerStrip()
{
    Procedure proc;
    Graph graph { proc };
    PollState state;
    TopTestedLoop loop(graph);
    buildRotatedLoopWithPoll(graph, loop, state);

    CHECK(stripMineLoops(proc));
    validate(proc);
    CHECK(!stripMineLoops(proc));
    auto code = compileProc(proc);
    struct Case {
        int64_t bound;
        int64_t numberOfPolls;
    };
    for (Case testCase : { Case { -1, 0 }, Case { 0, 0 }, Case { 1, 0 }, Case { 100, 0 }, Case { maxIterationsPerStrip, 0 }, Case { maxIterationsPerStrip + 1, 1 }, Case { 3 * maxIterationsPerStrip + 100, 3 } }) {
        state = { };
        CHECK_EQ(run(*code, testCase.bound), sumOfIntegersBelow(testCase.bound));
        CHECK_EQ(state.numberOfPolls, testCase.numberOfPolls);
        CHECK_EQ(state.numberOfStops, 0);
    }

    state = { };
    state.limit = -1;
    CHECK_EQ(run(*code, 3 * maxIterationsPerStrip + 100), sumOfIntegersBelow(3 * maxIterationsPerStrip + 100));
    CHECK_EQ(state.numberOfPolls, 3);
    CHECK_EQ(state.numberOfStops, 1);
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
        Procedure proc;
        Graph graph { proc };
        PollState state;
        TopTestedLoop loop(graph, testCase.relation, testCase.step, testCase.firstIndex);
        buildRotatedLoopWithPoll(graph, loop, state);

        CHECK(stripMineLoops(proc));
        validate(proc);
        uint64_t expected = 0;
        int64_t turns = 0;
        for (int64_t index = testCase.firstIndex; holds(testCase.relation, index, testCase.bound); index += testCase.step) {
            expected += static_cast<uint64_t>(index);
            ++turns;
        }
        CHECK_EQ(static_cast<uint64_t>(run(*compileProc(proc), testCase.bound)), expected);
        CHECK(state.numberOfPolls <= turns / 1000);
        CHECK(state.numberOfPolls >= turns * std::abs(testCase.step) / maxIterationsPerStrip - 1);
    }
}

static void testStripMineLoopsRequiresPollGenerator()
{
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop(graph);
    graph.increase(loop.body, loop.sum, graph.get(loop.body, loop.index));
    loop.endBody(loop.body);
    loop.rotate();

    CHECK(!stripMineLoops(proc));
    checkSumsIntegersBelowBound(proc);
}

static void testStripMineLoopsSkipsLoopWithTwoPolls()
{
    Procedure proc;
    Graph graph { proc };
    PollState state;
    TopTestedLoop loop(graph);
    BasicBlock* block = appendPoll(graph, appendPoll(graph, loop.body, state), state);
    graph.increase(block, loop.sum, graph.get(block, loop.index));
    loop.endBody(block);
    loop.rotate();

    CHECK(!stripMineLoops(proc));
    CHECK_EQ(run(*compileProc(proc), 10), 45);
    CHECK_EQ(state.numberOfPolls, 20);
}

static void testStripMineLoopsSkipsVariantBound()
{
    Procedure proc;
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
    graph.branch(header, graph.make(header, LessThan, graph.get(header, index), graph.load(header, Int64, pointer, 0, firstHeap)), body, exit);
    BasicBlock* block = appendPoll(graph, body, state);
    graph.increase(block, index, 1);
    graph.jump(block, header);
    graph.ret(exit, graph.get(exit, index));
    proc.resetReachability();
    fixSSA(proc);
    CHECK(rotateLoops(proc));
    fixSSA(proc);
    validate(proc);

    CHECK(!stripMineLoops(proc));
    int64_t memory[1] = { 10 };
    CHECK_EQ(invoke<int64_t>(*compileProc(proc), memory), 10);
    CHECK_EQ(state.numberOfPolls, 10);
}

static void testStripMineLoopsSkipsPollWithSharedLimit()
{
    Procedure proc;
    Graph graph { proc };
    PollState state;
    TopTestedLoop loop(graph);
    BasicBlock* slowPath = proc.addBlock();
    BasicBlock* continuation = proc.addBlock();
    Value* limit = appendPollLimit(graph, loop.body, state);
    graph.branch(loop.body, graph.make(loop.body, LessThan, limit, graph.int64(loop.body, 0)), rarely(slowPath), FrequentedBlock(continuation));
    graph.callLikePatchpoint(slowPath);
    graph.jump(slowPath, continuation);
    graph.increase(continuation, loop.sum, limit);
    loop.endBody(continuation);
    loop.rotate();

    CHECK(!stripMineLoops(proc));
    state.limit = 3;
    CHECK_EQ(run(*compileProc(proc), 10), 30);
    CHECK_EQ(state.numberOfPolls, 10);
}

struct BranchToRareBlocks {
    explicit BranchToRareBlocks(Graph& graph, double frequencyOfRareBlocks = 0.01)
        : root(graph.proc.addBlock())
        , firstRare(graph.proc.addBlock(frequencyOfRareBlocks))
        , next(graph.proc.addBlock())
        , secondRare(graph.proc.addBlock(frequencyOfRareBlocks))
        , hot(graph.proc.addBlock())
    {
        auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t*>(graph.proc, root);
        selector = arguments[0];
        operand = arguments[1];
        pointer = arguments[2];
    }

    void finish(Graph& graph, Value* inFirstRare, Value* inSecondRare, Value* onHotPath)
    {
        graph.branch(root, graph.make(root, Equal, selector, graph.int64(root, 1)), rarely(firstRare), FrequentedBlock(next));
        graph.ret(firstRare, inFirstRare);
        graph.branch(next, graph.make(next, Equal, selector, graph.int64(next, 2)), rarely(secondRare), FrequentedBlock(hot));
        graph.ret(secondRare, inSecondRare);
        graph.ret(hot, onHotPath);
        graph.proc.resetReachability();
        validate(graph.proc);
    }

    BasicBlock* root;
    BasicBlock* firstRare;
    BasicBlock* next;
    BasicBlock* secondRare;
    BasicBlock* hot;
    Value* selector;
    Value* operand;
    Value* pointer;
};

static void testSinkValuesMovesValueIntoRareUser()
{
    Procedure proc;
    Graph graph { proc };
    BranchToRareBlocks blocks(graph);
    Value* product = graph.make(blocks.root, Mul, blocks.operand, blocks.operand);
    Value* sum = graph.make(blocks.root, Add, product, blocks.selector);
    blocks.finish(graph, sum, blocks.operand, blocks.operand);

    CHECK(sinkValues(proc));
    validate(proc);
    CHECK_EQ(numberOfValues(blocks.root, Mul), 0u);
    CHECK_EQ(numberOfValues(blocks.root, Add), 0u);
    CHECK_EQ(numberOfValues(blocks.firstRare, Mul), 1u);
    CHECK_EQ(numberOfValues(blocks.firstRare, Add), 1u);
    auto code = compileProc(proc);
    CHECK_EQ(invoke<int64_t>(*code, 1, 5, nullptr), 26);
    CHECK_EQ(invoke<int64_t>(*code, 2, 5, nullptr), 5);
    CHECK_EQ(invoke<int64_t>(*code, 3, 5, nullptr), 5);
}

static void testSinkValuesClonesValueForTwoRareUsers()
{
    Procedure proc;
    Graph graph { proc };
    BranchToRareBlocks blocks(graph);
    Value* product = graph.make(blocks.root, Mul, blocks.operand, blocks.operand);
    blocks.finish(graph, product, graph.make(blocks.secondRare, Add, product, product), blocks.operand);

    CHECK(sinkValues(proc));
    validate(proc);
    CHECK_EQ(numberOfValues(blocks.root, Mul), 0u);
    CHECK_EQ(numberOfValues(blocks.next, Mul), 0u);
    CHECK_EQ(numberOfValues(blocks.firstRare, Mul), 1u);
    CHECK_EQ(numberOfValues(blocks.secondRare, Mul), 1u);
    auto code = compileProc(proc);
    CHECK_EQ(invoke<int64_t>(*code, 1, 5, nullptr), 25);
    CHECK_EQ(invoke<int64_t>(*code, 2, 5, nullptr), 50);
    CHECK_EQ(invoke<int64_t>(*code, 3, 5, nullptr), 5);
}

static void testSinkValuesKeepsValueUsedOnHotPath()
{
    Procedure proc;
    Graph graph { proc };
    BranchToRareBlocks blocks(graph);
    Value* product = graph.make(blocks.root, Mul, blocks.operand, blocks.operand);
    blocks.finish(graph, product, blocks.operand, product);

    CHECK(!sinkValues(proc));
    CHECK_EQ(numberOfValues(blocks.root, Mul), 1u);
    CHECK_EQ(numberOfValues(proc, Mul), 1u);
}

static void testSinkValuesKeepsValueWhoseUserIsAsFrequent()
{
    Procedure proc;
    Graph graph { proc };
    BranchToRareBlocks blocks(graph, 0.75);
    Value* product = graph.make(blocks.root, Mul, blocks.operand, blocks.operand);
    blocks.finish(graph, product, blocks.operand, blocks.operand);

    CHECK(!sinkValues(proc));
    CHECK_EQ(numberOfValues(blocks.root, Mul), 1u);
}

static void testSinkValuesKeepsValueUsedInSameBlock()
{
    Procedure proc;
    Graph graph { proc };
    BranchToRareBlocks blocks(graph);
    Value* product = graph.make(blocks.root, Mul, blocks.operand, blocks.operand);
    graph.store(blocks.root, product, blocks.pointer, 0, firstHeap);
    blocks.finish(graph, product, blocks.operand, blocks.operand);

    CHECK(!sinkValues(proc));
    CHECK_EQ(numberOfValues(blocks.root, Mul), 1u);
}

static void testSinkValuesKeepsLoads()
{
    Procedure proc;
    Graph graph { proc };
    BranchToRareBlocks blocks(graph);
    Value* loaded = graph.load(blocks.root, Int64, blocks.pointer, 0, firstHeap);
    graph.store(blocks.root, blocks.operand, blocks.pointer, 0, firstHeap);
    blocks.finish(graph, loaded, blocks.operand, blocks.operand);

    CHECK(!sinkValues(proc));
    CHECK_EQ(numberOfValues(blocks.root, Load), 1u);
    int64_t memory[1] = { 11 };
    CHECK_EQ(compileAndRun<int64_t>(proc, 1, 5, memory), 11);
}

static void testSinkValuesKeepsOpaque()
{
    Procedure proc;
    Graph graph { proc };
    BranchToRareBlocks blocks(graph);
    Value* opaque = graph.make(blocks.root, Opaque, blocks.operand);
    blocks.finish(graph, opaque, blocks.operand, blocks.operand);

    CHECK(!sinkValues(proc));
    CHECK_EQ(numberOfValues(blocks.root, Opaque), 1u);
}

static void testSinkValuesDoesNotSinkIntoLoop()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* preHeader = proc.addBlock(0.01);
    BasicBlock* loop = proc.addBlock(0.1);
    BasicBlock* exit = proc.addBlock(0.01);
    BasicBlock* hot = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root);
    Value* bound = arguments[0];
    Value* operand = arguments[1];
    Value* product = graph.make(root, Mul, operand, operand);
    Value* index = graph.phi(loop, Int64);
    graph.branch(root, graph.make(root, GreaterThan, bound, graph.int64(root, 0)), rarely(preHeader), FrequentedBlock(hot));
    graph.upsilon(preHeader, graph.int64(preHeader, 0), index);
    graph.jump(preHeader, loop);
    Value* next = graph.make(loop, Add, index, product);
    graph.upsilon(loop, next, index);
    graph.branch(loop, graph.make(loop, LessThan, next, bound), loop, exit);
    graph.ret(exit, next);
    graph.ret(hot, operand);
    proc.resetReachability();
    validate(proc);

    CHECK(sinkValues(proc));
    validate(proc);
    CHECK_EQ(numberOfValues(root, Mul), 0u);
    CHECK_EQ(numberOfValues(preHeader, Mul), 1u);
    CHECK_EQ(numberOfValues(loop, Mul), 0u);
    auto code = compileProc(proc);
    CHECK_EQ(invoke<int64_t>(*code, 10, 2), 12);
    CHECK_EQ(invoke<int64_t>(*code, 0, 2), 2);
}

static void testSinkValuesDoesNotSinkFromPreHeaderIntoLoop()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* loop = proc.addBlock(0.1);
    BasicBlock* exit = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root);
    Value* bound = arguments[0];
    Value* product = graph.make(root, Mul, arguments[1], arguments[1]);
    Value* index = graph.phi(loop, Int64);
    graph.upsilon(root, graph.int64(root, 0), index);
    graph.jump(root, loop);
    Value* next = graph.make(loop, Add, index, product);
    graph.upsilon(loop, next, index);
    graph.branch(loop, graph.make(loop, LessThan, next, bound), loop, exit);
    graph.ret(exit, next);
    proc.resetReachability();

    CHECK(!sinkValues(proc));
    CHECK_EQ(numberOfValues(root, Mul), 1u);
}

static void testCycleForestNestedLoops()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* outerHeader = proc.addBlock();
    BasicBlock* innerHeader = proc.addBlock();
    BasicBlock* innerBody = proc.addBlock();
    BasicBlock* outerLatch = proc.addBlock();
    BasicBlock* selfLoop = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Value* condition = cCallArgumentValues<int32_t>(proc, root)[0];
    graph.jump(root, outerHeader);
    graph.branch(outerHeader, condition, innerHeader, selfLoop);
    graph.branch(innerHeader, condition, innerBody, outerLatch);
    graph.jump(innerBody, innerHeader);
    graph.jump(outerLatch, outerHeader);
    graph.branch(selfLoop, condition, selfLoop, exit);
    graph.ret(exit, condition);
    proc.resetReachability();

    CycleForest forest(proc);
    CHECK_EQ(forest.size(), 3u);
    unsigned outer = forest.innermost(outerHeader);
    unsigned inner = forest.innermost(innerHeader);
    unsigned self = forest.innermost(selfLoop);
    CHECK(outer != CycleForest::none && inner != CycleForest::none && self != CycleForest::none);
    CHECK(outer != inner && outer != self && inner != self);
    CHECK_EQ(forest.innermost(root), CycleForest::none);
    CHECK_EQ(forest.innermost(exit), CycleForest::none);
    CHECK_EQ(forest.innermost(innerBody), inner);
    CHECK_EQ(forest.innermost(outerLatch), outer);
    CHECK(forest[outer].header() == outerHeader && forest[inner].header() == innerHeader && forest[self].header() == selfLoop);
    CHECK(forest[outer].isReducible() && forest[inner].isReducible() && forest[self].isReducible());
    CHECK_EQ(forest[outer].parent, CycleForest::none);
    CHECK_EQ(forest[inner].parent, outer);
    CHECK_EQ(forest[self].parent, CycleForest::none);
    CHECK_EQ(forest.depth(outer), 1u);
    CHECK_EQ(forest.depth(inner), 2u);
    CHECK_EQ(forest.depth(CycleForest::none), 0u);
    CHECK(forest.isHeader(outerHeader) && forest.isHeader(innerHeader) && forest.isHeader(selfLoop));
    CHECK(!forest.isHeader(root) && !forest.isHeader(innerBody) && !forest.isHeader(outerLatch));
    CHECK(forest.contains(outer, innerBody) && forest.contains(outer, outerLatch) && forest.contains(inner, innerBody));
    CHECK(!forest.contains(inner, outerLatch) && !forest.contains(outer, selfLoop) && !forest.contains(outer, root));
    CHECK_EQ(forest.nearestCommon(inner, outer), outer);
    CHECK_EQ(forest.nearestCommon(inner, self), CycleForest::none);

    auto& postOrder = forest.blocksInPostOrder();
    CHECK_EQ(postOrder.size(), 7u);
    CHECK(postOrder.last() == root);
    for (BasicBlock* block : proc)
        CHECK(postOrder.contains(block));
    CHECK(postOrder.find(exit) < postOrder.find(selfLoop));
    CHECK(postOrder.find(selfLoop) < postOrder.find(outerHeader));
    CHECK(postOrder.find(innerBody) < postOrder.find(innerHeader));
    CHECK(postOrder.find(outerLatch) < postOrder.find(innerHeader));
    CHECK(postOrder.find(innerHeader) < postOrder.find(outerHeader));
}

static void testCycleForestAcyclic()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* left = proc.addBlock();
    BasicBlock* right = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    Value* condition = cCallArgumentValues<int32_t>(proc, root)[0];
    graph.branch(root, condition, left, right);
    graph.jump(left, join);
    graph.jump(right, join);
    graph.ret(join, condition);
    proc.resetReachability();

    CycleForest forest(proc);
    CHECK_EQ(forest.size(), 0u);
    for (BasicBlock* block : proc) {
        CHECK_EQ(forest.innermost(block), CycleForest::none);
        CHECK(!forest.isHeader(block));
    }
    CHECK_EQ(forest.blocksInPostOrder().size(), 4u);
    CHECK(forest.blocksInPostOrder().first() == join);
    CHECK(forest.blocksInPostOrder().last() == root);
}

struct CycleWithTwoEntries {
    CycleWithTwoEntries(Graph& graph, FrequencyClass frequencyOfFirstEntry)
        : root(graph.proc.addBlock())
        , first(graph.proc.addBlock())
        , second(graph.proc.addBlock())
        , exit(graph.proc.addBlock())
    {
        condition = cCallArgumentValues<int32_t>(graph.proc, root)[0];
        graph.branch(root, condition, FrequentedBlock(first, frequencyOfFirstEntry), FrequentedBlock(second));
    }

    void finish(Graph& graph)
    {
        graph.jump(first, second);
        graph.branch(second, condition, first, exit);
        graph.ret(exit, condition);
        graph.proc.resetReachability();
    }

    BasicBlock* root;
    BasicBlock* first;
    BasicBlock* second;
    BasicBlock* exit;
    Value* condition;
};

static void testCycleForestIrreducibleCycle()
{
    for (FrequencyClass frequencyOfFirstEntry : { FrequencyClass::Normal, FrequencyClass::Rare }) {
        Procedure proc;
        Graph graph { proc };
        CycleWithTwoEntries blocks(graph, frequencyOfFirstEntry);
        blocks.finish(graph);
        CHECK(!proc.naturalLoops().numLoops());

        CycleForest forest(proc);
        CHECK_EQ(forest.size(), 1u);
        CHECK(!forest[0].isReducible());
        CHECK_EQ(forest[0].entries.size(), 2u);
        CHECK(forest[0].entries.contains(blocks.first) && forest[0].entries.contains(blocks.second));
        CHECK(forest[0].header() == (frequencyOfFirstEntry == FrequencyClass::Normal ? blocks.first : blocks.second));
        CHECK_EQ(forest.innermost(blocks.first), 0u);
        CHECK_EQ(forest.innermost(blocks.second), 0u);
        CHECK_EQ(forest.innermost(blocks.root), CycleForest::none);
        CHECK_EQ(forest.blocksInPostOrder().size(), 4u);
        CHECK(forest.blocksInPostOrder().last() == blocks.root);
    }
}

static void testCycleForestPrefersPollingEntryAsHeader()
{
    Procedure proc;
    Graph graph { proc };
    PollState state;
    CycleWithTwoEntries blocks(graph, FrequencyClass::Normal);
    appendPollLimit(graph, blocks.second, state);
    blocks.finish(graph);

    CycleForest forest(proc);
    CHECK_EQ(forest.size(), 1u);
    CHECK(forest[0].header() == blocks.second);
    CHECK_EQ(forest[0].entries.size(), 2u);
}

struct TwoLoads {
    explicit TwoLoads(Mutability mutability = Mutability::Mutable)
        : mutability(mutability)
        , root(proc.addBlock())
    {
        auto arguments = cCallArgumentValues<int64_t*, int64_t*, int64_t>(proc, root);
        pointer = arguments[0];
        otherPointer = arguments[1];
        operand = arguments[2];
        first = load(root);
    }

    Value* load(BasicBlock* block)
    {
        MemoryValue* result = graph.load(block, Int64, pointer, 0, firstHeap);
        result->setReadsMutability(mutability);
        return result;
    }

    unsigned numberOfLoadsLeft(BasicBlock* block, bool isPositionIndependent = true)
    {
        graph.ret(block, graph.make(block, Add, first, graph.make(block, Mul, load(block), graph.int64(block, 100))));
        reduceStrengthOnce(proc, isPositionIndependent);
        unsigned result = numberOfValues(proc, Load);
        code = compileProc(proc);
        return result;
    }

    int64_t run(int64_t* pointer, int64_t* otherPointer, int64_t operand) { return invoke<int64_t>(*code, pointer, otherPointer, operand); }

    Procedure proc;
    Graph graph { proc };
    Mutability mutability;
    BasicBlock* root;
    Value* pointer;
    Value* otherPointer;
    Value* operand;
    Value* first;
    std::unique_ptr<Compilation> code;
};

static void testMemoryNumberingEliminatesRedundantLoad()
{
    for (bool isPositionIndependent : { true, false }) {
        TwoLoads loads;
        CHECK_EQ(loads.numberOfLoadsLeft(loads.root, isPositionIndependent), isPositionIndependent ? 1u : 2u);
        int64_t memory[1] = { 3 };
        CHECK_EQ(loads.run(memory, memory, 0), 303);
    }
}

static void testMemoryNumberingKeepsLoadAfterAliasingStore()
{
    for (int32_t offset : { 0, 8 }) {
        TwoLoads loads;
        loads.graph.store(loads.root, loads.operand, loads.otherPointer, offset, firstHeap);
        CHECK_EQ(loads.numberOfLoadsLeft(loads.root), 2u);
        int64_t memory[2] = { 3, 4 };
        CHECK_EQ(loads.run(memory, memory, 5), offset ? 303 : 503);
    }
}

static void testMemoryNumberingEliminatesLoadAfterDisjointStore()
{
    TwoLoads loads;
    loads.graph.store(loads.root, loads.operand, loads.otherPointer, 0, secondHeap);
    CHECK_EQ(loads.numberOfLoadsLeft(loads.root), 1u);
    int64_t memory[1] = { 3 };
    int64_t otherMemory[1] = { 4 };
    CHECK_EQ(loads.run(memory, otherMemory, 5), 303);
    CHECK_EQ(otherMemory[0], 5);
}

static void testMemoryNumberingForwardsStoreToLoad()
{
    TwoLoads loads;
    loads.graph.store(loads.root, loads.operand, loads.pointer, 0, firstHeap);
    CHECK_EQ(loads.numberOfLoadsLeft(loads.root), 1u);
    int64_t memory[1] = { 3 };
    CHECK_EQ(loads.run(memory, memory, 5), 503);
    CHECK_EQ(memory[0], 5);
}

static void testMemoryNumberingDoesNotForwardNarrowStore()
{
    TwoLoads loads;
    loads.root->appendNew<MemoryValue>(loads.proc, Store8, Origin(), loads.graph.make(loads.root, Trunc, loads.operand), loads.pointer, 0, firstHeap);
    CHECK_EQ(loads.numberOfLoadsLeft(loads.root), 2u);
    int64_t memory[1] = { 0x300 };
    CHECK_EQ(loads.run(memory, memory, 5), 0x300 + 0x305 * 100);
}

static void testMemoryNumberingHeapRangeWrites()
{
    struct Case {
        HeapRange writes;
        unsigned numberOfLoadsLeft;
    };
    for (Case testCase : { Case { bothHeaps, 2 }, Case { HeapRange(0, 11), 2 }, Case { HeapRange::top(), 2 }, Case { HeapRange(11, 30), 1 }, Case { HeapRange(0, 10), 1 } }) {
        TwoLoads loads;
        loads.graph.emptyPatchpoint(loads.root)->effects.writes = testCase.writes;
        CHECK_EQ(loads.numberOfLoadsLeft(loads.root), testCase.numberOfLoadsLeft);
    }
}

static void testMemoryNumberingKeepsLoadAfterFence()
{
    TwoLoads loads;
    loads.root->appendNew<FenceValue>(loads.proc, Origin());
    CHECK_EQ(loads.numberOfLoadsLeft(loads.root), 2u);
}

static void testMemoryNumberingImmutableLoads()
{
    {
        TwoLoads loads(Mutability::Immutable);
        loads.graph.emptyPatchpoint(loads.root)->effects.writes = HeapRange::top();
        CHECK_EQ(loads.numberOfLoadsLeft(loads.root), 1u);
    }
    {
        TwoLoads loads(Mutability::Immutable);
        loads.graph.callLikePatchpoint(loads.root);
        CHECK_EQ(loads.numberOfLoadsLeft(loads.root), 2u);
    }
}

static void testMemoryNumberingAcrossDiamond()
{
    struct Case {
        std::optional<HeapRange> writesOnOneArm;
        unsigned numberOfLoadsLeft;
    };
    for (Case testCase : { Case { std::nullopt, 1 }, Case { secondHeap, 1 }, Case { firstHeap, 2 }, Case { bothHeaps, 2 } }) {
        TwoLoads loads;
        Graph& graph = loads.graph;
        BasicBlock* left = loads.proc.addBlock();
        BasicBlock* right = loads.proc.addBlock();
        BasicBlock* join = loads.proc.addBlock();
        graph.branch(loads.root, loads.operand, left, right);
        if (testCase.writesOnOneArm)
            graph.store(left, loads.operand, loads.otherPointer, 0, *testCase.writesOnOneArm);
        graph.store(left, loads.operand, loads.otherPointer, 8, HeapRange(20, 21));
        graph.jump(left, join);
        graph.store(right, loads.operand, loads.otherPointer, 16, HeapRange(21, 22));
        graph.jump(right, join);
        CHECK_EQ(loads.numberOfLoadsLeft(join), testCase.numberOfLoadsLeft);
        int64_t memory[3] = { 3, 0, 0 };
        bool overwrites = testCase.writesOnOneArm && testCase.writesOnOneArm->overlaps(firstHeap);
        if (!testCase.writesOnOneArm || overwrites)
            CHECK_EQ(loads.run(memory, memory, 5), overwrites ? 503 : 303);
        memory[0] = 3;
        CHECK_EQ(loads.run(memory, memory, 0), 303);
    }
}

static void testMemoryNumberingAcrossLoops()
{
    for (bool loopOverwrites : { false, true }) {
        TwoLoads loads;
        Graph& graph = loads.graph;
        BasicBlock* loop = loads.proc.addBlock();
        BasicBlock* exit = loads.proc.addBlock();
        Value* index = graph.phi(loop, Int64);
        graph.upsilon(loads.root, graph.int64(loads.root, 0), index);
        graph.jump(loads.root, loop);
        Value* next = graph.make(loop, Add, index, loads.load(loop));
        graph.store(loop, next, loads.otherPointer, 0, loopOverwrites ? firstHeap : secondHeap);
        graph.upsilon(loop, next, index);
        graph.branch(loop, graph.make(loop, LessThan, next, loads.operand), loop, exit);
        CHECK_EQ(loads.numberOfLoadsLeft(exit), loopOverwrites ? 3u : 1u);
        int64_t memory[1] = { 3 };
        int64_t otherMemory[1] = { 0 };
        CHECK_EQ(loads.run(memory, otherMemory, 7), 303);
        CHECK_EQ(otherMemory[0], 9);
        if (loopOverwrites)
            CHECK_EQ(loads.run(memory, memory, 7), 3 + 12 * 100);
    }
}

template<typename Functor>
static unsigned numberOfOperationsLeft(B3::Opcode opcode, bool isPositionIndependent, const Functor& between)
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root);
    Value* operand = arguments[0];
    Value* other = arguments[1];
    auto make = [&] {
        return opcode == Clz ? graph.make(root, Clz, operand) : graph.make(root, opcode, operand, other);
    };
    Value* first = make();
    between(graph, root);
    graph.ret(root, graph.make(root, Mul, first, make()));
    reduceStrengthOnce(proc, isPositionIndependent);
    return numberOfValues(proc, opcode);
}

static void testValueNumberingRecomputesCheapValuesAfterCalls()
{
    auto nothing = [](Graph&, BasicBlock*) { };
    auto call = [](Graph& graph, BasicBlock* block) {
        graph.callLikePatchpoint(block);
    };
    CHECK_EQ(numberOfOperationsLeft(Add, true, nothing), 1u);
    CHECK_EQ(numberOfOperationsLeft(Add, true, call), 2u);
    CHECK_EQ(numberOfOperationsLeft(Add, false, call), 1u);
    CHECK_EQ(numberOfOperationsLeft(Clz, true, nothing), 1u);
    CHECK_EQ(numberOfOperationsLeft(Clz, true, call), 1u);
}

static unsigned numberOfBranchesLeftAfterDominatingBranch(bool isPositionIndependent, bool meetsAgain)
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* taken = proc.addBlock();
    BasicBlock* notTaken = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t*>(proc, root);
    Value* operand = arguments[0];
    Value* pointer = arguments[1];
    auto isFive = [&](BasicBlock* block) {
        return graph.make(block, Equal, operand, graph.int64(block, 5));
    };
    auto returnOneOf = [&](BasicBlock* block, int64_t ifFive, int64_t otherwise) {
        BasicBlock* five = proc.addBlock();
        BasicBlock* other = proc.addBlock();
        graph.branch(block, isFive(block), five, other);
        graph.ret(five, graph.int64(five, ifFive));
        graph.ret(other, graph.int64(other, otherwise));
    };
    graph.branch(root, isFive(root), taken, notTaken);
    graph.store(taken, operand, pointer, 0, firstHeap);
    graph.store(notTaken, operand, pointer, 8, firstHeap);
    if (meetsAgain) {
        graph.jump(taken, join);
        graph.jump(notTaken, join);
        returnOneOf(join, 1, 4);
    } else {
        returnOneOf(taken, 1, 2);
        returnOneOf(notTaken, 3, 4);
        graph.ret(join, operand);
    }
    reduceStrengthOnce(proc, isPositionIndependent);
    unsigned result = numberOfValues(proc, Branch);
    auto code = compileProc(proc);
    int64_t memory[2] = { 0, 0 };
    CHECK_EQ(invoke<int64_t>(*code, 5, memory), 1);
    CHECK_EQ(invoke<int64_t>(*code, 6, memory), 4);
    CHECK_EQ(memory[0], 5);
    CHECK_EQ(memory[1], 6);
    return result;
}

static void testBranchDominatedByBranchOnSameConditionIsFolded()
{
    CHECK_EQ(numberOfBranchesLeftAfterDominatingBranch(true, false), 1u);
    CHECK_EQ(numberOfBranchesLeftAfterDominatingBranch(false, false), 3u);
    CHECK_EQ(numberOfBranchesLeftAfterDominatingBranch(true, true), 2u);
}

static unsigned numberOfDoublesLeft(Procedure& proc)
{
    return numberOfValues(proc, [](Value* value) {
        return value->type() == Double;
    });
}

static void testDoubleComparisonsOfConvertedIntegersBecomeIntegerComparisons()
{
    for (B3::Opcode comparison : { Equal, NotEqual, LessThan, GreaterThan, LessEqual, GreaterEqual }) {
        for (bool isPositionIndependent : { true, false }) {
            Procedure proc;
            Graph graph { proc };
            BasicBlock* root = proc.addBlock();
            auto arguments = cCallArgumentValues<int32_t, int32_t>(proc, root);
            Value* left = graph.make(root, Add, graph.make(root, IToD, arguments[0]), graph.number(root, 1));
            Value* right = graph.make(root, Mul, graph.make(root, IToD, arguments[1]), graph.number(root, 2));
            graph.ret(root, graph.make(root, comparison, left, right));
            reduceStrengthOnce(proc, isPositionIndependent);
            CHECK_EQ(!numberOfDoublesLeft(proc), isPositionIndependent);
            auto code = compileProc(proc);
            for (int32_t a : { 0, 1, -1, 5, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max() }) {
                for (int32_t b : { 0, 1, -1, 3, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max() }) {
                    int64_t wideLeft = static_cast<int64_t>(a) + 1;
                    int64_t wideRight = static_cast<int64_t>(b) * 2;
                    bool expected = comparison == Equal ? wideLeft == wideRight : comparison == NotEqual ? wideLeft != wideRight : holds(comparison, wideLeft, wideRight);
                    CHECK_EQ(!!invoke<int32_t>(*code, a, b), expected);
                }
            }
        }
    }
}

template<typename Functor>
static bool keepsDoublesInComparisonWith(const Functor& makeRight)
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t, int64_t, double>(proc, root);
    Value* word = arguments[0];
    graph.ret(root, graph.make(root, LessThan, graph.make(root, IToD, word), makeRight(graph, root, arguments[1], arguments[2])));
    reduceStrengthOnce(proc, true);
    return numberOfDoublesLeft(proc);
}

static void testDoubleComparisonsOfNonIntegersAreKept()
{
    CHECK(!keepsDoublesInComparisonWith([](Graph& graph, BasicBlock* block, Value*, Value*) {
        return graph.number(block, 9007199254740992.0);
    }));
    CHECK(!keepsDoublesInComparisonWith([](Graph& graph, BasicBlock* block, Value* wide, Value*) {
        return graph.make(block, IToD, graph.make(block, SShr, wide, graph.int32(block, 20)));
    }));

    CHECK(keepsDoublesInComparisonWith([](Graph& graph, BasicBlock* block, Value*, Value*) {
        return graph.number(block, 0.5);
    }));
    CHECK(keepsDoublesInComparisonWith([](Graph& graph, BasicBlock* block, Value*, Value*) {
        return graph.number(block, -0.0);
    }));
    CHECK(keepsDoublesInComparisonWith([](Graph& graph, BasicBlock* block, Value*, Value*) {
        return graph.number(block, 9007199254740994.0);
    }));
    CHECK(keepsDoublesInComparisonWith([](Graph& graph, BasicBlock* block, Value*, Value*) {
        return graph.number(block, PNaN);
    }));
    CHECK(keepsDoublesInComparisonWith([](Graph& graph, BasicBlock* block, Value* wide, Value*) {
        return graph.make(block, IToD, wide);
    }));
    CHECK(keepsDoublesInComparisonWith([](Graph&, BasicBlock*, Value*, Value* number) {
        return number;
    }));
    CHECK(keepsDoublesInComparisonWith([](Graph& graph, BasicBlock* block, Value* wide, Value*) {
        Value* large = graph.make(block, IToD, graph.make(block, SShr, wide, graph.int32(block, 20)));
        return graph.make(block, Mul, large, large);
    }));
}

static void testBitwiseDoubleComparisonsRespectNegativeZero()
{
    for (B3::Opcode operation : { Add, Mul }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<int32_t, int32_t, int32_t>(proc, root);
        Value* left = graph.make(root, operation, graph.make(root, IToD, arguments[0]), graph.make(root, IToD, arguments[1]));
        Value* right = graph.make(root, IToD, arguments[2]);
        graph.ret(root, graph.make(root, Equal, graph.make(root, BitwiseCast, left), graph.make(root, BitwiseCast, right)));
        reduceStrengthOnce(proc, true);
        CHECK_EQ(!numberOfDoublesLeft(proc), operation == Add);
        auto code = compileProc(proc);
        CHECK_EQ(invoke<int32_t>(*code, 2, 3, operation == Add ? 5 : 6), 1);
        CHECK_EQ(invoke<int32_t>(*code, 2, 3, 7), 0);
        CHECK_EQ(invoke<int32_t>(*code, -1, 0, 0), 0);
        CHECK_EQ(invoke<int32_t>(*code, -1, 1, 0), operation == Add ? 1 : 0);
    }
}

template<typename Functor>
static std::optional<bool> answerOfComparison(B3::Opcode comparison, int64_t constant, bool constantIsFirst, const Functor& makeOperand)
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int32_t>(proc, root);
    Value* operand = makeOperand(graph, root, arguments[0], arguments[1]);
    Value* other = operand->type() == Int32 ? graph.int32(root, static_cast<int32_t>(constant)) : graph.int64(root, constant);
    graph.ret(root, constantIsFirst ? graph.make(root, comparison, other, operand) : graph.make(root, comparison, operand, other));
    reduceStrengthOnce(proc, true);
    Value* returned = proc.at(0)->last()->child(0);
    if (!returned->hasInt32())
        return std::nullopt;
    return !!returned->asInt32();
}

static void testComparisonsFoldedByIntegerRange()
{
    auto lowFourBits = [](Graph& graph, BasicBlock* block, Value* wide, Value*) {
        return graph.make(block, BitAnd, wide, graph.int64(block, 15));
    };
    auto zeroExtended = [](Graph& graph, BasicBlock* block, Value*, Value* word) {
        return graph.make(block, ZExt32, word);
    };
    auto zeroExtendedPlusFive = [](Graph& graph, BasicBlock* block, Value*, Value* word) {
        return graph.make(block, Add, graph.make(block, ZExt32, word), graph.int64(block, 5));
    };
    auto signExtendedMinusOne = [](Graph& graph, BasicBlock* block, Value*, Value* word) {
        return graph.make(block, Add, graph.make(block, SExt32, word), graph.int64(block, -1));
    };
    auto anything = [](Graph&, BasicBlock*, Value* wide, Value*) {
        return wide;
    };

    CHECK(answerOfComparison(LessThan, 16, false, lowFourBits) == true);
    CHECK(answerOfComparison(LessThan, 0, false, lowFourBits) == false);
    CHECK(answerOfComparison(LessEqual, 15, false, lowFourBits) == true);
    CHECK(answerOfComparison(GreaterThan, 15, false, lowFourBits) == false);
    CHECK(answerOfComparison(GreaterThan, -1, false, lowFourBits) == true);
    CHECK(answerOfComparison(GreaterEqual, 0, false, lowFourBits) == true);
    CHECK(answerOfComparison(GreaterEqual, 16, false, lowFourBits) == false);
    CHECK(answerOfComparison(Equal, 16, false, lowFourBits) == false);
    CHECK(answerOfComparison(NotEqual, -1, false, lowFourBits) == true);
    CHECK(answerOfComparison(Below, 1ll << 32, false, zeroExtended) == true);
    CHECK(answerOfComparison(AboveEqual, 1ll << 32, false, zeroExtended) == false);
    CHECK(answerOfComparison(Above, 4, false, zeroExtendedPlusFive) == true);
    CHECK(answerOfComparison(BelowEqual, 4, false, zeroExtendedPlusFive) == false);
    CHECK(answerOfComparison(Below, 4, true, zeroExtendedPlusFive) == true);
    CHECK(answerOfComparison(AboveEqual, 1ll << 33, true, zeroExtendedPlusFive) == true);

    CHECK(!answerOfComparison(LessThan, 15, false, lowFourBits));
    CHECK(!answerOfComparison(LessEqual, 14, false, lowFourBits));
    CHECK(!answerOfComparison(GreaterThan, 0, false, lowFourBits));
    CHECK(!answerOfComparison(GreaterEqual, 1, false, lowFourBits));
    CHECK(!answerOfComparison(Equal, 15, false, lowFourBits));
    CHECK(!answerOfComparison(NotEqual, 0, false, lowFourBits));
    CHECK(!answerOfComparison(Below, 0xffffffffll, false, zeroExtended));
    CHECK(!answerOfComparison(Above, 5, false, zeroExtendedPlusFive));
    CHECK(!answerOfComparison(Below, 1ll << 40, false, signExtendedMinusOne));
    CHECK(!answerOfComparison(LessThan, 16, false, anything));
    CHECK(!answerOfComparison(Below, 16, false, anything));
}

static void testDoubleToIntegerConversionsOfConvertedIntegers()
{
    for (int32_t key : { keyOfTruncationToInt64, keyOfConversionToInt32, 0 }) {
        for (bool isWholeNumber : { true, false }) {
            Procedure proc;
            Graph graph { proc };
            BasicBlock* root = proc.addBlock();
            auto arguments = cCallArgumentValues<int32_t, double>(proc, root);
            Value* number = graph.make(root, Add, isWholeNumber ? graph.make(root, IToD, arguments[0]) : arguments[1], graph.number(root, 1));
            PatchpointValue* conversion = root->appendNew<PatchpointValue>(proc, key == keyOfConversionToInt32 ? Int32 : Int64, Origin());
            conversion->effects = Effects::none();
            conversion->pureFunctionOfChildrenKey = key;
            conversion->appendSomeRegister(number);
            conversion->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
                jit.truncateDoubleToInt64(params[1].fpr(), params[0].gpr());
            });
            graph.ret(root, key == keyOfConversionToInt32 ? graph.make(root, SExt32, conversion) : conversion);
            reduceStrengthOnce(proc, true);
            CHECK_EQ(numberOfValues(proc, Patchpoint), key && isWholeNumber ? 0u : 1u);
            auto code = compileProc(proc);
            CHECK_EQ(invoke<int64_t>(*code, 41, 41.0), 42);
            CHECK_EQ(invoke<int64_t>(*code, -1, -1.0), 0);
            if (key != keyOfConversionToInt32)
                CHECK_EQ(invoke<int64_t>(*code, std::numeric_limits<int32_t>::max(), 2147483647.0), 2147483648ll);
        }
    }
}

static unsigned numberOfPatchpointsLeft(bool isPositionIndependent, uint64_t firstKey, uint64_t secondKey)
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    PatchpointValue* first = graph.emptyPatchpoint(root, Int64);
    first->pureFunctionKey = firstKey;
    PatchpointValue* second = graph.emptyPatchpoint(root, Int64);
    second->pureFunctionKey = secondKey;
    graph.ret(root, graph.make(root, Add, first, second));
    reduceStrengthOnce(proc, isPositionIndependent);
    unsigned result = numberOfValues(proc, Patchpoint);
    CHECK_EQ(compileAndRun<int64_t>(proc), 0);
    return result;
}

static unsigned numberOfPatchpointsOfChildrenLeft(int32_t firstKey, int32_t secondKey, bool haveSameChild)
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root);
    Value* child = arguments[0];
    Value* otherChild = arguments[1];
    PatchpointValue* first = graph.emptyPatchpoint(root, Int64);
    first->pureFunctionOfChildrenKey = firstKey;
    first->appendSomeRegister(child);
    PatchpointValue* second = graph.emptyPatchpoint(root, Int64);
    second->pureFunctionOfChildrenKey = secondKey;
    second->appendSomeRegister(haveSameChild ? child : otherChild);
    graph.ret(root, graph.make(root, Add, first, second));
    reduceStrengthOnce(proc, true);
    return numberOfValues(proc, Patchpoint);
}

static void testPurePatchpointsAreValueNumbered()
{
    for (bool isPositionIndependent : { true, false }) {
        CHECK_EQ(numberOfPatchpointsLeft(isPositionIndependent, 7, 7), 1u);
        CHECK_EQ(numberOfPatchpointsLeft(isPositionIndependent, 7, 8), 2u);
        CHECK_EQ(numberOfPatchpointsLeft(isPositionIndependent, 0, 0), 2u);
    }
    CHECK_EQ(numberOfPatchpointsOfChildrenLeft(100, 100, true), 1u);
    CHECK_EQ(numberOfPatchpointsOfChildrenLeft(100, 100, false), 2u);
    CHECK_EQ(numberOfPatchpointsOfChildrenLeft(100, 101, true), 2u);
    CHECK_EQ(numberOfPatchpointsOfChildrenLeft(0, 0, true), 2u);
}

static void testReduceStrengthMergesBlockChain()
{
    for (bool isPositionIndependent : { true, false }) {
        Procedure proc;
        Graph graph { proc };
        constexpr unsigned numberOfBlocksInChain = 40;
        Vector<BasicBlock*> blocks;
        for (unsigned i = 0; i < numberOfBlocksInChain; ++i)
            blocks.append(proc.addBlock());
        Value* pointer = cCallArgumentValues<int64_t*>(proc, blocks[0])[0];
        for (unsigned i = 0; i < numberOfBlocksInChain; ++i) {
            unsigned place = (i * 7) % numberOfBlocksInChain;
            unsigned nextPlace = ((i + 1) * 7) % numberOfBlocksInChain;
            graph.store(blocks[place], graph.int64(blocks[place], i), pointer, i * 8, firstHeap);
            if (i + 1 < numberOfBlocksInChain)
                graph.jump(blocks[place], blocks[nextPlace]);
            else
                graph.ret(blocks[place], pointer);
        }
        reduceStrengthOnce(proc, isPositionIndependent);
        CHECK_EQ(numberOfBlocks(proc), 1u);
        CHECK_EQ(numberOfValues(proc, Store), numberOfBlocksInChain);
        int64_t memory[numberOfBlocksInChain] = { };
        compileAndRun<int64_t*>(proc, memory);
        for (unsigned i = 0; i < numberOfBlocksInChain; ++i)
            CHECK_EQ(memory[i], static_cast<int64_t>(i));
    }
}

static void testReduceStrengthEliminatesSingleValuePhi()
{
    for (bool isOneValue : { true, false }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* left = proc.addBlock();
        BasicBlock* right = proc.addBlock();
        BasicBlock* join = proc.addBlock();
        auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t*>(proc, root);
        Value* first = arguments[0];
        Value* second = arguments[1];
        Value* pointer = arguments[2];
        Value* phi = graph.phi(join, Int64);
        graph.branch(root, first, left, right);
        graph.store(left, first, pointer, 0, firstHeap);
        graph.upsilon(left, second, phi);
        graph.jump(left, join);
        graph.store(right, second, pointer, 0, firstHeap);
        graph.upsilon(right, isOneValue ? second : first, phi);
        graph.jump(right, join);
        graph.ret(join, phi);
        reduceStrengthOnce(proc, false);
        CHECK_EQ(numberOfValues(proc, Phi), isOneValue ? 0u : 1u);
        int64_t memory[1];
        auto code = compileProc(proc);
        CHECK_EQ(invoke<int64_t>(*code, 1, 2, memory), 2);
        CHECK_EQ(invoke<int64_t>(*code, 0, 2, memory), isOneValue ? 2 : 0);
    }
}

static void testMemoryNumberingWhenEdgeIntoJoinIsRemoved()
{
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t*, int32_t, int32_t>(proc, root);
    BasicBlock* stores = proc.addBlock();
    BasicBlock* storesElsewhere = proc.addBlock();
    BasicBlock* testsAgain = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    BasicBlock* leaves = proc.addBlock();
    HeapRange heap(10, 11);
    HeapRange otherHeap(11, 12);
    auto load = [&](BasicBlock* block) {
        MemoryValue* value = block->appendNew<MemoryValue>(proc, Load, Int32, Origin(), arguments[0]);
        value->setRange(heap);
        return value;
    };

    Value* before = load(root);
    root->setSuccessors(FrequentedBlock(stores), FrequentedBlock(testsAgain));
    root->appendNew<Value>(proc, Branch, Origin(), arguments[1]);

    stores->appendNew<MemoryValue>(proc, Store, Origin(), stores->appendNew<Const32Value>(proc, Origin(), 5), arguments[0], 0, heap);
    stores->setSuccessors(FrequentedBlock(storesElsewhere), FrequentedBlock(join));
    stores->appendNew<Value>(proc, Branch, Origin(), arguments[2]);

    storesElsewhere->appendNew<MemoryValue>(proc, Store, Origin(), storesElsewhere->appendNew<Const32Value>(proc, Origin(), 1), arguments[0], 4, otherHeap);
    storesElsewhere->setSuccessors(FrequentedBlock(join));
    storesElsewhere->appendNew<Value>(proc, Jump, Origin());

    testsAgain->setSuccessors(FrequentedBlock(join), FrequentedBlock(leaves));
    testsAgain->appendNew<Value>(proc, Branch, Origin(), arguments[1]);

    leaves->clearSuccessors();
    leaves->appendNew<Value>(proc, Return, Origin(), leaves->appendNew<Const32Value>(proc, Origin(), 0));

    Value* after = load(join);
    join->clearSuccessors();
    join->appendNew<Value>(proc, Return, Origin(), join->appendNew<Value>(proc, Add, Origin(), join->appendNew<Value>(proc, Mul, Origin(), before, join->appendNew<Const32Value>(proc, Origin(), 100)), after));

    proc.resetReachability();
    proc.setPositionIndependent();
    reduceStrength(proc, ReduceStrengthPass::Initial);
    proc.setPositionIndependent(false);

    auto code = compileProc(proc);
    for (int32_t alsoElsewhere : { 0, 1 }) {
        int32_t memory[2] = { 1, 2 };
        CHECK_EQ(invoke<int32_t>(*code, memory, 1, alsoElsewhere), 105);
        CHECK_EQ(memory[0], 5);
        CHECK_EQ(memory[1], alsoElsewhere ? 1 : 2);
    }
    int32_t memory[2] = { 1, 2 };
    CHECK_EQ(invoke<int32_t>(*code, memory, 0, 1), 0);
    CHECK_EQ(memory[0], 1);
}

static unsigned numberOfInt64Constants(BasicBlock* block, int64_t constant)
{
    unsigned result = 0;
    for (Value* value : *block)
        result += value->isInt64(constant);
    return result;
}

static void testMoveConstantsMaterializesByFrequency()
{
    constexpr int64_t large = 0x123456789abcll;
    struct Case {
        bool byFrequency;
        double frequencyOfUsers;
        bool isMaterializedAtUsers;
    };
    for (Case testCase : { Case { true, 0.01, true }, Case { false, 0.01, false }, Case { true, 0.75, false } }) {
        Procedure proc;
        Graph graph { proc };
        BranchToRareBlocks blocks(graph, testCase.frequencyOfUsers);
        Value* inFirstRare = graph.make(blocks.firstRare, Add, blocks.operand, graph.int64(blocks.firstRare, large));
        Value* inSecondRare = graph.make(blocks.secondRare, Mul, blocks.operand, graph.int64(blocks.secondRare, large));
        blocks.finish(graph, inFirstRare, inSecondRare, blocks.operand);
        if (testCase.byFrequency)
            proc.setMaterializesConstantsByFrequency();

        moveConstants(proc);
        validate(proc);
        CHECK_EQ(numberOfInt64Constants(blocks.root, large), testCase.isMaterializedAtUsers ? 0u : 1u);
        CHECK_EQ(numberOfInt64Constants(blocks.firstRare, large), testCase.isMaterializedAtUsers ? 1u : 0u);
        CHECK_EQ(numberOfInt64Constants(blocks.secondRare, large), testCase.isMaterializedAtUsers ? 1u : 0u);
        auto code = compileProc(proc);
        CHECK_EQ(invoke<int64_t>(*code, 1, 5, nullptr), large + 5);
        CHECK_EQ(invoke<int64_t>(*code, 2, 5, nullptr), large * 5);
        CHECK_EQ(invoke<int64_t>(*code, 3, 5, nullptr), 5);
    }
}

static void testMoveConstantsByFrequencyHoistsOutOfLoop()
{
    constexpr int64_t large = 0x123456789abcll;
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop(graph);
    loop.header->setFrequency(10);
    loop.body->setFrequency(10);
    graph.increase(loop.body, loop.sum, large);
    loop.endBody(loop.body);
    proc.setMaterializesConstantsByFrequency();

    moveConstants(proc);
    validate(proc);
    CHECK_EQ(numberOfInt64Constants(loop.root, large), 1u);
    CHECK_EQ(numberOfInt64Constants(loop.body, large), 0u);
    CHECK_EQ(run(*compileProc(proc), 3), 3 * large);
}

static void testMoveConstantsPositionIndependentDoubles()
{
    for (bool isPositionIndependent : { true, false }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        graph.ret(root, graph.make(root, Add, cCallArgumentValues<double>(proc, root)[0], graph.number(root, 0.1)));
        proc.resetReachability();
        proc.setPositionIndependent(isPositionIndependent);

        moveConstants(proc);
        validate(proc);
        CHECK_EQ(numberOfValues(proc, ConstDouble), 0u);
        CHECK_EQ(numberOfValues(proc, BitwiseCast), isPositionIndependent ? 1u : 0u);
        CHECK_EQ(numberOfValues(proc, Load), isPositionIndependent ? 0u : 1u);
        CHECK_EQ(compileAndRun<double>(proc, 1.0), 1.0 + 0.1);
    }
}

struct TailWithTwoPredecessors {
    explicit TailWithTwoPredecessors(Graph& graph)
        : root(graph.proc.addBlock())
        , left(graph.proc.addBlock())
        , right(graph.proc.addBlock())
        , tail(graph.proc.addBlock())
        , taken(graph.proc.addBlock())
        , notTaken(graph.proc.addBlock())
    {
        auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t*>(graph.proc, root);
        first = arguments[0];
        second = arguments[1];
        pointer = arguments[2];
        graph.store(left, first, pointer, 0, firstHeap);
        graph.store(right, second, pointer, 0, firstHeap);
        graph.ret(taken, graph.int64(taken, 10));
        graph.ret(notTaken, graph.int64(notTaken, 20));
    }

    void duplicate(Graph& graph, bool onlyDecidedTests = false)
    {
        graph.jump(left, tail);
        graph.jump(right, tail);
        graph.proc.resetReachability();
        validate(graph.proc);
        graph.proc.setPositionIndependent();
        duplicateTails(graph.proc, onlyDecidedTests);
        graph.proc.setPositionIndependent(false);
        fixSSA(graph.proc);
        validate(graph.proc);
    }

    BasicBlock* root;
    BasicBlock* left;
    BasicBlock* right;
    BasicBlock* tail;
    BasicBlock* taken;
    BasicBlock* notTaken;
    Value* first;
    Value* second;
    Value* pointer;
};

static void testDuplicateTailsThreadsJumpOnConstantPhi()
{
    for (bool onlyDecidedTests : { false, true }) {
        Procedure proc;
        Graph graph { proc };
        TailWithTwoPredecessors blocks(graph);
        Value* phi = graph.phi(blocks.tail, Int64);
        graph.branch(blocks.root, blocks.first, blocks.left, blocks.right);
        graph.upsilon(blocks.left, graph.int64(blocks.left, 0), phi);
        graph.upsilon(blocks.right, blocks.second, phi);
        graph.branch(blocks.tail, phi, blocks.taken, blocks.notTaken);
        blocks.duplicate(graph, onlyDecidedTests);

        CHECK(blocks.left->last()->opcode() == Jump);
        CHECK(blocks.left->successorBlock(0) == blocks.notTaken);
        CHECK(blocks.right->successorBlock(0) == blocks.tail);
        auto code = compileProc(proc);
        int64_t memory[1];
        CHECK_EQ(invoke<int64_t>(*code, 1, 1, memory), 20);
        CHECK_EQ(invoke<int64_t>(*code, 0, 1, memory), 10);
        CHECK_EQ(invoke<int64_t>(*code, 0, 0, memory), 20);
    }
}

static void testDuplicateTailsThreadsJumpDecidedByDominatingBranch()
{
    for (bool testsTheOpposite : { false, true }) {
        Procedure proc;
        Graph graph { proc };
        TailWithTwoPredecessors blocks(graph);
        graph.branch(blocks.root, graph.make(blocks.root, Equal, blocks.first, graph.int64(blocks.root, 5)), blocks.left, blocks.right);
        graph.branch(blocks.tail, graph.make(blocks.tail, testsTheOpposite ? NotEqual : Equal, blocks.first, graph.int64(blocks.tail, 5)), blocks.taken, blocks.notTaken);
        blocks.duplicate(graph);

        CHECK(blocks.left->successorBlock(0) == (testsTheOpposite ? blocks.notTaken : blocks.taken));
        CHECK(blocks.right->successorBlock(0) == (testsTheOpposite ? blocks.taken : blocks.notTaken));
        CHECK_EQ(numberOfValues(proc, Branch), 1u);
        auto code = compileProc(proc);
        int64_t memory[1];
        CHECK_EQ(invoke<int64_t>(*code, 5, 0, memory), testsTheOpposite ? 20 : 10);
        CHECK_EQ(invoke<int64_t>(*code, 6, 0, memory), testsTheOpposite ? 10 : 20);
    }
}

static void testDuplicateTailsKeepsUndecidedTest()
{
    Procedure proc;
    Graph graph { proc };
    TailWithTwoPredecessors blocks(graph);
    graph.branch(blocks.root, graph.make(blocks.root, Equal, blocks.first, graph.int64(blocks.root, 5)), blocks.left, blocks.right);
    graph.branch(blocks.tail, graph.make(blocks.tail, Equal, blocks.second, graph.int64(blocks.tail, 5)), blocks.taken, blocks.notTaken);
    blocks.duplicate(graph);

    CHECK(blocks.left->successorBlock(0) == blocks.tail);
    CHECK(blocks.right->successorBlock(0) == blocks.tail);
    CHECK_EQ(numberOfValues(proc, Branch), 2u);
}

static void testDuplicateTailsSkipsTailWithPatchpoint()
{
    Procedure proc;
    Graph graph { proc };
    TailWithTwoPredecessors blocks(graph);
    graph.branch(blocks.root, blocks.first, blocks.left, blocks.right);
    graph.emptyPatchpoint(blocks.tail)->effects.writesLocalState = true;
    graph.branch(blocks.tail, blocks.first, blocks.taken, blocks.notTaken);
    blocks.duplicate(graph);

    CHECK(blocks.left->successorBlock(0) == blocks.tail);
    CHECK(blocks.right->successorBlock(0) == blocks.tail);
    CHECK_EQ(numberOfValues(proc, Patchpoint), 1u);
}

static void testDuplicateTailsSkipsLoopHeader()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Value* turns = cCallArgumentValues<int64_t>(proc, root)[0];
    Value* goesOn = graph.phi(header, Int64);
    Value* index = graph.phi(header, Int64);
    graph.upsilon(root, graph.int64(root, 1), goesOn);
    graph.upsilon(root, graph.int64(root, 0), index);
    graph.jump(root, header);
    graph.branch(header, goesOn, body, exit);
    Value* next = graph.make(body, Add, index, graph.int64(body, 1));
    graph.upsilon(body, graph.make(body, ZExt32, graph.make(body, LessThan, next, turns)), goesOn);
    graph.upsilon(body, next, index);
    graph.jump(body, header);
    graph.ret(exit, index);
    proc.resetReachability();
    proc.setPositionIndependent();
    duplicateTails(proc);
    proc.setPositionIndependent(false);
    fixSSA(proc);
    validate(proc);

    CHECK(root->successorBlock(0) == header);
    CHECK(body->successorBlock(0) == header);
    CHECK_EQ(compileAndRun<int64_t>(proc, 4), 4);
}

static void testFoldPathConstantsFoldsSelect()
{
    for (bool isPositionIndependent : { true, false }) {
        Procedure proc;
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
        proc.setPositionIndependent(isPositionIndependent);
        foldPathConstants(proc);
        proc.setPositionIndependent(false);
        validate(proc);

        CHECK_EQ(numberOfValues(taken, Select), isPositionIndependent ? 0u : 1u);
        CHECK_EQ(numberOfValues(notTaken, Select), 1u);
        auto code = compileProc(proc);
        CHECK_EQ(invoke<int64_t>(*code, 1, 2, 3), 2);
        CHECK_EQ(invoke<int64_t>(*code, 0, 2, 3), 2);
        CHECK_EQ(invoke<int64_t>(*code, 0, 0, 3), 3);
    }
}

static void testSwitchInPositionIndependentCode()
{
    struct Case {
        unsigned degree;
        bool isPositionIndependent;
        bool hasTableInCode;
    };
    for (Case testCase : { Case { 127, true, false }, Case { 128, true, isARM64() }, Case { 300, true, isARM64() }, Case { 300, false, false } }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* fallThrough = proc.addBlock();
        SwitchValue* switchValue = root->appendNew<SwitchValue>(proc, Origin(), cCallArgumentValues<int64_t>(proc, root)[0]);
        switchValue->setFallThrough(FrequentedBlock(fallThrough));
        graph.ret(fallThrough, graph.int64(fallThrough, -1));
        for (unsigned i = 0; i < testCase.degree; ++i) {
            BasicBlock* target = proc.addBlock();
            graph.ret(target, graph.int64(target, i * 3 + 1));
            switchValue->appendCase(SwitchCase(i + 10, FrequentedBlock(target)));
        }
        proc.setPositionIndependent(testCase.isPositionIndependent);

        auto code = compileProc(proc);
        CHECK_EQ(proc.code().tablesInCode().size(), testCase.hasTableInCode ? 1u : 0u);
        if (testCase.hasTableInCode)
            CHECK_EQ(proc.code().tablesInCode()[0].second, testCase.degree);
        for (unsigned i = 0; i < testCase.degree; ++i)
            CHECK_EQ(invoke<int64_t>(*code, i + 10), static_cast<int64_t>(i * 3 + 1));
        for (int64_t other : { static_cast<int64_t>(-1), static_cast<int64_t>(0), static_cast<int64_t>(9), static_cast<int64_t>(testCase.degree + 10), static_cast<int64_t>(1) << 40, std::numeric_limits<int64_t>::min() })
            CHECK_EQ(invoke<int64_t>(*code, other), -1);
    }
}

template<typename Functor>
static void buildChainOfEqualityTests(Graph& graph, unsigned degree, const Functor& sourceAndCaseValue)
{
    Procedure& proc = graph.proc;
    BasicBlock* block = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, block);
    Value* first = arguments[0];
    Value* second = arguments[1];
    for (unsigned i = 0; i < degree; ++i) {
        BasicBlock* target = proc.addBlock();
        BasicBlock* next = proc.addBlock();
        auto [usesSecond, caseValue] = sourceAndCaseValue(i);
        graph.branch(block, graph.make(block, Equal, usesSecond ? second : first, graph.int64(block, caseValue)), target, next);
        graph.ret(target, graph.int64(target, i));
        block = next;
    }
    graph.ret(block, graph.int64(block, -1));
    proc.resetReachability();
}

static void testInferSwitchesManyCases()
{
    constexpr unsigned degree = 400;
    Procedure proc;
    Graph graph { proc };
    auto caseValueAt = [](unsigned i) -> int64_t {
        if (i == 200)
            return 5 * 7;
        if (i == 250)
            return std::numeric_limits<int64_t>::min();
        if (i == 300)
            return std::numeric_limits<int64_t>::max();
        return static_cast<int64_t>(i) * 7;
    };
    buildChainOfEqualityTests(graph, degree, [&](unsigned i) {
        return std::pair { false, caseValueAt(i) };
    });

    CHECK(inferSwitches(proc));
    validate(proc);
    CHECK_EQ(numberOfValues(proc, Branch), 0u);
    CHECK_EQ(numberOfValues(proc, Switch), 1u);
    CHECK_EQ(proc.at(0)->last()->as<SwitchValue>()->numCaseValues(), degree - 1);
    auto code = compileProc(proc);
    for (unsigned i = 0; i < degree; ++i)
        CHECK_EQ(invoke<int64_t>(*code, caseValueAt(i), 0), static_cast<int64_t>(i == 200 ? 5 : i));
    CHECK_EQ(invoke<int64_t>(*code, 1, 0), -1);
    CHECK_EQ(invoke<int64_t>(*code, 200 * 7, 0), -1);
}

static void testInferSwitchesIgnoresTestOfOtherValue()
{
    constexpr unsigned degree = 300;
    Procedure proc;
    Graph graph { proc };
    buildChainOfEqualityTests(graph, degree, [&](unsigned i) {
        return std::pair { i >= 200, static_cast<int64_t>(i) };
    });

    CHECK(inferSwitches(proc));
    validate(proc);
    CHECK_EQ(numberOfValues(proc, Branch), 0u);
    CHECK_EQ(numberOfValues(proc, Switch), 2u);
    CHECK_EQ(proc.at(0)->last()->as<SwitchValue>()->numCaseValues(), 200u);
    auto code = compileProc(proc);
    CHECK_EQ(invoke<int64_t>(*code, 150, 250), 150);
    CHECK_EQ(invoke<int64_t>(*code, 250, 250), 250);
    CHECK_EQ(invoke<int64_t>(*code, 250, 150), -1);
}

struct LoopWithArms {
    explicit LoopWithArms(Graph& graph)
        : graph(graph)
        , root(graph.proc.addBlock())
        , header(graph.proc.addBlock())
        , arm(graph.proc.addBlock())
        , rareArm(graph.proc.addBlock(0.01))
        , latch(graph.proc.addBlock())
        , exit(graph.proc.addBlock())
        , index(graph.proc.addVariable(Int64))
        , sum(graph.proc.addVariable(Int64))
    {
        auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(graph.proc, root);
        bound = arguments[0];
        pointer = arguments[1];
        operand = arguments[2];
        graph.set(root, index, graph.int64(root, 0));
        graph.set(root, sum, graph.int64(root, 0));
        graph.jump(root, header);
    }

    bool hoist(bool isPositionIndependent)
    {
        BasicBlock* next = graph.proc.addBlock();
        graph.branch(header, graph.make(header, BitAnd, graph.get(header, index), graph.int64(header, 1)), arm, next);
        graph.jump(arm, next);
        graph.branch(next, graph.make(next, Equal, graph.get(next, index), graph.int64(next, 1000)), rarely(rareArm), FrequentedBlock(latch));
        graph.jump(rareArm, latch);
        graph.increase(latch, index, 1);
        graph.branch(latch, graph.make(latch, LessThan, graph.get(latch, index), bound), header, exit);
        graph.ret(exit, graph.get(exit, sum));
        graph.proc.resetReachability();
        fixSSA(graph.proc);
        graph.proc.setPositionIndependent(isPositionIndependent);
        bool result = hoistLoopInvariantValues(graph.proc);
        graph.proc.setPositionIndependent(false);
        validate(graph.proc);
        return result;
    }

    Graph& graph;
    BasicBlock* root;
    BasicBlock* header;
    BasicBlock* arm;
    BasicBlock* rareArm;
    BasicBlock* latch;
    BasicBlock* exit;
    Variable* index;
    Variable* sum;
    Value* bound;
    Value* pointer;
    Value* operand;
};

static void testHoistLoopInvariantValuesOnlyFromGuaranteedBlocks()
{
    for (bool isPositionIndependent : { true, false }) {
        Procedure proc;
        Graph graph { proc };
        LoopWithArms loop(graph);
        graph.increase(loop.header, loop.sum, graph.make(loop.header, Mul, loop.operand, loop.operand));
        graph.increase(loop.arm, loop.sum, graph.make(loop.arm, BitOr, loop.operand, loop.bound));
        graph.increase(loop.latch, loop.sum, graph.make(loop.latch, BitXor, loop.operand, loop.bound));

        CHECK(loop.hoist(isPositionIndependent));
        CHECK_EQ(numberOfValuesInLoops(proc, Mul), 0u);
        CHECK_EQ(numberOfValuesInLoops(proc, BitXor), 0u);
        CHECK_EQ(numberOfValuesInLoops(proc, BitOr), isPositionIndependent ? 1u : 0u);
        CHECK_EQ(run(*compileProc(proc), 4, nullptr, 9), 4 * 81 + 2 * (9 | 4) + 4 * (9 ^ 4));
    }
}

static void testHoistLoopInvariantValuesKeepsLoadsInLoopsWithCalls()
{
    struct Case {
        bool isPositionIndependent;
        bool callIsRare;
        bool isHoisted;
    };
    for (Case testCase : { Case { true, false, false }, Case { true, true, true }, Case { false, false, true } }) {
        Procedure proc;
        Graph graph { proc };
        LoopWithArms loop(graph);
        graph.increase(loop.header, loop.sum, graph.load(loop.header, Int64, loop.pointer, 0, firstHeap));
        graph.callLikePatchpoint(testCase.callIsRare ? loop.rareArm : loop.latch);

        loop.hoist(testCase.isPositionIndependent);
        CHECK_EQ(numberOfValuesInLoops(proc, Load), testCase.isHoisted ? 0u : 1u);
        int64_t memory[1] = { 6 };
        CHECK_EQ(run(*compileProc(proc), 4, memory), 24);
    }
}

static void testHoistLoopInvariantValuesLimitsHoistedLoads()
{
    for (bool isPositionIndependent : { true, false }) {
        Procedure proc;
        Graph graph { proc };
        LoopWithArms loop(graph);
        constexpr unsigned numberOfLoads = 10;
        for (unsigned i = 0; i < numberOfLoads; ++i)
            graph.increase(loop.header, loop.sum, graph.load(loop.header, Int64, loop.pointer, i * 8, firstHeap));

        CHECK(loop.hoist(isPositionIndependent));
        CHECK_EQ(numberOfValuesInLoops(proc, Load), isPositionIndependent ? 2u : 0u);
        int64_t memory[numberOfLoads] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
        CHECK_EQ(run(*compileProc(proc), 2, memory), 110);
    }
}

static void testHoistLoopInvariantValuesHoistsImmutableLoadPastStores()
{
    for (Mutability mutability : { Mutability::Immutable, Mutability::Mutable }) {
        for (bool isPositionIndependent : { true, false }) {
            Procedure proc;
            Graph graph { proc };
            LoopWithArms loop(graph);
            MemoryValue* loaded = graph.load(loop.header, Int64, loop.pointer, 0, firstHeap);
            loaded->setReadsMutability(mutability);
            graph.increase(loop.header, loop.sum, loaded);
            graph.store(loop.latch, loop.operand, loop.pointer, 8, firstHeap);

            loop.hoist(isPositionIndependent);
            CHECK_EQ(numberOfValuesInLoops(proc, Load), mutability == Mutability::Immutable ? 0u : 1u);
            int64_t memory[2] = { 6, 0 };
            CHECK_EQ(run(*compileProc(proc), 4, memory, 3), 24);
            CHECK_EQ(memory[1], 3);
        }
    }
}

static void testLargePredecessorLists()
{
    Procedure proc;
    constexpr unsigned numberOfPredecessors = 200;
    BasicBlock* target = proc.addBlock();
    Vector<BasicBlock*> blocks;
    for (unsigned i = 0; i < numberOfPredecessors; ++i)
        blocks.append(proc.addBlock());

    LargePredecessorLists<BasicBlock> lists;
    for (BasicBlock* block : blocks)
        CHECK(lists.add(target, block));
    CHECK_EQ(target->numPredecessors(), numberOfPredecessors);
    for (BasicBlock* block : blocks)
        CHECK(!lists.add(target, block));
    CHECK_EQ(target->numPredecessors(), numberOfPredecessors);

    for (unsigned i = 0; i < numberOfPredecessors; i += 2) {
        CHECK(lists.remove(target, blocks[i]));
        CHECK(!lists.remove(target, blocks[i]));
    }
    CHECK_EQ(target->numPredecessors(), numberOfPredecessors / 2);
    for (unsigned i = 0; i < numberOfPredecessors; ++i)
        CHECK_EQ(target->containsPredecessor(blocks[i]), !!(i & 1));

    CHECK(lists.replace(target, blocks[1], blocks[0]));
    CHECK(target->containsPredecessor(blocks[0]) && !target->containsPredecessor(blocks[1]));
    CHECK_EQ(target->numPredecessors(), numberOfPredecessors / 2);
    CHECK(lists.replace(target, blocks[3], blocks[5]));
    CHECK(target->containsPredecessor(blocks[5]) && !target->containsPredecessor(blocks[3]));
    CHECK_EQ(target->numPredecessors(), numberOfPredecessors / 2 - 1);
    CHECK(!lists.replace(target, blocks[2], blocks[5]));

    for (unsigned i = 0; i < numberOfPredecessors; ++i)
        lists.remove(target, blocks[i]);
    CHECK_EQ(target->numPredecessors(), 0u);
    CHECK(lists.add(target, blocks[7]));
    CHECK(!lists.add(target, blocks[7]));
    CHECK_EQ(target->numPredecessors(), 1u);
}

static void testLargePredecessorListsDuplicatePredecessor()
{
    Procedure proc;
    BasicBlock* target = proc.addBlock();
    Vector<BasicBlock*> blocks;
    for (unsigned i = 0; i < 100; ++i) {
        blocks.append(proc.addBlock());
        target->predecessors().append(blocks.last());
    }
    target->predecessors().append(blocks[0]);

    LargePredecessorLists<BasicBlock> lists;
    CHECK(!lists.add(target, blocks[50]));
    BasicBlock* other = proc.addBlock();
    CHECK(lists.add(target, other));
    CHECK_EQ(target->numPredecessors(), 102u);
    CHECK(lists.remove(target, blocks[50]));
    CHECK(!target->containsPredecessor(blocks[50]));
    CHECK_EQ(target->numPredecessors(), 101u);
}

static void testBreakCriticalEdgesManyPredecessors()
{
    constexpr unsigned degree = 150;
    Procedure proc;
    Graph graph { proc };
    BasicBlock* block = proc.addBlock();
    BasicBlock* shared = proc.addBlock();
    Value* operand = cCallArgumentValues<int64_t>(proc, block)[0];
    Value* phi = graph.phi(shared, Int64);
    for (unsigned i = 0; i < degree; ++i) {
        BasicBlock* next = proc.addBlock();
        graph.upsilon(block, graph.int64(block, i * 2), phi);
        graph.branch(block, graph.make(block, Equal, operand, graph.int64(block, i)), shared, next);
        block = next;
    }
    graph.ret(block, graph.int64(block, -1));
    graph.ret(shared, phi);
    proc.resetReachability();
    CHECK_EQ(shared->numPredecessors(), degree);

    breakCriticalEdges(proc);
    validate(proc);
    CHECK_EQ(shared->numPredecessors(), degree);
    for (BasicBlock* predecessor : shared->predecessors()) {
        CHECK_EQ(predecessor->numSuccessors(), 1u);
        CHECK_EQ(predecessor->numPredecessors(), 1u);
        CHECK(predecessor->last()->opcode() == Jump);
    }
    CHECK_EQ(numberOfBlocks(proc), 2 * degree + 2);
    auto code = compileProc(proc);
    for (unsigned i = 0; i < degree; ++i)
        CHECK_EQ(invoke<int64_t>(*code, i), static_cast<int64_t>(i * 2));
    CHECK_EQ(invoke<int64_t>(*code, degree), -1);
}

static void testBreakCriticalEdgesPadFrequency()
{
    for (bool isPositionIndependent : { true, false }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock(0.25);
        BasicBlock* other = proc.addBlock(4);
        BasicBlock* shared = proc.addBlock(4);
        Value* condition = cCallArgumentValues<int64_t>(proc, root)[0];
        graph.branch(root, condition, shared, other);
        graph.jump(other, shared);
        graph.ret(shared, condition);
        proc.resetReachability();
        proc.setPositionIndependent(isPositionIndependent);
        breakCriticalEdges(proc);
        validate(proc);

        BasicBlock* pad = root->successorBlock(0);
        CHECK(pad != shared && pad->successorBlock(0) == shared);
        CHECK_EQ(pad->frequency(), isPositionIndependent ? 0.25 : 4.0);
    }
}

static void testResetReachabilityRemovesUpsilonsOfDeadPhis()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* dead = proc.addBlock();
    BasicBlock* live = proc.addBlock();
    Value* operand = cCallArgumentValues<int64_t>(proc, root)[0];
    Value* deadPhi = graph.phi(dead, Int64);
    Value* livePhi = graph.phi(live, Int64);
    graph.upsilon(root, operand, deadPhi);
    graph.upsilon(root, operand, livePhi);
    graph.jump(root, live);
    graph.ret(dead, deadPhi);
    graph.ret(live, livePhi);

    proc.resetReachability();
    validate(proc);
    CHECK_EQ(numberOfBlocks(proc), 2u);
    CHECK_EQ(numberOfValues(proc, Upsilon), 1u);
    CHECK_EQ(numberOfValues(proc, Phi), 1u);
    CHECK_EQ(compileAndRun<int64_t>(proc, 42), 42);
}

static void testCachedBlockOrdersAreInvalidated()
{
    for (bool isPositionIndependent : { true, false }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* exit = proc.addBlock();
        graph.jump(root, exit);
        graph.ret(exit, graph.int64(exit, 1));
        proc.resetReachability();
        proc.setPositionIndependent(isPositionIndependent);
        CHECK(proc.blocksInPreOrder() == (Vector<BasicBlock*> { root, exit }));
        CHECK(proc.blocksInPostOrder() == (Vector<BasicBlock*> { exit, root }));
        CHECK(proc.blocksInPreOrder() == (Vector<BasicBlock*> { root, exit }));

        BasicBlock* middle = proc.addBlock();
        root->setSuccessors(FrequentedBlock(middle));
        graph.jump(middle, exit);
        proc.resetReachability();
        proc.invalidateCFG();
        CHECK(proc.blocksInPreOrder() == (Vector<BasicBlock*> { root, middle, exit }));
        CHECK(proc.blocksInPostOrder() == (Vector<BasicBlock*> { exit, middle, root }));
    }
}

static void testUseCountsDuplicateChild()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t>(proc, root);
    Value* first = arguments[0];
    Value* second = arguments[1];
    Value* third = arguments[2];
    Value* sum = graph.make(root, Add, first, first);
    Value* difference = graph.make(root, Sub, sum, second);
    graph.ret(root, graph.select(root, second, second, difference));

    UseCounts useCounts(proc);
    CHECK_EQ(useCounts.numUses(first), 2u);
    CHECK_EQ(useCounts.numUsingInstructions(first), 1u);
    CHECK_EQ(useCounts.numUses(second), 3u);
    CHECK_EQ(useCounts.numUsingInstructions(second), 2u);
    CHECK_EQ(useCounts.numUses(third), 0u);
    CHECK_EQ(useCounts.numUses(sum), 1u);
    CHECK_EQ(useCounts.numUsingInstructions(difference), 1u);
}

template<typename Functor>
static unsigned numberOfMove32ForZeroExtensionOf(bool isPositionIndependent, const Functor& makeWord)
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t, int32_t, int64_t>(proc, root);
    Value* first = arguments[0];
    Value* second = arguments[1];
    Value* wide = arguments[2];
    proc.setPositionIndependent(isPositionIndependent);
    graph.ret(root, graph.make(root, ZExt32, makeWord(graph, root, first, second, wide)));
    lowerToAirForTesting(proc);
    unsigned result = 0;
    for (Air::BasicBlock* block : proc.code()) {
        for (Air::Inst& inst : *block)
            result += inst.kind.opcode == Air::Move32;
    }
    return result;
}

static void testRedundantZeroExtension()
{
    auto binary = [](B3::Opcode opcode) {
        return [opcode](Graph& graph, BasicBlock* block, Value* first, Value* second, Value*) {
            return graph.make(block, opcode, first, second);
        };
    };
    auto shift = [](B3::Opcode opcode, int32_t amount) {
        return [opcode, amount](Graph& graph, BasicBlock* block, Value* first, Value*, Value*) {
            return graph.make(block, opcode, first, graph.int32(block, amount));
        };
    };
    auto truncation = [](Graph& graph, BasicBlock* block, Value*, Value*, Value* wide) {
        return graph.make(block, Trunc, graph.make(block, Add, wide, wide));
    };
    for (B3::Opcode opcode : { Add, Sub, Mul, BitAnd, BitOr, BitXor, Equal, LessThan, Below }) {
        CHECK_EQ(numberOfMove32ForZeroExtensionOf(true, binary(opcode)), 0u);
        CHECK_EQ(numberOfMove32ForZeroExtensionOf(false, binary(opcode)), 1u);
    }
    CHECK_EQ(numberOfMove32ForZeroExtensionOf(true, shift(Shl, 3)), 0u);
    CHECK_EQ(numberOfMove32ForZeroExtensionOf(true, shift(ZShr, 31)), 0u);

    CHECK_EQ(numberOfMove32ForZeroExtensionOf(true, shift(Shl, 0)), 1u);
    CHECK_EQ(numberOfMove32ForZeroExtensionOf(true, shift(Shl, 32)), 1u);
    CHECK_EQ(numberOfMove32ForZeroExtensionOf(true, shift(SShr, 3)), 1u);
    CHECK_EQ(numberOfMove32ForZeroExtensionOf(true, binary(Shl)), 1u);
    CHECK_EQ(numberOfMove32ForZeroExtensionOf(true, truncation), 1u);
}

static void testZeroExtensionClearsUpperHalfInPositionIndependentCode()
{
    struct Case {
        B3::Opcode opcode;
        uint32_t (*reference)(uint32_t, uint32_t);
    };
    for (Case testCase : { Case { Add, [](uint32_t a, uint32_t b) { return a + b; } }, Case { Sub, [](uint32_t a, uint32_t b) { return a - b; } }, Case { Mul, [](uint32_t a, uint32_t b) { return a * b; } },
        Case { BitOr, [](uint32_t a, uint32_t b) { return a | b; } }, Case { BitXor, [](uint32_t a, uint32_t b) { return a ^ b; } }, Case { BitAnd, [](uint32_t a, uint32_t b) { return a & b; } } }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root);
        Value* first = graph.make(root, Trunc, arguments[0]);
        Value* second = graph.make(root, Trunc, arguments[1]);
        graph.ret(root, graph.make(root, ZExt32, graph.make(root, testCase.opcode, first, second)));
        proc.setPositionIndependent();
        auto code = compileProc(proc);
        for (int64_t a : { 0ll, 1ll, -1ll, 0x7fffffffll, 0x80000000ll, 0x123456789abcdef0ll }) {
            for (int64_t b : { 0ll, 1ll, -1ll, 0x7fffffffll, -0xfffffffdll })
                CHECK_EQ(invoke<uint64_t>(*code, a, b), static_cast<uint64_t>(testCase.reference(static_cast<uint32_t>(a), static_cast<uint32_t>(b))));
        }
    }
}

static void buildSumOfTwoSignExtendedLoads(Graph& graph)
{
    BasicBlock* root = graph.proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t*, int64_t>(graph.proc, root);
    Value* pointer = arguments[0];
    Value* index = arguments[1];
    Value* atOffset = graph.make(root, SExt32, graph.load(root, Int32, pointer, 4, firstHeap));
    Value* atIndex = graph.make(root, SExt32, graph.load(root, Int32, graph.make(root, Add, pointer, graph.make(root, Shl, index, graph.int32(root, 2))), 0, firstHeap));
    graph.ret(root, graph.make(root, Add, atOffset, atIndex));
}

static void testSignExtendingLoad32()
{
    {
        Procedure proc;
        Graph graph { proc };
        buildSumOfTwoSignExtendedLoads(graph);
        lowerToAirForTesting(proc);
        unsigned numberOfExtendingLoads = 0;
        for (Air::BasicBlock* block : proc.code()) {
            for (Air::Inst& inst : *block) {
                CHECK(inst.kind.opcode != Air::Move32);
                numberOfExtendingLoads += inst.kind.opcode == Air::SignExtend32To64 && inst.args()[0].isMemory();
            }
        }
        CHECK_EQ(numberOfExtendingLoads, 2u);
    }
    Procedure proc;
    Graph graph { proc };
    buildSumOfTwoSignExtendedLoads(graph);
    auto code = compileProc(proc);
    int32_t memory[3] = { -5, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max() };
    CHECK_EQ(invoke<int64_t>(*code, memory, 0), static_cast<int64_t>(std::numeric_limits<int32_t>::min()) - 5);
    CHECK_EQ(invoke<int64_t>(*code, memory, 1), 2 * static_cast<int64_t>(std::numeric_limits<int32_t>::min()));
    CHECK_EQ(invoke<int64_t>(*code, memory, 2), -1);
}

static void testMul64ByImmediate()
{
    for (int64_t factor : { 3ll, 10ll, -7ll, 1000003ll, 0x7fffffffll, 0x123456789ll }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        graph.ret(root, graph.make(root, Mul, cCallArgumentValues<int64_t>(proc, root)[0], graph.int64(root, factor)));
        auto code = compileProc(proc);
        for (int64_t operand : { 0ll, 1ll, -1ll, 12345ll, 0x100000001ll })
            CHECK_EQ(invoke<int64_t>(*code, operand), static_cast<int64_t>(static_cast<uint64_t>(operand) * static_cast<uint64_t>(factor)));
    }
}

template<typename Functor>
static std::unique_ptr<Compilation> compileWithLoopPhases(Procedure& proc, const Functor& inspectOptimizedProcedure)
{
    proc.setPositionIndependent();
    proc.setMaterializesConstantsByFrequency();
    proc.setRotatesLoops();
    proc.setLastPhase(createSharedTask<void(Procedure&)>([&](Procedure& optimized) {
        inspectOptimizedProcedure(optimized);
    }));
    return compileProc(proc, 2);
}

static void testLoopPhasesArraySum()
{
    Procedure proc;
    Graph graph { proc };
    PollState state;
    TopTestedLoop loop(graph);
    BasicBlock* outOfBounds = proc.addBlock();
    BasicBlock* inBounds = proc.addBlock();
    BasicBlock* block = appendPoll(graph, loop.body, state);
    Value* length = graph.make(block, ZExt32, graph.load(block, Int32, loop.pointer, 0, firstHeap));
    graph.branch(block, graph.make(block, AboveEqual, graph.get(block, loop.index), length), rarely(outOfBounds), FrequentedBlock(inBounds));
    graph.ret(outOfBounds, graph.int64(outOfBounds, -1));
    Value* address = graph.make(inBounds, Add, loop.pointer, graph.make(inBounds, Shl, graph.get(inBounds, loop.index), graph.int32(inBounds, 3)));
    graph.increase(inBounds, loop.sum, graph.load(inBounds, Int64, address, 8, secondHeap));
    loop.endBody(inBounds);

    Vector<unsigned> numberOfLoads;
    auto code = compileWithLoopPhases(proc, [&](Procedure& optimized) {
        numberOfLoads = numberOfValuesInEachLoop(optimized, Load);
    });
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
    CHECK_EQ(run(*code, size + 1, memory.mutableSpan().data()), -1);

    state = { };
    state.limit = -1;
    CHECK_EQ(run(*code, size, memory.mutableSpan().data()), expected);
    CHECK_EQ(state.numberOfStops, 1);
}

static void testLoopPhasesFoldTestsImpliedByLoopCondition()
{
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop(graph);
    BasicBlock* block = loop.body;
    addIfComparisonHolds(graph, block, loop.sum, graph.make(block, LessThan, graph.get(block, loop.index), loop.bound), 1, 1000);
    addIfComparisonHolds(graph, block, loop.sum, graph.make(block, GreaterEqual, graph.get(block, loop.index), loop.bound), 2000, 10);
    loop.endBody(block);

    unsigned numberOfBranchesInLoops = 0;
    bool hasDeadArms = true;
    auto code = compileWithLoopPhases(proc, [&](Procedure& optimized) {
        numberOfBranchesInLoops = numberOfValuesInLoops(optimized, Branch);
        hasDeadArms = hasInt64Constant(optimized, 1000) || hasInt64Constant(optimized, 2000);
    });
    CHECK_EQ(numberOfBranchesInLoops, 1u);
    CHECK(!hasDeadArms);
    for (int64_t bound : { -1, 0, 1, 7 })
        CHECK_EQ(run(*code, bound), std::max<int64_t>(bound, 0) * 11);
}

static void testLoopPhasesAreOptIn()
{
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop(graph);
    graph.increase(loop.body, loop.sum, graph.get(loop.body, loop.index));
    loop.endBody(loop.body);

    bool headerEndsInBranch = false;
    proc.setLastPhase(createSharedTask<void(Procedure&)>([&](Procedure& optimized) {
        B3::NaturalLoops& loops = optimized.naturalLoops();
        CHECK_EQ(loops.numLoops(), 1u);
        headerEndsInBranch = loops.loop(0).header()->last()->opcode() == Branch;
    }));
    auto code = compileProc(proc, 2);
    CHECK(headerEndsInBranch);
    CHECK_EQ(run(*code, 10), 45);
}

void addBunTests(const TestConfig* config, Deque<RefPtr<SharedTask<void()>>>& tasks)
{
    RUN(testRotateLoopsMovesTestToBottom());
    RUN(testRotateLoopsBottomTestedLoop());
    RUN(testRotateLoopsSkipsLargeTest());
    RUN(testRotateLoopsSkipsUncloneableTest());
    RUN(testRotateLoopsSkipsRareBackEdge());
    RUN(testRotateLoopsHonorsHeaderFilter());
    RUN(testRotateLoopsNestedLoops());
    RUN(testSimplifyTerminals());
    RUN(testSimplifyTerminalsKeepsNonConstantBranch());
    RUN(testFoldComparisonsWithBounds());
    RUN(testFoldComparisonsWithBoundsKeepsUnrelatedComparisons());
    RUN(testFoldComparisonsWithBoundsRequiresRelationOnEntry());
    RUN(testAnalyzeCountedLoop());
    RUN(testEliminateRedundantPhiCycles());
    RUN(testNarrowPhisOfExtendedWords());
    RUN(testFoldComparisonsWithWhatIsOutOfRange());
    RUN(testVersionLoopsHoistsLoadAndGuard());
    RUN(testVersionLoopsHoistsLoadPastStoreToDisjointHeap());
    RUN(testVersionLoopsKeepsLoadClobberedByLoop());
    RUN(testVersionLoopsSkipsLoopWithCall());
    RUN(testVersionLoopsSkipsLargeLoop());
    RUN(testVersionLoopsSkipsLoopWithNothingToHoist());
    RUN(testVersionLoopsOnlyInnermostLoops());
    RUN(testVersionLoopsRareEdgeContinuesInOriginalLoop());
    RUN(testVersionLoopsUnswitchesInvariantBranch());
    RUN(testVersionLoopsKeepsVariantBranch());
    RUN(testVersionLoopsStripMinesPollInFastVersion());
    RUN(testStripMineLoopsPollsOncePerStrip());
    RUN(testStripMineLoopsAllRelationsAndSteps());
    RUN(testStripMineLoopsRequiresPollGenerator());
    RUN(testStripMineLoopsSkipsLoopWithTwoPolls());
    RUN(testStripMineLoopsSkipsVariantBound());
    RUN(testStripMineLoopsSkipsPollWithSharedLimit());
    RUN(testSinkValuesMovesValueIntoRareUser());
    RUN(testSinkValuesClonesValueForTwoRareUsers());
    RUN(testSinkValuesKeepsValueUsedOnHotPath());
    RUN(testSinkValuesKeepsValueWhoseUserIsAsFrequent());
    RUN(testSinkValuesKeepsValueUsedInSameBlock());
    RUN(testSinkValuesKeepsLoads());
    RUN(testSinkValuesKeepsOpaque());
    RUN(testSinkValuesDoesNotSinkIntoLoop());
    RUN(testSinkValuesDoesNotSinkFromPreHeaderIntoLoop());
    RUN(testCycleForestNestedLoops());
    RUN(testCycleForestAcyclic());
    RUN(testCycleForestIrreducibleCycle());
    RUN(testCycleForestPrefersPollingEntryAsHeader());
    RUN(testMemoryNumberingEliminatesRedundantLoad());
    RUN(testMemoryNumberingKeepsLoadAfterAliasingStore());
    RUN(testMemoryNumberingEliminatesLoadAfterDisjointStore());
    RUN(testMemoryNumberingForwardsStoreToLoad());
    RUN(testMemoryNumberingDoesNotForwardNarrowStore());
    RUN(testMemoryNumberingHeapRangeWrites());
    RUN(testMemoryNumberingKeepsLoadAfterFence());
    RUN(testMemoryNumberingImmutableLoads());
    RUN(testMemoryNumberingAcrossDiamond());
    RUN(testMemoryNumberingAcrossLoops());
    RUN(testValueNumberingRecomputesCheapValuesAfterCalls());
    RUN(testBranchDominatedByBranchOnSameConditionIsFolded());
    RUN(testDoubleComparisonsOfConvertedIntegersBecomeIntegerComparisons());
    RUN(testDoubleComparisonsOfNonIntegersAreKept());
    RUN(testBitwiseDoubleComparisonsRespectNegativeZero());
    RUN(testComparisonsFoldedByIntegerRange());
    RUN(testDoubleToIntegerConversionsOfConvertedIntegers());
    RUN(testPurePatchpointsAreValueNumbered());
    RUN(testReduceStrengthMergesBlockChain());
    RUN(testReduceStrengthEliminatesSingleValuePhi());
    RUN(testMemoryNumberingWhenEdgeIntoJoinIsRemoved());
    RUN(testMoveConstantsMaterializesByFrequency());
    RUN(testMoveConstantsByFrequencyHoistsOutOfLoop());
    RUN(testMoveConstantsPositionIndependentDoubles());
    RUN(testDuplicateTailsThreadsJumpOnConstantPhi());
    RUN(testDuplicateTailsThreadsJumpDecidedByDominatingBranch());
    RUN(testDuplicateTailsKeepsUndecidedTest());
    RUN(testDuplicateTailsSkipsTailWithPatchpoint());
    RUN(testDuplicateTailsSkipsLoopHeader());
    RUN(testFoldPathConstantsFoldsSelect());
    RUN(testSwitchInPositionIndependentCode());
    RUN(testInferSwitchesManyCases());
    RUN(testInferSwitchesIgnoresTestOfOtherValue());
    RUN(testHoistLoopInvariantValuesOnlyFromGuaranteedBlocks());
    RUN(testHoistLoopInvariantValuesKeepsLoadsInLoopsWithCalls());
    RUN(testHoistLoopInvariantValuesLimitsHoistedLoads());
    RUN(testHoistLoopInvariantValuesHoistsImmutableLoadPastStores());
    RUN(testLargePredecessorLists());
    RUN(testLargePredecessorListsDuplicatePredecessor());
    RUN(testBreakCriticalEdgesManyPredecessors());
    RUN(testBreakCriticalEdgesPadFrequency());
    RUN(testResetReachabilityRemovesUpsilonsOfDeadPhis());
    RUN(testCachedBlockOrdersAreInvalidated());
    RUN(testUseCountsDuplicateChild());
    RUN(testRedundantZeroExtension());
    RUN(testZeroExtensionClearsUpperHalfInPositionIndependentCode());
    RUN(testSignExtendingLoad32());
    RUN(testMul64ByImmediate());
    RUN(testLoopPhasesArraySum());
    RUN(testLoopPhasesFoldTestsImpliedByLoopCondition());
    RUN(testLoopPhasesAreOptIn());
}

#endif // ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)
