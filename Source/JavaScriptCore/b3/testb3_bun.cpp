/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3.h"

#if ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)

#include "AOTAirPasses.h"
#include "AirGenerate.h"
#include "B3BasicBlockUtils.h"
#include "B3CompilationCohort.h"
#include "B3CountedLoops.h"
#include "B3CycleForest.h"
#include "B3DuplicateTails.h"
#include "B3EliminateDeadCode.h"
#include "B3FoldPathConstants.h"
#include "B3Generate.h"
#include "B3HoistLoopInvariantValues.h"
#include "B3InferSwitches.h"
#include "B3NaturalLoops.h"
#include "B3RotateLoops.h"
#include "B3SeparateIntegerPaths.h"
#include "B3SinkValues.h"
#include "B3StripMineLoops.h"
#include "B3Variable.h"
#include "B3VersionLoops.h"
#include "LinkBuffer.h"
#include <wtf/SetForScope.h>
#include <wtf/WeakRandom.h>

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

static void checkLoopThatCountsDown(int64_t startLessLimit, B3::Opcode relation, int64_t bound, int64_t step, bool limitIsWord, bool isFolded)
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* index = proc.addVariable(Int64);
    Variable* sum = proc.addVariable(Int64);
    Value* argument = cCallArgumentValues<int64_t>(proc, root)[0];
    Value* limit = limitIsWord ? graph.make(root, ZExt32, graph.make(root, Trunc, argument)) : argument;
    Value* boundValue = graph.int64(root, bound);
    graph.set(root, index, graph.make(root, Add, limit, graph.int64(root, startLessLimit)));
    graph.set(root, sum, graph.int64(root, 0));
    graph.jump(root, header);
    graph.branch(header, graph.make(header, relation, graph.get(header, index), boundValue), body, exit);
    graph.ret(exit, graph.get(exit, sum));
    BasicBlock* block = body;
    addIfComparisonHolds(graph, block, sum, graph.make(block, Below, graph.get(block, index), limit), 1, 1000);
    addIfComparisonHolds(graph, block, sum, graph.make(block, AboveEqual, graph.get(block, index), limit), 2000, 10);
    addIfComparisonHolds(graph, block, sum, graph.make(block, Above, limit, graph.get(block, index)), 100, 3000);
    addIfComparisonHolds(graph, block, sum, graph.make(block, GreaterEqual, graph.get(block, index), limit), 4000, 0);
    graph.increase(block, index, step);
    graph.jump(block, header);
    proc.resetReachability();
    fixSSA(proc);
    validate(proc);
    CHECK(rotateLoops(proc));
    fixSSA(proc);
    validate(proc);

    CHECK_EQ(foldComparisonsWithBounds(proc), isFolded);
    validate(proc);
    CHECK_EQ(numberOfValuesInLoops(proc, Below), isFolded ? 0u : 1u);
    CHECK_EQ(numberOfValuesInLoops(proc, AboveEqual), isFolded ? 0u : 1u);
    CHECK_EQ(numberOfValuesInLoops(proc, Above), isFolded ? 0u : 1u);
    auto code = compileProc(proc);
    for (int64_t given : { static_cast<int64_t>(-3), static_cast<int64_t>(0), static_cast<int64_t>(1), static_cast<int64_t>(2), static_cast<int64_t>(7), static_cast<int64_t>(100) }) {
        int64_t limitAtRunTime = limitIsWord ? static_cast<int64_t>(static_cast<uint32_t>(given)) : given;
        if (limitAtRunTime > 1000)
            continue;
        int64_t expected = 0;
        for (int64_t i = limitAtRunTime + startLessLimit; holds(relation, i, bound); i += step) {
            expected += holds(Below, i, limitAtRunTime) ? 1 : 1000;
            expected += holds(AboveEqual, i, limitAtRunTime) ? 2000 : 10;
            expected += holds(Above, limitAtRunTime, i) ? 100 : 3000;
            expected += holds(GreaterEqual, i, limitAtRunTime) ? 4000 : 0;
        }
        CHECK_EQ(invoke<int64_t>(*code, given), expected);
    }
}

static void checkNeighbourInLoop(bool countsUp, int64_t start, int64_t bound, int64_t added, bool isFolded)
{
    Procedure proc;
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
    graph.increase(block, index, countsUp ? 1 : -1);
    graph.jump(block, header);
    proc.resetReachability();
    fixSSA(proc);
    validate(proc);
    CHECK(rotateLoops(proc));
    fixSSA(proc);
    validate(proc);

    CHECK_EQ(foldComparisonsWithBounds(proc), isFolded);
    validate(proc);
    CHECK_EQ(numberOfValuesInLoops(proc, Below), isFolded ? 0u : 1u);
    CHECK_EQ(numberOfValuesInLoops(proc, BelowEqual), isFolded ? 0u : 1u);
    auto code = compileProc(proc);
    for (int64_t given : { static_cast<int64_t>(0), static_cast<int64_t>(1), static_cast<int64_t>(2), static_cast<int64_t>(3), static_cast<int64_t>(9), static_cast<int64_t>(100) }) {
        int64_t expected = 0;
        for (int64_t i = countsUp ? start : given + start; countsUp ? i < given : i >= bound; i += countsUp ? 1 : -1) {
            expected += holds(Below, i + added, given) ? 1 : 1000;
            expected += holds(BelowEqual, given, i + added) ? 2000 : 10;
        }
        CHECK_EQ(invoke<int64_t>(*code, given), expected);
    }
}

static void checkNeighbourInLoopThatLoadsItsLimit(int64_t start, int64_t added, bool isFolded)
{
    Procedure proc;
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
    Value* limit = graph.make(header, ZExt32, graph.load(header, Int32, pointer, 0, firstHeap));
    graph.branch(header, graph.make(header, LessThan, graph.get(header, index), limit), body, exit);
    graph.ret(exit, graph.get(exit, sum));
    graph.increase(body, sum, 7);
    graph.branch(body, graph.make(body, Below, graph.make(body, Add, graph.get(body, index), addedValue), limit), FrequentedBlock(inside), rarely(outside));
    graph.ret(outside, graph.int64(outside, -1));
    graph.store(inside, graph.get(inside, sum), pointer, 8, firstHeap);
    graph.increase(inside, index, 1);
    graph.jump(inside, header);
    proc.resetReachability();
    fixSSA(proc);
    validate(proc);
    CHECK(rotateLoops(proc));
    fixSSA(proc);
    validate(proc);

    CHECK_EQ(foldComparisonsWithBounds(proc), isFolded);
    validate(proc);
    CHECK_EQ(numberOfValuesInLoops(proc, Below), isFolded ? 0u : 1u);
    auto code = compileProc(proc);
    for (int64_t given : { static_cast<int64_t>(0), static_cast<int64_t>(1), static_cast<int64_t>(2), static_cast<int64_t>(3), static_cast<int64_t>(9), static_cast<int64_t>(100) }) {
        int64_t memory[2] = { given, 0 };
        int64_t expected = 0;
        for (int64_t i = start; i < given; ++i) {
            expected += 7;
            if (!holds(Below, i + added, given)) {
                expected = -1;
                break;
            }
        }
        CHECK_EQ(invoke<int64_t>(*code, memory), expected);
    }
}

static void testFoldComparisonsOfNeighbourWithLimitLoadedAtLoopTest()
{
    checkNeighbourInLoopThatLoadsItsLimit(1, -1, true);
    checkNeighbourInLoopThatLoadsItsLimit(2, -2, true);
    checkNeighbourInLoopThatLoadsItsLimit(2, -1, true);
    checkNeighbourInLoopThatLoadsItsLimit(0, -1, false);
    checkNeighbourInLoopThatLoadsItsLimit(0, 1, false);
    checkNeighbourInLoopThatLoadsItsLimit(1, -2, false);
}

static void testFoldComparisonsOfNeighbourOfLoopVariableWithLimit()
{
    checkNeighbourInLoop(true, 1, 0, -1, true);
    checkNeighbourInLoop(true, 2, 0, -1, true);
    checkNeighbourInLoop(true, 2, 0, -2, true);
    checkNeighbourInLoop(true, 0, 0, 0, true);
    checkNeighbourInLoop(false, -2, 0, 1, true);
    checkNeighbourInLoop(false, -1, 1, -1, true);
    checkNeighbourInLoop(false, -3, 0, 2, true);
}

static void testFoldComparisonsKeepsNeighbourThatMayBeOutside()
{
    checkNeighbourInLoop(true, 0, 0, -1, false);
    checkNeighbourInLoop(true, 1, 0, -2, false);
    checkNeighbourInLoop(true, 0, 0, 1, false);
    checkNeighbourInLoop(true, -1, 0, 0, false);
    checkNeighbourInLoop(false, -1, 0, 1, false);
    checkNeighbourInLoop(false, -1, 0, -1, false);
    checkNeighbourInLoop(false, -2, 0, 2, false);
}

static void testFoldComparisonsWithWhatLoopThatCountsDownStartsBelow()
{
    checkLoopThatCountsDown(-1, GreaterEqual, 0, -1, true, true);
    checkLoopThatCountsDown(-1, GreaterThan, -1, -1, true, true);
    checkLoopThatCountsDown(-2, GreaterEqual, 1, -1, true, true);
    checkLoopThatCountsDown(-1, GreaterThan, 0, -3, true, true);
}

static void testFoldComparisonsKeepsWhatLoopThatCountsDownMayNotBeBelow()
{
    checkLoopThatCountsDown(0, GreaterEqual, 0, -1, true, false);
    checkLoopThatCountsDown(1, GreaterEqual, 0, -1, true, false);
    checkLoopThatCountsDown(-1, GreaterEqual, -1, -1, true, false);
    checkLoopThatCountsDown(-1, GreaterThan, -2, -1, true, false);
    checkLoopThatCountsDown(-1, GreaterEqual, 0, -1, false, false);
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

static void testVersionLoopsKeepsDeepArmOfChainInFastVersion()
{
    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop(graph);
    BasicBlock* rare = proc.addBlock();
    BasicBlock* hot = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    Value* loaded = graph.load(loop.body, Int64, loop.pointer, 0, firstHeap);
    graph.branch(loop.body, graph.make(loop.body, Equal, loaded, loop.extra), FrequentedBlock(hot), rarely(rare));
    graph.increase(rare, loop.sum, 1000);
    graph.jump(rare, hot);
    graph.increase(hot, loop.sum, loaded);
    BasicBlock* test = hot;
    double frequency = 1;
    for (unsigned arm = 0; arm < 6; ++arm) {
        frequency /= 2;
        BasicBlock* armBlock = proc.addBlock(frequency);
        BasicBlock* next = arm < 5 ? proc.addBlock(frequency) : join;
        graph.branch(test, graph.make(test, Equal, graph.make(test, BitAnd, graph.get(test, loop.index), graph.int64(test, 7)), graph.int64(test, arm)), armBlock, next);
        graph.store(armBlock, graph.get(armBlock, loop.index), loop.pointer, 8, secondHeap);
        graph.jump(armBlock, join);
        test = next;
    }
    loop.endBody(join);
    loop.rotate();

    CHECK(rotateAndVersionLoops(proc));
    checkNumberOfValuesInEachLoop(proc, Store, { 6, 6 });
    checkNumberOfValuesInEachLoop(proc, Load, { 0, 1 });
    checkLoopWithInvariantGuard(proc);
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

static void checkEndOfFirstStrip(B3::Opcode relation, bool boundIsZeroExtended, int64_t start, unsigned numberOfSelects)
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    Value* argument = cCallArgumentValues<int64_t>(proc, root)[0];
    Value* bound = boundIsZeroExtended ? graph.make(root, ZExt32, graph.make(root, Trunc, argument)) : argument;
    CountedLoop loop;
    loop.relation = relation;
    loop.step = 1;
    graph.ret(root, appendEndOfStrip(proc, root, loop, graph.int64(root, start), bound, true));
    CHECK_EQ(numberOfValues(proc, Select), numberOfSelects);
    auto code = compileProc(proc);
    for (int64_t given : { static_cast<int64_t>(0), static_cast<int64_t>(1), static_cast<int64_t>(5), maxIterationsPerStrip - 1, maxIterationsPerStrip, maxIterationsPerStrip + 1, 20 * maxIterationsPerStrip }) {
        int64_t end = invoke<int64_t>(*code, given);
        if (holds(relation, start, given))
            CHECK_EQ(end, std::min(given, start + maxIterationsPerStrip));
        else
            CHECK(!holds(relation, start + 1, end));
    }
}

static void testEndOfFirstStripFromZeroToZeroExtendedBound()
{
    checkEndOfFirstStrip(LessThan, true, 0, 1);
    checkEndOfFirstStrip(Below, true, 0, 1);
    checkEndOfFirstStrip(LessThan, true, -3, 1);
    checkEndOfFirstStrip(LessThan, true, 1, 2);
    checkEndOfFirstStrip(Below, true, 1, 2);
    checkEndOfFirstStrip(LessThan, false, 0, 2);
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
    PollState state;
    TopTestedLoop loop(graph);
    BasicBlock* block = appendPoll(graph, loop.body, state);
    PatchpointValue* limit = loop.body->at(loop.body->size() - 4)->as<PatchpointValue>();
    graph.increase(block, loop.sum, graph.get(block, loop.index));
    loop.endBody(block);
    loop.rotate();

    RefPtr<StackmapGenerator> generator = proc.loopPollGenerator();
    CHECK(limit->generator() == generator);
    proc.setLoopPollGenerator(nullptr);
    limit->setGenerator(RefPtr<StackmapGenerator>());
    CHECK(!stripMineLoops(proc));
    limit->setGenerator(generator);
    CHECK_EQ(run(*compileProc(proc), 10), 45);
    CHECK_EQ(state.numberOfPolls, 10);
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

static void testSinkValuesClonesPatchpointForTwoRareUsersOnlyIfItIsPureFunctionOfKey()
{
    for (uint64_t key : { 7, 0 }) {
        Procedure proc;
        Graph graph { proc };
        BranchToRareBlocks blocks(graph);
        PatchpointValue* patchpoint = graph.emptyPatchpoint(blocks.root, Int64);
        patchpoint->pureFunctionKey = key;
        blocks.finish(graph, patchpoint, graph.make(blocks.secondRare, Add, patchpoint, blocks.operand), blocks.operand);

        CHECK_EQ(sinkValues(proc), !!key);
        validate(proc);
        CHECK_EQ(numberOfValues(blocks.root, Patchpoint), key ? 0u : 1u);
        CHECK_EQ(numberOfValues(blocks.next, Patchpoint), 0u);
        CHECK_EQ(numberOfValues(blocks.firstRare, Patchpoint), key ? 1u : 0u);
        CHECK_EQ(numberOfValues(blocks.secondRare, Patchpoint), key ? 1u : 0u);
        auto code = compileProc(proc);
        CHECK_EQ(invoke<int64_t>(*code, 1, 5, nullptr), 0);
        CHECK_EQ(invoke<int64_t>(*code, 2, 5, nullptr), 5);
        CHECK_EQ(invoke<int64_t>(*code, 3, 5, nullptr), 5);
    }
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

template<typename T>
static void testRangeOfShiftByAnyAmount(B3::Opcode opcode, int32_t amount, bool isPositionIndependent)
{
    using UnsignedT = std::make_unsigned_t<T>;
    constexpr bool is32 = sizeof(T) == 4;
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    auto integer = [&](int32_t value) {
        return is32 ? graph.int32(root, value) : graph.int64(root, value);
    };
    Value* argument = cCallArgumentValues<int64_t>(proc, root)[0];
    Value* shifted = graph.make(root, opcode, is32 ? graph.make(root, Trunc, argument) : argument, graph.int32(root, amount));
    Value* isLess = graph.make(root, LessThan, shifted, integer(-8));
    Value* lowByte = graph.make(root, BitAnd, shifted, integer(255));
    graph.ret(root, graph.make(root, Add, graph.make(root, ZExt32, isLess), graph.make(root, Shl, is32 ? graph.make(root, ZExt32, lowByte) : lowByte, graph.int32(root, 1))));
    proc.setPositionIndependent(isPositionIndependent);
    auto code = compileProc(proc, 2);

    unsigned amountTaken = amount & (sizeof(T) * 8 - 1);
    for (int64_t input : { static_cast<int64_t>(0), static_cast<int64_t>(5), static_cast<int64_t>(-1), static_cast<int64_t>(-9), static_cast<int64_t>(-1000), static_cast<int64_t>(0x7fffffff), static_cast<int64_t>(-0x80000000ll), std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min() }) {
        T operand = static_cast<T>(input);
        T expected;
        if (opcode == Shl)
            expected = static_cast<T>(static_cast<UnsignedT>(operand) << amountTaken);
        else if (opcode == SShr)
            expected = operand >> amountTaken;
        else
            expected = static_cast<T>(static_cast<UnsignedT>(operand) >> amountTaken);
        CHECK_EQ(invoke<int64_t>(*code, input), (expected < -8) + 2 * static_cast<int64_t>(expected & 255));
    }
}

static void testRangesOfShiftsByAnyAmount()
{
    for (B3::Opcode opcode : { Shl, SShr, ZShr }) {
        for (int32_t amount : { 0, 1, 31, 32, 33, 63, 64, 65, 96, 128, -1, -32, -64, -128 }) {
            for (bool isPositionIndependent : { true, false }) {
                testRangeOfShiftByAnyAmount<int32_t>(opcode, amount, isPositionIndependent);
                testRangeOfShiftByAnyAmount<int64_t>(opcode, amount, isPositionIndependent);
            }
        }
    }
}

static void testTruncOfWordShiftedLeftAndBackByAnyAmount()
{
    for (int32_t amount : { 0, 1, 12, 30, 31, 32, 33, 60, 63, 64, 76, -1, -4, -33, -52, -60, -64 }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        Value* word = graph.make(root, SExt32, graph.make(root, Trunc, cCallArgumentValues<int64_t>(proc, root)[0]));
        Value* shifted = graph.make(root, SShr, graph.make(root, Shl, word, graph.int32(root, amount)), graph.int32(root, amount));
        graph.ret(root, graph.make(root, SExt32, graph.make(root, Trunc, shifted)));
        auto code = compileProc(proc, 2);

        unsigned amountTaken = amount & 63;
        for (int32_t input : { 0, 1, -1, 7, 8, -9, 0x12345678, std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::min() }) {
            int64_t expected = static_cast<int64_t>(static_cast<uint64_t>(static_cast<int64_t>(input)) << amountTaken) >> amountTaken;
            CHECK_EQ(invoke<int64_t>(*code, static_cast<int64_t>(input)), static_cast<int64_t>(static_cast<int32_t>(expected)));
        }
    }
}

struct NumberKeptAsWord {
    explicit NumberKeptAsWord(Graph& graph, double frequencyOfOtherPath = 0.001)
        : root(graph.proc.addBlock())
        , integerPath(graph.proc.addBlock())
        , otherPath(graph.proc.addBlock(frequencyOfOtherPath))
        , join(graph.proc.addBlock())
    {
        auto arguments = cCallArgumentValues<int32_t, double, int64_t*>(graph.proc, root);
        word = arguments[0];
        other = arguments[1];
        pointer = arguments[2];
        Value* isOther = graph.make(root, LessThan, word, graph.int32(root, -1000));
        if (frequencyOfOtherPath < 0.5)
            graph.branch(root, isOther, rarely(otherPath), FrequentedBlock(integerPath));
        else
            graph.branch(root, isOther, otherPath, integerPath);
        number = graph.phi(join, Double);
        graph.upsilon(integerPath, graph.make(integerPath, IToD, word), number);
        graph.jump(integerPath, join);
        graph.upsilon(otherPath, other, number);
        graph.jump(otherPath, join);
    }

    Value* truncate(Graph& graph, BasicBlock* block, Value* value)
    {
        PatchpointValue* result = block->appendNew<PatchpointValue>(graph.proc, Int64, Origin());
        result->effects = Effects::none();
        result->pureFunctionOfChildrenKey = keyOfTruncationToInt64;
        result->appendSomeRegister(value);
        result->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
            jit.truncateDoubleToInt64(params[1].fpr(), params[0].gpr());
        });
        return result;
    }

    bool separate(Graph& graph)
    {
        graph.proc.resetReachability();
        validate(graph.proc);
        bool result = separateIntegerPaths(graph.proc);
        validate(graph.proc);
        reduceStrengthOnce(graph.proc, true);
        eliminateDeadCode(graph.proc);
        validate(graph.proc);
        return result;
    }

    unsigned numberOfDoublesOnLikelyPaths(Graph& graph)
    {
        unsigned result = 0;
        for (BasicBlock* block : graph.proc) {
            if (block->frequency() < 0.5)
                continue;
            for (Value* value : *block)
                result += value->type() == Double && value != other && !value->isConstant();
        }
        return result;
    }

    BasicBlock* root;
    BasicBlock* integerPath;
    BasicBlock* otherPath;
    BasicBlock* join;
    Value* word;
    Value* other;
    Value* pointer;
    Value* number;
};

static void testSeparateIntegerPathsFoldsConversionBackToInteger()
{
    Procedure proc;
    Graph graph { proc };
    NumberKeptAsWord blocks(graph);
    BasicBlock* isWhole = proc.addBlock();
    BasicBlock* isNotWhole = proc.addBlock(0.001);
    Value* truncated = blocks.truncate(graph, blocks.join, blocks.number);
    graph.branch(blocks.join, graph.make(blocks.join, Equal, graph.make(blocks.join, IToD, truncated), blocks.number), FrequentedBlock(isWhole), rarely(isNotWhole));
    graph.ret(isWhole, truncated);
    graph.ret(isNotWhole, graph.int64(isNotWhole, -1));

    CHECK(blocks.separate(graph));
    CHECK_EQ(numberOfValues(proc, Patchpoint), 1u);
    CHECK_EQ(blocks.numberOfDoublesOnLikelyPaths(graph), 0u);
    auto code = compileProc(proc);
    CHECK_EQ(invoke<int64_t>(*code, 7, 0.5, nullptr), 7);
    CHECK_EQ(invoke<int64_t>(*code, -1000, 0.5, nullptr), -1000);
    CHECK_EQ(invoke<int64_t>(*code, -1001, 12.0, nullptr), 12);
    CHECK_EQ(invoke<int64_t>(*code, -1001, 12.5, nullptr), -1);
    CHECK_EQ(invoke<int64_t>(*code, -1001, std::numeric_limits<double>::quiet_NaN(), nullptr), -1);
}

static void testSeparateIntegerPathsFollowsSumsIntoLaterBlocks()
{
    Procedure proc;
    Graph graph { proc };
    NumberKeptAsWord blocks(graph);
    BasicBlock* next = proc.addBlock();
    BasicBlock* isSmall = proc.addBlock();
    BasicBlock* isLarge = proc.addBlock();
    Value* sum = graph.make(blocks.join, Add, blocks.number, graph.number(blocks.join, 1));
    graph.jump(blocks.join, next);
    graph.branch(next, graph.make(next, LessThan, sum, graph.number(next, 10)), isSmall, isLarge);
    graph.ret(isSmall, graph.int64(isSmall, 1));
    graph.ret(isLarge, graph.int64(isLarge, 2));

    CHECK(blocks.separate(graph));
    CHECK_EQ(blocks.numberOfDoublesOnLikelyPaths(graph), 0u);
    auto code = compileProc(proc);
    CHECK_EQ(invoke<int64_t>(*code, 8, 0.0, nullptr), 1);
    CHECK_EQ(invoke<int64_t>(*code, 9, 0.0, nullptr), 2);
    CHECK_EQ(invoke<int64_t>(*code, -1001, 8.5, nullptr), 1);
    CHECK_EQ(invoke<int64_t>(*code, -1001, 9.5, nullptr), 2);
    CHECK_EQ(invoke<int64_t>(*code, -1001, std::numeric_limits<double>::quiet_NaN(), nullptr), 2);
}

static void testSeparateIntegerPathsCopiesNothingBehindLastUse()
{
    Procedure proc;
    Graph graph { proc };
    NumberKeptAsWord blocks(graph);
    Value* truncated = blocks.truncate(graph, blocks.join, blocks.number);
    graph.store(blocks.join, truncated, blocks.pointer, 0, firstHeap);
    graph.store(blocks.join, graph.int64(blocks.join, 5), blocks.pointer, 8, firstHeap);
    graph.ret(blocks.join, graph.int64(blocks.join, 9));

    CHECK(blocks.separate(graph));
    CHECK_EQ(numberOfValues(proc, Store), 2u);
    CHECK_EQ(numberOfValues(proc, Return), 1u);
    CHECK_EQ(numberOfValues(proc, Patchpoint), 1u);
    auto code = compileProc(proc);
    int64_t stored[2] = { 0, 0 };
    CHECK_EQ(invoke<int64_t>(*code, 3, 0.0, stored), 9);
    CHECK_EQ(stored[0], 3);
    CHECK_EQ(stored[1], 5);
    CHECK_EQ(invoke<int64_t>(*code, -1001, 6.75, stored), 9);
    CHECK_EQ(stored[0], 6);
}

static void testSeparateIntegerPathsStopsAtCallSite()
{
    Procedure proc;
    Graph graph { proc };
    NumberKeptAsWord blocks(graph);
    graph.callLikePatchpoint(blocks.join)->effects.writes = HeapRange::top();
    graph.ret(blocks.join, blocks.truncate(graph, blocks.join, blocks.number));

    CHECK(!blocks.separate(graph));
    CHECK_EQ(numberOfValues(proc, Patchpoint), 2u);
    auto code = compileProc(proc);
    CHECK_EQ(invoke<int64_t>(*code, 3, 0.0, nullptr), 3);
    CHECK_EQ(invoke<int64_t>(*code, -1001, 6.75, nullptr), 6);
}

static void testSeparateIntegerPathsKeepsJoinOfLikelyPaths()
{
    Procedure proc;
    Graph graph { proc };
    NumberKeptAsWord blocks(graph, 8);
    graph.ret(blocks.join, blocks.truncate(graph, blocks.join, blocks.number));

    CHECK(!blocks.separate(graph));
    CHECK_EQ(numberOfValues(proc, Phi), 1u);
}

static void testSeparateIntegerPathsKeepsNumberThatStaysDouble()
{
    Procedure proc;
    Graph graph { proc };
    NumberKeptAsWord blocks(graph);
    graph.ret(blocks.join, graph.make(blocks.join, BitwiseCast, graph.make(blocks.join, Sqrt, blocks.number)));

    CHECK(!blocks.separate(graph));
    CHECK_EQ(numberOfValues(proc, Phi), 1u);
}

static void testSeparateIntegerPathsLeavesJoinOfOnePath()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    Value* number = graph.phi(join, Double);
    graph.upsilon(root, graph.make(root, IToD, cCallArgumentValues<int32_t>(proc, root)[0]), number);
    graph.jump(root, join);
    PatchpointValue* truncated = join->appendNew<PatchpointValue>(proc, Int64, Origin());
    truncated->effects = Effects::none();
    truncated->pureFunctionOfChildrenKey = keyOfTruncationToInt64;
    truncated->appendSomeRegister(number);
    truncated->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.truncateDoubleToInt64(params[1].fpr(), params[0].gpr());
    });
    graph.ret(join, truncated);

    proc.resetReachability();
    validate(proc);
    CHECK(!separateIntegerPaths(proc));
    validate(proc);
    CHECK_EQ(numberOfBlocks(proc), 2u);
}

