/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "B3CheckValue.h"
#include "B3CountedLoops.h"
#include "B3LoopVersionPlan.h"
#include "B3PhiChildren.h"
#include "B3RegisterPressure.h"

namespace {

struct LoopProcedure {
    explicit LoopProcedure(const Policy& policy = loopPolicy(), B3::Opcode relation = LessThan, int64_t step = 1, int64_t firstIndex = 0)
        : proc(policy)
        , loop(graph, relation, step, firstIndex)
    {
    }

    Procedure proc;
    Graph graph { proc };
    PollState state;
    TopTestedLoop loop;
};

struct FlaggedMemory {
    int64_t key { 7 };
    int64_t scratch { 0 };
    uint8_t flags[16] { };
};

struct RangeCheckedLoop {
    RangeCheckedLoop(Graph& graph, TopTestedLoop& loop)
        : graph(graph)
        , loop(loop)
        , out(graph.proc.addBlock())
    {
        graph.ret(out, graph.int64(out, -1));
    }

    BasicBlock* appendCheck(BasicBlock* block, Value* index, int32_t lengthOffset, HeapRange lengthHeap)
    {
        BasicBlock* inBounds = graph.proc.addBlock();
        Value* length = graph.make(block, ZExt32, graph.load(block, Int32, loop.pointer, lengthOffset, lengthHeap));
        graph.branch(block, graph.make(block, Below, index, length), FrequentedBlock(inBounds), rarely(out));
        return inBounds;
    }

    void addElementToSum(BasicBlock* block)
    {
        Value* address = graph.make(block, Add, loop.pointer, graph.make(block, Shl, graph.get(block, loop.index), graph.int32(block, 3)));
        graph.increase(block, loop.sum, graph.load(block, Int64, address, 16, secondHeap));
    }

    Graph& graph;
    TopTestedLoop& loop;
    BasicBlock* out;
};

struct CheckedArray {
    CheckedArray(int64_t firstLength, int64_t secondLength)
    {
        words[0] = firstLength;
        words[1] = secondLength;
        for (int64_t i = 0; i < numberOfElements; ++i)
            words[2 + i] = i * 7 - 3;
    }

    static constexpr int64_t numberOfElements = 38;
    int64_t words[2 + numberOfElements];
};

struct RangeCheckRun {
    int64_t bound;
    int64_t firstLength;
    int64_t secondLength;
    int64_t shrinkingIndex { -1 };
};

} // anonymous namespace

static bool accessesHeap(Value* value, HeapRange range)
{
    auto* access = value->as<MemoryValue>();
    return access && access->range() == range;
}

static void checkAccessCountInEachLoop(Procedure& proc, HeapRange range, const Vector<unsigned>& expected)
{
    checkValueCountInEachLoop(proc, [&](Value* value) {
        return accessesHeap(value, range);
    }, toUTF8CString("accesses to heap ", range), expected);
}

static void endBodyAndRotate(TopTestedLoop& loop, BasicBlock* bodyLastBlock)
{
    loop.endBody(bodyLastBlock);
    loop.rotate();
}

static BasicBlock* appendGuard(Graph& graph, BasicBlock* block, Value* condition, Variable* sum, int64_t summandOnRarePath)
{
    BasicBlock* rare = graph.proc.addBlock();
    BasicBlock* likely = graph.proc.addBlock();
    graph.branch(block, condition, FrequentedBlock(likely), rarely(rare));
    graph.increase(rare, sum, summandOnRarePath);
    graph.jump(rare, likely);
    return likely;
}

static BasicBlock* appendInvariantGuard(Graph& graph, TopTestedLoop& loop)
{
    Value* loaded = graph.load(loop.body, Int64, loop.pointer, 0, firstHeap);
    BasicBlock* hot = appendGuard(graph, loop.body, graph.make(loop.body, Equal, loaded, loop.extra), loop.sum, 1000);
    graph.increase(hot, loop.sum, loaded);
    return hot;
}

static void checkLoopWithInvariantGuard(Procedure& proc)
{
    auto code = compileProc(proc);
    for (int64_t bound : { -1, 0, 1, 2, 9 }) {
        int64_t iterations = std::max<int64_t>(bound, 0);
        int64_t memory[2] = { 7, 0 };
        CHECK_EQ(run(*code, bound, memory, 7), iterations * 7);
        CHECK_EQ(run(*code, bound, memory, 8), iterations * 1007);
    }
}

static void appendExitingCheck(Graph& graph, BasicBlock* block, Value* condition, int64_t resultOnExit)
{
    CheckValue* check = block->appendNew<CheckValue>(graph.proc, Check, Origin(), condition);
    check->setGenerator([resultOnExit](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        jit.move(CCallHelpers::TrustedImm64(resultOnExit), GPRInfo::returnValueGPR);
        params.code().emitEpilogue(jit);
    });
}

