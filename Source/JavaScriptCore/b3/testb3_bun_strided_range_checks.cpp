/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "B3AffineIndex.h"
#include "B3EliminateDeadCode.h"
#include "B3LoopRangeChecks.h"
#include "B3LoopUtilities.h"
#include "B3LoopVersionPlan.h"

namespace {

enum class IndexKind : uint8_t { ZeroExtendedInt32, SignExtendedInt32, Int64 };

struct StridedMemory {
    static constexpr int64_t numberOfElements = 70;

    StridedMemory(int64_t length, int32_t scale, int32_t offset)
        : length(length)
        , scale(scale)
        , offset(offset)
        , vector(elements)
    {
        for (int64_t i = 0; i < numberOfElements; ++i)
            elements[i] = i * 7 - 3;
    }

    int64_t length;
    int32_t scale;
    int32_t offset;
    int64_t* vector;
    int64_t elements[numberOfElements];
};

struct StridedLoopShape {
    B3::Opcode relation { LessThan };
    int32_t step { 1 };
    IndexKind indexKind { IndexKind::ZeroExtendedInt32 };
    std::optional<int32_t> constantScale;
    bool isRotated { true };
    bool isBottomTested { false };
    Vector<int32_t> addends { 0 };
    std::optional<int64_t> lengthStoredOnRarePath;
    std::optional<int64_t> lengthClobberedOnRarePath;
    bool addsOnRarePath { false };
    bool continuesEarly { false };
};

// for (int32_t i = start; i REL bound; i += step) sum += elements[scale * i + offset + addend], returning -1 at the first index that is out of bounds.
// A bottom-tested loop runs its first iteration unconditionally.
struct StridedLoop {
    StridedLoop(const Policy& policy, const StridedLoopShape& shape)
        : proc(policy)
        , shape(shape)
    {
        BasicBlock* root = proc.addBlock();
        BasicBlock* header = proc.addBlock();
        BasicBlock* body = proc.addBlock();
        BasicBlock* out = proc.addBlock();
        BasicBlock* exit = proc.addBlock();
        Variable* counter = proc.addVariable(Int32);
        Variable* sum = proc.addVariable(Int64);
        auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root).eager();
        Value* bound = graph.make(root, Trunc, arguments[0]);
        Value* pointer = arguments[1];
        Value* start = graph.make(root, Trunc, arguments[2]);
        Value* scale = shape.constantScale ? graph.int32(root, *shape.constantScale) : graph.load(root, Int32, pointer, OBJECT_OFFSETOF(StridedMemory, scale), thirdHeap);
        Value* offset = graph.load(root, Int32, pointer, OBJECT_OFFSETOF(StridedMemory, offset), thirdHeap);
        graph.set(root, counter, start);
        graph.set(root, sum, graph.int64(root, 0));
        graph.jump(root, shape.isBottomTested ? body : header);
        Value* test = nullptr;
        if (!shape.isBottomTested) {
            test = graph.make(header, shape.relation, graph.get(header, counter), bound);
            graph.branch(header, test, body, exit);
        }