static void testSeparateIntegerPathsOfNumberAskedWhetherItIsTruthy()
{
    for (bool usesAbsoluteValue : { false, true }) {
        Procedure proc;
        Graph graph { proc };
        NumberKeptAsWord blocks(graph);
        BasicBlock* isTruthy = proc.addBlock();
        BasicBlock* isFalsy = proc.addBlock();
        if (usesAbsoluteValue)
            graph.branch(blocks.join, graph.make(blocks.join, GreaterThan, graph.make(blocks.join, Abs, blocks.number), graph.number(blocks.join, 0)), isTruthy, isFalsy);
        else
            graph.branch(blocks.join, graph.make(blocks.join, EqualOrUnordered, blocks.number, graph.number(blocks.join, 0)), isFalsy, isTruthy);
        graph.ret(isTruthy, graph.int64(isTruthy, 1));
        graph.ret(isFalsy, graph.int64(isFalsy, 0));

        CHECK(blocks.separate(graph));
        CHECK_EQ(blocks.numberOfDoublesOnLikelyPaths(graph), 0u);
        auto code = compileProc(proc);
        CHECK_EQ(invoke<int64_t>(*code, 0, 1.5, nullptr), 0);
        CHECK_EQ(invoke<int64_t>(*code, 5, 0.0, nullptr), 1);
        CHECK_EQ(invoke<int64_t>(*code, -5, 0.0, nullptr), 1);
        CHECK_EQ(invoke<int64_t>(*code, -1001, 0.0, nullptr), 0);
        CHECK_EQ(invoke<int64_t>(*code, -1001, -0.0, nullptr), 0);
        CHECK_EQ(invoke<int64_t>(*code, -1001, -2.5, nullptr), 1);
        CHECK_EQ(invoke<int64_t>(*code, -1001, std::numeric_limits<double>::quiet_NaN(), nullptr), 0);
    }
}

static void testSeparateIntegerPathsOfTwoNumbersReadOneBehindTheOther()
{
    Procedure proc;
    Graph graph { proc };
    NumberKeptAsWord blocks(graph);
    BasicBlock* secondIntegerPath = proc.addBlock();
    BasicBlock* secondOtherPath = proc.addBlock(0.001);
    BasicBlock* secondJoin = proc.addBlock();
    BasicBlock* isLess = proc.addBlock();
    BasicBlock* isNotLess = proc.addBlock();
    Value* secondWord = graph.load(blocks.join, Int32, blocks.pointer, 0, firstHeap);
    graph.branch(blocks.join, graph.make(blocks.join, LessThan, secondWord, graph.int32(blocks.join, -1000)), rarely(secondOtherPath), FrequentedBlock(secondIntegerPath));
    Value* secondNumber = graph.phi(secondJoin, Double);
    graph.upsilon(secondIntegerPath, graph.make(secondIntegerPath, IToD, secondWord), secondNumber);
    graph.jump(secondIntegerPath, secondJoin);
    graph.upsilon(secondOtherPath, graph.load(secondOtherPath, Double, blocks.pointer, 8, firstHeap), secondNumber);
    graph.jump(secondOtherPath, secondJoin);
    graph.branch(secondJoin, graph.make(secondJoin, LessThan, blocks.number, secondNumber), isLess, isNotLess);
    graph.ret(isLess, graph.int64(isLess, 1));
    graph.ret(isNotLess, graph.int64(isNotLess, 2));

    CHECK(blocks.separate(graph));
    unsigned numberOfComparisonsOfDoublesOnLikelyPaths = 0;
    for (BasicBlock* block : proc) {
        if (block->frequency() < 0.5)
            continue;
        for (Value* value : *block)
            numberOfComparisonsOfDoublesOnLikelyPaths += value->opcode() == LessThan && value->child(0)->type() == Double;
    }
    CHECK_EQ(numberOfComparisonsOfDoublesOnLikelyPaths, 0u);
    auto code = compileProc(proc);
    struct {
        int64_t word;
        double other;
    } second;
    auto run = [&](int32_t word, double other, int32_t secondWord, double secondOther) {
        second.word = secondWord;
        second.other = secondOther;
        return invoke<int64_t>(*code, word, other, &second);
    };
    double notANumber = std::numeric_limits<double>::quiet_NaN();
    CHECK_EQ(run(3, 0, 4, 0), 1);
    CHECK_EQ(run(4, 0, 4, 0), 2);
    CHECK_EQ(run(-1001, 3.5, 4, 0), 1);
    CHECK_EQ(run(-1001, 4.5, 4, 0), 2);
    CHECK_EQ(run(3, 0, -1001, 3.5), 1);
    CHECK_EQ(run(4, 0, -1001, 3.5), 2);
    CHECK_EQ(run(-1001, 3.25, -1001, 3.5), 1);
    CHECK_EQ(run(-1001, notANumber, 4, 0), 2);
    CHECK_EQ(run(3, 0, -1001, notANumber), 2);
}

static void testSeparateIntegerPathsInsideLoop()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* header = proc.addBlock(10);
    BasicBlock* integerPath = proc.addBlock(10);
    BasicBlock* otherPath = proc.addBlock(0.01);
    BasicBlock* join = proc.addBlock(10);
    BasicBlock* done = proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t*, double, int64_t>(proc, root);
    Value* words = arguments[0];
    Value* other = arguments[1];
    Value* count = arguments[2];
    Value* index = graph.phi(header, Int64);
    Value* sum = graph.phi(header, Int64);
    graph.upsilon(root, graph.int64(root, 0), index);
    graph.upsilon(root, graph.int64(root, 0), sum);
    graph.jump(root, header);
    Value* word = graph.load(header, Int32, graph.make(header, Add, words, graph.make(header, Shl, index, graph.int32(header, 2))), 0, firstHeap);
    graph.branch(header, graph.make(header, LessThan, word, graph.int32(header, -1000)), rarely(otherPath), FrequentedBlock(integerPath));
    Value* number = graph.phi(join, Double);
    graph.upsilon(integerPath, graph.make(integerPath, IToD, word), number);
    graph.jump(integerPath, join);
    graph.upsilon(otherPath, other, number);
    graph.jump(otherPath, join);
    PatchpointValue* truncated = join->appendNew<PatchpointValue>(proc, Int64, Origin());
    truncated->effects = Effects::none();
    truncated->pureFunctionOfChildrenKey = keyOfTruncationToInt64;
    truncated->appendSomeRegister(number);
    truncated->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.truncateDoubleToInt64(params[1].fpr(), params[0].gpr());
    });
    Value* nextSum = graph.make(join, Add, sum, truncated);
    Value* nextIndex = graph.make(join, Add, index, graph.int64(join, 1));
    graph.upsilon(join, nextIndex, index);
    graph.upsilon(join, nextSum, sum);
    graph.branch(join, graph.make(join, LessThan, nextIndex, count), header, done);
    graph.ret(done, nextSum);

    proc.resetReachability();
    validate(proc);
    CHECK(separateIntegerPaths(proc));
    validate(proc);
    reduceStrengthOnce(proc, true);
    eliminateDeadCode(proc);
    validate(proc);
    CHECK_EQ(numberOfValues(proc, Patchpoint), 1u);
    auto code = compileProc(proc);
    int32_t given[5] = { 1, -1001, 20, -1001, 300 };
    CHECK_EQ(invoke<int64_t>(*code, given, 4000.75, 5), 8321);
    CHECK_EQ(invoke<int64_t>(*code, given, 4000.75, 1), 1);
    CHECK_EQ(invoke<int64_t>(*code, given, -0.5, 2), 1);
}