static void checkExcursion(bool hasSmallPath, uint64_t additions, bool copiesLargePath)
{
    LoopProcedure procedure;
    auto& [proc, graph, state, loop] = procedure;
    BasicBlock* unexpected = proc.addBlock();
    BasicBlock* block = proc.addBlock();
    Value* loaded = graph.load(loop.body, Int64, loop.pointer, 0, firstHeap);
    graph.branch(loop.body, graph.make(loop.body, Below, loaded, graph.int64(loop.body, 100)), FrequentedBlock(block), rarely(unexpected));
    graph.callLikePatchpoint(unexpected);
    graph.jump(unexpected, block);
    auto appendRarePath = [&](int64_t mask, int64_t summand, uint64_t numberOfAdditions) {
        BasicBlock* rare = proc.addBlock();
        BasicBlock* next = proc.addBlock();
        Value* remainder = graph.make(block, BitAnd, graph.get(block, loop.index), graph.int64(block, mask));
        graph.branch(block, graph.make(block, Equal, remainder, loop.extra), rarely(rare), FrequentedBlock(next));
        graph.increase(rare, loop.sum, rare->appendNew<Const64Value>(proc, numberedOrigin(summand), summand));
        for (uint64_t i = 0; i < numberOfAdditions; ++i)
            graph.increase(rare, loop.sum, 1);
        graph.jump(rare, next);
        block = next;
    };
    if (hasSmallPath)
        appendRarePath(3, 1000, 0);
    appendRarePath(7, 2000, additions);
    graph.increase(block, loop.sum, graph.make(block, Mul, loaded, graph.get(block, loop.index)));
    endBodyAndRotate(loop, block);

    CHECK(versionLoopsAndValidate(proc));
    CHECK_EQ(numberOfValues(proc, [](Value* value) { return value->isInt64(1000); }), hasSmallPath ? 2u : 0u);
    CHECK_EQ(numberOfValues(proc, [](Value* value) { return value->isInt64(2000); }), copiesLargePath ? 2u : 1u);
    CHECK_EQ(limitHits(proc, Limit::MaxClonedValuesPerExcursion), copiesLargePath ? 0u : 1u);
    if (additions > defaultValueOf(Limit::MaxClonedValuesPerExcursion))
        CHECK(hasNumber(proc.report().firstOriginOf(Limit::MaxClonedValuesPerExcursion), 2000));
    auto code = compileProc(proc);
    for (int64_t bound : { -1, 0, 1, 2, 5, 40 }) {
        for (int64_t rareRemainder : { -1, 0, 1, 3, 4 }) {
            for (int64_t factor : { 7, 200 }) {
                int64_t memory[1] = { factor };
                int64_t sum = 0;
                for (int64_t i = 0; i < bound; ++i) {
                    sum += factor * i + (hasSmallPath && (i & 3) == rareRemainder ? 1000 : 0);
                    sum += (i & 7) == rareRemainder ? 2000 + static_cast<int64_t>(additions) : 0;
                }
                CHECK_EQ(run(*code, bound, memory, rareRemainder), sum);
            }
        }
    }
}

static void testVersionLoopsExcursionLimit()
{
    constexpr uint64_t limit = defaultValueOf(Limit::MaxClonedValuesPerExcursion);
    constexpr uint64_t numberOfOtherClonedValues = 6;
    checkExcursion(true, limit + 1, false);
    checkExcursion(false, limit + 1, false);
    checkExcursion(false, limit - numberOfOtherClonedValues, true);
    checkExcursion(false, limit - numberOfOtherClonedValues + 1, false);
}

template<typename Functor>
static void buildLoopWithVaryingGuard(Graph& graph, TopTestedLoop& loop, const Functor& makeCondition, bool writesFlags)
{
    BasicBlock* guarded = graph.proc.addBlock();
    BasicBlock* next = graph.proc.addBlock();
    Value* key = graph.load(loop.body, Int64, loop.pointer, 0, firstHeap);
    BasicBlock* checked = appendGuard(graph, loop.body, graph.make(loop.body, Equal, key, graph.int64(loop.body, 7)), loop.sum, 1000);
    graph.branch(checked, makeCondition(checked), FrequentedBlock(guarded), rarely(next));
    graph.increase(guarded, loop.sum, graph.load(guarded, Int64, loop.extra, 8, secondHeap));
    graph.jump(guarded, next);
    if (writesFlags)
        graph.store(next, graph.get(next, loop.sum), loop.pointer, 8, thirdHeap);
    endBodyAndRotate(loop, next);
}

static int64_t runLoopWithVaryingGuard(Compilation& code, int64_t bound, FlaggedMemory& memory, int64_t* object)
{
    return run(code, bound, reinterpret_cast<int64_t*>(&memory), reinterpret_cast<int64_t>(object));
}

static void testVersionLoopsHoistsLoadAfterPeeledGuard()
{
    for (bool writesFlags : { false, true }) {
        LoopProcedure procedure;
        auto& [proc, graph, state, loop] = procedure;
        buildLoopWithVaryingGuard(graph, loop, [&](BasicBlock* block) {
            Value* address = graph.make(block, Add, loop.pointer, graph.get(block, loop.index));
            return block->appendNew<MemoryValue>(proc, Load8Z, Origin(), address, 16, thirdHeap);
        }, writesFlags);

        CHECK(versionLoopsAndValidate(proc));
        if (writesFlags)
            checkAccessCountInEachLoop(proc, secondHeap, { 1, 1 });
        else {
            checkAccessCountInEachLoop(proc, secondHeap, { 0, 1 });
            checkAccessCountInEachLoop(proc, thirdHeap, { 1, 1 });
            CHECK_EQ(numberOfValues(proc, [](Value* value) { return accessesHeap(value, thirdHeap); }), 3u);
        }

        auto code = compileProc(proc);
        int64_t object[2] = { 0, 5 };
        auto sumWithFlags = [&](uint8_t evenFlag, uint8_t oddFlag, int64_t* guardedObject) {
            FlaggedMemory memory;
            for (unsigned i = 0; i < std::size(memory.flags); ++i)
                memory.flags[i] = (i & 1) ? oddFlag : evenFlag;
            return runLoopWithVaryingGuard(*code, 10, memory, guardedObject);
        };
        CHECK_EQ(sumWithFlags(0, 0, nullptr), 0);
        CHECK_EQ(sumWithFlags(1, 1, object), 50);
        CHECK_EQ(sumWithFlags(0, 1, object), 25);
        CHECK_EQ(sumWithFlags(1, 0, object), 25);
    }
}