        appendPoll(graph, body, state);
        BasicBlock* block = body;
        if (shape.lengthStoredOnRarePath || shape.lengthClobberedOnRarePath) {
            BasicBlock* rare = proc.addBlock();
            BasicBlock* next = proc.addBlock();
            graph.branch(block, graph.make(block, Equal, graph.get(block, counter), graph.int32(block, 3)), rarely(rare), FrequentedBlock(next));
            if (shape.lengthStoredOnRarePath)
                graph.store(rare, graph.int64(rare, *shape.lengthStoredOnRarePath), pointer, 0, firstHeap);
            else {
                PatchpointValue* clobber = graph.callLikePatchpoint(rare);
                clobber->effects.writes = firstHeap;
                clobber->append(pointer, ValueRep::SomeRegister);
                clobber->clobber(RegisterSet::macroClobberedGPRs());
                clobber->setGenerator([length = *shape.lengthClobberedOnRarePath](CCallHelpers& jit, const StackmapGenerationParams& params) {
                    AllowMacroScratchRegisterUsage allowScratch(jit);
                    jit.store64(CCallHelpers::TrustedImm32(static_cast<int32_t>(length)), CCallHelpers::Address(params[0].gpr()));
                });
            }
            graph.jump(rare, next);
            block = next;
        }
        if (shape.addsOnRarePath) {
            BasicBlock* rare = proc.addBlock();
            BasicBlock* next = proc.addBlock();
            graph.branch(block, graph.make(block, Equal, graph.get(block, counter), graph.int32(block, 3)), rarely(rare), FrequentedBlock(next));
            graph.increase(rare, sum, 1000);
            graph.jump(rare, next);
            block = next;
        }
        Value* vector = nullptr;
        for (int32_t addend : shape.addends) {
            BasicBlock* inBounds = proc.addBlock();
            Value* index = appendIndex(block, graph.get(block, counter), scale, offset, addend);
            Value* length = graph.load(block, Int64, pointer, 0, firstHeap);
            vector = graph.load(block, Int64, pointer, OBJECT_OFFSETOF(StridedMemory, vector), thirdHeap);
            graph.branch(block, graph.make(block, Below, index, length), FrequentedBlock(inBounds), rarely(out));
            Value* address = graph.make(inBounds, Add, vector, graph.make(inBounds, Shl, index, graph.int32(inBounds, 3)));
            graph.increase(inBounds, sum, graph.load(inBounds, Int64, address, 0, secondHeap));
            block = inBounds;
        }
        Value* current = graph.get(block, counter);
        Value* isOdd = graph.make(block, BitAnd, current, graph.int32(block, 1));
        Value* next = graph.make(block, Add, current, graph.int32(block, shape.step));
        graph.set(block, counter, next);
        if (shape.continuesEarly) {
            BasicBlock* more = proc.addBlock();
            graph.branch(block, isOdd, more, header);
            Value* index = appendIndex(more, current, scale, offset, shape.addends.last());
            Value* address = graph.make(more, Add, vector, graph.make(more, Shl, index, graph.int32(more, 3)));
            graph.increase(more, sum, graph.load(more, Int64, address, 0, secondHeap));
            block = more;
        }
        if (shape.isBottomTested) {
            test = graph.make(block, shape.relation, next, bound);
            graph.branch(block, test, body, exit);
        } else
            graph.jump(block, header);
        graph.ret(out, graph.int64(out, -1));
        graph.ret(exit, graph.get(exit, sum));
        proc.setKnownInduction(DemotedInduction { counter, shape.isBottomTested ? body : header, next, test, shape.step, false });
        proc.resetReachability();
        fixSSA(proc);
        validate(proc);
        if (shape.isRotated && !shape.isBottomTested) {
            CHECK(rotateLoops(proc));
            fixSSA(proc);
            validate(proc);
        }
    }

    Value* appendIndex(BasicBlock* block, Value* counter, Value* scale, Value* offset, int32_t addend)
    {
        if (shape.indexKind == IndexKind::Int64) {
            auto extend = [&](Value* value) {
                return graph.make(block, SExt32, value);
            };
            Value* product = graph.make(block, Mul, extend(counter), extend(scale));
            return graph.make(block, Add, graph.make(block, Add, product, extend(offset)), graph.int64(block, addend));
        }
        Value* index = graph.make(block, Add, graph.make(block, Add, graph.make(block, Mul, counter, scale), offset), graph.int32(block, addend));
        return graph.make(block, shape.indexKind == IndexKind::SignExtendedInt32 ? SExt32 : ZExt32, index);
    }

    Procedure proc;
    Graph graph { proc };
    PollState state;
    StridedLoopShape shape;
};

struct ExpectedRun {
    int64_t result { 0 };
    int64_t numberOfTurns { 0 };
    bool readsOutsideElements { false };
};

} // anonymous namespace

static bool holdsForInt32(B3::Opcode relation, int32_t left, int32_t right)
{
    if (isUnsignedRelation(relation))
        return holds(relation, static_cast<uint32_t>(left), static_cast<uint32_t>(right));
    return holds(relation, left, right);
}