static void testComparisonsOfConvertedIntegersWithUnorderedResults()
{
    for (bool usesAbsoluteValue : { false, true }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        Value* number = graph.make(root, IToD, cCallArgumentValues<int32_t>(proc, root)[0]);
        Value* isTruthy = usesAbsoluteValue
            ? graph.make(root, GreaterThan, graph.make(root, Abs, number), graph.number(root, 0))
            : graph.make(root, Equal, graph.make(root, EqualOrUnordered, number, graph.number(root, 0)), graph.int32(root, 0));
        graph.ret(root, graph.make(root, ZExt32, isTruthy));
        reduceStrengthOnce(proc, true);
        eliminateDeadCode(proc);
        CHECK_EQ(numberOfValues(proc, IToD), 0u);
        auto code = compileProc(proc);
        CHECK_EQ(invoke<int64_t>(*code, 0), 0);
        CHECK_EQ(invoke<int64_t>(*code, 1), 1);
        CHECK_EQ(invoke<int64_t>(*code, -1), 1);
        CHECK_EQ(invoke<int64_t>(*code, std::numeric_limits<int32_t>::min()), 1);
    }
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

static void testConversionToDoubleTakesWordThatWasSignExtended()
{
    for (B3::Opcode extension : { SExt32, ZExt32 }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        graph.ret(root, graph.make(root, IToD, graph.make(root, extension, cCallArgumentValues<int32_t>(proc, root)[0])));
        reduceStrengthOnce(proc, true);
        for (Value* value : proc.values()) {
            if (value->opcode() == IToD)
                CHECK(value->child(0)->type() == (extension == SExt32 ? Int32 : Int64));
        }
        auto code = compileProc(proc);
        for (int32_t word : { 0, 1, -1, 1 << 30, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max() })
            CHECK_EQ(invoke<double>(*code, word), extension == SExt32 ? static_cast<double>(word) : static_cast<double>(static_cast<uint32_t>(word)));
    }
}

static void testBitwiseOperationsOnSignExtendedWordsAreNarrowed()
{
    constexpr int32_t words[] = { 0, 1, -1, 255, 256, -256, 1 << 30, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max() };
    auto expected = [](B3::Opcode opcode, int64_t a, int64_t b) {
        return opcode == BitAnd ? a & b : opcode == BitOr ? a | b : a ^ b;
    };
    for (B3::Opcode opcode : { BitAnd, BitOr, BitXor }) {
        {
            Procedure proc;
            Graph graph { proc };
            BasicBlock* root = proc.addBlock();
            auto arguments = cCallArgumentValues<int32_t, int32_t>(proc, root);
            graph.ret(root, graph.make(root, opcode, graph.make(root, SExt32, arguments[0]), graph.make(root, SExt32, arguments[1])));
            reduceStrengthOnce(proc, true);
            for (Value* value : proc.values()) {
                if (value->opcode() == opcode)
                    CHECK(value->type() == Int32);
            }
            auto code = compileProc(proc);
            for (int32_t a : words) {
                for (int32_t b : words)
                    CHECK_EQ(invoke<int64_t>(*code, a, b), expected(opcode, a, b));
            }
        }
        for (int64_t constant : { static_cast<int64_t>(255), static_cast<int64_t>(-256), static_cast<int64_t>(std::numeric_limits<int32_t>::min()), static_cast<int64_t>(0xfffffff0ll), static_cast<int64_t>(1) << 40 }) {
            Procedure proc;
            Graph graph { proc };
            BasicBlock* root = proc.addBlock();
            graph.ret(root, graph.make(root, opcode, graph.make(root, SExt32, cCallArgumentValues<int32_t>(proc, root)[0]), graph.int64(root, constant)));
            reduceStrengthOnce(proc, true);
            bool fitsWord = constant == static_cast<int32_t>(constant);
            for (Value* value : proc.values()) {
                if (value->opcode() == opcode)
                    CHECK(value->type() == (fitsWord ? Int32 : Int64));
            }
            auto code = compileProc(proc);
            for (int32_t a : words)
                CHECK_EQ(invoke<int64_t>(*code, a), expected(opcode, a, constant));
        }
    }
}

static void testEqualityOfExtendedWordsIsNarrowed()
{
    constexpr int32_t words[] = { 0, 1, -1, 255, -256, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max() };
    for (B3::Opcode extension : { SExt32, ZExt32 }) {
        auto extended = [&](int32_t word) {
            return extension == SExt32 ? static_cast<int64_t>(word) : static_cast<int64_t>(static_cast<uint32_t>(word));
        };
        for (B3::Opcode opcode : { Equal, NotEqual }) {
            {
                Procedure proc;
                Graph graph { proc };
                BasicBlock* root = proc.addBlock();
                auto arguments = cCallArgumentValues<int32_t, int32_t>(proc, root);
                graph.ret(root, graph.make(root, opcode, graph.make(root, extension, arguments[0]), graph.make(root, extension, arguments[1])));
                reduceStrengthOnce(proc, true);
                for (Value* value : proc.values()) {
                    if (value->opcode() == opcode)
                        CHECK(value->child(0)->type() == Int32);
                }
                auto code = compileProc(proc);
                for (int32_t a : words) {
                    for (int32_t b : words)
                        CHECK_EQ(invoke<int32_t>(*code, a, b), static_cast<int32_t>((a == b) == (opcode == Equal)));
                }
            }
            for (int64_t constant : { static_cast<int64_t>(0), static_cast<int64_t>(255), static_cast<int64_t>(-1), static_cast<int64_t>(0xffffffffll), static_cast<int64_t>(std::numeric_limits<int32_t>::min()), static_cast<int64_t>(1) << 40 }) {
                Procedure proc;
                Graph graph { proc };
                BasicBlock* root = proc.addBlock();
                graph.ret(root, graph.make(root, opcode, graph.make(root, extension, cCallArgumentValues<int32_t>(proc, root)[0]), graph.int64(root, constant)));
                reduceStrengthOnce(proc, true);
                for (Value* value : proc.values()) {
                    if (value->opcode() == opcode)
                        CHECK(value->child(0)->type() == Int32);
                }
                auto code = compileProc(proc);
                for (int32_t a : words)
                    CHECK_EQ(invoke<int32_t>(*code, a), static_cast<int32_t>((extended(a) == constant) == (opcode == Equal)));
            }
        }
    }
}

static void testShiftsAndSwitchesOfExtendedWordsAreNarrowed()
{
    constexpr int32_t words[] = { 0, 1, -1, 255, -256, 65537, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max() };
    for (B3::Opcode extension : { SExt32, ZExt32 }) {
        auto extended = [&](int32_t word) {
            return extension == SExt32 ? static_cast<int64_t>(word) : static_cast<int64_t>(static_cast<uint32_t>(word));
        };
        for (B3::Opcode opcode : { SShr, ZShr }) {
            for (int32_t amount : { 0, 1, 31, 32, 40 }) {
                Procedure proc;
                Graph graph { proc };
                BasicBlock* root = proc.addBlock();
                graph.ret(root, graph.make(root, opcode, graph.make(root, extension, cCallArgumentValues<int32_t>(proc, root)[0]), graph.int32(root, amount)));
                reduceStrengthOnce(proc, true);
                auto code = compileProc(proc);
                for (int32_t a : words)
                    CHECK_EQ(invoke<int64_t>(*code, a), opcode == SShr ? extended(a) >> amount : static_cast<int64_t>(static_cast<uint64_t>(extended(a)) >> amount));
            }
        }
        for (B3::Opcode opcode : { BitAnd, BitOr, BitXor }) {
            for (int64_t constant : { static_cast<int64_t>(255), static_cast<int64_t>(0xfffffff0ll), static_cast<int64_t>(-256), static_cast<int64_t>(1) << 40 }) {
                Procedure proc;
                Graph graph { proc };
                BasicBlock* root = proc.addBlock();
                graph.ret(root, graph.make(root, opcode, graph.make(root, extension, cCallArgumentValues<int32_t>(proc, root)[0]), graph.int64(root, constant)));
                reduceStrengthOnce(proc, true);
                auto code = compileProc(proc);
                for (int32_t a : words)
                    CHECK_EQ(invoke<int64_t>(*code, a), opcode == BitAnd ? extended(a) & constant : opcode == BitOr ? extended(a) | constant : extended(a) ^ constant);
            }
        }
    }
    for (bool hasWideCase : { false, true }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* otherwise = proc.addBlock();
        Vector<int64_t> cases { 0, 7, -1, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max() };
        if (hasWideCase)
            cases.append(static_cast<int64_t>(1) << 32 | 7);
        SwitchValue* switchValue = root->appendNew<SwitchValue>(proc, Origin(), graph.make(root, SExt32, cCallArgumentValues<int32_t>(proc, root)[0]));
        switchValue->setFallThrough(FrequentedBlock(otherwise));
        for (unsigned i = 0; i < cases.size(); ++i) {
            BasicBlock* target = proc.addBlock();
            switchValue->appendCase(SwitchCase(cases[i], FrequentedBlock(target)));
            graph.ret(target, graph.int32(target, i + 1));
        }
        graph.ret(otherwise, graph.int32(otherwise, 0));
        proc.resetReachability();
        reduceStrengthOnce(proc, true);
        CHECK(switchValue->child(0)->type() == (hasWideCase ? Int64 : Int32));
        auto code = compileProc(proc);
        for (int32_t a : { 0, 7, -1, 8, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max(), 255 }) {
            int32_t expected = 0;
            for (unsigned i = 0; i < cases.size(); ++i) {
                if (cases[i] == a)
                    expected = i + 1;
            }
            CHECK_EQ(invoke<int32_t>(*code, a), expected);
        }
    }
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
        if (onlyDecidedTests)
            CHECK(blocks.right->successorBlock(0) == blocks.tail);
        else {
            CHECK(blocks.right->last()->opcode() == Branch);
            CHECK(blocks.right->successorBlock(0) == blocks.taken);
            CHECK(blocks.right->successorBlock(1) == blocks.notTaken);
        }
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

static void testDuplicateTailsFollowsTestsOfOneValue()
{
    for (bool onlyDecidedTests : { false, true }) {
        Procedure proc;
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
        blocks.duplicate(graph, onlyDecidedTests);

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
}

static void testDuplicateTailsFollowsTestsThroughPhisOnThePath()
{
    Procedure proc;
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
    blocks.duplicate(graph);

    CHECK(blocks.left->last()->opcode() == Jump);
    CHECK(blocks.left->successorBlock(0) == blocks.notTaken);
    auto code = compileProc(proc);
    int64_t memory[1];
    CHECK_EQ(invoke<int64_t>(*code, 1, 7, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 1, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 0, 2, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 4, memory), 10);
}

static void testDuplicateTailsThreadsJumpOnVariableSetOnPath()
{
    for (bool onlyDecidedTests : { false, true }) {
        Procedure proc;
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
        blocks.duplicate(graph, onlyDecidedTests);

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
}

static void testDuplicateTailsReadsVariableAsLastSetAheadOfGet()
{
    for (bool setsAheadOfGet : { false, true }) {
        Procedure proc;
        Graph graph { proc };
        TailWithTwoPredecessors blocks(graph);
        BasicBlock* secondTest = proc.addBlock();
        BasicBlock* stillSet = proc.addBlock();
        Variable* flag = proc.addVariable(Int64);
        graph.branch(blocks.root, blocks.first, blocks.left, blocks.right);
        graph.set(blocks.left, flag, graph.int64(blocks.left, 1));
        graph.set(blocks.right, flag, blocks.second);
        Value* read = setsAheadOfGet ? nullptr : graph.get(blocks.tail, flag);
        graph.set(blocks.tail, flag, graph.int64(blocks.tail, 0));
        if (setsAheadOfGet)
            read = graph.get(blocks.tail, flag);
        graph.branch(blocks.tail, read, secondTest, blocks.notTaken);
        graph.branch(secondTest, graph.get(secondTest, flag), stillSet, blocks.taken);
        graph.ret(stillSet, graph.int64(stillSet, 30));
        blocks.duplicate(graph);

        CHECK(blocks.left->last()->opcode() == Jump);
        CHECK(blocks.left->successorBlock(0) == (setsAheadOfGet ? blocks.notTaken : blocks.taken));
        auto code = compileProc(proc);
        int64_t memory[1];
        CHECK_EQ(invoke<int64_t>(*code, 1, 7, memory), setsAheadOfGet ? 20 : 10);
        CHECK_EQ(invoke<int64_t>(*code, 0, 7, memory), setsAheadOfGet ? 20 : 10);
        CHECK_EQ(invoke<int64_t>(*code, 0, 0, memory), 20);
    }
}

static void testDuplicateTailsKeepsTestOfVariableNotSetOnPath()
{
    Procedure proc;
    Graph graph { proc };
    TailWithTwoPredecessors blocks(graph);
    Variable* flag = proc.addVariable(Int64);
    Variable* other = proc.addVariable(Int64);
    graph.set(blocks.root, flag, blocks.second);
    graph.branch(blocks.root, blocks.first, blocks.left, blocks.right);
    graph.set(blocks.left, other, graph.int64(blocks.left, 1));
    graph.set(blocks.right, other, graph.int64(blocks.right, 0));
    graph.branch(blocks.tail, graph.get(blocks.tail, flag), blocks.taken, blocks.notTaken);
    blocks.duplicate(graph, true);

    CHECK(blocks.left->successorBlock(0) == blocks.tail);
    CHECK(blocks.right->successorBlock(0) == blocks.tail);
    auto code = compileProc(proc);
    int64_t memory[1];
    CHECK_EQ(invoke<int64_t>(*code, 1, 1, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 1, 0, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 1, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 0, 0, memory), 20);
}

static void testDuplicateTailsFollowsTestsBehindJoinThatOnlyJumps()
{
    Procedure proc;
    Graph graph { proc };
    TailWithTwoPredecessors blocks(graph);
    BasicBlock* test = proc.addBlock();
    graph.branch(blocks.root, graph.make(blocks.root, Equal, blocks.first, graph.int64(blocks.root, 5)), blocks.left, blocks.right);
    graph.jump(blocks.tail, test);
    graph.branch(test, graph.make(test, Equal, blocks.first, graph.int64(test, 5)), blocks.taken, blocks.notTaken);
    blocks.duplicate(graph);

    CHECK(blocks.left->successorBlock(0) == blocks.taken);
    CHECK(blocks.right->successorBlock(0) == blocks.notTaken);
    CHECK_EQ(numberOfValues(proc, Branch), 1u);
    auto code = compileProc(proc);
    int64_t memory[1];
    CHECK_EQ(invoke<int64_t>(*code, 5, 0, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 6, 0, memory), 20);
}

static void testDuplicateTailsFollowsPhiHandedOnByBlockThatJumps()
{
    Procedure proc;
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
    blocks.duplicate(graph);

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

static void testDuplicateTailsStopsFollowingAtUndecidedTest()
{
    Procedure proc;
    Graph graph { proc };
    TailWithTwoPredecessors blocks(graph);
    BasicBlock* secondTest = proc.addBlock();
    Value* phi = graph.phi(blocks.tail, Int64);
    graph.branch(blocks.root, blocks.first, blocks.left, blocks.right);
    graph.upsilon(blocks.left, graph.int64(blocks.left, 0), phi);
    graph.upsilon(blocks.right, blocks.second, phi);
    graph.branch(blocks.tail, phi, blocks.taken, secondTest);
    graph.branch(secondTest, graph.make(secondTest, Equal, blocks.second, graph.int64(secondTest, 5)), blocks.taken, blocks.notTaken);
    blocks.duplicate(graph);

    CHECK(blocks.left->last()->opcode() == Jump);
    CHECK(blocks.left->successorBlock(0) == secondTest);
    auto code = compileProc(proc);
    int64_t memory[1];
    CHECK_EQ(invoke<int64_t>(*code, 1, 5, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 1, 6, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 0, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 6, memory), 10);
}

static void testDuplicateTailsFollowsTestsBehindTestCopiedUndecided()
{
    for (bool onlyDecidedTests : { false, true }) {
        Procedure proc;
        Graph graph { proc };
        TailWithTwoPredecessors blocks(graph);
        BasicBlock* repeated = proc.addBlock();
        BasicBlock* asked = proc.addBlock();
        BasicBlock* other = proc.addBlock();
        Value* phi = graph.phi(blocks.tail, Int32);
        Value* flag = graph.make(blocks.root, BitAnd, blocks.first, graph.int64(blocks.root, 1));
        graph.branch(blocks.root, flag, blocks.right, blocks.left);
        graph.upsilon(blocks.left, graph.make(blocks.left, Equal, blocks.second, graph.int64(blocks.left, 5)), phi);
        graph.upsilon(blocks.right, graph.make(blocks.right, Above, blocks.second, graph.int64(blocks.right, 100)), phi);
        graph.branch(blocks.tail, phi, repeated, asked);
        graph.branch(repeated, graph.make(repeated, Equal, blocks.second, graph.int64(repeated, 5)), blocks.taken, other);
        graph.branch(asked, flag, other, blocks.notTaken);
        graph.ret(other, graph.int64(other, 30));
        blocks.duplicate(graph, onlyDecidedTests);

        auto behindJumps = [](BasicBlock* block) {
            while (block->size() == 1 && block->last()->opcode() == Jump)
                block = block->successorBlock(0);
            return block;
        };
        if (onlyDecidedTests) {
            CHECK(blocks.left->successorBlock(0) == blocks.tail);
            CHECK(blocks.right->successorBlock(0) == blocks.tail);
        } else {
            CHECK(blocks.left->last()->opcode() == Branch);
            CHECK(behindJumps(blocks.left->successorBlock(0)) == blocks.taken);
            CHECK(behindJumps(blocks.left->successorBlock(1)) == blocks.notTaken);
            CHECK(blocks.right->last()->opcode() == Branch);
            CHECK(behindJumps(blocks.right->successorBlock(0)) == repeated);
            CHECK(behindJumps(blocks.right->successorBlock(1)) == other);
            CHECK_EQ(numberOfValues(proc, Branch), 4u);
        }
        auto code = compileProc(proc);
        int64_t memory[1];
        CHECK_EQ(invoke<int64_t>(*code, 0, 5, memory), 10);
        CHECK_EQ(invoke<int64_t>(*code, 0, 6, memory), 20);
        CHECK_EQ(invoke<int64_t>(*code, 0, 200, memory), 20);
        CHECK_EQ(invoke<int64_t>(*code, 1, 200, memory), 30);
        CHECK_EQ(invoke<int64_t>(*code, 1, 5, memory), 30);
    }
}

static void testDuplicateTailsFollowsTestsBehindDecidedAndCopiedTests()
{
    Procedure proc;
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
    blocks.duplicate(graph);

    CHECK(blocks.left->last()->opcode() == Branch);
    BasicBlock* behind = blocks.left->successorBlock(0);
    while (behind->size() == 1 && behind->last()->opcode() == Jump)
        behind = behind->successorBlock(0);
    CHECK(behind == blocks.taken);
    auto code = compileProc(proc);
    int64_t memory[1];
    CHECK_EQ(invoke<int64_t>(*code, 1, 5, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 1, 6, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 0, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 5, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 200, memory), 30);
}

static bool duplicateTailsMakesVariables(Graph& graph, TailWithTwoPredecessors& blocks)
{
    graph.jump(blocks.left, blocks.tail);
    graph.jump(blocks.right, blocks.tail);
    graph.proc.resetReachability();
    validate(graph.proc);
    graph.proc.setPositionIndependent();
    duplicateTails(graph.proc, false);
    graph.proc.setPositionIndependent(false);
    validate(graph.proc);
    bool result = !graph.proc.variables().isEmpty();
    fixSSA(graph.proc);
    validate(graph.proc);
    return result;
}

static void testDuplicateTailsKeepsPhiThatOnlyItsBlockUses()
{
    Procedure proc;
    Graph graph { proc };
    TailWithTwoPredecessors blocks(graph);
    Value* phi = graph.phi(blocks.tail, Int32);
    graph.branch(blocks.root, blocks.first, blocks.left, blocks.right);
    graph.upsilon(blocks.left, graph.make(blocks.left, Equal, blocks.second, graph.int64(blocks.left, 5)), phi);
    graph.upsilon(blocks.right, graph.make(blocks.right, Above, blocks.second, graph.int64(blocks.right, 100)), phi);
    graph.branch(blocks.tail, phi, blocks.taken, blocks.notTaken);
    CHECK(!duplicateTailsMakesVariables(graph, blocks));

    CHECK(blocks.left->last()->opcode() == Branch);
    CHECK(blocks.right->last()->opcode() == Branch);
    CHECK_EQ(numberOfValues(proc, Phi), 0u);
    CHECK_EQ(numberOfValues(proc, Upsilon), 0u);
    auto code = compileProc(proc);
    int64_t memory[1];
    CHECK_EQ(invoke<int64_t>(*code, 1, 5, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 1, 200, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 5, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 200, memory), 10);
}

static void testDuplicateTailsDemotesPhiThatAnotherBlockUses()
{
    Procedure proc;
    Graph graph { proc };
    TailWithTwoPredecessors blocks(graph);
    BasicBlock* user = proc.addBlock();
    Value* phi = graph.phi(blocks.tail, Int64);
    graph.branch(blocks.root, blocks.first, blocks.left, blocks.right);
    graph.upsilon(blocks.left, blocks.second, phi);
    graph.upsilon(blocks.right, graph.make(blocks.right, Add, blocks.second, graph.int64(blocks.right, 1)), phi);
    graph.branch(blocks.tail, phi, user, blocks.notTaken);
    graph.ret(user, phi);
    CHECK(duplicateTailsMakesVariables(graph, blocks));

    auto code = compileProc(proc);
    int64_t memory[1];
    CHECK_EQ(invoke<int64_t>(*code, 1, 7, memory), 7);
    CHECK_EQ(invoke<int64_t>(*code, 1, 0, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 7, memory), 8);
    CHECK_EQ(invoke<int64_t>(*code, 0, -1ll, memory), 20);
}

static void testDuplicateTailsDemotesPhiWhoseUpsilonStandsAheadOfBranch()
{
    Procedure proc;
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
    CHECK(duplicateTailsMakesVariables(graph, blocks));

    auto code = compileProc(proc);
    int64_t memory[1];
    CHECK_EQ(invoke<int64_t>(*code, 1, 3, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 1, 0, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 1, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 0, -1ll, memory), 20);
    CHECK_EQ(invoke<int64_t>(*code, 0, 2, memory), 10);
    CHECK_EQ(invoke<int64_t>(*code, 0, 4, memory), 20);
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
            CHECK_EQ(proc.code().tablesInCode()[0].second, testCase.degree + 1);
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

static void testCompilationCohortRunsEachPhaseForAllMembers()
{
    constexpr unsigned size = 4;
    using Step = std::pair<unsigned, unsigned>;
    CompilationCohort cohort;
    for (unsigned round = 0; round < 2; ++round) {
        Vector<Step> steps;
        cohort.run(size, [&](unsigned member) {
            steps.append({ 0, member });
            CompilationCohort::yieldBeforePhase("first"_s);
            steps.append({ 1, member });
            if (member == 2) {
                CompilationCohort::yieldBeforePhase("extra"_s);
                steps.append({ 2, member });
            }
            CompilationCohort::yieldBeforePhase("last"_s);
            steps.append({ 3, member });
        });
        if (CompilationCohort::interleavesMembers)
            CHECK(steps == (Vector<Step> { { 0, 0 }, { 0, 1 }, { 0, 2 }, { 0, 3 }, { 1, 0 }, { 1, 1 }, { 1, 2 }, { 1, 3 }, { 3, 0 }, { 3, 1 }, { 3, 3 }, { 2, 2 }, { 3, 2 } }));
        else
            CHECK(steps == (Vector<Step> { { 0, 0 }, { 1, 0 }, { 3, 0 }, { 0, 1 }, { 1, 1 }, { 3, 1 }, { 0, 2 }, { 1, 2 }, { 2, 2 }, { 3, 2 }, { 0, 3 }, { 1, 3 }, { 3, 3 } }));
    }
    CompilationCohort::yieldBeforePhase("outsideOfCohort"_s);
}

static void testCompilationCohortCompilesProcedures()
{
    constexpr unsigned size = CompilationCohort::maxSize;
    std::array<std::unique_ptr<Compilation>, size> compilations;
    CompilationCohort cohort;
    cohort.run(size, [&](unsigned member) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<int64_t>(proc, root);
        graph.ret(root, graph.make(root, Mul, graph.make(root, Add, arguments[0], graph.int64(root, member)), graph.int64(root, 3)));
        compilations[member] = compileProc(proc);
    });
    for (unsigned member = 0; member < size; ++member)
        CHECK_EQ(invoke<int64_t>(*compilations[member], 5), static_cast<int64_t>((5 + member) * 3));
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

static void buildXorOfTwoSignExtendedLoads(Graph& graph)
{
    BasicBlock* root = graph.proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t*, int64_t>(graph.proc, root);
    Value* pointer = arguments[0];
    Value* index = arguments[1];
    Value* atOffset = graph.make(root, SExt32, graph.load(root, Int32, pointer, 4, firstHeap));
    Value* atIndex = graph.make(root, SExt32, graph.load(root, Int32, graph.make(root, Add, pointer, graph.make(root, Shl, index, graph.int32(root, 2))), 0, firstHeap));
    graph.ret(root, graph.make(root, BitXor, atOffset, atIndex));
}

static void testSignExtendingLoad32()
{
    {
        Procedure proc;
        Graph graph { proc };
        buildXorOfTwoSignExtendedLoads(graph);
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
    buildXorOfTwoSignExtendedLoads(graph);
    auto code = compileProc(proc);
    int32_t memory[3] = { -5, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max() };
    for (int64_t index = 0; index < 3; ++index)
        CHECK_EQ(invoke<int64_t>(*code, memory, index), static_cast<int64_t>(memory[1]) ^ static_cast<int64_t>(memory[index]));
}

static void testDominatorsAfterLoopHeaderBecomesJump()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* outerHeader = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* left = proc.addBlock();
    BasicBlock* right = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* latch = proc.addBlock();
    BasicBlock* outerTest = proc.addBlock();
    BasicBlock* outerLatch = proc.addBlock();
    BasicBlock* done = proc.addBlock();

    auto arguments = cCallArgumentValues<int64_t*, int64_t>(proc, root);
    Value* words = arguments[0];
    auto decrement = [&](BasicBlock* block, int32_t offset) {
        Value* result = graph.make(block, Sub, graph.load(block, Int64, words, offset, firstHeap), graph.int64(block, 1));
        graph.store(block, result, words, offset, firstHeap);
        return graph.make(block, GreaterThan, result, graph.int64(block, 0));
    };
    graph.jump(root, outerHeader);
    graph.store(outerHeader, graph.make(outerHeader, Add, graph.load(outerHeader, Int64, words, 0, firstHeap), graph.int64(outerHeader, 1)), words, 0, firstHeap);
    graph.jump(outerHeader, header);
    graph.branch(header, arguments[1], left, right);
    graph.jump(left, join);
    graph.jump(right, join);
    graph.jump(join, body);
    graph.branch(body, decrement(body, 8), latch, outerTest);
    graph.jump(latch, header);
    graph.branch(outerTest, decrement(outerTest, 16), outerLatch, done);
    graph.jump(outerLatch, outerHeader);
    graph.ret(done, graph.load(done, Int64, words, 0, firstHeap));

    proc.resetReachability();
    proc.setOptLevel(2);
    proc.setPositionIndependent();
    proc.dominators();
    reduceStrength(proc);
    proc.setPositionIndependent(false);
    validate(proc);

    auto code = compileProc(proc);
    int64_t memory[3] = { 0, 3, 2 };
    CHECK_EQ(invoke<int64_t>(*code, memory, 1), 2);
    CHECK_EQ(memory[1], -1);
    CHECK_EQ(memory[2], 0);
}

template<typename Functor>
static void checkReadStaysBelowNullCheck(const Functor& appendRead)
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* loop = proc.addBlock();
    BasicBlock* isNull = proc.addBlock();
    BasicBlock* next = proc.addBlock();
    BasicBlock* done = proc.addBlock();

    auto arguments = cCallArgumentValues<int64_t*, int64_t>(proc, root);
    Value* pointer = arguments[0];
    Value* remaining = graph.phi(loop, Int64);
    Value* sum = graph.phi(loop, Int64);
    Value* nextSum = graph.phi(next, Int64);
    graph.upsilon(root, arguments[1], remaining);
    graph.upsilon(root, graph.int64(root, 0), sum);
    graph.jump(root, loop);

    graph.upsilon(loop, sum, nextSum);
    graph.branch(loop, pointer, next, isNull);

    Value* read = appendRead(graph, isNull, pointer);
    graph.upsilon(isNull, graph.make(isNull, Add, sum, read), nextSum);
    graph.jump(isNull, next);

    Value* nextRemaining = graph.make(next, Sub, remaining, graph.int64(next, 1));
    graph.upsilon(next, nextRemaining, remaining);
    graph.upsilon(next, graph.make(next, Add, nextSum, graph.int64(next, 1)), sum);
    graph.branch(next, nextRemaining, loop, done);

    graph.ret(done, nextSum);

    proc.resetReachability();
    foldPathConstants(proc);
    hoistLoopInvariantValues(proc);
    validate(proc);
    CHECK_EQ(numberOfValues(root, read->opcode()), 0u);
    CHECK_EQ(numberOfValues(isNull, read->opcode()), 1u);

    auto code = compileProc(proc);
    int64_t words[2] = { 5, 6 };
    CHECK_EQ(invoke<int64_t>(*code, words, 3), 2);
}

static void testHoistableLoadStaysBelowNullCheckOfItsBase()
{
    for (bool addsOffset : { false, true }) {
        checkReadStaysBelowNullCheck([&](Graph& graph, BasicBlock* block, Value* pointer) -> Value* {
            Value* address = addsOffset ? graph.make(block, Add, pointer, graph.int64(block, 8)) : pointer;
            MemoryValue* load = graph.load(block, Int64, address, 0, firstHeap);
            load->setControlDependent(false);
            return load;
        });
    }
}

static void testHoistablePatchpointStaysBelowNullCheckOfItsOperand()
{
    checkReadStaysBelowNullCheck([](Graph& graph, BasicBlock* block, Value* pointer) -> Value* {
        PatchpointValue* patchpoint = graph.emptyPatchpoint(block, Int64);
        patchpoint->append(pointer, ValueRep::SomeRegister);
        return patchpoint;
    });
}

static void testLow32BitsOfLoad64WithScaledIndex()
{
    Procedure proc;
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t*, int64_t>(proc, root);
    Value* offset = root->appendNew<Value>(proc, Shl, Origin(), arguments[1], root->appendNew<Const32Value>(proc, Origin(), 3));
    Value* address = root->appendNew<Value>(proc, Add, Origin(), arguments[0], offset);
    Value* word = root->appendNew<MemoryValue>(proc, Load, Int64, Origin(), address);
    Value* low = root->appendNew<Value>(proc, BitAnd, Origin(), word, root->appendNew<Const64Value>(proc, Origin(), 0xffffffff));
    root->appendNew<Value>(proc, Return, Origin(), low);

    auto code = compileProc(proc);
    int64_t words[2] = { 0x1111111122222222, 0x3333333344444444 };
    CHECK_EQ(invoke<int64_t>(*code, words, 0), 0x22222222);
    CHECK_EQ(invoke<int64_t>(*code, words, 1), 0x44444444);
}

static void testSelectOnInt64WithZeroLowHalf()
{
    for (unsigned optLevel : { 1u, 2u }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t, int64_t>(proc, root);
        Value* predicate = graph.make(root, Div, arguments[0], arguments[2]);
        Value* ifTrue = graph.make(root, Div, arguments[1], arguments[2]);
        graph.ret(root, graph.select(root, predicate, ifTrue, arguments[3]));
        auto code = compileProc(proc, optLevel);
        for (int64_t value : { static_cast<int64_t>(0), static_cast<int64_t>(1), static_cast<int64_t>(-1), static_cast<int64_t>(1ll << 32), static_cast<int64_t>(3ll << 32), static_cast<int64_t>(-1ll << 32), static_cast<int64_t>(0xfffe000000000000ull), std::numeric_limits<int64_t>::min() })
            CHECK_EQ(invoke<int64_t>(*code, value, 7, 1, 9), value ? 7 : 9);
    }
}

static void testDeclaredWidthsOf64BitSources()
{
    auto declaredWidths = [](Air::Inst inst) {
        Vector<Width> widths;
        inst.forEachArg([&](Air::Arg&, Air::Arg::Role, Bank, Width width) {
            widths.append(width);
        });
        return widths;
    };
    Air::Tmp a(GPRInfo::regT0);
    Air::Tmp b(GPRInfo::regT1);
    Air::Tmp c(GPRInfo::regT2);
    Air::Arg condition = Air::Arg::resCond(MacroAssembler::NonZero);

    Vector<Width> widths = declaredWidths(Air::Inst(Air::ExtractRegister64, nullptr, a, b, Air::Arg::imm(8), c));
    CHECK_EQ(widths[0], Width64);
    CHECK_EQ(widths[1], Width64);
    CHECK_EQ(widths[3], Width64);

    widths = declaredWidths(Air::Inst(Air::MoveConditionallyTest64, nullptr, condition, a, b, c, c));
    CHECK_EQ(widths[1], Width64);
    CHECK_EQ(widths[2], Width64);

    widths = declaredWidths(Air::Inst(Air::MoveConditionallyTest64, nullptr, condition, a, b, c, c, c));
    CHECK_EQ(widths[1], Width64);
    CHECK_EQ(widths[2], Width64);
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

    unsigned numberOfBranches = 0;
    proc.setLastPhase(createSharedTask<void(Procedure&)>([&](Procedure& optimized) {
        CHECK_EQ(optimized.naturalLoops().numLoops(), 1u);
        numberOfBranches = numberOfValues(optimized, Branch);
    }));
    auto code = compileProc(proc, 2);
    CHECK_EQ(numberOfBranches, 1u);
    CHECK_EQ(run(*code, 10), 45);
}

namespace {

constexpr HeapRange thirdHeap { 12, 13 };
constexpr HeapRange allHeaps { 10, 13 };

B3::Opcode relationWithSwappedOperands(B3::Opcode relation)
{
    switch (relation) {
    case LessThan:
        return GreaterThan;
    case LessEqual:
        return GreaterEqual;
    case GreaterThan:
        return LessThan;
    case GreaterEqual:
        return LessEqual;
    case Below:
        return Above;
    case BelowEqual:
        return AboveEqual;
    case Above:
        return Below;
    case AboveEqual:
        return BelowEqual;
    default:
        return relation;
    }
}

B3::Opcode negatedRelation(B3::Opcode relation)
{
    switch (relation) {
    case Equal:
        return NotEqual;
    case NotEqual:
        return Equal;
    case LessThan:
        return GreaterEqual;
    case LessEqual:
        return GreaterThan;
    case GreaterThan:
        return LessEqual;
    case GreaterEqual:
        return LessThan;
    case Below:
        return AboveEqual;
    case BelowEqual:
        return Above;
    case Above:
        return BelowEqual;
    case AboveEqual:
        return Below;
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return relation;
    }
}

struct RandomProgramState {
    PollState poll;
    int64_t effectHash { 0 };
    std::array<int64_t, 3> failedCheckOperands { };
};

int64_t JIT_OPERATION_ATTRIBUTES randomProgramCallee(RandomProgramState* state, int64_t operand)
{
    state->effectHash = static_cast<int64_t>(static_cast<uint64_t>(state->effectHash) * 3 + 1000 + static_cast<uint64_t>(operand));
    return static_cast<int64_t>(static_cast<uint64_t>(operand) + 5);
}

struct PatchpointModel {
    enum class Kind : uint8_t { Nothing, Constant, AddConstant, LoadWord, IncrementWord, TruncateDouble, Observable, PollLimit, PollHandler };

    Kind kind { Kind::Nothing };
    int64_t immediate { 0 };
};

using PatchpointModels = HashMap<Value*, PatchpointModel>;

bool hasCallSite(Value* value)
{
    if (value->opcode() == CCall)
        return true;
    if (value->opcode() != Patchpoint)
        return false;
    Effects effects = value->effects();
    return effects.exitsSideways || effects.writes || effects.writesPinned || effects.fence;
}

class RandomProgram {
public:
    static constexpr unsigned wordsPerRegion = 16;
    static constexpr unsigned numberOfWords = 3 * wordsPerRegion;
    static constexpr unsigned numberOfSmallWords = 8;
    static constexpr unsigned numberOfGuardWords = 16;
    static constexpr int64_t failedCheckResult = -77;

    RandomProgram(Procedure& proc, unsigned seed, RandomProgramState& state, bool hasCallShapes, bool isForPlainPipeline, bool isForLevel0, PatchpointModels* models = nullptr)
        : m_graph { proc }
        , m_proc(proc)
        , m_random(seed)
        , m_coldOperandRandom(seed + 0x85ebca6bu)
        , m_state(state)
        , m_hasCallShapes(hasCallShapes)
        , m_isForPlainPipeline(isForPlainPipeline)
        , m_isForLevel0(isForLevel0)
        , m_models(models)
    {
        m_block = m_proc.addBlock();
        auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(m_proc, m_block);
        m_arguments = { arguments[0], arguments[2] };
        m_pointer = arguments[1];
        unsigned numberOfVariables = 3 + pick(10);
        for (unsigned i = 0; i < numberOfVariables; ++i) {
            m_variables.append(m_proc.addVariable(Int64));
            set(m_variables[i], i < m_arguments.size() ? m_arguments[i] : constant());
        }
        m_pointerOrNull = m_graph.select(m_block, make(chance(50) ? LessThan : NotEqual, m_arguments[pick(2)], constant()), m_pointer, int64(0));
        m_statementsLeft = 8 + pick(28);
        while (m_statementsLeft)
            statements(3);
        Value* hash = int64(0);
        for (Variable* variable : m_variables)
            hash = make(Add, make(Mul, hash, int64(31)), get(variable));
        m_graph.ret(m_block, hash);
        m_proc.resetReachability();
        validate(m_proc);
    }

private:
    static constexpr double loopFrequencyScale = 8;
    static constexpr double rareFrequencyScale = 1.0 / 256;

    struct Loop {
        BasicBlock* continueTarget { nullptr };
        BasicBlock* breakTarget { nullptr };
        BasicBlock* headerForExtraLatch { nullptr };
        Variable* index { nullptr };
        int64_t delta { 0 };
    };

    struct Place {
        Value* base;
        int32_t offset;
        HeapRange range;
    };

    using Model = PatchpointModel::Kind;

    template<typename T>
    T* modeled(T* patchpoint, Model kind, int64_t immediate = 0)
    {
        if (m_models)
            m_models->add(patchpoint, PatchpointModel { kind, immediate });
        return patchpoint;
    }

    unsigned pick(unsigned limit) { return m_random.getUint32(limit); }
    bool chance(unsigned percent) { return pick(100) < percent; }

    BasicBlock* newBlock(double scale = 1) { return m_proc.addBlock(m_frequency * scale); }
    Value* int32(int32_t value) { return m_graph.int32(m_block, value); }
    Value* int64(int64_t value) { return m_graph.int64(m_block, value); }
    Value* make(B3::Opcode opcode, Value* child) { return m_graph.make(m_block, opcode, child); }
    Value* make(B3::Opcode opcode, Value* left, Value* right) { return m_graph.make(m_block, opcode, left, right); }
    Value* get(Variable* variable) { return m_graph.get(m_block, variable); }
    void set(Variable* variable, Value* value) { m_graph.set(m_block, variable, value); }
    void jump(BasicBlock* target) { m_graph.jump(m_block, target); }
    void branch(Value* predicate, FrequentedBlock taken, FrequentedBlock notTaken) { m_graph.branch(m_block, predicate, taken, notTaken); }
    Variable* randomVariable() { return m_variables[pick(m_variables.size())]; }

    Value* constant()
    {
        static constexpr int64_t constants[] = {
            0, 1, -1, 2, 3, 5, 7, 8, 15, 16, 31, 32, 63, 64, 255, 256, 1000, -2, -8, -128, 0x7fffffff, -0x80000000ll, 0xffffffffll, 0x100000000ll,
            0x123456789abcdefll, std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min()
        };
        return int64(constants[pick(std::size(constants))]);
    }

    Value* randomLoopIndex()
    {
        Vector<Variable*, 4> indices;
        for (auto& loop : m_loops) {
            if (loop.index)
                indices.append(loop.index);
        }
        if (indices.isEmpty())
            return nullptr;
        Variable* index = chance(70) ? indices.last() : indices[pick(indices.size())];
        Value* result = get(index);
        if (index->type() == Int32)
            return make(chance(50) ? SExt32 : ZExt32, result);
        return result;
    }

    Value* leaf()
    {
        switch (pick(10)) {
        case 0:
        case 1:
            return constant();
        case 2:
        case 3:
        case 4:
            return get(randomVariable());
        case 5:
            return m_arguments[pick(m_arguments.size())];
        case 6:
            if (!m_available.isEmpty())
                return m_available[pick(m_available.size())];
            return constant();
        case 7:
        case 8:
            if (Value* index = randomLoopIndex())
                return index;
            return get(randomVariable());
        default:
            return load(0);
        }
    }

    Value* expression(unsigned depth)
    {
        static constexpr B3::Opcode arithmetic[] = { Add, Sub, Mul, BitAnd, BitOr, BitXor };
        if (!depth || chance(20))
            return leaf();
        --depth;
        switch (pick(15)) {
        case 0:
        case 1:
        case 2:
        case 3: {
            B3::Opcode opcode = arithmetic[pick(std::size(arithmetic))];
            Value* left = expression(depth);
            Value* right = expression(depth);
            return make(opcode, left, right);
        }
        case 4: {
            static constexpr B3::Opcode shifts[] = { Shl, SShr, ZShr };
            B3::Opcode opcode = shifts[pick(std::size(shifts))];
            Value* value = expression(depth);
            Value* amount = chance(70) ? int32(pick(64)) : make(Trunc, expression(depth));
            return make(opcode, value, amount);
        }
        case 5:
            return make(ZExt32, condition(depth));
        case 6: {
            Value* predicate = condition(depth);
            Value* ifTrue = expression(depth);
            Value* ifFalse = expression(depth);
            return m_graph.select(m_block, predicate, ifTrue, ifFalse);
        }
        case 7:
        case 8: {
            B3::Opcode opcode = arithmetic[pick(std::size(arithmetic))];
            Value* left = make(Trunc, expression(depth));
            Value* right = make(Trunc, expression(depth));
            Value* result = make(opcode, left, right);
            return make(chance(50) ? SExt32 : ZExt32, result);
        }
        case 9: {
            Value* left = expression(depth);
            Value* right = expression(depth);
            return m_block->appendNew<Value>(m_proc, chill(chance(50) ? Div : Mod), Origin(), left, right);
        }
        case 10: {
            Value* result = doubleExpression(depth + 1);
            switch (pick(4)) {
            case 0:
                return make(BitwiseCast, result);
            case 1:
                return make(SExt32, convertToInteger(result, Int32, keyOfConversionToInt32));
            case 2:
                return convertToInteger(result, Int64, keyOfTruncationToInt64);
            default:
                return convertToInteger(result, Int64, 0);
            }
        }
        case 11:
        case 12:
            return load(depth);
        case 13:
            switch (pick(4)) {
            case 0:
                return make(Opaque, expression(depth));
            case 1:
                return make(Identity, expression(depth));
            default:
                return make(Neg, expression(depth));
            }
        default:
            return make(SExt32, make(chance(50) ? SExt8 : SExt16, make(Trunc, expression(depth))));
        }
    }

    Value* doubleExpression(unsigned depth)
    {
        static constexpr double numbers[] = { 0, -0.0, 0.5, 1, -1, 2, 1.5, 100, 2147483647, 2147483648.0, -2147483648.0, -2147483649.0, 4294967296.0, 1e10 };
        static constexpr B3::Opcode opcodes[] = { Add, Sub, Mul };
        unsigned kind = pick(10);
        if (depth && kind < 3) {
            B3::Opcode opcode = opcodes[pick(std::size(opcodes))];
            Value* left = doubleExpression(depth - 1);
            Value* right = doubleExpression(depth - 1);
            return make(opcode, left, right);
        }
        if (depth && kind < 4)
            return make(chance(50) ? Neg : Abs, doubleExpression(depth - 1));
        if (kind < 6)
            return m_graph.number(m_block, numbers[pick(std::size(numbers))]);
        if (kind < 9)
            return make(IToD, make(Trunc, expression(depth)));
        return make(IToD, expression(depth));
    }

    Value* convertToInteger(Value* number, Type type, int32_t key)
    {
        PatchpointValue* result = m_block->appendNew<PatchpointValue>(m_proc, type, Origin());
        result->effects = Effects::none();
        result->pureFunctionOfChildrenKey = key;
        result->appendSomeRegister(number);
        result->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
            jit.truncateDoubleToInt64(params[1].fpr(), params[0].gpr());
        });
        return modeled(result, Model::TruncateDouble);
    }

    Value* condition(unsigned depth)
    {
        static constexpr B3::Opcode relations[] = { Equal, NotEqual, LessThan, GreaterThan, LessEqual, GreaterEqual, Above, Below, AboveEqual, BelowEqual };
        switch (pick(8)) {
        case 0: {
            B3::Opcode relation = relations[pick(std::size(relations))];
            Value* left = make(Trunc, expression(depth));
            Value* right = chance(50) ? int32(static_cast<int32_t>(pick(20)) - 4) : make(Trunc, expression(depth));
            return make(relation, left, right);
        }
        case 1: {
            B3::Opcode relation = relations[pick(6)];
            Value* left = doubleExpression(depth);
            Value* right = doubleExpression(depth);
            return make(relation, left, right);
        }
        case 2:
            return make(chance(50) ? NotEqual : Equal, make(BitAnd, expression(depth), int64(1ll << pick(10))), int64(0));
        case 3:
            if (depth)
                return make(BitXor, condition(depth - 1), int32(1));
            [[fallthrough]];
        default: {
            B3::Opcode relation = relations[pick(std::size(relations))];
            Value* left = expression(depth);
            Value* right = chance(40) ? constant() : expression(depth);
            return make(relation, left, right);
        }
        }
    }

    Place place(unsigned region, unsigned depth)
    {
        int32_t offset = region * wordsPerRegion * sizeof(int64_t);
        HeapRange range = !region ? firstHeap : region == 1 ? secondHeap : thirdHeap;
        if (chance(15))
            range = region < 2 && chance(50) ? bothHeaps : allHeaps;
        else if (chance(5))
            range = HeapRange::top();
        if (chance(55))
            return { m_pointer, static_cast<int32_t>(offset + pick(chance(60) ? 3 : wordsPerRegion) * sizeof(int64_t)), range };
        Value* index = chance(60) ? randomLoopIndex() : nullptr;
        if (!index)
            index = expression(depth);
        return { wordAddress(index), offset, range };
    }

    Value* wordAddress(Value* index)
    {
        return make(Add, m_pointer, make(Shl, make(BitAnd, index, int64(wordsPerRegion - 1)), int32(3)));
    }

    Value* load(unsigned depth)
    {
        static constexpr B3::Opcode narrowLoads[] = { Load8Z, Load8S, Load16Z, Load16S };
        unsigned region = pick(3);
        Place where = place(region, depth);
        unsigned width = pick(100);
        MemoryValue* result;
        if (width < 70)
            result = m_graph.load(m_block, Int64, where.base, where.offset, where.range);
        else if (width < 90)
            result = m_graph.load(m_block, Int32, where.base, where.offset + (chance(50) ? 4 : 0), where.range);
        else
            result = m_block->appendNew<MemoryValue>(m_proc, narrowLoads[pick(std::size(narrowLoads))], Origin(), where.base, where.offset + static_cast<int32_t>(2 * pick(4)), where.range);
        if (region == 2 && chance(50))
            result->setReadsMutability(Mutability::Immutable);
        if (chance(30))
            result->setControlDependent(false);
        if (result->type() == Int32)
            return make(chance(50) ? SExt32 : ZExt32, result);
        return result;
    }

    Value* loadSmallWord()
    {
        MemoryValue* result = m_graph.load(m_block, Int32, m_pointer, (2 * wordsPerRegion + pick(numberOfSmallWords)) * sizeof(int64_t), thirdHeap);
        if (chance(50))
            result->setReadsMutability(Mutability::Immutable);
        if (chance(30))
            result->setControlDependent(false);
        return make(ZExt32, result);
    }

    void store()
    {
        Place where = place(pick(2), 1);
        Value* value = expression(2);
        unsigned width = pick(100);
        if (width < 70)
            m_graph.store(m_block, value, where.base, where.offset, where.range);
        else if (width < 90)
            m_graph.store(m_block, make(Trunc, value), where.base, where.offset + (chance(50) ? 4 : 0), where.range);
        else
            m_block->appendNew<MemoryValue>(m_proc, chance(50) ? Store8 : Store16, Origin(), make(Trunc, value), where.base, where.offset + static_cast<int32_t>(2 * pick(4)), where.range);
    }

    void opaqueUse(Value* value)
    {
        PatchpointValue* result = modeled(m_graph.emptyPatchpoint(m_block), Model::Nothing);
        result->effects.writesLocalState = true;
        result->append(ConstrainedValue(value, ValueRep::ColdAny));
        result->setGenerator([](CCallHelpers&, const StackmapGenerationParams&) { });
    }

    Value* opaqueZero()
    {
        return modeled(m_graph.emptyPatchpoint(m_block, Int64), Model::Constant, 0);
    }

    using ColdOperands = Vector<Value*, 3>;

    ColdOperands coldOperands()
    {
        ColdOperands result;
        if (!m_hasCallShapes)
            return result;
        auto variable = [&] {
            return get(m_variables[m_coldOperandRandom.getUint32(m_variables.size())]);
        };
        for (unsigned count = m_coldOperandRandom.getUint32(4); count--;) {
            Value* value = variable();
            switch (m_coldOperandRandom.getUint32(6)) {
            case 0:
                value = make(Trunc, value);
                break;
            case 1:
                value = make(Add, value, variable());
                break;
            case 2:
                value = make(IToD, value);
                break;
            case 3: {
                uint64_t bits = m_coldOperandRandom.getUint32();
                value = int64(static_cast<int64_t>(bits << m_coldOperandRandom.getUint32(33)));
                break;
            }
            default:
                break;
            }
            result.append(value);
        }
        return result;
    }

    static Vector<bool, 3> appendColdOperands(StackmapValue* stackmap, const ColdOperands& operands, bool inArgumentRegisters = false)
    {
        Vector<bool, 3> isInt32;
        unsigned nextGPR = 1;
        unsigned nextFPR = 0;
        for (Value* operand : operands) {
            ValueRep rep = ValueRep::ColdAny;
            if (inArgumentRegisters && operand->type() == Double)
                rep = ValueRep::reg(FPRInfo::toArgumentRegister(nextFPR++));
            else if (inArgumentRegisters)
                rep = ValueRep::reg(GPRInfo::toArgumentRegister(nextGPR++));
            stackmap->append(ConstrainedValue(operand, rep));
            isInt32.append(operand->type() == Int32);
        }
        return isInt32;
    }

    Value* observablePatchpoint(Value* operand)
    {
        ColdOperands cold = coldOperands();
        bool isCallShaped = m_hasCallShapes && m_coldOperandRandom.getUint32(100) < 35;
        bool hasTwoSuccessors = m_hasCallShapes && m_coldOperandRandom.getUint32(100) < 15;
        bool isTerminal = hasTwoSuccessors && !m_isForLevel0;
        PatchpointValue* result = m_block->appendNew<PatchpointValue>(m_proc, Int64, Origin());
        result->append(ConstrainedValue(operand, isCallShaped ? ValueRep::reg(GPRInfo::argumentGPR0) : ValueRep::SomeRegister));
        Vector<bool, 3> isInt32 = appendColdOperands(result, cold, isCallShaped);
        RegisterSet lateClobbers;
        if (isCallShaped) {
            if (!isTerminal || !m_isForPlainPipeline)
                result->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
            lateClobbers = RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters());
        }
        result->effects = Effects::none();
        switch (pick(7)) {
        case 0:
            result->effects.writesLocalState = true;
            break;
        case 1:
            result->effects.exitsSideways = true;
            result->effects.reads = HeapRange::top();
            break;
        case 2:
            result->effects.reads = firstHeap;
            result->effects.writes = firstHeap;
            break;
        case 3:
            result->effects.fence = true;
            break;
        case 4:
            result->effects.writesPinned = true;
            break;
        case 5:
            result->effects.writesLocalState = true;
            result->effects.readsLocalState = true;
            break;
        default:
            result->effects = Effects::forCall();
            break;
        }
        for (unsigned i = 0; i < GPRInfo::numberOfArgumentRegisters; ++i) {
            if (chance(50))
                lateClobbers.add(GPRInfo::toArgumentRegister(i));
        }
        result->clobberLate(lateClobbers);
        result->clobber(RegisterSet::macroClobberedGPRs());
        result->effects.terminal = isTerminal;
        result->numGPScratchRegisters = isCallShaped ? 0 : 3;
        lateClobbers.exclude(RegisterSet::macroClobberedGPRs());
        if (!m_hasCallShapes)
            lateClobbers = { };
        int32_t number = ++m_observablePatchpointCount;
        modeled(result, Model::Observable, number);
        result->setGenerator([number, isInt32, lateClobbers, isCallShaped, isTerminal, state = &m_state](CCallHelpers& jit, const StackmapGenerationParams& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            GPRReg address = isCallShaped ? GPRInfo::argumentGPR4 : params.gpScratch(0);
            GPRReg order = isCallShaped ? GPRInfo::argumentGPR5 : params.gpScratch(1);
            GPRReg twice = isCallShaped ? GPRInfo::nonPreservedNonArgumentGPR0 : params.gpScratch(2);
            jit.move(CCallHelpers::TrustedImmPtr(&state->effectHash), address);
            jit.load64(CCallHelpers::Address(address), order);
            jit.add64(order, order, twice);
            jit.add64(twice, order);
            jit.add64(CCallHelpers::TrustedImm32(number), order);
            jit.add64(params[1].gpr(), order);
            for (unsigned i = 0; i < isInt32.size(); ++i) {
                params[2 + i].emitRestore(jit, twice);
                if (isInt32[i])
                    jit.zeroExtend32ToWord(twice, twice);
                jit.add64(order, order);
                jit.add64(twice, order);
            }
            jit.store64(order, CCallHelpers::Address(address));
            jit.move(params[1].gpr(), twice);
            jit.add64(CCallHelpers::TrustedImm32(3), params[1].gpr(), params[0].gpr());
            jit.move(CCallHelpers::TrustedImm64(0x0bad0bad0bad0badll), order);
            lateClobbers.forEach([&](Reg reg) {
                if (reg.isFPR())
                    jit.move64ToDouble(order, reg.fpr());
                else if (reg.gpr() != params[0].gpr() && reg.gpr() != twice && reg.gpr() != order)
                    jit.move(order, reg.gpr());
            });
            if (!isTerminal)
                return;
            CCallHelpers::Jump toRareSuccessor = jit.branchTest64(CCallHelpers::NonZero, twice, CCallHelpers::TrustedImm32(1));
            CCallHelpers::Jump toUsualSuccessor;
            if (!params.fallsThroughToSuccessor(0))
                toUsualSuccessor = jit.jump();
            Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
            params.addLatePath([=](CCallHelpers& jit) {
                toRareSuccessor.linkTo(*labels[1], &jit);
                if (toUsualSuccessor.isSet())
                    toUsualSuccessor.linkTo(*labels[0], &jit);
            });
        });
        if (hasTwoSuccessors) {
            BasicBlock* usual = newBlock();
            BasicBlock* rare = newBlock(rareFrequencyScale);
            BasicBlock* next = newBlock();
            if (isTerminal)
                m_block->setSuccessors(FrequentedBlock(usual), rarely(rare));
            else
                branch(make(BitAnd, operand, int64(1)), rarely(rare), FrequentedBlock(usual));
            Variable* outcome = m_proc.addVariable(Int64);
            m_block = usual;
            set(outcome, result);
            jump(next);
            m_block = rare;
            set(outcome, int64(1 + m_coldOperandRandom.getUint32(1000)));
            jump(next);
            m_block = next;
            return get(outcome);
        }
        return result;
    }

    Value* movablePatchpoint(unsigned depth)
    {
        switch (pick(5)) {
        case 0: {
            unsigned region = pick(3);
            int32_t offset = (region * wordsPerRegion + pick(3)) * sizeof(int64_t);
            PatchpointValue* result = m_block->appendNew<PatchpointValue>(m_proc, Int64, Origin());
            result->append(ConstrainedValue(m_pointer, ValueRep::SomeRegister));
            result->effects = Effects::none();
            result->effects.reads = !region ? firstHeap : region == 1 ? secondHeap : thirdHeap;
            result->setGenerator([offset](CCallHelpers& jit, const StackmapGenerationParams& params) {
                jit.load64(CCallHelpers::Address(params[1].gpr(), offset), params[0].gpr());
            });
            return modeled(result, Model::LoadWord, offset);
        }
        case 1: {
            uint64_t key = 1 + pick(3);
            PatchpointValue* result = m_block->appendNew<PatchpointValue>(m_proc, Int64, Origin());
            result->effects = Effects::none();
            result->pureFunctionKey = key;
            result->setGenerator([key](CCallHelpers& jit, const StackmapGenerationParams& params) {
                jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(key * 11)), params[0].gpr());
            });
            return modeled(result, Model::Constant, key * 11);
        }
        default: {
            int32_t key = chance(50) ? 0 : 100 + pick(2);
            Value* operand = expression(depth);
            PatchpointValue* result = m_block->appendNew<PatchpointValue>(m_proc, Int64, Origin());
            result->append(ConstrainedValue(operand, ValueRep::SomeRegister));
            result->effects = Effects::none();
            result->pureFunctionOfChildrenKey = key;
            if (!key && chance(30))
                result->effects.readsLocalState = true;
            if (!key && chance(30))
                result->effects.readsPinned = true;
            result->setGenerator([key](CCallHelpers& jit, const StackmapGenerationParams& params) {
                jit.add64(CCallHelpers::TrustedImm32(key + 3), params[1].gpr(), params[0].gpr());
            });
            return modeled(result, Model::AddConstant, key + 3);
        }
        }
    }

    void storeBurst()
    {
        unsigned count = 6 + pick(10);
        for (unsigned i = 0; i < count; ++i) {
            unsigned region = pick(2);
            HeapRange range = chance(70) ? (region ? secondHeap : firstHeap) : chance(50) ? bothHeaps : allHeaps;
            m_graph.store(m_block, chance(50) ? constant() : get(randomVariable()), m_pointer, (region * wordsPerRegion + pick(wordsPerRegion)) * sizeof(int64_t), range);
        }
    }

    void patchpoint()
    {
        switch (pick(8)) {
        case 0:
            modeled(m_graph.callLikePatchpoint(m_block), Model::Nothing);
            return;
        case 1: {
            unsigned region = pick(2);
            int32_t offset = (region * wordsPerRegion + pick(chance(60) ? 3 : wordsPerRegion)) * sizeof(int64_t);
            PatchpointValue* result = m_block->appendNew<PatchpointValue>(m_proc, Void, Origin());
            result->append(ConstrainedValue(m_pointer, ValueRep::SomeRegister));
            result->effects = Effects::none();
            result->effects.reads = region ? secondHeap : firstHeap;
            result->effects.writes = result->effects.reads;
            result->clobber(RegisterSet::macroClobberedGPRs());
            if (chance(50))
                result->clobberLate(RegisterSet::argumentGPRs());
            result->setGenerator([offset](CCallHelpers& jit, const StackmapGenerationParams& params) {
                AllowMacroScratchRegisterUsage allowScratch(jit);
                jit.add64(CCallHelpers::TrustedImm32(1), CCallHelpers::Address(params[0].gpr(), offset));
            });
            modeled(result, Model::IncrementWord, offset);
            return;
        }
        case 2:
            switch (pick(3)) {
            case 0:
                m_block->appendNew<FenceValue>(m_proc, Origin());
                return;
            case 1:
                m_block->appendNew<Value>(m_proc, Nop, Origin());
                return;
            default: {
                Value* operand = expression(1);
                Value* result = m_block->appendNew<CCallValue>(m_proc, Int64, Origin(),
                    m_block->appendNew<ConstPtrValue>(m_proc, Origin(), tagCFunction<OperationPtrTag>(randomProgramCallee)),
                    m_block->appendNew<ConstPtrValue>(m_proc, Origin(), &m_state), operand);
                if (chance(50))
                    m_available.append(result);
                else
                    set(randomVariable(), result);
                return;
            }
            }
        case 3:
        case 4:
        case 5: {
            Value* result = observablePatchpoint(expression(1));
            if (chance(50))
                m_available.append(result);
            else if (chance(70))
                set(randomVariable(), result);
            return;
        }
        default: {
            Value* result = movablePatchpoint(1);
            if (chance(50))
                m_available.append(result);
            else
                set(randomVariable(), result);
            return;
        }
        }
    }

    void check()
    {
        Value* left = expression(2);
        ColdOperands cold = coldOperands();
        CheckValue* result = m_block->appendNew<CheckValue>(m_proc, Check, Origin(), make(Equal, left, constant()));
        Vector<bool, 3> isInt32 = appendColdOperands(result, cold);
        result->setGenerator([isInt32, state = &m_state](CCallHelpers& jit, const StackmapGenerationParams& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            GPRReg operands = InvalidGPRReg;
            for (unsigned i = 0; i < GPRInfo::numberOfArgumentRegisters && operands == InvalidGPRReg; ++i) {
                GPRReg candidate = GPRInfo::toArgumentRegister(i);
                bool isFree = candidate != GPRInfo::returnValueGPR;
                for (unsigned j = 0; j < isInt32.size(); ++j)
                    isFree &= !params[j].isGPR() || params[j].gpr() != candidate;
                if (isFree)
                    operands = candidate;
            }
            jit.move(CCallHelpers::TrustedImmPtr(state->failedCheckOperands.data()), operands);
            for (bool storesRegisters : { true, false }) {
                for (unsigned i = 0; i < isInt32.size(); ++i) {
                    if (params[i].isReg() != storesRegisters)
                        continue;
                    CCallHelpers::Address slot(operands, static_cast<int32_t>(i * sizeof(int64_t)));
                    if (params[i].isFPR()) {
                        jit.storeDouble(params[i].fpr(), slot);
                        continue;
                    }
                    GPRReg source = GPRInfo::returnValueGPR;
                    if (params[i].isGPR())
                        source = params[i].gpr();
                    else
                        params[i].emitRestore(jit, source);
                    if (isInt32[i])
                        jit.store32(source, slot);
                    else
                        jit.store64(source, slot);
                }
            }
            jit.move(CCallHelpers::TrustedImm64(failedCheckResult), GPRInfo::returnValueGPR);
            params.code().emitEpilogue(jit);
        });
    }

    PatchpointValue* appendPollHandler(Type type)
    {
        PatchpointValue* handler = modeled(m_block->appendNew<PatchpointValue>(m_proc, type, Origin()), Model::PollHandler);
        handler->effects = Effects::none();
        handler->effects.reads = HeapRange::top();
        handler->effects.writes = HeapRange::top();
        handler->numGPScratchRegisters = 1;
        handler->clobber(RegisterSet::macroClobberedGPRs());
        handler->setGenerator([state = &m_state.poll, hasResult = type != Void](CCallHelpers& jit, const StackmapGenerationParams& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            GPRReg scratch = params.gpScratch(0);
            jit.move(CCallHelpers::TrustedImmPtr(state), scratch);
            jit.add64(CCallHelpers::TrustedImm32(1), CCallHelpers::Address(scratch, OBJECT_OFFSETOF(PollState, numberOfStops)));
            jit.store64(CCallHelpers::TrustedImm32(0), CCallHelpers::Address(scratch, OBJECT_OFFSETOF(PollState, limit)));
            if (hasResult)
                jit.move(CCallHelpers::TrustedImm32(0), params[0].gpr());
        });
        return handler;
    }

    void poll()
    {
        enum class PollVariant { None, LimitUsedTwice, NonConstantComparand, ConditionUsedTwice, SlowPathNotRare, SharedSlowPath, NoHandler, TwoHandlers, HandlerResultUsed, HandlerUsesLoopValue, ConditionInOtherBlock, SwappedOperands, Count };
        PollVariant variant = chance(70) ? PollVariant::None : static_cast<PollVariant>(1 + pick(static_cast<unsigned>(PollVariant::Count) - 1));

        BasicBlock* slowPath = newBlock(rareFrequencyScale);
        BasicBlock* continuation = newBlock();
        if (variant == PollVariant::SharedSlowPath) {
            BasicBlock* next = newBlock();
            branch(condition(1), rarely(slowPath), FrequentedBlock(next));
            m_block = next;
        }
        Value* loopValue = variant == PollVariant::HandlerUsesLoopValue ? make(Add, get(randomVariable()), int64(1)) : nullptr;
        PatchpointValue* limit = modeled(appendPollLimit(m_graph, m_block, m_state.poll), Model::PollLimit);
        if (variant == PollVariant::LimitUsedTwice)
            opaqueUse(limit);
        if (variant == PollVariant::ConditionInOtherBlock) {
            BasicBlock* next = newBlock();
            jump(next);
            m_block = next;
        }
        Value* zero = variant == PollVariant::NonConstantComparand ? opaqueZero() : int64(0);
        Value* predicate = variant == PollVariant::SwappedOperands ? make(GreaterThan, zero, limit) : make(LessThan, limit, zero);
        if (variant == PollVariant::ConditionUsedTwice)
            opaqueUse(predicate);
        branch(predicate, FrequentedBlock(slowPath, variant == PollVariant::SlowPathNotRare ? FrequencyClass::Normal : FrequencyClass::Rare), FrequentedBlock(continuation));

        m_block = slowPath;
        if (variant != PollVariant::NoHandler) {
            PatchpointValue* handler = appendPollHandler(variant == PollVariant::HandlerResultUsed ? Int64 : Void);
            if (loopValue)
                handler->append(ConstrainedValue(loopValue, ValueRep::ColdAny));
            if (variant == PollVariant::HandlerResultUsed)
                opaqueUse(handler);
            if (variant == PollVariant::TwoHandlers)
                appendPollHandler(Void);
        }
        jump(continuation);
        m_block = continuation;
    }

    void returnStatement()
    {
        m_graph.ret(m_block, expression(1));
        m_block = newBlock();
    }

    void exitLoop()
    {
        Loop& loop = m_loops[chance(85) ? m_loops.size() - 1 : pick(m_loops.size())];
        switch (pick(5)) {
        case 0:
        case 1:
            jump(loop.breakTarget);
            break;
        case 2:
        case 3:
            if (loop.headerForExtraLatch && chance(30)) {
                Value* index = get(loop.index);
                set(loop.index, make(Add, index, index->type() == Int32 ? int32(static_cast<int32_t>(loop.delta)) : int64(loop.delta)));
                jump(loop.headerForExtraLatch);
            } else
                jump(loop.continueTarget);
            break;
        default:
            m_graph.ret(m_block, expression(1));
            break;
        }
        m_block = newBlock();
    }

    void branchArm(BasicBlock* block, double scale, unsigned depth, BasicBlock* join)
    {
        SetForScope frequency(m_frequency, m_frequency * scale);
        size_t availableValueCount = m_available.size();
        bool isRare = scale < 0.1;
        m_block = block;
        statements(depth);
        if (!m_loops.isEmpty() && chance(isRare ? 50 : 15))
            exitLoop();
        else if (chance(isRare ? 25 : 4))
            returnStatement();
        jump(join);
        m_available.shrink(availableValueCount);
    }

    void ifStatement(unsigned depth)
    {
        Value* predicate = condition(2);
        unsigned shape = pick(100);
        bool thenIsRare = shape < 25;
        bool hasElse = chance(50);
        bool elseIsRare = hasElse && shape >= 25 && shape < 35;
        double scaleOfThen = thenIsRare ? rareFrequencyScale : 0.5;
        double scaleOfElse = elseIsRare ? rareFrequencyScale : 0.5;
        BasicBlock* thenBlock = newBlock(scaleOfThen);
        BasicBlock* join = newBlock();
        BasicBlock* elseBlock = hasElse ? newBlock(scaleOfElse) : join;
        branch(predicate, FrequentedBlock(thenBlock, thenIsRare ? FrequencyClass::Rare : FrequencyClass::Normal), FrequentedBlock(elseBlock, elseIsRare ? FrequencyClass::Rare : FrequencyClass::Normal));
        branchArm(thenBlock, scaleOfThen, depth, join);
        if (hasElse)
            branchArm(elseBlock, scaleOfElse, depth, join);
        m_block = join;
    }

    void switchStatement(unsigned depth)
    {
        Value* subject = expression(2);
        if (chance(60))
            subject = make(BitAnd, subject, int64(7));
        unsigned numberOfCases = 2 + pick(7);
        int64_t firstCase = pick(4);
        int64_t stride = 1 + pick(2);
        double scale = 1.0 / (numberOfCases + 1);
        BasicBlock* join = newBlock();
        Vector<BasicBlock*, 10> arms;
        Vector<int64_t, 10> caseValues;
        for (unsigned i = 0; i <= numberOfCases; ++i)
            arms.append(newBlock(scale));
        for (unsigned i = 0; i < numberOfCases; ++i)
            caseValues.append(firstCase + i * stride);
        if (chance(10))
            caseValues[pick(numberOfCases)] = chance(50) ? std::numeric_limits<int64_t>::min() : std::numeric_limits<int64_t>::max();
        for (unsigned begin = 0; begin < numberOfCases;) {
            unsigned end = chance(60) ? numberOfCases : begin + 1 + pick(numberOfCases - begin);
            BasicBlock* next = end < numberOfCases ? newBlock() : arms.last();
            if (chance(50)) {
                SwitchValue* switchValue = m_block->appendNew<SwitchValue>(m_proc, Origin(), subject);
                switchValue->setFallThrough(FrequentedBlock(next));
                for (unsigned i = begin; i < end; ++i)
                    switchValue->appendCase(SwitchCase(caseValues[i], FrequentedBlock(chance(10) ? arms[pick(arms.size())] : arms[i])));
            } else {
                for (unsigned i = begin; i < end; ++i) {
                    BasicBlock* nextTest = i + 1 < end ? newBlock() : next;
                    Value* tested = chance(6) ? expression(1) : subject;
                    int64_t caseValue = chance(6) ? caseValues[pick(numberOfCases)] : caseValues[i];
                    if (chance(20))
                        branch(make(NotEqual, tested, int64(caseValue)), FrequentedBlock(nextTest), FrequentedBlock(arms[i]));
                    else
                        branch(make(Equal, tested, int64(caseValue)), FrequentedBlock(arms[i]), FrequentedBlock(nextTest));
                    m_block = nextTest;
                    if (chance(8))
                        set(randomVariable(), expression(1));
                }
            }
            m_block = next;
            begin = end;
        }
        for (BasicBlock* block : arms)
            branchArm(block, scale, std::min(depth, 1u), join);
        m_block = join;
    }

    void longEqualityChain()
    {
        Value* subject = make(BitAnd, expression(2), int64(255));
        Value* otherSubject = expression(1);
        unsigned numberOfTests = 125 + pick(60);
        unsigned stride = chance(50) ? 1 : 3;
        unsigned oddTestIndex = chance(50) ? pick(numberOfTests) : numberOfTests;
        unsigned oddTestKind = pick(3);
        BasicBlock* join = newBlock();
        Vector<BasicBlock*, 5> arms;
        for (unsigned i = 0; i < 5; ++i)
            arms.append(newBlock(0.2));
        for (unsigned i = 0; i < numberOfTests; ++i) {
            BasicBlock* next = i + 1 < numberOfTests ? newBlock() : arms.last();
            Value* tested = subject;
            int64_t caseValue = i * stride % 256;
            if (i == oddTestIndex) {
                if (!oddTestKind)
                    tested = otherSubject;
                else
                    caseValue = oddTestKind == 1 ? std::numeric_limits<int64_t>::min() : std::numeric_limits<int64_t>::max();
            }
            branch(make(Equal, tested, int64(caseValue)), FrequentedBlock(arms[pick(4)]), FrequentedBlock(next));
            m_block = next;
        }
        for (BasicBlock* block : arms)
            branchArm(block, 0.2, 0, join);
        m_block = join;
    }

    void boundsCheckedAccess()
    {
        bool reliesOnBoundsCheck = chance(50);
        Value* index = randomLoopIndex();
        if (!index || (reliesOnBoundsCheck && chance(30)))
            index = reliesOnBoundsCheck ? expression(1) : make(BitAnd, expression(1), int64(31));
        if (chance(25))
            index = make(Add, index, int64(pick(3)));
        Value* length;
        switch (pick(4)) {
        case 0:
        case 1:
            length = loadSmallWord();
            break;
        case 2:
            length = int64(pick(reliesOnBoundsCheck ? wordsPerRegion + 1 : 40));
            break;
        default:
            length = make(BitAnd, get(randomVariable()), int64(31));
            break;
        }
        if (reliesOnBoundsCheck && !length->hasInt64())
            length = make(BitAnd, length, int64(wordsPerRegion - 1));
        BasicBlock* outOfBounds = newBlock(rareFrequencyScale);
        BasicBlock* inBounds = newBlock();
        BasicBlock* join = newBlock();
        if (chance(50))
            branch(make(AboveEqual, index, length), rarely(outOfBounds), FrequentedBlock(inBounds));
        else
            branch(make(Below, index, length), FrequentedBlock(inBounds), rarely(outOfBounds));

        m_block = outOfBounds;
        switch (pick(4)) {
        case 0:
            m_graph.ret(m_block, int64(-1));
            m_block = newBlock();
            break;
        case 1:
            if (!m_loops.isEmpty())
                exitLoop();
            break;
        case 2:
            set(randomVariable(), expression(1));
            break;
        default:
            break;
        }
        jump(join);

        m_block = inBounds;
        unsigned region = pick(3);
        int32_t offset = region * wordsPerRegion * sizeof(int64_t);
        HeapRange range = !region ? firstHeap : region == 1 ? secondHeap : thirdHeap;
        Value* address = reliesOnBoundsCheck ? make(Add, m_pointer, make(Shl, index, int32(3))) : wordAddress(index);
        if (region < 2 && chance(40))
            m_graph.store(m_block, expression(1), address, offset, range);
        else {
            Variable* variable = randomVariable();
            set(variable, make(Add, get(variable), m_graph.load(m_block, Int64, address, offset, range)));
        }
        jump(join);
        m_block = join;
    }

    void nullCheckedAccess()
    {
        Value* pointer = chance(70) ? m_pointerOrNull : m_graph.select(m_block, condition(1), m_pointer, int64(0));
        BasicBlock* isNull = newBlock(chance(50) ? rareFrequencyScale : 0.5);
        BasicBlock* isNotNull = newBlock();
        BasicBlock* join = newBlock();
        FrequentedBlock ifNull(isNull, chance(50) ? FrequencyClass::Rare : FrequencyClass::Normal);
        if (chance(50))
            branch(make(Equal, pointer, int64(0)), ifNull, FrequentedBlock(isNotNull));
        else
            branch(pointer, FrequentedBlock(isNotNull), ifNull);

        m_block = isNull;
        if (chance(30))
            set(randomVariable(), expression(1));
        else if (chance(20) && !m_loops.isEmpty())
            exitLoop();
        jump(join);

        m_block = isNotNull;
        unsigned region = pick(3);
        int32_t offset = (region * wordsPerRegion + pick(chance(60) ? 3 : wordsPerRegion)) * sizeof(int64_t);
        HeapRange range = !region ? firstHeap : region == 1 ? secondHeap : thirdHeap;
        if (region < 2 && chance(30))
            m_graph.store(m_block, expression(1), pointer, offset, range);
        else {
            Variable* variable = randomVariable();
            MemoryValue* loaded = m_graph.load(m_block, Int64, pointer, offset, range);
            if (region == 2 && chance(50))
                loaded->setReadsMutability(Mutability::Immutable);
            set(variable, make(BitXor, get(variable), loaded));
        }
        jump(join);
        m_block = join;
    }

    void countedLoop(unsigned depth)
    {
        enum class Bound { Constant, Invariant, Variable, ImmutableLoad, Load };
        enum class Next { Plain, WrapsInt32, Saturates };

        bool is32 = chance(30);
        bool countsDown = chance(25);
        bool isBottomTested = chance(30);
        bool isGuarded = isBottomTested && chance(60);
        bool isLong = m_loops.isEmpty() && chance(3);
        int64_t mask = isLong ? (1 << 17) - 1 : m_loops.isEmpty() ? 31 : m_loops.size() == 1 ? 7 : 3;
        int64_t step = chance(70) ? 1 : 2 + pick(2);
        Bound boundKind = static_cast<Bound>(pick(5));
        Next nextKind = is32 ? Next::Plain : chance(12) ? Next::WrapsInt32 : chance(8) ? Next::Saturates : Next::Plain;
        bool mayAdvanceTwice = chance(8);
        bool hasSecondEntry = !isBottomTested && chance(4);
        bool mutatesBound = boundKind == Bound::Variable && chance(30);
        bool pollsInHeader = !isBottomTested && chance(10);
        bool pollsInBody = !pollsInHeader && chance(35);
        bool hasVariableStep = !is32 && nextKind == Next::Plain && chance(6);

        bool canUseExactTest = step == 1 && !hasVariableStep && !mayAdvanceTwice && !hasSecondEntry && !mutatesBound && (!isBottomTested || isGuarded);
        B3::Opcode relation;
        if (canUseExactTest && (boundKind == Bound::Constant || boundKind == Bound::Invariant) && chance(12))
            relation = NotEqual;
        else if (countsDown)
            relation = canUseExactTest && chance(20) ? Above : chance(50) ? GreaterThan : GreaterEqual;
        else if (!mutatesBound && chance(30))
            relation = chance(50) ? Below : BelowEqual;
        else
            relation = chance(60) ? LessThan : LessEqual;

        int64_t boundMask = countsDown ? 3 : mask;
        int64_t constantBound = relation == NotEqual && countsDown ? 0 : pick(boundMask + 1);
        if (relation == NotEqual && countsDown)
            boundKind = Bound::Constant;
        auto maskedExpression = [&](int64_t mask) {
            Value* result = make(BitAnd, expression(2), int64(mask));
            if (mask > maxIterationsPerStrip && chance(70))
                return make(BitOr, result, int64(maxIterationsPerStrip));
            return result;
        };
        Value* invariantBound = nullptr;
        Variable* boundVariable = nullptr;
        int32_t boundOffset = 0;
        switch (boundKind) {
        case Bound::Constant:
            break;
        case Bound::Invariant:
            invariantBound = maskedExpression(boundMask);
            break;
        case Bound::Variable:
            boundVariable = m_proc.addVariable(Int64);
            set(boundVariable, maskedExpression(boundMask));
            break;
        case Bound::ImmutableLoad:
            boundOffset = (2 * wordsPerRegion + pick(wordsPerRegion)) * sizeof(int64_t);
            break;
        case Bound::Load:
            boundOffset = pick(2 * wordsPerRegion) * sizeof(int64_t);
            break;
        }

        Variable* index = m_proc.addVariable(is32 ? Int32 : Int64);
        int64_t delta = countsDown ? -step : step;
        auto integer = [&](int64_t value) {
            return is32 ? int32(static_cast<int32_t>(value)) : int64(value);
        };
        auto toIndexType = [&](Value* value) {
            return is32 ? make(Trunc, value) : value;
        };
        auto bound = [&]() -> Value* {
            switch (boundKind) {
            case Bound::Constant:
                return integer(constantBound);
            case Bound::Invariant:
                return toIndexType(invariantBound);
            case Bound::Variable:
                return toIndexType(get(boundVariable));
            case Bound::ImmutableLoad:
                return toIndexType(make(BitAnd, m_graph.load(m_block, Int64, m_pointer, boundOffset, thirdHeap), int64(boundMask)));
            case Bound::Load:
                return toIndexType(make(BitAnd, m_graph.load(m_block, Int64, m_pointer, boundOffset, bothHeaps), int64(boundMask)));
            }
            RELEASE_ASSERT_NOT_REACHED();
            return nullptr;
        };
        auto test = [&](BasicBlock* staying, BasicBlock* leaving) {
            Value* left = get(index);
            Value* right = bound();
            B3::Opcode opcode = relation;
            if (chance(20)) {
                std::swap(left, right);
                opcode = relationWithSwappedOperands(opcode);
            }
            FrequentedBlock stay(staying);
            FrequentedBlock exit(leaving, chance(10) ? FrequencyClass::Rare : FrequencyClass::Normal);
            if (chance(25))
                branch(make(negatedRelation(opcode), left, right), exit, stay);
            else
                branch(make(opcode, left, right), stay, exit);
        };
        auto advance = [&] {
            Value* current = get(index);
            if (hasVariableStep) {
                Value* amount = make(BitOr, make(BitAnd, expression(1), int64(3)), int64(1));
                set(index, make(countsDown ? Sub : Add, current, amount));
                return;
            }
            Value* next = countsDown && chance(50) ? make(Sub, current, integer(step)) : make(Add, current, integer(delta));
            switch (nextKind) {
            case Next::Plain:
                set(index, next);
                return;
            case Next::WrapsInt32:
                set(index, make(SExt32, make(Trunc, next)));
                return;
            case Next::Saturates: {
                int64_t limit = countsDown ? -(1ll << 40) : 1ll << 40;
                BasicBlock* saturated = newBlock(rareFrequencyScale);
                BasicBlock* usual = newBlock();
                BasicBlock* join = newBlock();
                branch(make(countsDown ? LessThan : GreaterThan, next, int64(limit)), rarely(saturated), FrequentedBlock(usual));
                m_block = saturated;
                set(index, int64(limit));
                jump(join);
                m_block = usual;
                set(index, next);
                jump(join);
                m_block = join;
                return;
            }
            }
        };

        if (countsDown)
            set(index, toIndexType(maskedExpression(mask)));
        else
            set(index, relation == NotEqual || chance(60) ? integer(0) : chance(50) ? integer(pick(4)) : toIndexType(make(BitAnd, expression(1), int64(3))));

        BasicBlock* exit = newBlock();
        size_t availableValueCount = m_available.size();
        {
            SetForScope frequency(m_frequency, m_frequency * loopFrequencyScale);
            SetForScope isInLongLoop(m_isInLongLoop, isLong);
            BasicBlock* header = isBottomTested ? nullptr : newBlock();
            BasicBlock* body = newBlock();
            BasicBlock* latch = newBlock();
            if (isGuarded)
                test(body, exit);
            else if (isBottomTested)
                jump(body);
            else if (hasSecondEntry)
                branch(condition(1), FrequentedBlock(body), FrequentedBlock(header));
            else
                jump(header);
            if (header) {
                m_block = header;
                if (pollsInHeader)
                    poll();
                test(body, exit);
            }

            m_block = body;
            if (pollsInBody)
                poll();
            m_loops.append(Loop { latch, exit, nextKind == Next::Plain && !hasVariableStep ? header : nullptr, index, delta });
            statements(isLong ? 0 : depth);
            m_loops.removeLast();
            if (mayAdvanceTwice) {
                BasicBlock* again = newBlock(0.5);
                BasicBlock* join = newBlock();
                branch(condition(1), FrequentedBlock(again), FrequentedBlock(join));
                m_block = again;
                set(index, make(Add, get(index), integer(delta)));
                jump(join);
                m_block = join;
            }
            if (mutatesBound)
                set(boundVariable, make(Add, get(boundVariable), int64(countsDown ? 1 : -1)));
            jump(latch);

            m_available.shrink(availableValueCount);
            m_block = latch;
            advance();
            if (header)
                jump(header);
            else
                test(body, exit);
        }
        m_available.shrink(availableValueCount);
        m_block = exit;
    }

    void fuelBoundedLoop(unsigned depth)
    {
        static constexpr B3::Opcode relations[] = { Equal, NotEqual, LessThan, GreaterThan, LessEqual, GreaterEqual, Above, Below, AboveEqual, BelowEqual };
        static constexpr int64_t steps[] = { 0, 1, -1, 2, -2, 3, maxIterationsPerStrip, -maxIterationsPerStrip, maxIterationsPerStrip + 1, -maxIterationsPerStrip - 1, 100000, 1ll << 40, std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max() };
        enum class Next { AddStep, AddStepToOtherValue, AddNonConstant, DoubleIndex, SubtractFromBound, WrapsInt32, SaturateAtInt64Limit, Count };

        B3::Opcode relation = relations[pick(std::size(relations))];
        int64_t step = steps[pick(std::size(steps))];
        Next nextKind = chance(50) ? Next::AddStep : static_cast<Next>(pick(static_cast<unsigned>(Next::Count)));
        bool isBottomTested = chance(40);
        bool isGuarded = isBottomTested && chance(50);
        bool boundIsInvariant = chance(75);
        bool spendsFuelFirst = chance(50);

        Variable* fuel = m_proc.addVariable(Int64);
        Variable* index = m_proc.addVariable(Int64);
        Variable* boundVariable = m_proc.addVariable(Int64);
        set(fuel, make(Add, make(BitAnd, expression(1), int64(m_loops.isEmpty() ? 31 : 3)), int64(1)));
        set(index, expression(2));
        Value* invariantBound = expression(2);
        set(boundVariable, invariantBound);

        auto bound = [&] {
            return boundIsInvariant ? invariantBound : get(boundVariable);
        };
        auto test = [&](BasicBlock* staying, BasicBlock* leaving) {
            Value* left = get(index);
            Value* right = bound();
            if (chance(25))
                branch(make(negatedRelation(relation), left, right), FrequentedBlock(leaving), FrequentedBlock(staying));
            else
                branch(make(relation, left, right), FrequentedBlock(staying), FrequentedBlock(leaving));
        };
        auto spendFuel = [&](BasicBlock* exit) {
            BasicBlock* next = newBlock();
            set(fuel, make(Sub, get(fuel), int64(1)));
            branch(make(Equal, get(fuel), int64(0)), FrequentedBlock(exit, chance(50) ? FrequencyClass::Rare : FrequencyClass::Normal), FrequentedBlock(next));
            m_block = next;
        };

        BasicBlock* exit = newBlock();
        size_t availableValueCount = m_available.size();
        {
            SetForScope frequency(m_frequency, m_frequency * loopFrequencyScale);
            BasicBlock* header = isBottomTested ? nullptr : newBlock();
            BasicBlock* body = newBlock();
            BasicBlock* latch = newBlock();
            if (isGuarded)
                test(body, exit);
            else if (chance(8))
                branch(condition(1), FrequentedBlock(latch), FrequentedBlock(header ? header : body));
            else
                jump(header ? header : body);
            if (header) {
                m_block = header;
                test(body, exit);
            }
            m_block = body;
            if (spendsFuelFirst)
                spendFuel(exit);
            if (chance(35))
                poll();
            m_loops.append(Loop { latch, exit, nullptr, index, 0 });
            statements(depth);
            m_loops.removeLast();
            if (!boundIsInvariant && chance(50))
                set(boundVariable, expression(1));
            jump(latch);

            m_available.shrink(availableValueCount);
            m_block = latch;
            if (!spendsFuelFirst)
                spendFuel(exit);
            Value* current = get(index);
            switch (nextKind) {
            case Next::AddStep:
                set(index, make(Add, current, int64(step)));
                break;
            case Next::AddStepToOtherValue:
                set(index, make(Add, get(randomVariable()), int64(step)));
                break;
            case Next::AddNonConstant:
                set(index, make(Add, current, expression(1)));
                break;
            case Next::DoubleIndex:
                set(index, make(Add, current, current));
                break;
            case Next::SubtractFromBound:
                set(index, make(Sub, bound(), current));
                break;
            case Next::WrapsInt32:
                set(index, make(SExt32, make(Trunc, make(Add, current, int64(step)))));
                break;
            case Next::SaturateAtInt64Limit: {
                int64_t limit = chance(50) ? std::numeric_limits<int64_t>::max() : chance(50) ? std::numeric_limits<int64_t>::min() : 20;
                Value* next = make(Add, current, int64(step));
                BasicBlock* saturated = newBlock(rareFrequencyScale);
                BasicBlock* usual = newBlock();
                BasicBlock* join = newBlock();
                branch(make(step < 0 ? LessThan : GreaterThan, next, int64(limit)), rarely(saturated), FrequentedBlock(usual));
                m_block = saturated;
                set(index, int64(limit));
                jump(join);
                m_block = usual;
                set(index, next);
                jump(join);
                m_block = join;
                break;
            }
            case Next::Count:
                RELEASE_ASSERT_NOT_REACHED();
            }
            if (header)
                jump(header);
            else
                test(body, exit);
        }
        m_available.shrink(availableValueCount);
        m_block = exit;
    }

    void flagTestedAfterJoin(unsigned depth)
    {
        Variable* flag = randomVariable();
        int64_t first = pick(3);
        int64_t second = pick(3);
        BasicBlock* thenBlock = newBlock(0.5);
        BasicBlock* elseBlock = newBlock(0.5);
        BasicBlock* join = newBlock();
        Value* tested = expression(1);
        branch(make(chance(50) ? LessThan : Equal, tested, constant()), FrequentedBlock(thenBlock), FrequentedBlock(elseBlock));
        m_block = thenBlock;
        set(flag, int64(first));
        jump(join);
        m_block = elseBlock;
        set(flag, chance(70) ? int64(second) : expression(1));
        jump(join);
        m_block = join;
        BasicBlock* taken = newBlock(0.5);
        BasicBlock* after = newBlock();
        Value* seen = get(flag);
        if (chance(30))
            seen = make(chance(50) ? Add : BitXor, seen, int64(1));
        static constexpr B3::Opcode relations[] = { Equal, NotEqual, LessThan, GreaterEqual, Below };
        Value* predicate = chance(30) ? seen : make(relations[pick(std::size(relations))], seen, int64(pick(3)));
        if (chance(20))
            predicate = make(Equal, predicate, predicate->type() == Int32 ? int32(0) : int64(0));
        branch(predicate, FrequentedBlock(taken), FrequentedBlock(after));
        branchArm(taken, 0.5, depth, after);
        m_block = after;
    }

    void branchToSameBlock()
    {
        BasicBlock* next = newBlock();
        branch(condition(1), FrequentedBlock(next), FrequentedBlock(next));
        m_block = next;
    }

    void whileLoop(unsigned depth)
    {
        Variable* fuel = m_proc.addVariable(Int64);
        set(fuel, make(Add, make(BitAnd, expression(1), int64(m_loops.isEmpty() ? 15 : 3)), int64(1)));
        BasicBlock* exit = newBlock();
        size_t availableValueCount = m_available.size();
        {
            SetForScope frequency(m_frequency, m_frequency * loopFrequencyScale);
            BasicBlock* header = newBlock();
            BasicBlock* body = newBlock();
            BasicBlock* end = newBlock();
            if (chance(8))
                branch(condition(1), FrequentedBlock(chance(50) ? body : end), FrequentedBlock(header));
            else
                jump(header);
            m_block = header;
            if (chance(80))
                branch(condition(2), FrequentedBlock(body), FrequentedBlock(exit, chance(15) ? FrequencyClass::Rare : FrequencyClass::Normal));
            else
                jump(body);
            m_block = body;
            if (chance(30))
                poll();
            m_loops.append(Loop { end, exit, nullptr, nullptr, 0 });
            statements(depth);
            m_loops.removeLast();
            jump(end);
            m_block = end;
            set(fuel, make(Sub, get(fuel), int64(1)));
            branch(make(NotEqual, get(fuel), int64(0)), FrequentedBlock(header), FrequentedBlock(exit));
        }
        m_available.shrink(availableValueCount);
        m_block = exit;
    }

    void statements(unsigned depth)
    {
        unsigned count = 1 + pick(5);
        for (unsigned i = 0; i < count && m_statementsLeft; ++i) {
            --m_statementsLeft;
            statement(depth);
        }
    }

    void statement(unsigned depth)
    {
        unsigned kind = pick(100);
        bool mayNestLoop = depth && m_loops.size() < 3 && !m_isInLongLoop;
        if (kind < 1)
            storeBurst();
        else if (kind < 10)
            store();
        else if (kind < 16)
            m_available.append(expression(2));
        else if (kind < 22)
            patchpoint();
        else if (kind < 23)
            check();
        else if (kind < 40 && depth)
            ifStatement(depth - 1);
        else if (kind < 56 && mayNestLoop)
            countedLoop(depth - 1);
        else if (kind < 61 && mayNestLoop)
            fuelBoundedLoop(depth - 1);
        else if (kind < 65 && mayNestLoop)
            whileLoop(depth - 1);
        else if (kind < 71 && depth)
            switchStatement(depth - 1);
        else if (kind < 75 && depth)
            flagTestedAfterJoin(depth - 1);
        else if (kind < 76) {
            if (depth == 3 && chance(25))
                longEqualityChain();
            else
                branchToSameBlock();
        }
        else if (kind < 82)
            boundsCheckedAccess();
        else if (kind < 86)
            nullCheckedAccess();
        else
            set(randomVariable(), expression(3));
    }

    Graph m_graph;
    Procedure& m_proc;
    WeakRandom m_random;
    WeakRandom m_coldOperandRandom;
    RandomProgramState& m_state;
    bool m_hasCallShapes;
    bool m_isForPlainPipeline;
    bool m_isForLevel0;
    PatchpointModels* m_models;
    BasicBlock* m_block { nullptr };
    double m_frequency { 1 };
    unsigned m_statementsLeft { 0 };
    int32_t m_observablePatchpointCount { 0 };
    bool m_isInLongLoop { false };
    std::array<Value*, 2> m_arguments;
    Value* m_pointer { nullptr };
    Value* m_pointerOrNull { nullptr };
    Vector<Variable*, 12> m_variables;
    Vector<Value*> m_available;
    Vector<Loop, 4> m_loops;
};

class RandomProgramEvaluator {
public:
    RandomProgramEvaluator(Procedure& proc, const PatchpointModels& models, RandomProgramState& state)
        : m_proc(proc)
        , m_models(models)
        , m_state(state)
    {
        m_values.fill(0, proc.values().size());
        m_variables.fill(0, proc.variables().size());
    }

    int64_t run(int64_t first, int64_t* pointer, int64_t second)
    {
        m_arguments = { static_cast<uint64_t>(first), std::bit_cast<uint64_t>(pointer), static_cast<uint64_t>(second) };
        BasicBlock* block = m_proc.at(0);
        for (;;) {
            for (Value* value : *block)
                evaluate(value);
            if (m_isDone)
                return m_result;
            block = block->successorBlock(m_successor);
        }
    }

private:
    uint64_t in(Value* value, unsigned index) const { return m_values[value->child(index)->index()]; }
    double number(Value* value, unsigned index) const { return std::bit_cast<double>(in(value, index)); }
    int64_t integer(Value* value, unsigned index) const
    {
        uint64_t bits = in(value, index);
        return value->child(index)->type() == Int32 ? static_cast<int32_t>(bits) : static_cast<int64_t>(bits);
    }

    void set(Value* value, uint64_t bits) { m_values[value->index()] = value->type() == Int32 ? static_cast<uint32_t>(bits) : bits; }
    void set(Value* value, double result) { m_values[value->index()] = std::bit_cast<uint64_t>(result); }

    void finish(int64_t result)
    {
        m_isDone = true;
        m_result = result;
    }

    template<typename T>
    static T& at(uint64_t address) { return *std::bit_cast<T*>(address); }

    static int64_t truncate(double value)
    {
        constexpr double limit = 9223372036854775808.0;
        if (value >= -limit && value < limit)
            return static_cast<int64_t>(value);
        if (isX86() || value < 0)
            return std::numeric_limits<int64_t>::min();
        return value != value ? 0 : std::numeric_limits<int64_t>::max();
    }

    template<typename Functor>
    void arithmetic(Value* value, const Functor& functor)
    {
        if (value->type() == Double)
            set(value, functor(number(value, 0), number(value, 1)));
        else
            set(value, functor(in(value, 0), in(value, 1)));
    }

    template<typename Functor>
    void compare(Value* value, const Functor& functor)
    {
        if (value->child(0)->type() == Double)
            set(value, static_cast<uint64_t>(functor(number(value, 0), number(value, 1))));
        else
            set(value, static_cast<uint64_t>(functor(integer(value, 0), integer(value, 1))));
    }

    template<typename Functor>
    void compareUnsigned(Value* value, const Functor& functor) { set(value, static_cast<uint64_t>(functor(in(value, 0), in(value, 1)))); }

    void evaluatePatchpoint(Value* value)
    {
        PatchpointModel model = m_models.get(value);
        switch (model.kind) {
        case PatchpointModel::Kind::Nothing:
            return;
        case PatchpointModel::Kind::Constant:
            set(value, static_cast<uint64_t>(model.immediate));
            return;
        case PatchpointModel::Kind::AddConstant:
            set(value, in(value, 0) + static_cast<uint64_t>(model.immediate));
            return;
        case PatchpointModel::Kind::LoadWord:
            set(value, at<uint64_t>(in(value, 0) + model.immediate));
            return;
        case PatchpointModel::Kind::IncrementWord:
            ++at<uint64_t>(in(value, 0) + model.immediate);
            return;
        case PatchpointModel::Kind::TruncateDouble:
            set(value, static_cast<uint64_t>(truncate(number(value, 0))));
            return;
        case PatchpointModel::Kind::Observable: {
            uint64_t hash = static_cast<uint64_t>(m_state.effectHash) * 3 + static_cast<uint64_t>(model.immediate) + in(value, 0);
            for (unsigned i = 1; i < value->numChildren(); ++i)
                hash = hash * 2 + in(value, i);
            m_state.effectHash = static_cast<int64_t>(hash);
            set(value, in(value, 0) + 3);
            m_successor = static_cast<unsigned>(in(value, 0) & 1);
            return;
        }
        case PatchpointModel::Kind::PollLimit:
            ++m_state.poll.numberOfPolls;
            set(value, static_cast<uint64_t>(m_state.poll.limit));
            return;
        case PatchpointModel::Kind::PollHandler:
            ++m_state.poll.numberOfStops;
            m_state.poll.limit = 0;
            if (value->type() != Void)
                set(value, static_cast<uint64_t>(0));
            return;
        }
    }

    void evaluate(Value* value)
    {
        if (m_isDone)
            return;
        switch (value->opcode()) {
        case Nop:
        case Fence:
            return;
        case Identity:
        case Opaque:
        case BitwiseCast:
        case Trunc:
        case ZExt32:
            set(value, in(value, 0));
            return;
        case Const32:
            set(value, static_cast<uint64_t>(value->asInt32()));
            return;
        case Const64:
            set(value, static_cast<uint64_t>(value->asInt64()));
            return;
        case ConstDouble:
            set(value, value->asDouble());
            return;
        case ArgumentReg:
            for (unsigned i = 0; i < m_arguments.size(); ++i) {
                if (value->as<ArgumentRegValue>()->argumentReg() == Reg(GPRInfo::toArgumentRegister(i)))
                    set(value, m_arguments[i]);
            }
            return;
        case B3::Get:
            set(value, m_variables[value->as<VariableValue>()->variable()->index()]);
            return;
        case B3::Set:
            m_variables[value->as<VariableValue>()->variable()->index()] = in(value, 0);
            return;
        case Add:
            arithmetic(value, [](auto left, auto right) { return left + right; });
            return;
        case Sub:
            arithmetic(value, [](auto left, auto right) { return left - right; });
            return;
        case Mul:
            arithmetic(value, [](auto left, auto right) { return left * right; });
            return;
        case Div:
            RELEASE_ASSERT(value->kind().isChill() && value->type() == Int64);
            set(value, static_cast<uint64_t>(chillDiv(integer(value, 0), integer(value, 1))));
            return;
        case Mod:
            RELEASE_ASSERT(value->kind().isChill() && value->type() == Int64);
            set(value, static_cast<uint64_t>(chillMod(integer(value, 0), integer(value, 1))));
            return;
        case Neg:
            if (value->type() == Double)
                set(value, -number(value, 0));
            else
                set(value, -in(value, 0));
            return;
        case Abs:
            set(value, std::fabs(number(value, 0)));
            return;
        case BitAnd:
            set(value, in(value, 0) & in(value, 1));
            return;
        case BitOr:
            set(value, in(value, 0) | in(value, 1));
            return;
        case BitXor:
            set(value, in(value, 0) ^ in(value, 1));
            return;
        case Shl:
            set(value, in(value, 0) << (in(value, 1) & (value->type() == Int32 ? 31 : 63)));
            return;
        case SShr:
            set(value, static_cast<uint64_t>(integer(value, 0) >> (in(value, 1) & (value->type() == Int32 ? 31 : 63))));
            return;
        case ZShr:
            set(value, in(value, 0) >> (in(value, 1) & (value->type() == Int32 ? 31 : 63)));
            return;
        case SExt8:
            set(value, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(in(value, 0)))));
            return;
        case SExt16:
            set(value, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(in(value, 0)))));
            return;
        case SExt32:
            set(value, static_cast<uint64_t>(integer(value, 0)));
            return;
        case IToD:
            set(value, static_cast<double>(integer(value, 0)));
            return;
        case Equal:
            compare(value, [](auto left, auto right) { return left == right; });
            return;
        case NotEqual:
            compare(value, [](auto left, auto right) { return left != right; });
            return;
        case LessThan:
            compare(value, [](auto left, auto right) { return left < right; });
            return;
        case GreaterThan:
            compare(value, [](auto left, auto right) { return left > right; });
            return;
        case LessEqual:
            compare(value, [](auto left, auto right) { return left <= right; });
            return;
        case GreaterEqual:
            compare(value, [](auto left, auto right) { return left >= right; });
            return;
        case Above:
            compareUnsigned(value, [](uint64_t left, uint64_t right) { return left > right; });
            return;
        case Below:
            compareUnsigned(value, [](uint64_t left, uint64_t right) { return left < right; });
            return;
        case AboveEqual:
            compareUnsigned(value, [](uint64_t left, uint64_t right) { return left >= right; });
            return;
        case BelowEqual:
            compareUnsigned(value, [](uint64_t left, uint64_t right) { return left <= right; });
            return;
        case Select:
            set(value, in(value, 0) ? in(value, 1) : in(value, 2));
            return;
        case Load: {
            uint64_t address = in(value, 0) + value->as<MemoryValue>()->offset();
            set(value, value->type() == Int32 ? at<uint32_t>(address) : at<uint64_t>(address));
            return;
        }
        case Load8Z:
            set(value, static_cast<uint64_t>(at<uint8_t>(in(value, 0) + value->as<MemoryValue>()->offset())));
            return;
        case Load8S:
            set(value, static_cast<uint64_t>(static_cast<int64_t>(at<int8_t>(in(value, 0) + value->as<MemoryValue>()->offset()))));
            return;
        case Load16Z:
            set(value, static_cast<uint64_t>(at<uint16_t>(in(value, 0) + value->as<MemoryValue>()->offset())));
            return;
        case Load16S:
            set(value, static_cast<uint64_t>(static_cast<int64_t>(at<int16_t>(in(value, 0) + value->as<MemoryValue>()->offset()))));
            return;
        case Store: {
            uint64_t address = in(value, 1) + value->as<MemoryValue>()->offset();
            if (value->child(0)->type() == Int32)
                at<uint32_t>(address) = static_cast<uint32_t>(in(value, 0));
            else
                at<uint64_t>(address) = in(value, 0);
            return;
        }
        case Store8:
            at<uint8_t>(in(value, 1) + value->as<MemoryValue>()->offset()) = static_cast<uint8_t>(in(value, 0));
            return;
        case Store16:
            at<uint16_t>(in(value, 1) + value->as<MemoryValue>()->offset()) = static_cast<uint16_t>(in(value, 0));
            return;
        case CCall:
            set(value, static_cast<uint64_t>(randomProgramCallee(std::bit_cast<RandomProgramState*>(in(value, 1)), static_cast<int64_t>(in(value, 2)))));
            return;
        case Patchpoint:
            evaluatePatchpoint(value);
            return;
        case Check:
            if (!in(value, 0))
                return;
            for (unsigned i = 1; i < value->numChildren(); ++i)
                m_state.failedCheckOperands[i - 1] = static_cast<int64_t>(in(value, i));
            finish(RandomProgram::failedCheckResult);
            return;
        case Jump:
            m_successor = 0;
            return;
        case Branch:
            m_successor = !in(value, 0);
            return;
        case Switch: {
            SwitchValue* switchValue = value->as<SwitchValue>();
            m_successor = switchValue->numCaseValues();
            for (unsigned i = 0; i < switchValue->numCaseValues(); ++i) {
                if (switchValue->caseValue(i) == integer(value, 0))
                    m_successor = i;
            }
            return;
        }
        case Return:
            finish(static_cast<int64_t>(in(value, 0)));
            return;
        default:
            dataLogLn("The evaluator does not know ", deepDump(m_proc, value));
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    Procedure& m_proc;
    const PatchpointModels& m_models;
    RandomProgramState& m_state;
    Vector<uint64_t> m_values;
    Vector<uint64_t> m_variables;
    std::array<uint64_t, 3> m_arguments { };
    unsigned m_successor { 0 };
    bool m_isDone { false };
    int64_t m_result { 0 };
};