static void testVersionLoopsSkipsConstantPeeledGuard()
{
    LoopProcedure procedure;
    auto& [proc, graph, state, loop] = procedure;
    buildLoopWithVaryingGuard(graph, loop, [&](BasicBlock* block) {
        return graph.make(block, Above, graph.get(block, loop.index), graph.int64(block, 0));
    }, false);

    CHECK(versionLoopsAndValidate(proc));
    checkAccessCountInEachLoop(proc, firstHeap, { 0, 1 });
    checkAccessCountInEachLoop(proc, secondHeap, { 1, 1 });
    auto code = compileProc(proc);
    FlaggedMemory memory;
    int64_t object[2] = { 0, 5 };
    CHECK_EQ(runLoopWithVaryingGuard(*code, 10, memory, object), 45);
    CHECK_EQ(runLoopWithVaryingGuard(*code, 1, memory, nullptr), 0);
}

static void testVersionLoopsPeelsConstantGuardThatPasses()
{
    LoopProcedure procedure;
    auto& [proc, graph, state, loop] = procedure;
    buildLoopWithVaryingGuard(graph, loop, [&](BasicBlock* block) {
        Value* lowBit = graph.make(block, BitAnd, graph.get(block, loop.index), graph.int64(block, 1));
        return graph.make(block, Equal, lowBit, graph.int64(block, 0));
    }, false);

    CHECK(versionLoopsAndValidate(proc));
    checkAccessCountInEachLoop(proc, firstHeap, { 0, 1 });
    checkAccessCountInEachLoop(proc, secondHeap, { 0, 1 });
    auto code = compileProc(proc);
    FlaggedMemory memory;
    int64_t object[2] = { 0, 5 };
    CHECK_EQ(runLoopWithVaryingGuard(*code, 10, memory, object), 25);
    CHECK_EQ(runLoopWithVaryingGuard(*code, 1, memory, object), 5);
    CHECK_EQ(runLoopWithVaryingGuard(*code, 0, memory, nullptr), 0);
}

static void testVersionLoopsSharesEqualPureValues()
{
    enum class Operation : uint8_t { Product, PureFunction };
    for (Operation operation : { Operation::Product, Operation::PureFunction }) {
        for (bool areEqual : { true, false }) {
            LoopProcedure procedure;
            auto& [proc, graph, state, loop] = procedure;
            auto append = [&](BasicBlock* block, int64_t payload) -> Value* {
                if (operation == Operation::Product)
                    return graph.make(block, Mul, loop.bound, graph.int64(block, payload));
                PatchpointValue* patchpoint = graph.emptyPatchpoint(block, Int64);
                patchpoint->pureFunction = PureFunction { static_cast<uint64_t>(payload), true };
                patchpoint->appendSomeRegister(loop.bound);
                return patchpoint;
            };
            buildLoopWithVaryingGuard(graph, loop, [&](BasicBlock* block) {
                Value* first = append(block, 3);
                graph.increase(block, loop.sum, graph.make(block, Sub, first, append(block, areEqual ? 3 : 5)));
                Value* address = graph.make(block, Add, loop.pointer, graph.get(block, loop.index));
                return block->appendNew<MemoryValue>(proc, Load8Z, Origin(), address, 16, thirdHeap);
            }, false);

            CHECK(versionLoopsAndValidate(proc));
            B3::Opcode opcode = operation == Operation::Product ? Mul : Patchpoint;
            checkAccessCountInEachLoop(proc, secondHeap, { 0, 1 });
            checkValueCountInEachLoop(proc, opcode, { 0, 2 });
            CHECK_EQ(numberOfValues(proc, opcode), areEqual ? 3u : 4u);

            auto code = compileProc(proc);
            int64_t difference = operation == Operation::Product && !areEqual ? -20 : 0;
            int64_t object[2] = { 0, 5 };
            FlaggedMemory memory;
            CHECK_EQ(runLoopWithVaryingGuard(*code, 10, memory, nullptr), 10 * difference);
            std::ranges::fill(memory.flags, 1);
            CHECK_EQ(runLoopWithVaryingGuard(*code, 10, memory, object), 10 * (difference + 5));
        }
    }
}

static void testVersionLoopsPollIsNoGuard()
{
    LoopProcedure procedure;
    auto& [proc, graph, state, loop] = procedure;
    appendPoll(graph, loop.body, state)->effects.exitsSideways = true;
    endBodyAndRotate(loop, appendInvariantGuard(graph, loop));

    CHECK(versionLoopsAndValidate(proc));
    checkValueCountInEachLoop(proc, Load, { 0, 1 });
    checkLoopWithInvariantGuard(proc);
}

static void testVersionLoopsHoistsCheck()
{
    LoopProcedure procedure;
    auto& [proc, graph, state, loop] = procedure;
    appendExitingCheck(graph, loop.body, graph.make(loop.body, Equal, loop.extra, graph.int64(loop.body, 13)), -13);
    graph.increase(loop.body, loop.sum, graph.get(loop.body, loop.index));
    endBodyAndRotate(loop, loop.body);

    CHECK(versionLoopsAndValidate(proc));
    checkValueCountInEachLoop(proc, Check, { 0, 1 });
    CHECK_EQ(numberOfValues(proc, Check), 1u);
    CHECK_EQ(numberOfValues(proc, [](Value* value) { return value->opcode() == Equal && value->child(1)->isInt32(0); }), 1u);
    auto code = compileProc(proc);
    for (int64_t bound : { 0, 1, 10 }) {
        CHECK_EQ(run(*code, bound, nullptr, 7), sumBelow(bound));
        CHECK_EQ(run(*code, bound, nullptr, 13), bound ? -13 : 0);
    }
}

static void testVersionLoopsDropsRepeatedCheck()
{
    for (bool isRepeated : { true, false }) {
        LoopProcedure procedure;
        auto& [proc, graph, state, loop] = procedure;
        BasicBlock* hot = appendInvariantGuard(graph, loop);
        Value* index = graph.get(hot, loop.index);
        Value* condition = graph.make(hot, Equal, index, graph.int64(hot, 5));
        appendExitingCheck(graph, hot, condition, -5);
        appendExitingCheck(graph, hot, isRepeated ? condition : graph.make(hot, Equal, index, graph.int64(hot, 3)), -3);
        endBodyAndRotate(loop, hot);

        CHECK(versionLoopsAndValidate(proc));
        checkValueCountInEachLoop(proc, Check, { isRepeated ? 1u : 2u, 2 });
        auto code = compileProc(proc);
        int64_t memory[1] = { 7 };
        CHECK_EQ(run(*code, 3, memory, 7), 21);
        CHECK_EQ(run(*code, 10, memory, 7), isRepeated ? -5 : -3);
    }
}