static uint64_t indexFor(const StridedLoopShape& shape, int32_t counter, int32_t scale, int32_t offset, int32_t addend)
{
    if (shape.indexKind == IndexKind::Int64)
        return static_cast<uint64_t>(static_cast<int64_t>(counter) * scale + offset + addend);
    uint32_t index = static_cast<uint32_t>(counter) * static_cast<uint32_t>(scale) + static_cast<uint32_t>(offset) + static_cast<uint32_t>(addend);
    if (shape.indexKind == IndexKind::SignExtendedInt32)
        return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(index)));
    return index;
}

static ExpectedRun expectedRun(const StridedLoopShape& shape, StridedMemory memory, int32_t start, int32_t bound)
{
    ExpectedRun run;
    int32_t scale = shape.constantScale.value_or(memory.scale);
    for (int32_t counter = start; (shape.isBottomTested && !run.numberOfTurns) || holdsForInt32(shape.relation, counter, bound); counter = static_cast<int32_t>(static_cast<uint32_t>(counter) + static_cast<uint32_t>(shape.step))) {
        ++run.numberOfTurns;
        if (counter == 3) {
            if (shape.lengthStoredOnRarePath)
                memory.length = *shape.lengthStoredOnRarePath;
            if (shape.lengthClobberedOnRarePath)
                memory.length = *shape.lengthClobberedOnRarePath;
        }
        if (counter == 3 && shape.addsOnRarePath)
            run.result += 1000;
        for (int32_t addend : shape.addends) {
            uint64_t index = indexFor(shape, counter, scale, memory.offset, addend);
            if (index >= static_cast<uint64_t>(memory.length)) {
                run.result = -1;
                return run;
            }
            if (index >= static_cast<uint64_t>(StridedMemory::numberOfElements)) {
                run.readsOutsideElements = true;
                return run;
            }
            run.result += memory.elements[index];
            if (shape.continuesEarly && (counter & 1) && addend == shape.addends.last())
                run.result += memory.elements[index];
        }
    }
    return run;
}

static bool isRangeCheck(Value* value)
{
    return !!rangeCheckFor(value);
}

static constexpr int32_t minInt32 = std::numeric_limits<int32_t>::min();
static constexpr int32_t maxInt32 = std::numeric_limits<int32_t>::max();

static void checkRuns(StridedLoop& loop)
{
    const StridedLoopShape& shape = loop.shape;
    auto code = compileProc(loop.proc);
    Vector<int32_t> scales { 0, 1, -1, 2, -2, 3, 7, 1 << 16, 1 << 30, maxInt32, minInt32 };
    if (shape.constantScale)
        scales = { 0 };
    bool increases = shape.step > 0;
    for (int32_t scale : scales) {
        for (int32_t offset : { 0, 1, -1, 5, -5, 34, 69, 70, maxInt32, minInt32 }) {
            for (int64_t length : { int64_t { 0 }, int64_t { 1 }, int64_t { 2 }, int64_t { 9 }, int64_t { 10 }, int64_t { 11 }, int64_t { 35 }, int64_t { 70 }, int64_t { 1 } << 31, (int64_t { 1 } << 31) + 1, int64_t { 1 } << 33, int64_t { -1 } }) {
                for (int32_t start : { 0, 1, -1, 3, -4, 12 }) {
                    for (int32_t bound : { -3, 0, 1, 2, 5, 10, 11, 36 }) {
                        if (isUnsignedRelation(shape.relation) && (start < 0 || bound < 0))
                            continue;
                        StridedMemory memory(length, scale, offset);
                        ExpectedRun expected = expectedRun(shape, memory, increases ? start : bound, increases ? bound : start);
                        if (expected.readsOutsideElements)
                            continue;
                        CHECK_EQ(run(*code, increases ? bound : start, reinterpret_cast<int64_t*>(&memory), increases ? start : bound), expected.result);
                    }
                }
            }
        }
    }
}