class RandomProgramChecker {
public:
    explicit RandomProgramChecker(unsigned seed, bool hasCallShapes = true)
        : m_seed(seed)
        , m_hasCallShapes(hasCallShapes)
        , m_random(seed + 0x9e3779b9u)
    {
    }

    void run()
    {
        Version reference;
        reference.name = "at level 0"_s;
        RandomProgram(reference.proc, m_seed, reference.state, m_hasCallShapes, true, true);
        m_callSiteCount = numberOfValues(reference.proc, hasCallSite);
        m_hasChecks = numberOfValues(reference.proc, Check);
        checkCycleForest(reference.proc);
        reference.code = compileProc(reference.proc, 0);

        Version evaluated;
        evaluated.name = "when evaluated"_s;
        PatchpointModels models;
        RandomProgram(evaluated.proc, m_seed, evaluated.state, m_hasCallShapes, true, false, &models);

        Version versions[5];
        for (auto& version : std::span(versions).first(4)) {
            RandomProgram(version.proc, m_seed, version.state, m_hasCallShapes, !compilesAsDriverDoes, &version == &versions[2]);
            version.proc.setPositionIndependent();
            version.proc.setMaterializesConstantsByFrequency();
            version.proc.setRotatesLoops();
            if (chance(70)) {
                version.proc.code().setChargesForCalleeSaves();
                version.proc.code().setLaysRareBlocksInGaps();
                version.proc.code().setNotesStartOfRareCode();
            }
            if (chance(50)) {
                version.proc.pinRegister(GPRInfo::numberTagRegister);
                version.proc.pinRegister(GPRInfo::notCellMaskRegister);
            }
            version.proc.setLastPhase(createSharedTask<void(Procedure&)>([&](Procedure& optimized) {
                didRunPhase(optimized, version.name);
            }));
        }
        versions[0].name = "at level 2"_s;
        versions[0].code = compileAsDriverDoes(versions[0].proc, 2);
        versions[1].name = "at level 1"_s;
        versions[1].code = compileAsDriverDoes(versions[1].proc, 1);
        versions[2].name = "after a random subset of the phases, then at level 0"_s;
        runRandomPhases(versions[2].proc, versions[2].name);
        versions[2].code = compileAsDriverDoes(versions[2].proc, 0);
        versions[3].name = "after a random subset of the phases, then at level 2"_s;
        runRandomPhases(versions[3].proc, versions[3].name);
        versions[3].code = compileAsDriverDoes(versions[3].proc, 2);
        versions[4].name = "at level 2, not position-independent"_s;
        RandomProgram(versions[4].proc, m_seed, versions[4].state, m_hasCallShapes, true, false);
        versions[4].code = compileProc(versions[4].proc, 2);

        static constexpr int64_t arguments[] = { 0, 1, -1, 2, 5, 13, 31, 100, 65537, 0x7fffffff, -0x80000000ll, 0x123456789ll, std::numeric_limits<int64_t>::min() };
        for (unsigned round = 0; round < 6; ++round) {
            Vector<int64_t> memory(RandomProgram::numberOfWords);
            for (unsigned i = 0; i < memory.size(); ++i) {
                bool isSmall = i >= 2 * RandomProgram::wordsPerRegion && i < 2 * RandomProgram::wordsPerRegion + RandomProgram::numberOfSmallWords;
                if (isSmall || m_random.getUint32(3))
                    memory[i] = m_random.getUint32(40);
                else
                    memory[i] = static_cast<int64_t>(static_cast<uint64_t>(m_random.getUint32()) << 32 | m_random.getUint32());
            }
            int64_t first = arguments[m_random.getUint32(std::size(arguments))];
            int64_t second = arguments[m_random.getUint32(std::size(arguments))];
            int64_t limit = round & 1 ? -1 : 0;

            reference.call(first, second, memory, limit);
            evaluated.evaluate(models, first, second, memory, limit);
            if (!evaluated.agreesWith(reference))
                fail(differenceBetween(reference, evaluated, first, second, round));
            for (auto& version : versions) {
                version.call(first, second, memory, limit);
                if (version.agreesWith(reference))
                    continue;
                fail(differenceBetween(reference, version, first, second, round));
            }
        }
    }

private:
#if ENABLE(AOT)
    static constexpr bool compilesAsDriverDoes = true;
#else
    static constexpr bool compilesAsDriverDoes = false;
#endif