static void testVersionLoopsPeelsCheck()
{
    for (bool exitsBeforeLoad : { false, true }) {
        LoopProcedure procedure;
        auto& [proc, graph, state, loop] = procedure;
        BasicBlock* hot = appendInvariantGuard(graph, loop);
        Value* object = graph.load(hot, Int64, loop.pointer, 8, firstHeap);
        Value* address = graph.make(hot, Add, loop.pointer, graph.get(hot, loop.index));
        appendExitingCheck(graph, hot, hot->appendNew<MemoryValue>(proc, Load8Z, Origin(), address, 16, thirdHeap), -1);
        if (exitsBeforeLoad)
            graph.emptyPatchpoint(hot)->effects = Effects::forCheck();
        graph.increase(hot, loop.sum, graph.load(hot, Int64, object, 8, secondHeap));
        endBodyAndRotate(loop, hot);

        CHECK(versionLoopsAndValidate(proc));
        checkAccessCountInEachLoop(proc, secondHeap, { exitsBeforeLoad ? 1u : 0u, 1 });
        checkValueCountInEachLoop(proc, Check, { 1, 1 });
        CHECK_EQ(numberOfValues(proc, Check), 2u);
        CHECK_EQ(numberOfValues(proc, [](Value* value) { return accessesHeap(value, thirdHeap); }), exitsBeforeLoad ? 2u : 3u);
        auto code = compileProc(proc);
        int64_t guardedObject[2] = { 0, 5 };
        FlaggedMemory memory;
        memory.scratch = reinterpret_cast<int64_t>(guardedObject);
        CHECK_EQ(run(*code, 10, reinterpret_cast<int64_t*>(&memory), 7), 120);
        memory.flags[4] = 1;
        CHECK_EQ(run(*code, 10, reinterpret_cast<int64_t*>(&memory), 7), -1);
        memory.flags[0] = 1;
        memory.scratch = 0;
        CHECK_EQ(run(*code, 10, reinterpret_cast<int64_t*>(&memory), 7), -1);
    }
}

static unsigned countHoistsBeyondRegisters(unsigned loadsBeyondPreservedRegisters)
{
    LoopProcedure procedure;
    auto& [proc, graph, state, loop] = procedure;
    unsigned numberOfLoads = registersPreservedBy(proc, graph.callLikePatchpoint(loop.body)).gprs + loadsBeyondPreservedRegisters;
    for (unsigned i = 0; i < numberOfLoads; ++i)
        graph.increase(loop.body, loop.sum, graph.load(loop.body, Int64, loop.pointer, static_cast<int32_t>(8 * i), firstHeap));
    endBodyAndRotate(loop, loop.body);

    proc.resetValueOwners();
    auto plans = LoopVersionPlanner(proc).plan(proc.naturalLoops().loop(0));
    CHECK_EQ(plans.size(), 1u);
    unsigned result = plans[0].hoisted.numberOfHoistsBeyondRegisters;
    CHECK(versionLoopsAndValidate(proc));
    checkValueCountInEachLoop(proc, Load, { 0 });
    int64_t memory[32];
    std::ranges::fill(memory, 3);
    CHECK_EQ(run(*compileProc(proc), 10, memory), 30 * static_cast<int64_t>(numberOfLoads));
    return result;
}

static void testVersionLoopsCountsRegistersAcrossCalls()
{
    for (unsigned loadsBeyondPreservedRegisters : { 0u, 1u, 4u })
        CHECK_EQ(countHoistsBeyondRegisters(loadsBeyondPreservedRegisters), loadsBeyondPreservedRegisters);
}

static bool isRangeCheck(Value* value)
{
    return value->opcode() == Below && value->child(1)->opcode() == ZExt32;
}

static void checkRangeCheckCountInEachLoop(Procedure& proc, const Vector<unsigned>& expected)
{
    checkValueCountInEachLoop(proc, isRangeCheck, "range checks"_s, expected);
}

static void buildLoopWithRangeCheck(Graph& graph, TopTestedLoop& loop, PollState& state, unsigned pollsPerIteration = 1)
{
    RangeCheckedLoop checks(graph, loop);
    for (unsigned i = 0; i < pollsPerIteration; ++i)
        appendPoll(graph, loop.body, state);
    BasicBlock* inBounds = checks.appendCheck(loop.body, graph.get(loop.body, loop.index), 0, firstHeap);
    checks.addElementToSum(inBounds);
    endBodyAndRotate(loop, inBounds);
}

using NeighborMaker = Value* (*)(Graph&, BasicBlock*, Value* index);

static Value* indexPlusOne(Graph& graph, BasicBlock* block, Value* index) { return graph.make(block, Add, index, graph.int64(block, 1)); }
static Value* onePlusIndex(Graph& graph, BasicBlock* block, Value* index) { return graph.make(block, Add, graph.int64(block, 1), index); }
static Value* indexMinusNegativeOne(Graph& graph, BasicBlock* block, Value* index) { return graph.make(block, Sub, index, graph.int64(block, -1)); }
static Value* indexPlusNegativeOne(Graph& graph, BasicBlock* block, Value* index) { return graph.make(block, Add, index, graph.int64(block, -1)); }

