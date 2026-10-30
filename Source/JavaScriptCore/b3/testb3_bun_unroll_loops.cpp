/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "B3EliminateDeadCode.h"
#include "B3EliminateRedundantPhiCycles.h"
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

static void testUnrollLoopsDropsBranchThatRepeatsTestAtTop()
{
    constexpr int64_t reported = -1000;
    struct Case {
        bool readsBoundAgain;
        bool storesBetweenReads;
        bool overlooksStore;
    };
    // The bound as the test has it, and read once more from where the test's bound was read.
    for (auto [readsBoundAgain, storesBetweenReads, overlooksStore] : { Case { false, false, false }, Case { true, false, false }, Case { true, true, false }, Case { true, true, true } }) {
        Policy policy = loopPolicy();
        policy.validation.logsMovedLoads = true;
        policy.validation.overlooksWritesBetweenReadsOfBound = overlooksStore;
        Procedure proc(policy);
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* header = proc.addBlock();
        BasicBlock* body = proc.addBlock();
        BasicBlock* stays = proc.addBlock();
        BasicBlock* out = proc.addBlock();
        BasicBlock* exit = proc.addBlock();
        Variable* index = proc.addVariable(Int64);
        Variable* sum = proc.addVariable(Int64);
        auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root).eager();
        Value* pointer = arguments[1];
        Value* bound = graph.load(root, Int64, pointer, 0, firstHeap);
        if (storesBetweenReads)
            graph.store(root, arguments[2], pointer, 0, firstHeap);
        Value* limit = readsBoundAgain ? graph.load(root, Int64, pointer, 0, firstHeap) : bound;
        graph.set(root, index, graph.int64(root, 0));
        graph.set(root, sum, graph.int64(root, 0));
        graph.jump(root, header);
        graph.branch(header, graph.make(header, LessThan, graph.get(header, index), bound), body, exit);
        graph.branch(body, graph.make(body, GreaterThan, limit, graph.get(body, index)), FrequentedBlock(stays), rarely(out));
        graph.increase(stays, sum, graph.get(stays, index));
        graph.increase(stays, sum, CountedSum::zeroReadAt(graph, stays, index));
        graph.increase(stays, index, 1);
        graph.jump(stays, header);
        graph.ret(out, graph.make(out, Sub, graph.int64(out, -1), graph.get(out, sum)));
        graph.ret(exit, graph.get(exit, sum));
        proc.resetReachability();
        fixSSA(proc);
        validate(proc);

        CHECK(unrollLoopsAndValidate(proc));
        if (storesBetweenReads && !overlooksStore)
            checkValueCountInEachLoop(proc, Branch, { 2, 1 + Limits::maxTurnsPerUnrolledLoop });
        else
            checkValueCountInEachLoop(proc, Branch, { 1, 2 });
        bool isChecked = proc.movedLoadLog().insertChecks(proc, createSharedTask<void(CCallHelpers&, const MovedLoad&, GPRReg, GPRReg)>([](CCallHelpers& jit, const MovedLoad&, GPRReg, GPRReg) {
            jit.move(CCallHelpers::TrustedImm64(reported), GPRInfo::returnValueGPR);
            jit.emitFunctionEpilogue();
            jit.ret();
        }));
        CHECK_EQ(isChecked, readsBoundAgain && (!storesBetweenReads || overlooksStore));
        auto code = compileProc(proc);
        for (int64_t turnsOfLoop = 0; turnsOfLoop < 40; ++turnsOfLoop) {
            for (int64_t stored : { 0, 5, 100 }) {
                int64_t word = turnsOfLoop;
                int64_t limitInRun = storesBetweenReads ? stored : turnsOfLoop;
                int64_t expected = limitInRun < turnsOfLoop ? -1 - sumBelow(limitInRun) : sumBelow(turnsOfLoop);
                CHECK_EQ(run(*code, 0, &word, stored), overlooksStore && stored != turnsOfLoop ? reported : expected);
            }
        }
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

enum class SidePath : uint8_t { EndsTurnItself, ComesBackInTurn };

static void checkLoopWithSidePath(TestPlace place, bool isRare, SidePath sidePath, bool counterHasRoom)
{
    constexpr int64_t fewestTurns = Limits::minTurnsPerUnrolledLoop;
    CountedSum sum(LessThan, 1, 0);
    auto& [proc, graph, loop, relation, step, firstIndex] = sum;
    PollState state;
    appendPoll(graph, loop.body, state);
    BasicBlock* side = proc.addBlock();
    BasicBlock* stays = proc.addBlock();
    BasicBlock* last = proc.addBlock();
    Value* isMarked = graph.make(loop.body, Equal, graph.make(loop.body, BitAnd, graph.get(loop.body, loop.index), loop.extra), graph.int64(loop.body, 0));
    graph.branch(loop.body, isMarked, isRare ? rarely(side) : unlikely(side), FrequentedBlock(stays));
    graph.increase(side, loop.sum, 1000);
    if (sidePath == SidePath::EndsTurnItself) {
        graph.increase(side, loop.index, 1);
        graph.jump(side, loop.header);
    } else
        graph.jump(side, last);
    graph.increase(stays, loop.sum, graph.get(stays, loop.index));
    graph.jump(stays, last);
    loop.endBody(last);
    if (counterHasRoom)
        proc.setKnownRange(loop.counter(), IntRange(0, 1 << 20));
    if (place == TestPlace::Bottom) {
        loop.rotate();
        eliminateRedundantPhiCycles(proc);
    }

    CHECK(unrollLoopsAndValidate(proc));
    auto code = compileProc(proc);
    for (int64_t mask : { 0ll, 1ll, 3ll, 7ll, 15ll, 63ll, -1ll }) {
        for (int64_t bound : { 0, 1, 7, 8, 9, 10, 16, 17, 18, 64, 67, 800 }) {
            int64_t expected = 0;
            for (int64_t index = 0; index < bound; ++index)
                expected += index & mask ? index : 1000;
            state = { };
            CHECK_EQ(run(*code, bound, nullptr, mask), expected);
            if (bound != 800)
                continue;
            // With this mask only the first turn takes the side path.
            bool takesSidePathOnce = mask == -1;
            // A loop that was turned round has a side path that ends its turn outside: it comes in again through the test in front of the loop, and polls there.
            if (place == TestPlace::Bottom && sidePath == SidePath::EndsTurnItself) {
                if (takesSidePathOnce)
                    CHECK(state.numberOfPolls <= bound / fewestTurns + fewestTurns);
            } else if (!isRare)
                CHECK(state.numberOfPolls <= bound / fewestTurns + fewestTurns);
            else if (!counterHasRoom)
                CHECK_EQ(state.numberOfPolls, bound);
            else if (takesSidePathOnce)
                CHECK(state.numberOfPolls <= 1 + bound / fewestTurns + fewestTurns);
        }
    }
}

// What lies behind an edge that is merely unlikely is written out with the turns: whoever leaves them ends in the loop itself and stays there.
// What lies behind a rare edge has one copy, behind which the turns are entered again where the counter is known not to wrap around.
// The polls tell how many turns ran one by one: the turns that are written out poll once for all.
static void testUnrollLoopsComesBackBehindSidePaths()
{
    for (TestPlace place : { TestPlace::Top, TestPlace::Bottom }) {
        for (bool isRare : { false, true }) {
            for (SidePath sidePath : { SidePath::EndsTurnItself, SidePath::ComesBackInTurn }) {
                for (bool counterHasRoom : { false, true })
                    checkLoopWithSidePath(place, isRare, sidePath, counterHasRoom);
            }
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

static void testUnrollLoopsReadsOnePlaceOnceATurn()
{
    Policy policy = loopPolicy();
    policy.optimization.redundancyElimination = RedundancyElimination::MemoryNumbering;
    for (bool storesBetween : { false, true }) {
        CountedSum sum(LessThan, 1, 0, policy, Walks::Nothing);
        auto& [proc, graph, loop, relation, step, firstIndex] = sum;
        auto address = [&] {
            return graph.make(loop.body, Add, loop.pointer, graph.make(loop.body, Shl, graph.get(loop.body, loop.index), graph.int32(loop.body, 2)));
        };
        auto read = [&] {
            return graph.make(loop.body, ZExt32, graph.load(loop.body, Int32, address(), 0, firstHeap));
        };
        graph.increase(loop.body, loop.sum, read());
        if (storesBetween)
            graph.store(loop.body, graph.int32(loop.body, 5), address(), 0, firstHeap);
        graph.increase(loop.body, loop.sum, read());
        sum.finish(loop.body, TestPlace::Bottom);

        CHECK(unrollLoopsAndValidate(proc));
        proc.setOptLevel(2);
        reduceStrength(proc);
        eliminateDeadCode(proc);
        CHECK_EQ(numberOfValues(proc, Load), 1 + (storesBetween ? Limits::minTurnsPerUnrolledLoop : Limits::maxTurnsPerUnrolledLoop));
        auto code = compileProc(proc);
        for (int64_t bound = 1; bound <= 40; ++bound) {
            int32_t words[40];
            int64_t expected = 0;
            for (int32_t index = 0; index < 40; ++index) {
                words[index] = 100 + index;
                if (index < bound)
                    expected += words[index] + (storesBetween ? 5 : words[index]);
            }
            CHECK_EQ(run(*code, bound, reinterpret_cast<int64_t*>(words)), expected);
        }
    }
}

// A way that is seldom taken tests what the turn has read, stores, and comes back in front of the turn's second read of the same place.
static void testUnrollLoopsReadsAgainBehindSidePathThatStores()
{
    constexpr int64_t numberOfWords = 40;
    for (TestPlace place : { TestPlace::Top, TestPlace::Bottom }) {
        for (bool isRare : { false, true }) {
            for (Type type : { Int32, Int64 }) {
                for (bool storesThere : { false, true }) {
                    CountedSum sum(LessThan, 1, 0, loopPolicy(), Walks::Nothing);
                    auto& [proc, graph, loop, relation, step, firstIndex] = sum;
                    BasicBlock* side = proc.addBlock();
                    BasicBlock* last = proc.addBlock();
                    auto address = [&](BasicBlock* block) {
                        return graph.make(block, Add, loop.pointer, graph.make(block, Shl, graph.get(block, loop.index), graph.int32(block, 3)));
                    };
                    auto read = [&](BasicBlock* block) {
                        Value* word = graph.load(block, type, address(block), 0, firstHeap);
                        return type == Int32 ? graph.make(block, ZExt32, word) : word;
                    };
                    graph.branch(loop.body, graph.make(loop.body, Equal, read(loop.body), loop.extra), isRare ? rarely(side) : unlikely(side), FrequentedBlock(last));
                    Value* stored = type == Int32 ? graph.int32(side, 3) : graph.int64(side, 3);
                    if (storesThere)
                        graph.store(side, stored, address(side), 0, firstHeap);
                    else
                        graph.store(side, stored, address(side), numberOfWords * 8, secondHeap);
                    graph.jump(side, last);
                    graph.increase(last, loop.sum, read(last));
                    sum.finish(last, place);

                    CHECK(unrollLoopsAndValidate(proc));
                    auto code = compileProc(proc);
                    for (int64_t bound = 0; bound <= numberOfWords; ++bound) {
                        for (int64_t markedEvery : { 1, 2, 3, 7, 100 }) {
                            int64_t words[2 * numberOfWords] = { };
                            int64_t expected = 0;
                            for (int64_t index = 0; index < numberOfWords; ++index) {
                                bool isMarked = !((index + 1) % markedEvery);
                                words[index] = isMarked ? 100 : index;
                                if (index < bound)
                                    expected += isMarked && storesThere ? 3 : words[index];
                            }
                            CHECK_EQ(run(*code, bound, words, 100), expected);
                            for (int64_t index = 0; index < numberOfWords; ++index) {
                                bool wasStored = index < bound && !((index + 1) % markedEvery);
                                CHECK_EQ(words[index], wasStored && storesThere ? 3 : (index + 1) % markedEvery ? index : 100);
                                CHECK_EQ(words[numberOfWords + index], wasStored && !storesThere ? 3 : 0);
                            }
                        }
                    }
                }
            }
        }
    }
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

// A loop that walks a list of words and reads them is written out. One that stores words is not, nor one that picks among a few of them.
static void testUnrollLoopsSkipsLoopThatStoresWordsOrPicksAmongThem()
{
    enum class Access : uint8_t { LoadAtCounter, LoadAtLowBitsOfCounter, Store };
    for (Access access : { Access::LoadAtCounter, Access::LoadAtLowBitsOfCounter, Access::Store }) {
        CountedSum sum(LessThan, 1, 0);
        auto& [proc, graph, loop, relation, step, firstIndex] = sum;
        Value* index = graph.get(loop.body, loop.index);
        if (access == Access::LoadAtLowBitsOfCounter)
            index = graph.make(loop.body, BitAnd, index, graph.int64(loop.body, 1));
        Value* address = graph.make(loop.body, Add, loop.pointer, graph.make(loop.body, Shl, index, graph.int32(loop.body, 3)));
        if (access == Access::Store)
            graph.store(loop.body, loop.extra, address, 0, firstHeap);
        else
            graph.increase(loop.body, loop.sum, graph.load(loop.body, Int64, address, 0, firstHeap));
        sum.finish(loop.body, TestPlace::Bottom);

        CHECK_EQ(unrollLoopsAndValidate(proc), access == Access::LoadAtCounter);
        int64_t words[20];
        std::ranges::fill(words, 3);
        CHECK_EQ(compileAndRun<int64_t>(proc, static_cast<int64_t>(20), words, static_cast<int64_t>(5)), access == Access::Store ? 0 : 60);
        CHECK_EQ(words[19], access == Access::Store ? 5 : 3);
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

static Policy policyThatPollsBetweenStretches()
{
    Policy policy = loopPolicy();
    policy.loops.pollsBetweenStretches = true;
    return policy;
}

static PatchpointValue* appendPollOfShortTurns(Graph& graph, BasicBlock* block, PollState& state)
{
    PatchpointValue* poll = appendPoll(graph, block, state);
    poll->iterationsAreShort = true;
    return poll;
}

// No more turns than the limit pass between two polls, but for the turns that are written out once more and those that are left behind them. A loop of fewer turns polls nowhere.
static void checkPollsOfTurns(int64_t numberOfPolls, int64_t numberOfTurns)
{
    constexpr int64_t most = Limits::maxTurnsWithoutPoll + 2 * Limits::maxTurnsPerUnrolledLoop;
    CHECK((numberOfPolls + 1) * most >= numberOfTurns);
    CHECK(numberOfPolls <= numberOfTurns / Limits::maxTurnsWithoutPoll);
}

static void testUnrollLoopsPollsBetweenStretches()
{
    constexpr int64_t stretch = Limits::maxTurnsWithoutPoll;
    struct Direction {
        B3::Opcode relation;
        int64_t step;
    };
    for (TestPlace place : { TestPlace::Top, TestPlace::Bottom }) {
        for (Direction direction : { Direction { LessThan, 1 }, Direction { LessEqual, 3 }, Direction { GreaterThan, -1 }, Direction { Below, 2 } }) {
            for (int64_t first : { int64_t { 0 }, int64_t { 5 }, std::numeric_limits<int64_t>::max() - 4 * stretch * 3 }) {
                CountedSum sum(direction.relation, direction.step, first, policyThatPollsBetweenStretches());
                PollState state;
                appendPollOfShortTurns(sum.graph, sum.loop.body, state);
                sum.graph.increase(sum.loop.body, sum.loop.sum, sum.graph.get(sum.loop.body, sum.loop.index));
                sum.finish(sum.loop.body, place);

                CHECK(unrollLoopsAndValidate(sum.proc));
                CHECK_EQ(sum.proc.report().countOf(Counter::LoopsUnrolled), 1u);
                CHECK_EQ(sum.proc.report().countOf(Counter::LoopPollsRemoved), 1u);
                CHECK_EQ(numberOfValues(sum.proc, Patchpoint), 1u);
                auto code = compileProc(sum.proc);
                for (int64_t turns : { int64_t { 0 }, int64_t { 1 }, int64_t { 7 }, int64_t { 8 }, int64_t { 9 }, int64_t { 67 }, stretch - 1, stretch, stretch + 1, stretch + 9, 3 * stretch + 5 }) {
                    int64_t bound = first + turns * direction.step;
                    if (direction.step < 0 && first < turns && isUnsignedRelation(direction.relation))
                        continue;
                    state = { };
                    CHECK_EQ(run(*code, bound), sum.expected(bound, itself));
                    checkPollsOfTurns(state.numberOfPolls, sum.expected(bound, one));
                }
            }
        }
    }
}

// A loop that walks nothing is not written out. It goes without its poll where its turns are few, however far from zero they start, and a copy of it that polls between stretches takes the others.
// Where the lower end is not below zero, an upper end within a stretch of zero says that the turns are few. An upper end beyond that is held against the lower end,
// but for a lower end that is known to lie within a stretch of zero: whoever starts there and ends beyond has many turns or nearly so.
static void testUnrollLoopsLeavesPollToCopyOfLoopThatIsNotWrittenOut()
{
    constexpr int64_t stretch = Limits::maxTurnsWithoutPoll;
    constexpr int64_t greatest = std::numeric_limits<int64_t>::max();
    for (TestPlace place : { TestPlace::Top, TestPlace::Bottom }) {
        for (int64_t step : { 1, -2 }) {
            for (int64_t first : { int64_t { 3 }, -3 * stretch, stretch + 1, 7 * stretch + 3, greatest - 7 * stretch }) {
                for (bool boundIsNotBelowZero : { false, true }) {
                    // A counter that falls from so near to zero needs no copy.
                    if (step < 0 && boundIsNotBelowZero && first <= stretch * -step)
                        continue;
                    CountedSum sum(step > 0 ? LessThan : GreaterThan, step, first, policyThatPollsBetweenStretches(), Walks::Nothing);
                    PollState state;
                    appendPollOfShortTurns(sum.graph, sum.loop.body, state);
                    sum.graph.increase(sum.loop.body, sum.loop.sum, sum.graph.get(sum.loop.body, sum.loop.index));
                    sum.finish(sum.loop.body, place);
                    if (boundIsNotBelowZero)
                        sum.proc.setKnownRange(sum.loop.bound, IntRange(0, greatest));

                    CHECK(unrollLoopsAndValidate(sum.proc));
                    CHECK_EQ(sum.proc.report().countOf(Counter::LoopsUnrolled), 0u);
                    CHECK_EQ(sum.proc.report().countOf(Counter::LoopPollsRemoved), 1u);
                    checkValueCountInEachLoop(sum.proc, isLoopPoll, "polls"_s, { 0, 1 });
                    auto code = compileProc(sum.proc);
                    for (int64_t turns : { int64_t { -5 }, int64_t { 0 }, int64_t { 1 }, int64_t { 9 }, stretch - 4, stretch - 3, stretch - 2, stretch - 1, stretch, stretch + 1, 2 * stretch + 5 }) {
                        int64_t bound = first + turns * step;
                        if (boundIsNotBelowZero && bound < 0)
                            continue;
                        int64_t numberOfTurns = sum.expected(bound, one);
                        state = { };
                        CHECK_EQ(run(*code, bound), sum.expected(bound, itself));
                        bool startsNearZero = step > 0 && first >= 0 && first <= stretch * step;
                        bool hasFewTurns = startsNearZero ? bound <= stretch * step : turns <= stretch;
                        if (turns >= 0)
                            CHECK_EQ(state.numberOfPolls, hasFewTurns ? 0 : (numberOfTurns - 1) / stretch);
                    }
                }
            }
        }
    }
}

static void testUnrollLoopsRunsCopyOfLongTurnInStretches()
{
    constexpr int64_t stretch = Limits::maxTurnsWithoutPoll;
    constexpr int64_t numberOfSteps = 40;
    enum class Bound : uint8_t { IsReadByTestAlone, IsReadInTurn, IsReadBehindLoop };
    for (Bound reader : { Bound::IsReadByTestAlone, Bound::IsReadInTurn, Bound::IsReadBehindLoop }) {
        for (int64_t step : { 1, -3 }) {
            Procedure proc(policyThatPollsBetweenStretches());
            Graph graph { proc };
            PollState state;
            BasicBlock* root = proc.addBlock();
            BasicBlock* body = proc.addBlock();
            BasicBlock* exit = proc.addBlock();
            Variable* index = proc.addVariable(Int64);
            Variable* sum = proc.addVariable(Int64);
            auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root).eager();
            graph.set(root, index, arguments[0]);
            graph.set(root, sum, graph.int64(root, 0));
            graph.jump(root, body);
            appendPollOfShortTurns(graph, body, state);
            for (int64_t i = 0; i < numberOfSteps; ++i)
                graph.set(body, sum, graph.make(body, BitXor, graph.make(body, Add, graph.get(body, sum), graph.get(body, index)), graph.int64(body, 3 * i + 1)));
            if (reader == Bound::IsReadInTurn)
                graph.increase(body, sum, arguments[1]);
            graph.increase(body, index, step);
            graph.branch(body, graph.make(body, step > 0 ? LessThan : GreaterThan, graph.get(body, index), arguments[1]), body, exit);
            graph.ret(exit, reader == Bound::IsReadBehindLoop ? graph.make(exit, Add, graph.get(exit, sum), arguments[1]) : graph.get(exit, sum));
            proc.resetReachability();
            fixSSA(proc);

            CHECK(unrollLoopsAndValidate(proc));
            CHECK_EQ(proc.report().countOf(Counter::LoopsUnrolled), 0u);
            CHECK_EQ(proc.report().countOf(Counter::LoopPollsRemoved), 1u);
            auto code = compileProc(proc);
            for (int64_t first : { int64_t { 0 }, int64_t { -7 }, 5 * stretch + 1 }) {
                for (int64_t turns : { int64_t { 1 }, int64_t { 9 }, stretch, stretch + 1, 2 * stretch, 3 * stretch + 5 }) {
                    int64_t bound = first + turns * step;
                    uint64_t expected = 0;
                    for (int64_t at = first, turn = 0; turn < turns; ++turn, at += step) {
                        for (int64_t i = 0; i < numberOfSteps; ++i)
                            expected = (expected + static_cast<uint64_t>(at)) ^ static_cast<uint64_t>(3 * i + 1);
                        if (reader == Bound::IsReadInTurn)
                            expected += static_cast<uint64_t>(bound);
                    }
                    if (reader == Bound::IsReadBehindLoop)
                        expected += static_cast<uint64_t>(bound);
                    state = { };
                    CHECK_EQ(invoke<int64_t>(*code, first, bound), static_cast<int64_t>(expected));
                    if (isX86())
                        CHECK_EQ(state.numberOfPolls, (turns - 1) / stretch);
                }
            }
        }
    }
}

// The cloner hands such a comparison to every reader through a variable, its own branch too, and a Get has no operands to look at. The copy polls on every turn.
static void testUnrollLoopsLeavesPollToCopyOfLoopWhoseComparisonCrossesBlocks()
{
    constexpr int64_t stretch = Limits::maxTurnsWithoutPoll;
    enum class Comparison : uint8_t { StandsInFrontOfBlockOfBranch, IsReadInBodyToo };
    for (Comparison comparison : { Comparison::StandsInFrontOfBlockOfBranch, Comparison::IsReadInBodyToo }) {
        for (TestPlace place : { TestPlace::Top, TestPlace::Bottom }) {
            if (comparison == Comparison::IsReadInBodyToo && place == TestPlace::Bottom)
                continue;
            for (int64_t step : { 1, -2 }) {
                int64_t first = 7 * stretch + 3;
                CountedSum sum(step > 0 ? LessThan : GreaterThan, step, first, policyThatPollsBetweenStretches(), Walks::Nothing);
                PollState state;
                appendPollOfShortTurns(sum.graph, sum.loop.body, state);
                sum.graph.increase(sum.loop.body, sum.loop.sum, sum.graph.get(sum.loop.body, sum.loop.index));
                if (comparison == Comparison::IsReadInBodyToo)
                    sum.graph.increase(sum.loop.body, sum.loop.sum, sum.graph.make(sum.loop.body, ZExt32, sum.loop.header->last()->child(0)));
                sum.finish(sum.loop.body, place);

                if (comparison == Comparison::StandsInFrontOfBlockOfBranch) {
                    unsigned numberOfBranchesMoved = 0;
                    Vector<BasicBlock*> blocks;
                    for (BasicBlock* block : sum.proc)
                        blocks.append(block);
                    for (BasicBlock* block : blocks) {
                        if (block->last()->opcode() != Branch || !sum.proc.naturalLoops().innerMostLoopOf(block))
                            continue;
                        BasicBlock* branches = sum.proc.addBlock(block->frequency());
                        FrequentedBlock taken = block->taken();
                        FrequentedBlock notTaken = block->notTaken();
                        Value* branch = block->values().takeLast();
                        branches->append(branch);
                        branches->setSuccessors(taken, notTaken);
                        block->appendNew<Value>(sum.proc, Jump, branch->origin());
                        block->setSuccessors(FrequentedBlock(branches));
                        ++numberOfBranchesMoved;
                    }
                    CHECK_EQ(numberOfBranchesMoved, 1u);
                    sum.proc.resetReachability();
                    sum.proc.invalidateCFG();
                    sum.proc.resetValueOwners();
                    validate(sum.proc);
                }

                CHECK(unrollLoopsAndValidate(sum.proc));
                CHECK_EQ(sum.proc.report().countOf(Counter::LoopsUnrolled), 0u);
                CHECK_EQ(sum.proc.report().countOf(Counter::LoopPollsRemoved), 1u);
                checkValueCountInEachLoop(sum.proc, isLoopPoll, "polls"_s, { 0, 1 });
                auto code = compileProc(sum.proc);
                for (int64_t turns : { int64_t { -5 }, int64_t { 0 }, int64_t { 1 }, int64_t { 9 }, stretch - 1, stretch, stretch + 1, 2 * stretch + 5 }) {
                    int64_t bound = first + turns * step;
                    int64_t numberOfTurns = sum.expected(bound, one);
                    state = { };
                    CHECK_EQ(run(*code, bound), sum.expected(bound, itself) + (comparison == Comparison::IsReadInBodyToo ? numberOfTurns : 0));
                    CHECK_EQ(state.numberOfPolls, turns <= stretch ? 0 : numberOfTurns);
                }
            }
        }
    }
}

static void testUnrollLoopsDropsPollOfLoopWhoseTurnsAreKnownToBeFew()
{
    for (Walks walks : { Walks::Nothing, Walks::Data }) {
        for (int64_t greatestBound : { Limits::maxTurnsWithoutPoll, Limits::maxTurnsWithoutPoll + 1 }) {
            CountedSum sum(LessThan, 1, 0, policyThatPollsBetweenStretches(), walks);
            PollState state;
            appendPollOfShortTurns(sum.graph, sum.loop.body, state);
            sum.graph.increase(sum.loop.body, sum.loop.sum, sum.graph.get(sum.loop.body, sum.loop.index));
            sum.finish(sum.loop.body, TestPlace::Bottom);
            sum.proc.setKnownRange(sum.loop.bound, IntRange(0, greatestBound));

            CHECK(unrollLoopsAndValidate(sum.proc));
            CHECK_EQ(numberOfValues(sum.proc, Patchpoint), greatestBound == Limits::maxTurnsWithoutPoll ? 0u : 1u);
            auto code = compileProc(sum.proc);
            for (int64_t bound : { 0, 1, 9, 1000 }) {
                CHECK_EQ(run(*code, bound), sumBelow(bound));
                CHECK_EQ(state.numberOfPolls, 0);
            }
        }
    }
}

// The loop keeps polling on every iteration when its iterations may be long or the policy does not allow polling between stretches.
static void testUnrollLoopsKeepsPollThatMustRunOnEveryTurn()
{
    enum class Reason : uint8_t { IterationsMayBeLong, Policy };
    for (Reason reason : { Reason::IterationsMayBeLong, Reason::Policy }) {
        for (Walks walks : { Walks::Nothing, Walks::Data }) {
            CountedSum sum(LessThan, 1, 0, reason == Reason::Policy ? loopPolicy() : policyThatPollsBetweenStretches(), walks);
            PollState state;
            PatchpointValue* poll = appendPoll(sum.graph, sum.loop.body, state);
            poll->iterationsAreShort = reason != Reason::IterationsMayBeLong;
            sum.graph.increase(sum.loop.body, sum.loop.sum, sum.graph.get(sum.loop.body, sum.loop.index));
            sum.finish(sum.loop.body, TestPlace::Bottom);

            CHECK_EQ(unrollLoopsAndValidate(sum.proc), walks == Walks::Data);
            CHECK_EQ(sum.proc.report().countOf(Counter::LoopPollsRemoved), 0u);
            auto code = compileProc(sum.proc);
            CHECK_EQ(run(*code, 67), sumBelow(67));
            CHECK_EQ(state.numberOfPolls, walks == Walks::Data ? 67 / Limits::maxTurnsPerUnrolledLoop + 67 % Limits::maxTurnsPerUnrolledLoop : 67);
        }
    }
}

// What lies behind a rare edge has a copy of its own behind the turns that are written out, which asks again whether there is room for them. So the loop itself never gets more turns
// than are written out and goes without its poll. The turns that are written out poll once together.
static void testUnrollLoopsTakesPollOutOfLoopItselfWhereRarePathsComeBack()
{
    constexpr int64_t stretch = Limits::maxTurnsWithoutPoll;
    for (TestPlace place : { TestPlace::Top, TestPlace::Bottom }) {
        for (SidePath sidePath : { SidePath::EndsTurnItself, SidePath::ComesBackInTurn }) {
            // A loop that was turned round has a side path that ends its turn outside.
            if (place == TestPlace::Bottom && sidePath == SidePath::EndsTurnItself)
                continue;
            CountedSum sum(LessThan, 1, 0, policyThatPollsBetweenStretches());
            auto& [proc, graph, loop, relation, step, firstIndex] = sum;
            PollState state;
            appendPollOfShortTurns(graph, loop.body, state);
            BasicBlock* side = proc.addBlock();
            BasicBlock* stays = proc.addBlock();
            BasicBlock* last = proc.addBlock();
            Value* isMarked = graph.make(loop.body, Equal, graph.make(loop.body, BitAnd, graph.get(loop.body, loop.index), loop.extra), graph.int64(loop.body, 0));
            graph.branch(loop.body, isMarked, rarely(side), FrequentedBlock(stays));
            graph.increase(side, loop.sum, 1000);
            if (sidePath == SidePath::EndsTurnItself) {
                graph.increase(side, loop.index, 1);
                graph.jump(side, loop.header);
            } else
                graph.jump(side, last);
            graph.increase(stays, loop.sum, graph.get(stays, loop.index));
            graph.jump(stays, last);
            loop.endBody(last);
            proc.setKnownRange(loop.counter(), IntRange(0, 1 << 28));
            if (place == TestPlace::Bottom)
                loop.rotate();

            CHECK(unrollLoopsAndValidate(proc));
            CHECK(proc.report().countOf(Counter::LoopPollsRemoved));
            auto code = compileProc(proc);
            // With this mask only the first turn takes the side path.
            int64_t turns = 3 * stretch + 5;
            CHECK_EQ(run(*code, turns, nullptr, -1), sumBelow(turns) + 1000);
            CHECK(state.numberOfPolls >= turns / static_cast<int64_t>(Limits::maxTurnsPerUnrolledLoop) - 2);
            CHECK(state.numberOfPolls <= turns / static_cast<int64_t>(Limits::minTurnsPerUnrolledLoop) + 2);
            // Too few turns for those that are written out.
            state = { };
            CHECK_EQ(run(*code, 3, nullptr, -1), sumBelow(3) + 1000);
            CHECK_EQ(state.numberOfPolls, 0);
            // With this one every turn does.
            state = { };
            CHECK_EQ(run(*code, 800, nullptr, 0), 800 * 1000);
            CHECK(state.numberOfPolls >= 800 - 2 * static_cast<int64_t>(Limits::maxTurnsPerUnrolledLoop));
        }
    }
}

// A turn that leaves by a rare edge and has no such copy may be every turn. Such a loop keeps its poll.
static void testUnrollLoopsKeepsPollOfLoopThatComesBackFromBehindRareEdge()
{
    for (Walks walks : { Walks::Nothing, Walks::Data }) {
        CountedSum sum(LessThan, 1, 0, policyThatPollsBetweenStretches(), walks);
        auto& [proc, graph, loop, relation, step, firstIndex] = sum;
        PollState state;
        appendPollOfShortTurns(graph, loop.body, state);
        BasicBlock* rare = proc.addBlock();
        BasicBlock* stays = proc.addBlock();
        graph.branch(loop.body, graph.make(loop.body, Equal, graph.get(loop.body, loop.index), loop.extra), rarely(rare), FrequentedBlock(stays));
        graph.increase(rare, loop.sum, 1000);
        graph.increase(rare, loop.index, 1);
        graph.jump(rare, loop.header);
        graph.increase(stays, loop.sum, graph.get(stays, loop.index));
        sum.finish(stays, TestPlace::Top);

        unrollLoopsAndValidate(proc);
        CHECK_EQ(proc.report().countOf(Counter::LoopPollsRemoved), 0u);
        auto code = compileProc(proc);
        CHECK_EQ(run(*code, 40, nullptr, 11), sumBelow(40) - 11 + 1000);
        CHECK(state.numberOfPolls >= 40 / static_cast<int64_t>(Limits::maxTurnsPerUnrolledLoop));
    }
}

// Two counters that meet take the steps of both towards each other.
static void testUnrollLoopsLeavesPollToCopyOfLoopOfCountersThatMeet()
{
    constexpr int64_t stretch = Limits::maxTurnsWithoutPoll;
    Procedure proc(policyThatPollsBetweenStretches());
    Graph graph { proc };
    PollState state;
    BasicBlock* root = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* low = proc.addVariable(Int64);
    Variable* high = proc.addVariable(Int64);
    Variable* sum = proc.addVariable(Int64);
    auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root).eager();
    graph.set(root, low, arguments[0]);
    graph.set(root, high, arguments[1]);
    graph.set(root, sum, graph.int64(root, 0));
    graph.branch(root, graph.make(root, LessThan, arguments[0], arguments[1]), body, exit);
    appendPollOfShortTurns(graph, body, state);
    graph.increase(body, sum, 1);
    graph.increase(body, low, 1);
    graph.increase(body, high, -1);
    graph.branch(body, graph.make(body, LessThan, graph.get(body, low), graph.get(body, high)), body, exit);
    graph.ret(exit, graph.get(exit, sum));
    proc.resetReachability();
    fixSSA(proc);

    CHECK(unrollLoopsAndValidate(proc));
    CHECK_EQ(proc.report().countOf(Counter::LoopPollsRemoved), 1u);
    checkValueCountInEachLoop(proc, isLoopPoll, "polls"_s, { 0, 1 });
    auto code = compileProc(proc);
    for (int64_t distance : { int64_t { 0 }, int64_t { 1 }, int64_t { 2 }, int64_t { 9 }, 2 * stretch - 1, 2 * stretch, 2 * stretch + 1, 4 * stretch }) {
        int64_t turns = (distance + 1) / 2;
        state = { };
        CHECK_EQ(invoke<int64_t>(*code, int64_t { -7 }, -7 + distance), turns);
        CHECK_EQ(state.numberOfPolls, distance <= 2 * stretch ? 0 : turns);
    }
}

// The number that ends a loop may be a constant that stands in the loop. What is asked in front of the loop has a constant of its own.
static void testUnrollLoopsAsksInFrontOfLoopAboutBoundThatStandsInLoop()
{
    constexpr int64_t stretch = Limits::maxTurnsWithoutPoll;
    constexpr int64_t bound = 3 * stretch;
    Procedure proc(policyThatPollsBetweenStretches());
    Graph graph { proc };
    PollState state;
    BasicBlock* root = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* index = proc.addVariable(Int64);
    Variable* sum = proc.addVariable(Int64);
    auto arguments = cCallArgumentValues<int64_t>(proc, root).eager();
    graph.set(root, index, arguments[0]);
    graph.set(root, sum, graph.int64(root, 0));
    graph.jump(root, body);
    appendPollOfShortTurns(graph, body, state);
    graph.increase(body, sum, 1);
    graph.increase(body, index, 1);
    graph.branch(body, graph.make(body, LessThan, graph.get(body, index), graph.int64(body, bound)), body, exit);
    graph.ret(exit, graph.get(exit, sum));
    proc.resetReachability();
    fixSSA(proc);

    CHECK(unrollLoopsAndValidate(proc));
    CHECK_EQ(proc.report().countOf(Counter::LoopPollsRemoved), 1u);
    checkValueCountInEachLoop(proc, isLoopPoll, "polls"_s, { 0, 1 });
    auto code = compileProc(proc);
    for (int64_t turns : { int64_t { 1 }, int64_t { 9 }, stretch, stretch + 1, 3 * stretch }) {
        state = { };
        CHECK_EQ(invoke<int64_t>(*code, bound - turns), turns);
        CHECK_EQ(state.numberOfPolls, (turns - 1) / stretch);
    }
}

// Two loops in a row leave their polls to copies. The second goes on from the counter of the first, whichever of the first loop and its copy has run.
static void testUnrollLoopsAsksInFrontOfLoopAboutCounterOfCopiedLoop()
{
    constexpr int64_t stretch = Limits::maxTurnsWithoutPoll;
    Procedure proc(policyThatPollsBetweenStretches());
    Graph graph { proc };
    PollState state;
    BasicBlock* root = proc.addBlock();
    BasicBlock* first = proc.addBlock();
    BasicBlock* between = proc.addBlock();
    BasicBlock* second = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* index = proc.addVariable(Int64);
    Variable* sum = proc.addVariable(Int64);
    auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root).eager();
    graph.set(root, index, graph.int64(root, 0));
    graph.set(root, sum, graph.int64(root, 0));
    graph.jump(root, first);
    auto appendTurn = [&](BasicBlock* body, int64_t summand, Value* bound, BasicBlock* behind) {
        appendPollOfShortTurns(graph, body, state);
        graph.increase(body, sum, summand);
        graph.increase(body, index, 1);
        graph.branch(body, graph.make(body, LessThan, graph.get(body, index), bound), body, behind);
    };
    appendTurn(first, 1, arguments[0], between);
    graph.jump(between, second);
    appendTurn(second, 3, arguments[1], exit);
    graph.ret(exit, graph.get(exit, sum));
    proc.resetReachability();
    fixSSA(proc);

    CHECK(unrollLoopsAndValidate(proc));
    CHECK_EQ(proc.report().countOf(Counter::LoopPollsRemoved), 2u);
    auto code = compileProc(proc);
    auto pollsOf = [](int64_t turns) {
        return (turns - 1) / stretch;
    };
    for (int64_t firstTurns : { int64_t { 1 }, int64_t { 9 }, stretch, stretch + 1 }) {
        for (int64_t secondTurns : { int64_t { 1 }, int64_t { 9 }, stretch, stretch + 1 }) {
            state = { };
            CHECK_EQ(invoke<int64_t>(*code, firstTurns, firstTurns + secondTurns), firstTurns + 3 * secondTurns);
            CHECK_EQ(state.numberOfPolls, pollsOf(firstTurns) + pollsOf(secondTurns));
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
        for (Value* value : proc.values()) {
            if (value->type() == Int64 && value->opcode() == Phi)
                proc.setKnownRange(value, IntRange(0, 1ll << 40));
        }
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
        // The turns that are written out, the loop itself, and the loop of stretches around the turns, which makes a double of the integer behind each.
        checkValueCountInEachLoop(loop.proc, IToD, { 0, 1, 1 });
        checkValueCountInEachLoop(loop.proc, SExt32, { 0, 8, 8 });
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
    // Small words go through several stretches as integers.
    for (int32_t word : { std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::min() + 1, 3 }) {
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
    RUN(testUnrollLoopsDropsBranchThatRepeatsTestAtTop());
    RUN(testUnrollLoopsKeepsOtherWaysOut());
    RUN(testUnrollLoopsLeavesRareBlocksToLoopItself());
    RUN(testUnrollLoopsComesBackBehindSidePaths());
    RUN(testUnrollLoopsSkipsLoopWithCall());
    RUN(testUnrollLoopsSkipsLoopThatWalksNothing());
    RUN(testUnrollLoopsReadsOnePlaceOnceATurn());
    RUN(testUnrollLoopsReadsAgainBehindSidePathThatStores());
    RUN(testUnrollLoopsSkipsLoopWithLikelySecondWayOut());
    RUN(testUnrollLoopsStoresInOrder());
    RUN(testUnrollLoopsSkipsLoopThatStoresWordsOrPicksAmongThem());
    RUN(testUnrollLoopsPollsOnceForAllTurns());
    RUN(testUnrollLoopsPollsBetweenStretches());
    RUN(testUnrollLoopsLeavesPollToCopyOfLoopThatIsNotWrittenOut());
    RUN(testUnrollLoopsRunsCopyOfLongTurnInStretches());
    RUN(testUnrollLoopsLeavesPollToCopyOfLoopWhoseComparisonCrossesBlocks());
    RUN(testUnrollLoopsDropsPollOfLoopWhoseTurnsAreKnownToBeFew());
    RUN(testUnrollLoopsKeepsPollThatMustRunOnEveryTurn());
    RUN(testUnrollLoopsTakesPollOutOfLoopItselfWhereRarePathsComeBack());
    RUN(testUnrollLoopsKeepsPollOfLoopThatComesBackFromBehindRareEdge());
    RUN(testUnrollLoopsLeavesPollToCopyOfLoopOfCountersThatMeet());
    RUN(testUnrollLoopsAsksInFrontOfLoopAboutBoundThatStandsInLoop());
    RUN(testUnrollLoopsAsksInFrontOfLoopAboutCounterOfCopiedLoop());
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