    std::unique_ptr<Compilation> compileAsDriverDoes(Procedure& proc, unsigned level)
    {
#if ENABLE(AOT)
        proc.setOptLevel(level);
        generateToAir(proc);
        AOT::Typed::placeResultsOfInvokes(proc.code());
        Air::prepareForGeneration(proc.code());
        if (level) {
            AOT::Typed::resequenceMovesBeforePatches(proc.code());
            AOT::Typed::mergeIdenticalBlocks(proc.code());
            if (chance(50))
                AOT::Typed::returnInPlaceOfJumpingToAReturn(proc.code());
        }
        CCallHelpers jit;
        generate(proc, jit);
        LinkBuffer linkBuffer(jit, nullptr);
        return makeUnique<Compilation>(FINALIZE_CODE(linkBuffer, JITCompilationPtrTag, nullptr, "Compilation"), proc.releaseByproducts());
#else
        return compileProc(proc, level);
#endif
    }

    struct Version {
        void prepare(const Vector<int64_t>& initialMemory, int64_t limit)
        {
            memory.fill(guardWord, initialMemory.size() + 2 * RandomProgram::numberOfGuardWords);
            for (unsigned i = 0; i < initialMemory.size(); ++i)
                memory[RandomProgram::numberOfGuardWords + i] = initialMemory[i];
            state = { };
            state.poll.limit = limit;
        }