static void testVersionLoopsRemovesStridedRangeChecks()
{
    for (B3::Opcode relation : { LessThan, LessEqual, GreaterThan, GreaterEqual }) {
        for (int32_t stepSize : { 1, 2, 3 }) {
            for (IndexKind indexKind : { IndexKind::ZeroExtendedInt32, IndexKind::SignExtendedInt32, IndexKind::Int64 }) {
                for (std::optional<int32_t> constantScale : { std::optional<int32_t> { }, std::optional<int32_t> { 1 }, std::optional<int32_t> { 2 }, std::optional<int32_t> { 16 }, std::optional<int32_t> { -1 }, std::optional<int32_t> { -3 } }) {
                    for (bool isRotated : { false, true }) {
                        StridedLoopShape shape;
                        shape.relation = relation;
                        shape.step = isLessRelation(relation) ? stepSize : -stepSize;
                        shape.indexKind = indexKind;
                        shape.constantScale = constantScale;
                        shape.isRotated = isRotated;
                        StridedLoop loop(loopPolicy(), shape);
                        CHECK(versionLoopsAndValidate(loop.proc));
                        CHECK_EQ(loop.proc.report().countOf(Counter::RangeChecksRemoved), 1u);
                        checkValueCountInEachLoop(loop.proc, isRangeCheck, "range checks"_s, { 0, 1 });
                        checkRuns(loop);
                    }
                }
            }
        }
    }
}

static void testVersionLoopsRemovesStridedRangeChecksOfBottomTestedLoop()
{
    for (B3::Opcode relation : { LessThan, LessEqual, GreaterThan, GreaterEqual }) {
        for (IndexKind indexKind : { IndexKind::ZeroExtendedInt32, IndexKind::SignExtendedInt32, IndexKind::Int64 }) {
            for (std::optional<int32_t> constantScale : { std::optional<int32_t> { }, std::optional<int32_t> { 1 }, std::optional<int32_t> { 3 }, std::optional<int32_t> { -2 } }) {
                StridedLoopShape shape;
                shape.relation = relation;
                shape.step = isLessRelation(relation) ? 1 : -1;
                shape.indexKind = indexKind;
                shape.constantScale = constantScale;
                shape.isBottomTested = true;
                StridedLoop loop(loopPolicy(), shape);
                CHECK(versionLoopsAndValidate(loop.proc));
                CHECK_EQ(loop.proc.report().countOf(Counter::RangeChecksRemoved), 1u);
                checkRuns(loop);
            }
        }
    }
}

static void testVersionLoopsGuardsStridedRangeChecksAgainstCounterOverflow()
{
    for (IndexKind indexKind : { IndexKind::ZeroExtendedInt32, IndexKind::SignExtendedInt32, IndexKind::Int64 }) {
        for (bool increases : { true, false }) {
            for (bool isRotated : { false, true }) {
                StridedLoopShape shape;
                shape.relation = increases ? LessThan : GreaterThan;
                shape.step = increases ? 3 : -3;
                shape.indexKind = indexKind;
                shape.constantScale = increases ? 1 : -1;
                shape.isRotated = isRotated;
                StridedLoop loop(loopPolicy(), shape);
                CHECK(versionLoopsAndValidate(loop.proc));
                CHECK_EQ(loop.proc.report().countOf(Counter::RangeChecksRemoved), 1u);
                auto code = compileProc(loop.proc);
                int32_t start = increases ? maxInt32 - 7 : minInt32 + 7;
                for (int32_t distance : { 0, 1, 2, 3, 7 }) {
                    int32_t bound = increases ? maxInt32 - distance : minInt32 + distance;
                    StridedMemory memory(StridedMemory::numberOfElements, 0, increases ? -start : start);
                    ExpectedRun expected = expectedRun(shape, memory, start, bound);
                    CHECK(!expected.readsOutsideElements);
                    CHECK_EQ(expected.result == -1, !distance);
                    CHECK_EQ(run(*code, bound, reinterpret_cast<int64_t*>(&memory), start), expected.result);
                }
            }
        }
    }
}