static void buildLoopWithTwoRangeChecks(Graph& graph, TopTestedLoop& loop, PollState& state, NeighborMaker makeNeighbor, bool storesSecondLength)
{
    RangeCheckedLoop checks(graph, loop);
    appendPoll(graph, loop.body, state);
    BasicBlock* inFirst = checks.appendCheck(loop.body, graph.get(loop.body, loop.index), 0, firstHeap);
    BasicBlock* inSecond = checks.appendCheck(inFirst, makeNeighbor(graph, inFirst, graph.get(inFirst, loop.index)), 8, thirdHeap);
    checks.addElementToSum(inSecond);
    BasicBlock* last = inSecond;
    if (storesSecondLength) {
        BasicBlock* shrinks = graph.proc.addBlock();
        BasicBlock* stays = graph.proc.addBlock();
        graph.branch(inSecond, graph.make(inSecond, Equal, graph.get(inSecond, loop.index), loop.extra), rarely(shrinks), FrequentedBlock(stays));
        graph.store(shrinks, graph.int32(shrinks, 1), loop.pointer, 8, thirdHeap);
        graph.jump(shrinks, stays);
        last = stays;
    }
    endBodyAndRotate(loop, last);
}

static int64_t sumWithTwoRangeChecks(CheckedArray array, int64_t neighborOffset, const RangeCheckRun& testRun)
{
    int64_t sum = 0;
    for (int64_t i = 0; i < testRun.bound; ++i) {
        if (!holds(Below, i, array.words[0]) || !holds(Below, i + neighborOffset, array.words[1]))
            return -1;
        sum += array.words[2 + i];
        if (i == testRun.shrinkingIndex)
            array.words[1] = 1;
    }
    return sum;
}

static void checkRangeCheckRun(Compilation& code, int64_t neighborOffset, const RangeCheckRun& testRun)
{
    CheckedArray array(testRun.firstLength, testRun.secondLength);
    int64_t expected = sumWithTwoRangeChecks(array, neighborOffset, testRun);
    CHECK_EQ(run(code, testRun.bound, array.words, testRun.shrinkingIndex), expected);
}

static void checkLengthsAroundBound(Procedure& proc, int64_t neighborOffset)
{
    auto code = compileProc(proc);
    for (RangeCheckRun testRun : { RangeCheckRun { 10, 20, 20 }, RangeCheckRun { 10, 10, 11 }, RangeCheckRun { 10, 10, 10 }, RangeCheckRun { 10, 5, 20 }, RangeCheckRun { 0, 0, 0 } })
        checkRangeCheckRun(*code, neighborOffset, testRun);
}

static void testVersionLoopsRemovesRangeCheckAgainstOtherLimit()
{
    constexpr NeighborMaker nextIndexForms[] = { indexPlusOne, onePlusIndex, indexMinusNegativeOne };
    for (NeighborMaker makeNeighbor : nextIndexForms) {
        LoopProcedure procedure;
        auto& [proc, graph, state, loop] = procedure;
        buildLoopWithTwoRangeChecks(graph, loop, state, makeNeighbor, false);

        CHECK(versionLoopsAndValidate(proc));
        CHECK_EQ(proc.report().countOf(Counter::RangeChecksRemoved), 2u);
        checkRangeCheckCountInEachLoop(proc, { 0, 2 });
        checkLengthsAroundBound(proc, 1);
    }

    LoopProcedure procedure;
    auto& [proc, graph, state, loop] = procedure;
    buildLoopWithTwoRangeChecks(graph, loop, state, indexPlusOne, true);

    CHECK(versionLoopsAndValidate(proc));
    CHECK_EQ(proc.report().countOf(Counter::RangeChecksRemoved), 1u);
    CHECK_EQ(valueCountInEachLoop(proc, isRangeCheck).first(), 1u);
    auto code = compileProc(proc);
    checkRangeCheckRun(*code, 1, { 10, 20, 20, 3 });
    checkRangeCheckRun(*code, 1, { 10, 20, 20, -1 });
}

static void testVersionLoopsRangeCheckBoundLimit()
{
    constexpr unsigned limit = defaultValueOf(Limit::MaxRangeCheckBoundsPerLoop);
    for (unsigned numberOfChecks : { limit, limit + 1 }) {
        LoopProcedure procedure;
        auto& [proc, graph, state, loop] = procedure;
        RangeCheckedLoop checks(graph, loop);
        appendPoll(graph, loop.body, state);
        BasicBlock* block = loop.body;
        for (unsigned i = 0; i < numberOfChecks; ++i)
            block = checks.appendCheck(block, graph.get(block, loop.index), static_cast<int32_t>(8 * i), firstHeap);
        graph.increase(block, loop.sum, 1);
        endBodyAndRotate(loop, block);

        CHECK(versionLoopsAndValidate(proc));
        CHECK_EQ(proc.report().countOf(Counter::RangeChecksRemoved), limit);
        CHECK_EQ(limitHits(proc, Limit::MaxRangeCheckBoundsPerLoop) > 0, numberOfChecks > limit);
        checkRangeCheckCountInEachLoop(proc, { numberOfChecks - limit, numberOfChecks });
        auto code = compileProc(proc);
        int64_t lengths[limit + 1];
        std::ranges::fill(lengths, 20);
        CHECK_EQ(run(*code, 10, lengths), 10);
        lengths[numberOfChecks - 1] = 5;
        CHECK_EQ(run(*code, 10, lengths), -1);
    }
}

static void testVersionLoopsFirstIterationLeaves()
{
    LoopProcedure procedure;
    auto& [proc, graph, state, loop] = procedure;
    buildLoopWithTwoRangeChecks(graph, loop, state, indexPlusNegativeOne, false);

    CHECK(!versionLoopsAndValidate(proc));
    CHECK(hasRemark(proc, RemarkKind::LoopNotVersioned, GaveUpReason::NoSingleLikelyPath));
    checkLengthsAroundBound(proc, -1);
}

static int64_t sumBelowUnsignedBound(const CheckedArray& array, int64_t bound)
{
    int64_t sum = 0;
    for (int64_t i = 0; holds(Below, i, bound); ++i) {
        if (!holds(Below, i, array.words[0]))
            return -1;
        sum += array.words[2 + i];
    }
    return sum;
}