        void call(int64_t first, int64_t second, const Vector<int64_t>& initialMemory, int64_t limit)
        {
            prepare(initialMemory, limit);
            result = invoke<int64_t>(*code, first, memory.mutableSpan().subspan(RandomProgram::numberOfGuardWords).data(), second);
        }

        void evaluate(const PatchpointModels& models, int64_t first, int64_t second, const Vector<int64_t>& initialMemory, int64_t limit)
        {
            prepare(initialMemory, limit);
            result = RandomProgramEvaluator(proc, models, state).run(first, memory.mutableSpan().subspan(RandomProgram::numberOfGuardWords).data(), second);
        }

        bool hasIntactGuards() const
        {
            for (unsigned i = 0; i < RandomProgram::numberOfGuardWords; ++i) {
                if (memory[i] != guardWord || memory[memory.size() - 1 - i] != guardWord)
                    return false;
            }
            return true;
        }

        bool agreesWith(const Version& reference) const
        {
            return result == reference.result && memory == reference.memory && hasIntactGuards()
                && state.effectHash == reference.state.effectHash && state.failedCheckOperands == reference.state.failedCheckOperands;
        }

        static constexpr int64_t guardWord = 0x5a5a5a5a5a5a5a5all;

        ASCIILiteral name;
        Procedure proc;
        RandomProgramState state;
        std::unique_ptr<Compilation> code;
        Vector<int64_t> memory;
        int64_t result { 0 };
    };