static void testVersionLoopsRemovesStridedRangeChecksUnderUnsignedRelation()
{
    for (IndexKind indexKind : { IndexKind::ZeroExtendedInt32, IndexKind::SignExtendedInt32, IndexKind::Int64 }) {
        for (std::optional<int32_t> constantScale : { std::optional<int32_t> { }, std::optional<int32_t> { 4 }, std::optional<int32_t> { -2 } }) {
            StridedLoopShape shape;
            shape.relation = Below;
            shape.indexKind = indexKind;
            shape.constantScale = constantScale;
            StridedLoop loop(loopPolicy(), shape);
            versionLoopsAndValidate(loop.proc);
            bool isRemoved = indexKind != IndexKind::Int64;
            CHECK_EQ(loop.proc.report().countOf(Counter::RangeChecksRemoved), isRemoved ? 1u : 0u);
            checkRuns(loop);
        }
    }
}

static void testVersionLoopsSharesGuardsOfStridedNeighbors()
{
    for (IndexKind indexKind : { IndexKind::ZeroExtendedInt32, IndexKind::SignExtendedInt32, IndexKind::Int64 }) {
        for (std::optional<int32_t> constantScale : { std::optional<int32_t> { }, std::optional<int32_t> { 4 }, std::optional<int32_t> { -4 } }) {
            StridedLoopShape shape;
            shape.indexKind = indexKind;
            shape.constantScale = constantScale;
            shape.addends = { 2, -1, 0, 3 };
            StridedLoop loop(loopPolicy(), shape);
            loop.proc.resetValueOwners();
            auto plans = LoopVersionPlanner(loop.proc).plan(loop.proc.naturalLoops().loop(0));
            CHECK_EQ(plans.size(), 1u);
            const auto& checks = plans[0].rangeChecks.stridedChecks;
            CHECK_EQ(checks.size(), 1u);
            CHECK_EQ(checks[0].minOffset, -1);
            CHECK_EQ(checks[0].maxOffset, 3);
            CHECK_EQ(checks[0].isIncreasing, !constantScale || *constantScale > 0);
            CHECK_EQ(checks[0].needsScaleGuard, !constantScale);
            CHECK(versionLoopsAndValidate(loop.proc));
            checkValueCountInEachLoop(loop.proc, isRangeCheck, "range checks"_s, { 0, 4 });
            checkRuns(loop);
        }
    }
}

static void testVersionLoopsKeepsStridedRangeCheckWhoseLimitIsStoredInLoop()
{
    StridedLoopShape shape;
    shape.constantScale = 2;
    shape.lengthStoredOnRarePath = 9;
    StridedLoop loop(loopPolicy(), shape);
    versionLoopsAndValidate(loop.proc);
    CHECK_EQ(loop.proc.report().countOf(Counter::RangeChecksRemoved), 0u);
    checkRuns(loop);
}

static void testVersionLoopsLeavesStridedFastLoopWhenLimitIsClobbered()
{
    for (int64_t length : { 0, 9, 70 }) {
        for (std::optional<int32_t> constantScale : { std::optional<int32_t> { }, std::optional<int32_t> { 2 } }) {
            for (bool isRotated : { false, true }) {
                StridedLoopShape shape;
                shape.constantScale = constantScale;
                shape.isRotated = isRotated;
                shape.lengthClobberedOnRarePath = length;
                StridedLoop loop(loopPolicy(), shape);
                CHECK(versionLoopsAndValidate(loop.proc));
                CHECK_EQ(loop.proc.report().countOf(Counter::RangeChecksRemoved), 1u);
                checkRuns(loop);
            }
        }
    }
}