static void testVersionLoopsKeepsRangeCheckOfDecreasingLoop()
{
    LoopProcedure procedure(loopPolicy(), GreaterThan, -1, 9);
    auto& [proc, graph, state, loop] = procedure;
    buildLoopWithRangeCheck(graph, loop, state);

    CHECK(versionLoopsAndValidate(proc));
    CHECK_EQ(proc.report().countOf(Counter::RangeChecksRemoved), 0u);
    checkRangeCheckCountInEachLoop(proc, { 1 });
    auto code = compileProc(proc);
    for (int64_t length : { 20, 5 }) {
        CheckedArray array(length, 0);
        CHECK_EQ(run(*code, -1, array.words), sumBelowUnsignedBound(array, 10));
    }
}

static void testVersionLoopsRemovesRangeCheckOfLaggingIndex()
{
    LoopProcedure procedure(loopPolicy(), LessThan, 1, 1);
    auto& [proc, graph, state, loop] = procedure;
    RangeCheckedLoop checks(graph, loop);
    Variable* previous = proc.addVariable(Int64);
    Value* zero = proc.add<Const64Value>(Origin(), 0);
    loop.root->appendNonTerminal(zero);
    loop.root->appendNonTerminal(proc.add<VariableValue>(B3::Set, Origin(), previous, zero));
    appendPoll(graph, loop.body, state);
    BasicBlock* inBounds = checks.appendCheck(loop.body, graph.get(loop.body, previous), 0, firstHeap);
    graph.increase(inBounds, loop.sum, graph.get(inBounds, previous));
    graph.set(inBounds, previous, graph.get(inBounds, loop.index));
    endBodyAndRotate(loop, inBounds);

    CHECK(versionLoopsAndValidate(proc));
    CHECK_EQ(proc.report().countOf(Counter::RangeChecksRemoved), 1u);
    auto code = compileProc(proc);
    for (int64_t length : { 20, 9, 8, 0 }) {
        CheckedArray array(length, 0);
        CHECK_EQ(run(*code, 10, array.words), length >= 9 ? sumBelow(9) : -1);
        CHECK_EQ(run(*code, 1, array.words), 0);
    }
}

static void testVersionLoopsRemovesRangeCheckUnderUnsignedRelation()
{
    LoopProcedure procedure(loopPolicy(), Below);
    auto& [proc, graph, state, loop] = procedure;
    buildLoopWithRangeCheck(graph, loop, state);

    CHECK(versionLoopsAndValidate(proc));
    CHECK_EQ(proc.report().countOf(Counter::RangeChecksRemoved), 1u);
    auto code = compileProc(proc);
    for (int64_t bound : { 10, 0, -1 }) {
        for (int64_t length : { 20, 10, 5 }) {
            CheckedArray array(length, 0);
            CHECK_EQ(run(*code, bound, array.words), sumBelowUnsignedBound(array, bound));
        }
    }
}

static void testVersionLoopsRemovesRangeCheckOfInt32Counter()
{
    LoopProcedure procedure;
    auto& [proc, graph, state, loop] = procedure;
    RangeCheckedLoop checks(graph, loop);
    BasicBlock* head = proc.addBlock();
    BasicBlock* inBounds = proc.addBlock();
    Variable* index = proc.addVariable(Int32);
    Value* bound = graph.make(loop.body, Trunc, loop.bound);
    graph.set(loop.body, index, graph.int32(loop.body, 0));
    graph.jump(loop.body, head);
    Value* length = graph.load(head, Int32, loop.pointer, 0, firstHeap);
    Value* factor = graph.load(head, Int32, loop.pointer, 4, firstHeap);
    graph.branch(head, graph.make(head, Below, graph.get(head, index), length), FrequentedBlock(inBounds), rarely(checks.out));
    graph.increase(inBounds, loop.sum, graph.make(inBounds, SExt32, graph.make(inBounds, Mul, factor, graph.get(inBounds, index))));
    Value* next = graph.make(inBounds, Add, graph.get(inBounds, index), graph.int32(inBounds, 1));
    graph.set(inBounds, index, next);
    Value* test = graph.make(inBounds, LessThan, next, bound);
    graph.branch(inBounds, test, head, loop.exit);
    proc.setKnownInduction(DemotedInduction { index, head, next, test, 1, false });
    proc.resetReachability();
    fixSSA(proc);
    validate(proc);

    CHECK(versionLoopsAndValidate(proc));
    CHECK_EQ(proc.report().countOf(Counter::RangeChecksRemoved), 1u);
    CHECK_EQ(proc.report().countOf(Counter::LoopBoundsClamped), 1u);
    checkValueCountInEachLoop(proc, Below, { 0, 1 });
    CHECK_EQ(numberOfValues(proc, [](Value* value) { return value->opcode() == Select && value->type() == Int64; }), 1u);
    auto code = compileProc(proc);
    for (int32_t limit : { 0, 1, 5, std::numeric_limits<int32_t>::max(), -1 }) {
        for (int64_t iterations : { 0, 1, 10 }) {
            int32_t memory[2] = { limit, 3 };
            bool staysInBounds = static_cast<uint64_t>(iterations) <= static_cast<uint32_t>(limit);
            CHECK_EQ(run(*code, iterations, reinterpret_cast<int64_t*>(memory)), staysInBounds ? 3 * sumBelow(iterations) : -1);
        }
    }
}