    static String differenceBetween(const Version& reference, const Version& version, int64_t first, int64_t second, unsigned round)
    {
        return makeString("called with "_s, first, " and "_s, second, " in round "_s, round, " returned "_s, reference.result, ' ', reference.name, " but "_s, version.result, ' ', version.name, "; effect hashes: "_s, reference.state.effectHash, " and "_s, version.state.effectHash, "; memory: "_s, toString(listDump(reference.memory)), " and "_s, toString(listDump(version.memory)));
    }

    NO_RETURN_DUE_TO_CRASH void fail(const String& what)
    {
        crashLock.lock();
        RandomProgramState state;
        Procedure proc;
        RandomProgram(proc, m_seed, state, m_hasCallShapes, true, true);
        dataLogLn("The random program with seed ", m_seed, " ", what, ":\n", proc);
        CRASH();
    }

    void didRunPhase(Procedure& proc, ASCIILiteral name)
    {
        validate(proc);
        if (!m_hasChecks && numberOfValues(proc, hasCallSite) > m_callSiteCount)
            fail(makeString("has more call sites than it started with "_s, name));
    }

    void checkCycleForest(Procedure& proc)
    {
        CycleForest forest(proc);
        auto require = [&](bool condition, ASCIILiteral what) {
            if (!condition)
                fail(makeString("has an invalid cycle forest: "_s, what));
        };
        auto reaches = [&](BasicBlock* from, BasicBlock* to, const auto& mayPass) {
            IndexSet<BasicBlock*> seen;
            Vector<BasicBlock*> worklist { from };
            while (!worklist.isEmpty()) {
                for (BasicBlock* successor : worklist.takeLast()->successorBlocks()) {
                    if (successor == to)
                        return true;
                    if (mayPass(successor) && seen.add(successor))
                        worklist.append(successor);
                }
            }
            return false;
        };

        bool isReducible = true;
        for (unsigned cycle = 0; cycle < forest.size(); ++cycle) {
            auto& entries = forest[cycle].entries;
            BasicBlock* header = forest[cycle].header();
            unsigned parent = forest[cycle].parent;
            auto isMember = [&](BasicBlock* block) {
                return forest.contains(cycle, block);
            };
            isReducible &= forest[cycle].isReducible();
            require(isMember(header), "a header is outside its cycle"_s);
            require(forest[cycle].depth == forest.depth(parent) + 1, "a cycle is not one deeper than its parent"_s);
            require(reaches(header, header, isMember), "a header does not reach itself"_s);
            for (BasicBlock* block : proc) {
                if (!isMember(block)) {
                    require(!entries.contains(block), "an entry is outside its cycle"_s);
                    continue;
                }
                require(parent == CycleForest::none || (forest.contains(parent, block) && block != forest[parent].header()), "a cycle is not inside its parent"_s);
                require(block == header || (reaches(header, block, isMember) && reaches(block, header, isMember)), "a cycle is not strongly connected"_s);
                bool hasOutsidePredecessor = false;
                for (BasicBlock* predecessor : block->predecessors())
                    hasOutsidePredecessor |= !isMember(predecessor);
                require(entries.contains(block) == (hasOutsidePredecessor || block == header), "the entries of a cycle are not the blocks with outside predecessors"_s);
                if (forest.innermost(block) == cycle && block != header) {
                    require(!reaches(block, block, [&](BasicBlock* other) {
                        return isMember(other) && other != header;
                    }), "an inner cycle is missing"_s);
                }
            }
        }
        for (BasicBlock* block : proc) {
            if (forest.innermost(block) == CycleForest::none) {
                require(!reaches(block, block, [](BasicBlock*) {
                    return true;
                }), "a cycle is missing"_s);
            }
        }

        IndexMap<BasicBlock*, unsigned> position(proc.size(), UINT_MAX);
        unsigned positionCount = 0;
        for (BasicBlock* block : forest.blocksInPostOrder()) {
            require(position[block] == UINT_MAX, "a block is in the post order twice"_s);
            position[block] = positionCount++;
        }
        require(positionCount == numberOfBlocks(proc), "a block is missing from the post order"_s);
        for (BasicBlock* block : proc) {
            for (BasicBlock* successor : block->successorBlocks()) {
                unsigned cycle = forest.innermost(successor);
                unsigned common = forest.nearestCommon(forest.innermost(block), cycle);
                BasicBlock* target = successor;
                if (cycle == common) {
                    if (forest.isHeader(successor))
                        continue;
                } else {
                    while (forest[cycle].parent != common)
                        cycle = forest[cycle].parent;
                    target = forest[cycle].header();
                }
                require(position[target] < position[block], "a block comes before one of its successors in the post order"_s);
            }
        }

        if (!isReducible)
            return;
        B3::NaturalLoops& loops = proc.naturalLoops();
        require(loops.numLoops() == forest.size(), "the number of cycles differs from the number of natural loops"_s);
        for (BasicBlock* block : proc) {
            const B3::NaturalLoop* loop = loops.innerMostLoopOf(block);
            unsigned cycle = forest.innermost(block);
            require(!!loop == (cycle != CycleForest::none), "a block is in a cycle but not in a natural loop, or vice versa"_s);
            require(!loop || loop->header() == forest[cycle].header(), "a cycle and its natural loop have different headers"_s);
        }
    }