static void testVersionLoopsStrengthReducesStridedIndices()
{
    for (IndexKind indexKind : { IndexKind::ZeroExtendedInt32, IndexKind::SignExtendedInt32, IndexKind::Int64 }) {
        for (std::optional<int32_t> constantScale : { std::optional<int32_t> { }, std::optional<int32_t> { 3 }, std::optional<int32_t> { 16 }, std::optional<int32_t> { -1 } }) {
            for (bool isRotated : { false, true }) {
                for (bool isBottomTested : { false, true }) {
                    for (bool addsOnRarePath : { false, true }) {
                        for (bool continuesEarly : { false, true }) {
                            if (continuesEarly && (isRotated || isBottomTested))
                                continue;
                            StridedLoopShape shape;
                            shape.indexKind = indexKind;
                            shape.constantScale = constantScale;
                            shape.isRotated = isRotated;
                            shape.isBottomTested = isBottomTested;
                            shape.addsOnRarePath = addsOnRarePath;
                            shape.continuesEarly = continuesEarly;
                            shape.addends = { 0, 2 };
                            StridedLoop loop(loopPolicy(), shape);
                            CHECK(versionLoopsAndValidate(loop.proc));
                            CHECK_EQ(loop.proc.report().countOf(Counter::RangeChecksRemoved), 2u);
                            eliminateDeadCode(loop.proc);
                            unsigned numberOfProducts = continuesEarly ? 3 : 2;
                            checkValueCountInEachLoop(loop.proc, Mul, { addsOnRarePath ? numberOfProducts : 0u, numberOfProducts });
                            checkRuns(loop);
                        }
                    }
                }
            }
        }
    }
}

static void readLengthsFromViews(Procedure& proc)
{
    AbstractHeapRepository& heaps = proc.heaps();
    heaps.computeRangesAndDecorateInstructions();
    for (Value* value : proc.values()) {
        if (auto* load = value->as<MemoryValue>(); load && load->range() == firstHeap)
            load->setRange(heaps.rangeOf(heaps.JSArrayBufferView_length));
    }
}

static void testVersionLoopsStridedFastLoopWithinViewDoesNotPoll()
{
    Policy policy = loopPolicy();
    policy.loops.omitsPollsFromLoopsThatEndWithinData = true;
    for (bool limitIsLength : { false, true }) {
        for (std::optional<int32_t> constantScale : { std::optional<int32_t> { }, std::optional<int32_t> { 3 }, std::optional<int32_t> { -3 } }) {
            StridedLoopShape shape;
            shape.constantScale = constantScale;
            StridedLoop loop(policy, shape);
            if (limitIsLength)
                readLengthsFromViews(loop.proc);
            CHECK(versionLoopsAndValidate(loop.proc));
            CHECK_EQ(loop.proc.report().countOf(Counter::RangeChecksRemoved), 1u);
            checkValueCountInEachLoop(loop.proc, isLoopPoll, "polls"_s, { limitIsLength ? 0u : 1u, 1u });
            auto code = compileProc(loop.proc);
            for (int32_t scale : { 3, 1, 0, -3 }) {
                for (int32_t bound : { 0, 1, 10, 24, 1000 }) {
                    int32_t usedScale = constantScale.value_or(scale);
                    StridedMemory memory(70, scale, usedScale < 0 ? 69 : 0);
                    ExpectedRun expected = expectedRun(shape, memory, 0, bound);
                    loop.state = { };
                    CHECK_EQ(run(*code, bound, reinterpret_cast<int64_t*>(&memory), 0), expected.result);
                    bool takesFastLoop = expected.result != -1 && bound && (constantScale || scale > 0);
                    CHECK_EQ(loop.state.numberOfPolls, limitIsLength && takesFastLoop ? 0 : expected.numberOfTurns);
                }
            }
        }
    }
}

namespace {

struct AffineIndexTest {
    AffineIndexTest()
    {
        root = proc.addBlock();
        auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t>(proc, root).eager();
        counter = graph.make(root, Trunc, arguments[0]);
        width = graph.make(root, Trunc, arguments[1]);
        column = graph.make(root, Trunc, arguments[2]);
    }

    std::optional<AffineIndex> find(Value* value, IntegerInterval counterRange = { 0, 100 })
    {
        return affineIndexOf({ proc, counter, counterRange, [&](Value* candidate) -> Value* {
            return candidate == width || candidate == column ? candidate : nullptr;
        } }, value);
    }