static void checkPollsStayInFastLoop(const Policy& policy, unsigned pollsPerIteration)
{
    LoopProcedure procedure(policy);
    auto& [proc, graph, state, loop] = procedure;
    buildLoopWithRangeCheck(graph, loop, state, pollsPerIteration);

    unsigned numberOfClampedBounds = policy.loops.clampsLoopBounds;
    CHECK(versionLoopsAndValidate(proc));
    CHECK(!hasRemark(proc, RemarkKind::LoopBoundNotClamped));
    CHECK_EQ(proc.report().countOf(Counter::LoopBoundsClamped), numberOfClampedBounds);
    CHECK_EQ(proc.report().countOf(Counter::RangeChecksRemoved), numberOfClampedBounds);
    bool fastLoopLeavesForOriginalLoop = numberOfClampedBounds;
    checkRangeCheckCountInEachLoop(proc, fastLoopLeavesForOriginalLoop ? Vector<unsigned> { 0, 1 } : Vector<unsigned> { 1 });
    checkValueCountInEachLoop(proc, Load, fastLoopLeavesForOriginalLoop ? Vector<unsigned> { 1, 2 } : Vector<unsigned> { 1 });
    checkValueCountInEachLoop(proc, Patchpoint, Vector<unsigned>(FillWith { }, fastLoopLeavesForOriginalLoop ? 2 : 1, pollsPerIteration));
    auto code = compileProc(proc);
    for (int64_t length : { 20, 10, 5 })
        checkRangeCheckRun(*code, 0, { 10, length, length });
    CHECK_EQ(state.numberOfPolls, 26 * pollsPerIteration);
}

static void testVersionLoopsPollsStayInFastLoop()
{
    for (unsigned pollsPerIteration : { 0u, 1u, 2u })
        checkPollsStayInFastLoop(loopPolicy(), pollsPerIteration);
    checkPollsStayInFastLoop(remarkPolicy(), 1);
}

template<typename Functor>
static void checkBoundIsNotClamped(GaveUpReason reason, const Functor& buildLoop)
{
    LoopProcedure procedure;
    auto& [proc, graph, state, loop] = procedure;
    buildLoop(graph, loop);

    CHECK(versionLoopsAndValidate(proc));
    CHECK(hasRemark(proc, RemarkKind::LoopBoundNotClamped, reason));
    CHECK_EQ(proc.report().countOf(Counter::LoopBoundsClamped), 0u);
    int64_t memory[2] = { 7, 0 };
    CHECK_EQ(run(*compileProc(proc), 8, memory, 7), 56);
}

static void testVersionLoopsReportsUnclampedBound()
{
    checkBoundIsNotClamped(GaveUpReason::NotCounted, [](Graph& graph, TopTestedLoop& loop) {
        endBodyAndRotate(loop, appendInvariantGuard(graph, loop));
        graph.proc.knownInductions().clear();
    });
    checkBoundIsNotClamped(GaveUpReason::TestNotOnLikelyPath, [](Graph& graph, TopTestedLoop& loop) {
        BasicBlock* hot = appendInvariantGuard(graph, loop);
        BasicBlock* test = graph.proc.addBlock();
        Value* next = graph.make(hot, Add, graph.get(hot, loop.index), graph.int64(hot, 1));
        graph.set(hot, loop.index, next);
        graph.branch(hot, graph.make(hot, BitAnd, next, graph.int64(hot, 7)), FrequentedBlock(loop.body), rarely(test));
        Value* comparison = graph.make(test, LessThan, next, loop.bound);
        graph.branch(test, comparison, loop.body, loop.exit);
        graph.proc.setKnownInduction(DemotedInduction { loop.index, loop.body, next, comparison, 1, false });
        graph.proc.resetReachability();
        fixSSA(graph.proc);
        validate(graph.proc);
    });
    checkBoundIsNotClamped(GaveUpReason::EffectsBeforeTopTest, [](Graph& graph, TopTestedLoop& loop) {
        loop.header->appendNonTerminal(graph.proc.add<MemoryValue>(Store, Origin(), loop.bound, loop.pointer, 8, thirdHeap));
        loop.endBody(appendInvariantGuard(graph, loop));
    });
}

static void testVersionLoopsUnswitchLimit()
{
    constexpr uint64_t numberOfClonedValues = 78;
    for (uint64_t limit : { numberOfClonedValues, numberOfClonedValues - 1 }) {
        bool isWithinLimit = limit == numberOfClonedValues;
        Policy policy = loopPolicy();
        policy.limits.set(Limit::MaxClonedValuesPerUnswitchedLoop, limit);
        LoopProcedure procedure(policy);
        auto& [proc, graph, state, loop] = procedure;
        BasicBlock* odd = proc.addBlock();
        BasicBlock* even = proc.addBlock();
        BasicBlock* next = proc.addBlock();
        Value* isOdd = graph.make(loop.body, NotEqual, graph.make(loop.body, BitAnd, loop.extra, graph.int64(loop.body, 1)), graph.int64(loop.body, 0));
        graph.branch(loop.body, isOdd, odd, even);
        graph.increase(odd, loop.sum, graph.get(odd, loop.index));
        graph.jump(odd, next);
        graph.increase(even, loop.sum, 1);
        graph.jump(even, next);
        Value* value = graph.get(next, loop.index);
        for (unsigned i = 0; i < 30; ++i)
            value = graph.make(next, BitXor, value, graph.get(next, loop.index));
        graph.increase(next, loop.sum, graph.make(next, BitAnd, value, graph.int64(next, 0)));
        endBodyAndRotate(loop, next);

        CHECK(versionLoopsAndValidate(proc));
        CHECK_EQ(proc.naturalLoops().numLoops(), isWithinLimit ? 3u : 1u);
        CHECK_EQ(limitHits(proc, Limit::MaxClonedValuesPerUnswitchedLoop), isWithinLimit ? 0u : 1u);
        auto code = compileProc(proc);
        for (int64_t bound : { 0, 1, 10 }) {
            CHECK_EQ(run(*code, bound, nullptr, 3), sumBelow(bound));
            CHECK_EQ(run(*code, bound, nullptr, 4), bound);
        }
    }
}