    bool chance(unsigned percent) { return m_random.getUint32(100) < percent; }

    template<typename Functor>
    bool maybe(unsigned percent, Procedure& proc, ASCIILiteral name, const Functor& phase)
    {
        if (!chance(percent))
            return false;
        bool result = true;
        if constexpr (std::is_void_v<decltype(phase())>)
            phase();
        else
            result = phase();
        didRunPhase(proc, name);
        return result;
    }

    void runRandomPhases(Procedure& proc, ASCIILiteral name)
    {
        auto always = [&](const auto& phase) {
            return maybe(100, proc, name, phase);
        };
        auto sometimes = [&](const auto& phase) {
            return maybe(50, proc, name, phase);
        };
        auto often = [&](const auto& phase) {
            return maybe(75, proc, name, phase);
        };
        auto runFixSSA = [&] { return fixSSA(proc); };
        auto runHoisting = [&] { return hoistLoopInvariantValues(proc); };
        auto runFoldPathConstants = [&] { return foldPathConstants(proc); };
        auto runSimplifyTerminals = [&] { return simplifyTerminals(proc); };
        auto runEliminateDeadCode = [&] { return eliminateDeadCode(proc); };
        auto runEliminatePhiCycles = [&] { return eliminateRedundantPhiCycles(proc); };
        auto runFoldComparisonsWithBounds = [&] { return foldComparisonsWithBounds(proc); };

        proc.setOptLevel(2);
        proc.resetReachability();
        sometimes([&] { return reduceStrength(proc, ReduceStrengthPass::Initial); });
        sometimes([&] { return inferSwitches(proc); });
        sometimes([&] { duplicateTails(proc); });
        always(runFixSSA);
        sometimes(runFoldPathConstants);
        sometimes(runSimplifyTerminals);
        sometimes(runEliminatePhiCycles);
        sometimes(runHoisting);
        if (often([&] { return rotateLoops(proc); })) {
            always(runFixSSA);
            sometimes(runHoisting);
            sometimes(runFoldPathConstants);
            sometimes(runSimplifyTerminals);
            sometimes(runEliminateDeadCode);
        }
        if (often(runFoldComparisonsWithBounds)) {
            sometimes(runSimplifyTerminals);
            sometimes(runEliminateDeadCode);
        }
        Vector<BasicBlock*> headersOfVersionsTestedAtTheTop;
        if (often([&] { return versionLoops(proc, headersOfVersionsTestedAtTheTop); })) {
            always(runFixSSA);
            sometimes(runEliminatePhiCycles);
            sometimes([&] { return narrowPhisOfExtendedWords(proc); });
            sometimes([&] { return foldComparisonsWithWhatIsOutOfRange(proc); });
            sometimes(runEliminateDeadCode);
            if (!headersOfVersionsTestedAtTheTop.isEmpty() && often([&] { return rotateLoops(proc, &headersOfVersionsTestedAtTheTop); })) {
                always(runFixSSA);
                sometimes(runFoldPathConstants);
                sometimes(runSimplifyTerminals);
                sometimes(runEliminateDeadCode);
                if (sometimes(runFoldComparisonsWithBounds)) {
                    sometimes(runSimplifyTerminals);
                    sometimes(runEliminateDeadCode);
                }
            }
        }
        if (often([&] { return stripMineLoops(proc); }))
            sometimes(runHoisting);
        sometimes([&] { return sinkValues(proc); });
    }

    unsigned m_seed;
    bool m_hasCallShapes;
    WeakRandom m_random;
    unsigned m_callSiteCount { 0 };
    bool m_hasChecks { false };
};

} // anonymous namespace

static void testRandomPrograms(unsigned part)
{
    constexpr unsigned numberOfParts = 16;
    constexpr unsigned numberOfProgramsInPart = 60;
    unsigned firstSeed = (Options::defaultB3OptLevel() * numberOfParts + part) * numberOfProgramsInPart;
    for (unsigned i = 0; i < numberOfProgramsInPart; ++i)
        RandomProgramChecker(firstSeed + i).run();
}

static void testRandomProgramsThatOnceFailed()
{
    for (unsigned seed : { 198695u, 209039u, 343103u, 371708u, 471095u, 537269u, 835893u, 872292u, 1108466u, 1306836u, 1516518u, 1694723u, 1931155u, 2322767u, 2531676u, 3410061u, 3489678u }) {
        RandomProgramChecker(seed, false).run();
        RandomProgramChecker(seed, true).run();
    }
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
    RUN(testFoldComparisonsWithWhatLoopThatCountsDownStartsBelow());
    RUN(testFoldComparisonsOfNeighbourOfLoopVariableWithLimit());
    RUN(testFoldComparisonsOfNeighbourWithLimitLoadedAtLoopTest());
    RUN(testFoldComparisonsKeepsNeighbourThatMayBeOutside());
    RUN(testFoldComparisonsKeepsWhatLoopThatCountsDownMayNotBeBelow());
    RUN(testAnalyzeCountedLoop());
    RUN(testEliminateRedundantPhiCycles());
    RUN(testNarrowPhisOfExtendedWords());
    RUN(testFoldComparisonsWithWhatIsOutOfRange());
    RUN(testVersionLoopsHoistsLoadAndGuard());
    RUN(testVersionLoopsHoistsLoadPastStoreToDisjointHeap());
    RUN(testVersionLoopsKeepsLoadClobberedByLoop());
    RUN(testVersionLoopsSkipsLoopWithCall());
    RUN(testVersionLoopsSkipsLargeLoop());
    RUN(testVersionLoopsKeepsDeepArmOfChainInFastVersion());
    RUN(testVersionLoopsSkipsLoopWithNothingToHoist());
    RUN(testVersionLoopsOnlyInnermostLoops());
    RUN(testVersionLoopsRareEdgeContinuesInOriginalLoop());
    RUN(testVersionLoopsUnswitchesInvariantBranch());
    RUN(testVersionLoopsKeepsVariantBranch());
    RUN(testVersionLoopsStripMinesPollInFastVersion());
    RUN(testStripMineLoopsPollsOncePerStrip());
    RUN(testEndOfFirstStripFromZeroToZeroExtendedBound());
    RUN(testStripMineLoopsAllRelationsAndSteps());
    RUN(testStripMineLoopsRequiresPollGenerator());
    RUN(testStripMineLoopsSkipsLoopWithTwoPolls());
    RUN(testStripMineLoopsSkipsVariantBound());
    RUN(testStripMineLoopsSkipsPollWithSharedLimit());
    RUN(testSinkValuesMovesValueIntoRareUser());
    RUN(testSinkValuesClonesValueForTwoRareUsers());
    RUN(testSinkValuesClonesPatchpointForTwoRareUsersOnlyIfItIsPureFunctionOfKey());
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
    RUN(testRangesOfShiftsByAnyAmount());
    RUN(testTruncOfWordShiftedLeftAndBackByAnyAmount());
    RUN(testDoubleToIntegerConversionsOfConvertedIntegers());
    RUN(testSeparateIntegerPathsFoldsConversionBackToInteger());
    RUN(testSeparateIntegerPathsFollowsSumsIntoLaterBlocks());
    RUN(testSeparateIntegerPathsCopiesNothingBehindLastUse());
    RUN(testSeparateIntegerPathsStopsAtCallSite());
    RUN(testSeparateIntegerPathsKeepsJoinOfLikelyPaths());
    RUN(testSeparateIntegerPathsKeepsNumberThatStaysDouble());
    RUN(testSeparateIntegerPathsLeavesJoinOfOnePath());
    RUN(testSeparateIntegerPathsOfNumberAskedWhetherItIsTruthy());
    RUN(testSeparateIntegerPathsOfTwoNumbersReadOneBehindTheOther());
    RUN(testSeparateIntegerPathsInsideLoop());
    RUN(testComparisonsOfConvertedIntegersWithUnorderedResults());
    RUN(testPurePatchpointsAreValueNumbered());
    RUN(testConversionToDoubleTakesWordThatWasSignExtended());
    RUN(testBitwiseOperationsOnSignExtendedWordsAreNarrowed());
    RUN(testEqualityOfExtendedWordsIsNarrowed());
    RUN(testShiftsAndSwitchesOfExtendedWordsAreNarrowed());
    RUN(testReduceStrengthMergesBlockChain());
    RUN(testReduceStrengthEliminatesSingleValuePhi());
    RUN(testMemoryNumberingWhenEdgeIntoJoinIsRemoved());
    RUN(testMoveConstantsMaterializesByFrequency());
    RUN(testMoveConstantsByFrequencyHoistsOutOfLoop());
    RUN(testMoveConstantsPositionIndependentDoubles());
    RUN(testDuplicateTailsThreadsJumpOnConstantPhi());
    RUN(testDuplicateTailsThreadsJumpDecidedByDominatingBranch());
    RUN(testDuplicateTailsFollowsTestsOfOneValue());
    RUN(testDuplicateTailsFollowsTestsThroughPhisOnThePath());
    RUN(testDuplicateTailsThreadsJumpOnVariableSetOnPath());
    RUN(testDuplicateTailsReadsVariableAsLastSetAheadOfGet());
    RUN(testDuplicateTailsKeepsTestOfVariableNotSetOnPath());
    RUN(testDuplicateTailsFollowsTestsBehindJoinThatOnlyJumps());
    RUN(testDuplicateTailsFollowsPhiHandedOnByBlockThatJumps());
    RUN(testDuplicateTailsStopsFollowingAtUndecidedTest());
    RUN(testDuplicateTailsFollowsTestsBehindTestCopiedUndecided());
    RUN(testDuplicateTailsFollowsTestsBehindDecidedAndCopiedTests());
    RUN(testDuplicateTailsKeepsPhiThatOnlyItsBlockUses());
    RUN(testDuplicateTailsDemotesPhiThatAnotherBlockUses());
    RUN(testDuplicateTailsDemotesPhiWhoseUpsilonStandsAheadOfBranch());
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
    RUN(testCompilationCohortRunsEachPhaseForAllMembers());
    RUN(testCompilationCohortCompilesProcedures());
    RUN(testUseCountsDuplicateChild());
    RUN(testRedundantZeroExtension());
    RUN(testZeroExtensionClearsUpperHalfInPositionIndependentCode());
    RUN(testSignExtendingLoad32());
    RUN(testDominatorsAfterLoopHeaderBecomesJump());
    RUN(testHoistableLoadStaysBelowNullCheckOfItsBase());
    RUN(testHoistablePatchpointStaysBelowNullCheckOfItsOperand());
    RUN(testLow32BitsOfLoad64WithScaledIndex());
    RUN(testSelectOnInt64WithZeroLowHalf());
    RUN(testDeclaredWidthsOf64BitSources());
    RUN(testMul64ByImmediate());
    RUN(testLoopPhasesArraySum());
    RUN(testLoopPhasesFoldTestsImpliedByLoopCondition());
    RUN(testLoopPhasesAreOptIn());
    RUN(testRandomPrograms(0));
    RUN(testRandomPrograms(1));
    RUN(testRandomPrograms(2));
    RUN(testRandomPrograms(3));
    RUN(testRandomPrograms(4));
    RUN(testRandomPrograms(5));
    RUN(testRandomPrograms(6));
    RUN(testRandomPrograms(7));
    RUN(testRandomPrograms(8));
    RUN(testRandomPrograms(9));
    RUN(testRandomPrograms(10));
    RUN(testRandomPrograms(11));
    RUN(testRandomPrograms(12));
    RUN(testRandomPrograms(13));
    RUN(testRandomPrograms(14));
    RUN(testRandomPrograms(15));
    RUN(testRandomProgramsThatOnceFailed());
}

#endif // ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)