    Value* make(B3::Opcode opcode, Value* left, Value* right) { return graph.make(root, opcode, left, right); }
    Value* int32(int32_t value) { return graph.int32(root, value); }

    static InvariantSum sum(int64_t constant, Vector<InvariantTerm, 2> terms = { })
    {
        return { WTF::move(terms), constant };
    }

    Procedure proc;
    Graph graph { proc };
    BasicBlock* root;
    Value* counter;
    Value* width;
    Value* column;
};

} // anonymous namespace

static void testAffineIndexOf()
{
    AffineIndexTest test;
    Value* counter = test.counter;
    Value* width = test.width;
    Value* column = test.column;

    auto index = test.find(test.make(Add, test.make(Shl, counter, test.int32(2)), test.int32(3)));
    CHECK(index && index->scale == AffineIndexTest::sum(4) && index->offset == AffineIndexTest::sum(3));

    index = test.find(test.make(Add, test.make(Mul, counter, width), column));
    CHECK(index && index->scale == AffineIndexTest::sum(0, { { 1, width, nullptr } }) && index->offset == AffineIndexTest::sum(0, { { 1, column, nullptr } }));

    index = test.find(test.make(Mul, test.make(Add, counter, test.int32(1)), width));
    CHECK(index && index->scale == AffineIndexTest::sum(0, { { 1, width, nullptr } }) && index->offset == AffineIndexTest::sum(0, { { 1, width, nullptr } }));

    index = test.find(test.make(Sub, test.make(Mul, test.make(Add, counter, column), width), test.make(Mul, column, width)));
    CHECK(index && index->scale == AffineIndexTest::sum(0, { { 1, width, nullptr } }) && index->offset == AffineIndexTest::sum(0));

    index = test.find(test.make(Sub, test.make(Sub, width, test.int32(1)), counter));
    CHECK(index && index->scale == AffineIndexTest::sum(-1) && index->offset == AffineIndexTest::sum(-1, { { 1, width, nullptr } }));

    index = test.find(test.make(Sub, counter, counter));
    CHECK(index && index->scale == AffineIndexTest::sum(0) && index->offset == AffineIndexTest::sum(0));

    CHECK(!test.find(test.make(Mul, counter, counter)));
    CHECK(!test.find(test.make(Mul, test.make(Mul, test.make(Mul, counter, width), column), width)));
    CHECK(!test.find(test.make(BitAnd, counter, test.int32(7))));
    CHECK(!test.find(test.make(Shl, counter, width)));

    Value* extended = test.graph.make(test.root, ZExt32, test.make(Sub, counter, test.int32(1)));
    CHECK(!test.find(test.make(Add, extended, test.graph.int64(test.root, 1))));
    CHECK(test.find(test.make(Add, extended, test.graph.int64(test.root, 1)), { 1, 100 }));
    extended = test.graph.make(test.root, SExt32, test.make(Mul, counter, width));
    CHECK(!test.find(extended));
    CHECK(test.find(extended, { 0, 1 }));
}

void addBunStridedRangeCheckTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testAffineIndexOf());
    RUN(testVersionLoopsRemovesStridedRangeChecks());
    RUN(testVersionLoopsRemovesStridedRangeChecksOfBottomTestedLoop());
    RUN(testVersionLoopsGuardsStridedRangeChecksAgainstCounterOverflow());
    RUN(testVersionLoopsRemovesStridedRangeChecksUnderUnsignedRelation());
    RUN(testVersionLoopsSharesGuardsOfStridedNeighbors());
    RUN(testVersionLoopsStrengthReducesStridedIndices());
    RUN(testVersionLoopsKeepsStridedRangeCheckWhoseLimitIsStoredInLoop());
    RUN(testVersionLoopsLeavesStridedFastLoopWhenLimitIsClobbered());
    RUN(testVersionLoopsStridedFastLoopWithinViewDoesNotPoll());
}

#endif // ENABLE(B3_JIT)