static void testVersionLoopsTwoLoopsInARow()
{
    Procedure proc(loopPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* firstBody = proc.addBlock();
    BasicBlock* firstLatch = proc.addBlock();
    BasicBlock* middle = proc.addBlock();
    BasicBlock* secondHeader = proc.addBlock();
    BasicBlock* secondBody = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* index = proc.addVariable(Int64);
    Variable* sum = proc.addVariable(Int64);
    auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root);
    Value* bound = arguments[0];
    Value* pointer = arguments[1];
    Value* extra = arguments[2];
    graph.set(root, index, graph.int64(root, 0));
    graph.set(root, sum, graph.int64(root, 0));
    graph.jump(root, firstBody);

    Value* inner = graph.load(firstBody, Int64, pointer, 0, firstHeap);
    BasicBlock* firstHot = appendGuard(graph, firstBody, graph.make(firstBody, NotEqual, inner, graph.int64(firstBody, 0)), sum, 1000);
    graph.increase(firstHot, sum, 1);
    graph.increase(firstHot, index, 1);
    graph.branch(firstHot, graph.make(firstHot, LessThan, graph.get(firstHot, index), bound), firstLatch, middle);
    graph.jump(firstLatch, firstBody);
    graph.jump(middle, secondHeader);

    Value* secondBound = graph.make(secondHeader, Add, bound, graph.int64(secondHeader, 5));
    graph.branch(secondHeader, graph.make(secondHeader, LessThan, graph.get(secondHeader, index), secondBound), secondBody, exit);
    Value* value = graph.load(secondBody, Int64, inner, 8, secondHeap);
    BasicBlock* secondHot = appendGuard(graph, secondBody, graph.make(secondBody, Equal, value, extra), sum, 100);
    graph.increase(secondHot, sum, value);
    graph.increase(secondHot, index, 1);
    graph.jump(secondHot, secondHeader);
    graph.ret(exit, graph.get(exit, sum));
    proc.resetReachability();
    fixSSA(proc);
    validate(proc);
    CHECK(rotateLoops(proc));
    fixSSA(proc);
    validate(proc);

    CHECK(versionLoopsAndValidate(proc));
    CHECK_EQ(proc.report().countOf(Counter::LoopsVersioned), 2u);
    auto code = compileProc(proc);
    int64_t innerArray[2] = { 0, 9 };
    int64_t outer[1] = { reinterpret_cast<int64_t>(innerArray) };
    for (int64_t limit : { 0, 1, 3 }) {
        int64_t firstIterations = std::max<int64_t>(limit, 1);
        int64_t secondIterations = limit + 5 - firstIterations;
        CHECK_EQ(run(*code, limit, outer, 9), firstIterations + secondIterations * 9);
        CHECK_EQ(run(*code, limit, outer, 8), firstIterations + secondIterations * 109);
    }
}

static void testVersionLoopsRareBlockSeesHoistedValue()
{
    LoopProcedure procedure;
    auto& [proc, graph, state, loop] = procedure;
    BasicBlock* special = proc.addBlock();
    BasicBlock* hot = proc.addBlock();
    Value* loaded = graph.load(loop.body, Int64, loop.pointer, 0, firstHeap);
    BasicBlock* checked = appendGuard(graph, loop.body, graph.make(loop.body, Equal, loaded, loop.extra), loop.sum, 1000);
    graph.branch(checked, graph.make(checked, Equal, graph.get(checked, loop.index), graph.int64(checked, 4)), rarely(special), FrequentedBlock(hot));
    graph.callLikePatchpoint(special);
    graph.increase(special, loop.sum, loaded);
    graph.jump(special, hot);
    graph.increase(hot, loop.sum, graph.make(hot, Add, loaded, loaded));
    endBodyAndRotate(loop, hot);

    CHECK(versionLoopsAndValidate(proc));
    auto isDoubling = [](Value* value) {
        return value->opcode() == Add && value->child(0) == value->child(1);
    };
    checkValueCountInEachLoop(proc, Load, { 0, 1 });
    CHECK_EQ(valueCountInEachLoop(proc, isDoubling).first(), 0u);
    CHECK_EQ(numberOfValues(proc, isDoubling), 2u);
    auto code = compileProc(proc);
    int64_t memory[1] = { 7 };
    CHECK_EQ(run(*code, 10, memory, 7), 147);
    CHECK_EQ(run(*code, 3, memory, 7), 42);
    CHECK_EQ(run(*code, 10, memory, 8), 10147);
}

void addBunLoopPlanTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testVersionLoopsExcursionLimit());
    RUN(testVersionLoopsHoistsLoadAfterPeeledGuard());
    RUN(testVersionLoopsSkipsConstantPeeledGuard());
    RUN(testVersionLoopsPeelsConstantGuardThatPasses());
    RUN(testVersionLoopsSharesEqualPureValues());
    RUN(testVersionLoopsPollIsNoGuard());
    RUN(testVersionLoopsHoistsCheck());
    RUN(testVersionLoopsDropsRepeatedCheck());
    RUN(testVersionLoopsPeelsCheck());
    RUN(testVersionLoopsCountsRegistersAcrossCalls());
    RUN(testVersionLoopsRemovesRangeCheckAgainstOtherLimit());
    RUN(testVersionLoopsRangeCheckBoundLimit());
    RUN(testVersionLoopsFirstIterationLeaves());
    RUN(testVersionLoopsKeepsRangeCheckOfDecreasingLoop());
    RUN(testVersionLoopsRemovesRangeCheckOfLaggingIndex());
    RUN(testVersionLoopsRemovesRangeCheckUnderUnsignedRelation());
    RUN(testVersionLoopsRemovesRangeCheckOfInt32Counter());
    RUN(testVersionLoopsPollsStayInFastLoop());
    RUN(testVersionLoopsReportsUnclampedBound());
    RUN(testVersionLoopsUnswitchLimit());
    RUN(testVersionLoopsTwoLoopsInARow());
    RUN(testVersionLoopsRareBlockSeesHoistedValue());
}

#endif // ENABLE(B3_JIT)
