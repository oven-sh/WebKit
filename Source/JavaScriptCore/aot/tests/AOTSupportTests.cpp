/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCompileOptionWords.h"
#include "AOTCompileOptions.h"
#include "AOTCurrentWork.h"
#include "AOTDiagnostics.h"
#include "AOTDriver.h"
#include "AOTIR.h"
#include "AOTIRParser.h"
#include "AOTLimits.h"
#include "AOTNativeTest.h"
#include "AOTOptionWords.h"
#include "AOTPhaseTimes.h"
#include "AOTPlan.h"
#include "AOTProgramFile.h"
#include "AOTRowsOfLayouts.h"
#include "AOTSchemaEnums.h"
#include "AOTSort.h"
#include "AOTSortedSet.h"
#include "AOTStages.h"
#include "AOTStatistics.h"
#include "AOTSupportTestCommands.h"
#include "AOTTestHarness.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include "LineColumn.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <compare>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/EnumTraits.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>
#include <wtf/WeakRandom.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

using namespace Typed;

namespace {

struct DeletedOptionWord {
    ASCIILiteral key;
    ASCIILiteral word;
};

} // anonymous namespace

static constexpr unsigned numberOfWorkers = 8;

static constexpr ASCIILiteral programOfSupportTests = R"IR(module m0 path="/a.ts" displayPath="a.ts"
function f0 name="first" results=[{type=anything}]
function f1 name="second" parameters=[{type=anything}] results=[{type=anything}]
)IR"_s;

static Wire::CompileOptions optionsWith(std::initializer_list<ASCIILiteral> words)
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    for (ASCIILiteral word : words)
        AOT_TEST_CHECK(!applyOptionWord(options, word));
    return options;
}

static std::unique_ptr<Program> parseProgramOfSupportTests(BuildContext& build)
{
    std::unique_ptr<Program> program = parseProgram(programOfSupportTests, build);
    AOT_TEST_CHECK(program);
    AOT_TEST_CHECK(program->functions.size() == 2);
    return program;
}

static void testDiagnosticsOrderByTextPosition()
{
    auto errorAt = [](std::optional<LineColumn> position) {
        return InternalError { .kind = InternalErrorKind::IRTextError, .textPosition = position };
    };
    Diagnostics diagnostics;
    diagnostics.add(errorAt(LineColumn { 2, 1 }));
    diagnostics.add(errorAt(LineColumn { 1, 9 }));
    diagnostics.add(errorAt(std::nullopt));
    diagnostics.add(errorAt(LineColumn { 1, 4 }));
    diagnostics.add(errorAt(LineColumn { 1, 9 }));
    diagnostics.finish(nullptr);

    AOT_TEST_CHECK(diagnostics.userDiagnostics().isEmpty());
    AOT_TEST_CHECK(diagnostics.internalErrors().size() == 4);
    AOT_TEST_CHECK(diagnostics.messageOfInternalError(0) == "the IR text does not parse"_s);
    AOT_TEST_CHECK(diagnostics.messageOfInternalError(1) == "the IR text does not parse; line 1, column 4 of the IR text"_s);
    AOT_TEST_CHECK(diagnostics.messageOfInternalError(2) == "the IR text does not parse; line 1, column 9 of the IR text"_s);
    AOT_TEST_CHECK(diagnostics.messageOfInternalError(3) == "the IR text does not parse; line 2, column 1 of the IR text"_s);
}

static void testTextPositionIsLastContextClause()
{
    Diagnostics diagnostics;
    diagnostics.add(InternalError { .kind = InternalErrorKind::IRTextError, .function = FunctionID(1u), .textPosition = LineColumn { 4, 12 } });
    diagnostics.finish(nullptr);
    AOT_TEST_CHECK(diagnostics.messageOfInternalError(0) == "the IR text does not parse; function f1, line 4, column 12 of the IR text"_s);
}

static void testDiagnosticsKeepTwoLists()
{
    static_assert(!failsBuild(UserDiagnosticKind::RegExpWithoutCode));
    static_assert(failsBuild(UserDiagnosticKind::LimitExceeded));

    Diagnostics diagnostics;
    AOT_TEST_CHECK(!diagnostics.hasErrors());
    diagnostics.add(UserDiagnostic { .kind = UserDiagnosticKind::RegExpWithoutCode, .function = FunctionID(1u) });
    AOT_TEST_CHECK(!diagnostics.hasErrors());
    diagnostics.add(UserDiagnostic { .kind = UserDiagnosticKind::LimitExceeded, .function = FunctionID(0u), .limit = LimitID::MaxDataNodes, .count = 3 });
    AOT_TEST_CHECK(diagnostics.hasErrors());
    diagnostics.add(InternalError { .kind = InternalErrorKind::IRTextError });
    diagnostics.finish(nullptr);

    AOT_TEST_CHECK(diagnostics.userDiagnostics().size() == 2);
    AOT_TEST_CHECK(diagnostics.userDiagnostics()[0].kind == UserDiagnosticKind::LimitExceeded);
    AOT_TEST_CHECK(diagnostics.userDiagnostics()[1].kind == UserDiagnosticKind::RegExpWithoutCode);
    AOT_TEST_CHECK(diagnostics.internalErrors().size() == 1);
    AOT_TEST_CHECK(diagnostics.internalErrors()[0].kind == InternalErrorKind::IRTextError);

    Diagnostics onlyInternalError;
    onlyInternalError.add(InternalError { .kind = InternalErrorKind::IRTextError });
    AOT_TEST_CHECK(onlyInternalError.hasErrors());
}

static void testDeletedOptionWordsAreRefused()
{
    static constexpr DeletedOptionWord deletedWords[] = {
        { "disable"_s, "disable=pass:EliminateDeadCode"_s },
        { "only-passes"_s, "only-passes=EliminateDeadCode"_s },
        { "disable-in"_s, "disable-in=0:1"_s },
        { "limit"_s, "limit=MaxInlineDepth:1"_s },
        { "heat"_s, "heat=ReadOffStructure"_s },
        { "sabotage"_s, "sabotage=NoCallCollects"_s },
        { "count-branches"_s, "count-branches"_s },
        { "count-arrivals"_s, "count-arrivals"_s },
        { "drop-guard"_s, "drop-guard=GetSlot:0"_s },
        { "type-errors"_s, "type-errors=log"_s },
        { "layout-seed"_s, "layout-seed=1"_s },
    };

    const Wire::CompileOptions defaults = makeDefaultCompileOptions();
    for (const DeletedOptionWord& deleted : deletedWords) {
        Wire::CompileOptions options = defaults;
        std::optional<String> sentence = applyOptionWord(options, deleted.word);
        AOT_TEST_CHECK(sentence && sentence->contains(deleted.key));
        AOT_TEST_CHECK(options == defaults);
    }
}

static void testRefusedOptionWordChangesNothing()
{
    const Wire::CompileOptions defaults = makeDefaultCompileOptions();
    // The first name of the list is one that the schema has.
    for (ASCIILiteral word : { "report=Flow,NoSuchReport"_s, "threads=0"_s, "threads"_s, "obfuscate=1"_s, ""_s }) {
        Wire::CompileOptions options = defaults;
        AOT_TEST_CHECK(applyOptionWord(options, word));
        AOT_TEST_CHECK(options == defaults);
    }
}

static void testAppliedOptionWordsReachTheBuild()
{
    Wire::CompileOptions options = optionsWith({ "threads=3"_s });
    AOT_TEST_CHECK(options.threadCount == 3);
    BuildContext build(options);
    AOT_TEST_CHECK(build.threadCount() == 3);
}

static void testStatisticsFromManyThreads()
{
    constexpr unsigned numberOfCalls = 100000;
    constexpr uint64_t sumOfIndexes = static_cast<uint64_t>(numberOfCalls) * (numberOfCalls - 1) / 2;

    ParallelLoops loops(numberOfWorkers);
    Statistics statistics;
    forEachIndexInParallel(loops, numberOfCalls, [&](unsigned index) {
        statistics.count(StatisticID::TypesInterned);
        statistics.count(StatisticID::ValuesWalked, index);
        statistics.count(StatisticID::None);
    });

    Vector<uint64_t> values = statistics.values();
    AOT_TEST_CHECK(values.size() == numberOfStatistics);
    AOT_TEST_CHECK(values[static_cast<size_t>(StatisticID::TypesInterned)] == numberOfCalls);
    AOT_TEST_CHECK(values[static_cast<size_t>(StatisticID::ValuesWalked)] == sumOfIndexes);
    AOT_TEST_CHECK(std::accumulate(values.begin(), values.end(), uint64_t { 0 }) == numberOfCalls + sumOfIndexes);
}

struct NumberMovedByItsConstructor {
    NumberMovedByItsConstructor() = default;
    explicit NumberMovedByItsConstructor(unsigned number)
        : number(number)
    {
    }
    NumberMovedByItsConstructor(NumberMovedByItsConstructor&& other)
        : number(std::exchange(other.number, 0))
    {
    }
    NumberMovedByItsConstructor(const NumberMovedByItsConstructor&) = default;
    NumberMovedByItsConstructor& operator=(NumberMovedByItsConstructor&& other)
    {
        number = std::exchange(other.number, 0);
        return *this;
    }
    NumberMovedByItsConstructor& operator=(const NumberMovedByItsConstructor&) = default;
    ~NumberMovedByItsConstructor() { number = 0; }

    unsigned number { 0 };
};
static_assert(!movesAsBytes<NumberMovedByItsConstructor>);
static_assert(movesAsBytes<unsigned>);

// The pool sorts sixteen runs and merges them: in a list shorter than that some runs are empty.
static void testSortOnPoolOfListsOfEveryLength()
{
    constexpr unsigned longestOfEveryLength = 70;
    constexpr unsigned shuffleFactor = 7919;

    ParallelLoops loops(numberOfWorkers);
    auto check = [&](unsigned length) {
        Vector<NumberMovedByItsConstructor> moved;
        Vector<unsigned> bytes;
        for (unsigned index = 0; index < length; ++index) {
            unsigned number = 1 + index * shuffleFactor % (length / 2 + 1);
            moved.append(NumberMovedByItsConstructor(number));
            bytes.append(number);
        }
        Vector<unsigned> expected = bytes;
        std::ranges::sort(expected);
        sortOnPool(loops, moved.mutableSpan(), [](const NumberMovedByItsConstructor& a, const NumberMovedByItsConstructor& b) {
            return a.number < b.number;
        });
        sortOnPool(loops, bytes.mutableSpan(), std::less<unsigned> { });
        AOT_TEST_CHECK(bytes == expected);
        AOT_TEST_CHECK(moved.size() == length);
        for (unsigned index = 0; index < length; ++index)
            AOT_TEST_CHECK(moved[index].number == expected[index]);
    };
    for (unsigned length = 0; length <= longestOfEveryLength; ++length)
        check(length);
    check(10000);

    // A longer list has four times the runs, and a still longer one sixteen times. Whatever their number, equal elements stay as they came.
    auto checkOrderOfEqualElements = [&](unsigned length) {
        Vector<std::pair<unsigned, unsigned>> list(length, [&](size_t index) {
            return std::pair { static_cast<unsigned>(index * shuffleFactor % 97), static_cast<unsigned>(index) };
        });
        Vector<std::pair<unsigned, unsigned>> expected = list;
        std::ranges::stable_sort(expected, { }, &std::pair<unsigned, unsigned>::first);
        sortOnPool(loops, list.mutableSpan(), [](const std::pair<unsigned, unsigned>& a, const std::pair<unsigned, unsigned>& b) {
            return a.first < b.first;
        });
        AOT_TEST_CHECK(list == expected);
    };
    for (unsigned length : { 10000u, (1u << 17) - 1, (1u << 17) + 5, (1u << 21) - 1, (1u << 21) + 7 })
        checkOrderOfEqualElements(length);
}

namespace {

struct DeferredCall {
    void (*run)(void* argument);
    void* argument;
};

// A host pool has no argument of its own, and one native test runs at a time.
struct DeferredCalls {
    Lock lock;
    Vector<DeferredCall> calls WTF_GUARDED_BY_LOCK(lock);
};

// What it was moved from records nothing.
class DestructionRecorder {
    WTF_MAKE_NONCOPYABLE(DestructionRecorder);
public:
    DestructionRecorder(Vector<unsigned>& order, unsigned number)
        : m_order(&order)
        , m_number(number)
    {
    }
    DestructionRecorder(DestructionRecorder&& other)
        : m_order(std::exchange(other.m_order, nullptr))
        , m_number(other.m_number)
    {
    }
    ~DestructionRecorder()
    {
        if (m_order)
            m_order->append(m_number);
    }

private:
    Vector<unsigned>* m_order;
    unsigned m_number;
};

} // anonymous namespace

static DeferredCalls& deferredCalls()
{
    static NeverDestroyed<DeferredCalls> calls;
    return calls;
}

static Vector<DeferredCall> takeDeferredCalls()
{
    Locker locker { deferredCalls().lock };
    return std::exchange(deferredCalls().calls, { });
}

// Runs nothing until runDeferredCalls().
static constexpr HostThreadPool deferringTestPool { [](void (*run)(void*), void* argument) {
    Locker locker { deferredCalls().lock };
    deferredCalls().calls.append({ run, argument });
} };

// On a thread with the stack of a thread of a pool, and what the calls themselves leave to the pool too. Answers how many calls the pool held at first.
static size_t runDeferredCalls()
{
    Vector<DeferredCall> calls = takeDeferredCalls();
    size_t numberHeldAtFirst = calls.size();
    runOnBuildThread([&] {
        for (; !calls.isEmpty(); calls = takeDeferredCalls()) {
            for (DeferredCall call : calls)
                call.run(call.argument);
        }
    });
    return numberHeldAtFirst;
}

static void testBackgroundJobsAreLeftToHostPool()
{
    Vector<unsigned> order;
    unsigned widthOfJobLoops = 0;
    std::atomic<unsigned> visits { 0 };
    {
        ParallelLoops loops(4, &deferringTestPool);
        Ref<BackgroundJobs> jobs = BackgroundJobs::create(loops);
        jobs->add([&, owned = DestructionRecorder(order, 2)](const ParallelLoops& jobLoops) {
            UNUSED_PARAM(owned);
            widthOfJobLoops = jobLoops.width();
            forEachIndexInParallel(jobLoops, 1000, [&](unsigned) {
                visits.fetch_add(1, std::memory_order_relaxed);
            });
            order.append(1);
        });
        jobs->add([&](const ParallelLoops&) {
            order.append(3);
        });
        destroyInBackground(jobs.get(), DestructionRecorder(order, 4));
        AOT_TEST_CHECK(order.isEmpty() && jobs->numberOfJobsWaiting() == 3);
    }
    // The loops of the build and its reference to the jobs are gone. One call runs all jobs, and what a job owns dies in front of the next job.
    AOT_TEST_CHECK(runDeferredCalls() == 1);
    AOT_TEST_CHECK((order == Vector<unsigned> { 1, 2, 3, 4 }));
    AOT_TEST_CHECK(widthOfJobLoops == 4 && visits.load() == 1000);
}

static void testBackgroundJobsRunAtOnceWithoutHostPool()
{
    auto runsAtOnceWithBuildLoops = [](const ParallelLoops& loops) {
        const ParallelLoops* jobLoops = nullptr;
        Ref<BackgroundJobs> jobs = BackgroundJobs::create(loops);
        jobs->add([&](const ParallelLoops& given) {
            jobLoops = &given;
        });
        return jobLoops == &loops && !jobs->numberOfJobsWaiting();
    };
    ParallelLoops poolOfItsOwn(4);
    ParallelLoops one(1);
    ParallelLoops oneOnHostPool(1, &deferringTestPool);
    AOT_TEST_CHECK(runsAtOnceWithBuildLoops(poolOfItsOwn) && runsAtOnceWithBuildLoops(one) && runsAtOnceWithBuildLoops(oneOnHostPool));
    AOT_TEST_CHECK(!runDeferredCalls());
}

static constexpr ASCIILiteral programWithImage = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" module=m0 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = Call v1
    Return
}
)IR"_s;

static std::unique_ptr<Program> parseProgramWithImage(BuildContext& build)
{
    std::unique_ptr<Program> program = parseProgram(withStandInLibrary(String(programWithImage)), build);
    AOT_TEST_CHECK(program);
    return program;
}

// A build that only verifies runs no stage that could add a job of its own: what waits is the driver's.
static size_t numberOfJobsLeftByVerifying(const HostThreadPool* pool)
{
    BuildContext build(optionsWith({ "threads=4"_s, "verify-only"_s }), pool);
    compileWithMechanisms(parseProgramWithImage(build), build, mechanismsOfProcess());
    return build.backgroundJobs().numberOfJobsWaiting();
}

// The build hands over what the stages have left and does not wait. The pool comes to it when the build and its context are gone, and the output is none of it.
static void testStageResultsAreDestroyedInBackground()
{
    AOT_TEST_CHECK(numberOfJobsLeftByVerifying(&deferringTestPool) == 1);
    AOT_TEST_CHECK(runDeferredCalls());
    AOT_TEST_CHECK(!numberOfJobsLeftByVerifying(nullptr));

    std::unique_ptr<CompileOutput> output;
    {
        BuildContext build(optionsWith({ "threads=4"_s }), &deferringTestPool);
        output = compileWithMechanisms(parseProgramWithImage(build), build, mechanismsOfProcess());
        AOT_TEST_CHECK(output->hasImage() && build.backgroundJobs().numberOfJobsWaiting());
    }
    Vector<uint8_t> image(output->section(Wire::ResultSection::Image));
    AOT_TEST_CHECK(runDeferredCalls());
    AOT_TEST_CHECK(equalSpans(output->section(Wire::ResultSection::Image), image.span()));
}

static constexpr unsigned widthOfLaneTests = 4;

// Nobody goes on until `count` threads are here, so each of them is a thread of its own.
static void waitForOthers(std::atomic<unsigned>& numberArrived, unsigned count)
{
    numberArrived.fetch_add(1);
    while (numberArrived.load() < count)
        Thread::yield();
}

static void testLanesThatPoolNeverBeginsAreTheCallers()
{
    constexpr unsigned count = 4;
    const Thread& caller = Thread::currentSingleton();
    Vector<unsigned> order;
    std::atomic<unsigned> visits { 0 };
    {
        ParallelLoops loops(widthOfLaneTests, &deferringTestPool);
        runLanes(loops, count, [&](unsigned lane, const ParallelLoops& loopsOfLane) {
            AOT_TEST_CHECK(&Thread::currentSingleton() == &caller && &loopsOfLane == &loops && &loops.ofCurrentThread() == &loops);
            AOT_TEST_CHECK(!ParallelLoops::isInsideLoop());
            order.append(lane);
        });
        AOT_TEST_CHECK((order == Vector<unsigned> { 0, 1, 2, 3 }));
        // The loops and the lanes are gone when the pool comes: a call for each lane but the first, and none finds a lane to take.
    }
    AOT_TEST_CHECK(runDeferredCalls() == count - 1);
    AOT_TEST_CHECK(order.size() == count);

    // The pool comes while the caller is in its first lane, and takes the others.
    order.clear();
    runOnBuildThread([&] {
        ParallelLoops loops(widthOfLaneTests, &deferringTestPool);
        runLanes(loops, count, [&](unsigned lane, const ParallelLoops& loopsOfLane) {
            AOT_TEST_CHECK((&loopsOfLane == &loops) == !lane && &loops.ofCurrentThread() == &loopsOfLane);
            order.append(lane);
            forEachIndexInParallel(loopsOfLane, 100, [&](unsigned, unsigned worker) {
                AOT_TEST_CHECK(!worker);
                visits.fetch_add(1, std::memory_order_relaxed);
            });
            if (lane)
                return;
            for (DeferredCall call : takeDeferredCalls())
                call.run(call.argument);
            AOT_TEST_CHECK(&loops.ofCurrentThread() == &loops && !ParallelLoops::isInsideLoop());
        });
    });
    AOT_TEST_CHECK((order == Vector<unsigned> { 0, 1, 2, 3 }) && visits.load() == count * 100);
    runDeferredCalls();
}

static void checkLanesRunSideBySide(const ParallelLoops& loops)
{
    constexpr unsigned count = 3;
    static_assert(count <= widthOfLaneTests);
    const Thread& caller = Thread::currentSingleton();
    std::array<const Thread*, count> threads { };
    std::array<const ParallelLoops*, count> loopsOfLanes { };
    std::atomic<unsigned> numberArrived { 0 };
    runLanes(loops, count, [&](unsigned lane, const ParallelLoops& loopsOfLane) {
        waitForOthers(numberArrived, count);
        threads[lane] = &Thread::currentSingleton();
        loopsOfLanes[lane] = &loopsOfLane;
        AOT_TEST_CHECK(!ParallelLoops::isInsideLoop());
        AOT_TEST_CHECK(&loops.ofCurrentThread() == &loopsOfLane && loopsOfLane.areThoseOfCurrentThread());
        AOT_TEST_CHECK(loops.areThoseOfCurrentThread() == !lane);
        AOT_TEST_CHECK(loopsOfLane.width() == loops.width() && loopsOfLane.hostPool() == loops.hostPool());
        // It stands in for the thread of the lane.
        runOnBuildThread([&] {
            AOT_TEST_CHECK(&loops.ofCurrentThread() == &loopsOfLane);
        });
        // The loops of all lanes are alive until all are written down, so no two lie at one address.
        waitForOthers(numberArrived, 2 * count);
    });
    AOT_TEST_CHECK(threads[0] == &caller && loopsOfLanes[0] == &loops);
    AOT_TEST_CHECK(threads[1] != &caller && threads[2] != &caller && threads[1] != threads[2]);
    AOT_TEST_CHECK(loopsOfLanes[1] != &loops && loopsOfLanes[2] != &loops && loopsOfLanes[1] != loopsOfLanes[2]);
    AOT_TEST_CHECK(&loops.ofCurrentThread() == &loops);
}

static void testLanesRunSideBySide()
{
    ParallelLoops onHostPool(widthOfLaneTests, &poolOfNewThreads);
    checkLanesRunSideBySide(onHostPool);
    ParallelLoops onPoolOfItsOwn(widthOfLaneTests);
    checkLanesRunSideBySide(onPoolOfItsOwn);
}

// A row of marks for the loops of each lane. A thread finds its row by the loops that the build answers it, and its mark in the row by its worker number.
static void testWorkerNumbersOfLanesNeverIndexOneState()
{
    constexpr unsigned count = 3;
    constexpr unsigned numberOfIndices = 256;
    constexpr unsigned numberOfSpins = 1000;
    BuildContext build(optionsWith({ "threads=4"_s }), &poolOfNewThreads);
    AOT_TEST_CHECK(build.threadCount() == widthOfLaneTests);
    std::array<std::atomic<const ParallelLoops*>, count> loopsOfRows { };
    std::array<std::array<std::atomic<bool>, widthOfLaneTests>, count> isInUse { };
    std::atomic<unsigned> numberArrived { 0 };
    auto rowOf = [&](const ParallelLoops& loops) {
        unsigned row = 0;
        while (row < count && loopsOfRows[row].load() != &loops)
            ++row;
        return row;
    };
    runLanes(build.parallelLoops(), count, [&](unsigned lane, const ParallelLoops& loopsOfLane) {
        AOT_TEST_CHECK(&build.parallelLoops() == &loopsOfLane);
        loopsOfRows[lane].store(&loopsOfLane);
        waitForOthers(numberArrived, count);
        forEachIndexInParallel(build.parallelLoops(), numberOfIndices, [&](unsigned, unsigned worker) {
            AOT_TEST_CHECK(rowOf(build.parallelLoops()) == lane && worker < widthOfLaneTests);
            std::atomic<bool>& mark = isInUse[lane][worker];
            AOT_TEST_CHECK(!mark.exchange(true));
            for (unsigned spin = 0; spin < numberOfSpins; ++spin)
                AOT_TEST_CHECK(mark.load());
            mark.store(false);
        });
        waitForOthers(numberArrived, 2 * count);
    });
}

static void testNestedLoopInHelperOfLaneKeepsItsNumber()
{
    BuildContext build(optionsWith({ "threads=4"_s }), &poolOfNewThreads);
    runLanes(build.parallelLoops(), 2, [&](unsigned, const ParallelLoops& loopsOfLane) {
        const Thread& threadOfLane = Thread::currentSingleton();
        std::atomic<bool> hasSeenHelper { false };
        forEachIndexInParallel(build.parallelLoops(), 64, [&](unsigned, unsigned worker) {
            bool isHelper = &Thread::currentSingleton() != &threadOfLane;
            AOT_TEST_CHECK(isHelper == !!worker && &build.parallelLoops() == &loopsOfLane);
            forEachIndexInParallel(build.parallelLoops(), 4, [&](unsigned, unsigned workerOfNestedLoop) {
                AOT_TEST_CHECK(workerOfNestedLoop == worker);
            });
            if (isHelper)
                hasSeenHelper.store(true);
            // The thread of the lane leaves the other indices to a helper, which this pool always sends.
            while (!hasSeenHelper.load())
                Thread::yield();
        });
    });
}

static void testLanesStartedInLaneHaveLoopsOfTheirOwn()
{
    BuildContext build(optionsWith({ "threads=4"_s }), &poolOfNewThreads);
    const ParallelLoops& loopsOfBuild = build.parallelLoops();
    std::array<const ParallelLoops*, 4> loopsOfLanes { };
    std::atomic<unsigned> numberArrived { 0 };
    auto writeDownLoops = [&](unsigned lane) {
        loopsOfLanes[lane] = &build.parallelLoops();
        waitForOthers(numberArrived, loopsOfLanes.size());
    };
    auto startTwoLanes = [&](unsigned firstLane) {
        const ParallelLoops& loopsOfStarter = build.parallelLoops();
        runLanes(loopsOfStarter, {
            [&] { writeDownLoops(firstLane); },
            [&] { writeDownLoops(firstLane + 1); },
        });
        AOT_TEST_CHECK(&build.parallelLoops() == &loopsOfStarter && loopsOfLanes[firstLane] == &loopsOfStarter);
    };
    runLanes(loopsOfBuild, {
        [&] { startTwoLanes(0); },
        [&] { startTwoLanes(2); },
    });
    AOT_TEST_CHECK(loopsOfLanes[0] == &loopsOfBuild);
    for (unsigned lane = 0; lane < loopsOfLanes.size(); ++lane) {
        for (unsigned other = 0; other < lane; ++other)
            AOT_TEST_CHECK(loopsOfLanes[lane] != loopsOfLanes[other]);
    }
}

static void testLanesRunInOrderAtWidthOfOneAndInTask()
{
    auto runsInOrderOnCaller = [](const ParallelLoops& loops) {
        const Thread& caller = Thread::currentSingleton();
        bool isInsideLoop = ParallelLoops::isInsideLoop();
        Vector<unsigned> order;
        runLanes(loops, 3, [&](unsigned lane, const ParallelLoops& loopsOfLane) {
            AOT_TEST_CHECK(&Thread::currentSingleton() == &caller && &loopsOfLane == &loops && ParallelLoops::isInsideLoop() == isInsideLoop);
            order.append(lane);
        });
        return order == Vector<unsigned> { 0, 1, 2 };
    };
    ParallelLoops one(1);
    ParallelLoops oneOnHostPool(1, &deferringTestPool);
    AOT_TEST_CHECK(runsInOrderOnCaller(one) && runsInOrderOnCaller(oneOnHostPool));
    AOT_TEST_CHECK(!runDeferredCalls());

    // In a task, lanes have the thread of the task, as a loop has.
    auto runsInOrderInTasks = [&](const ParallelLoops& loops) {
        runTasksInParallel(loops, {
            [&] { AOT_TEST_CHECK(ParallelLoops::isInsideLoop() && runsInOrderOnCaller(loops)); },
            [&] { AOT_TEST_CHECK(ParallelLoops::isInsideLoop() && runsInOrderOnCaller(loops)); },
        });
    };
    ParallelLoops onHostPool(widthOfLaneTests, &poolOfNewThreads);
    ParallelLoops onPoolOfItsOwn(widthOfLaneTests);
    runsInOrderInTasks(onHostPool);
    runsInOrderInTasks(onPoolOfItsOwn);
}

static Vector<uint8_t> buildImageOn(const HostThreadPool* pool, ASCIILiteral threads)
{
    BuildContext build(optionsWith({ threads }), pool);
    std::unique_ptr<CompileOutput> output = compileWithMechanisms(parseProgramWithImage(build), build, mechanismsOfProcess());
    AOT_TEST_CHECK(output->hasImage());
    return Vector<uint8_t>(output->section(Wire::ResultSection::Image));
}

// One behind the other, side by side, and all on the thread of the build because the pool never comes.
static void testLanesLeaveImageAsItIs()
{
    Vector<uint8_t> image = buildImageOn(nullptr, "threads=1"_s);
    AOT_TEST_CHECK(buildImageOn(nullptr, "threads=4"_s) == image);
    AOT_TEST_CHECK(buildImageOn(&poolOfNewThreads, "threads=4"_s) == image);
    AOT_TEST_CHECK(buildImageOn(&deferringTestPool, "threads=4"_s) == image);
    AOT_TEST_CHECK(runDeferredCalls());
}

static void leavePlanAsItIs(Plan&)
{
}

// The table has a selector of a family that it lacks. The selector claims no unit and nothing mentions it, so Compile is none the worse for it, and checkPlan() asks every selector for its family.
static void addSelectorOfNoFamily(Plan& plan)
{
    MethodTablePlan table;
    table.selectors.append(MethodSelector { });
    plan.setMethodTable(WTF::move(table));
}

namespace {

struct BuildWithChangedPlan {
    bool hasImage { false };
    bool listsErrorOfCheck { false };
    uint64_t unitsCompiled { 0 };
};

} // anonymous namespace

static BuildWithChangedPlan buildWithChangedPlan(const HostThreadPool* pool, OptionWords words, void (*changePlan)(Plan&))
{
    BuildContext build(optionsWith(words), pool);
    std::unique_ptr<CompileOutput> output = compileParsedProgramWithChangedPlan(parseProgramWithImage(build), build, changePlan, mechanismsOfProcess());
    bool contextListsError = std::ranges::contains(build.diagnostics().internalErrors(), InternalErrorKind::PlacementCheckFailed, &InternalError::kind);
    return {
        .hasImage = output->hasImage(),
        .listsErrorOfCheck = contextListsError && !output->section(Wire::ResultSection::InternalErrors).empty(),
        .unitsCompiled = build.statistics().values()[static_cast<unsigned>(StatisticID::UnitsCompiled)],
    };
}

// Units are compiled although the plan fails, so the build did not wait for the check before Compile. It did wait before Image.
static void testPlanThatFailsItsCheckBesideCompileGivesNoImage()
{
    auto stopsBehindCompile = [](const HostThreadPool* pool, OptionWords words) {
        BuildWithChangedPlan build = buildWithChangedPlan(pool, words, addSelectorOfNoFamily);
        return !build.hasImage && build.listsErrorOfCheck && build.unitsCompiled;
    };
    AOT_TEST_CHECK(stopsBehindCompile(&poolOfNewThreads, { "threads=4"_s }));
    AOT_TEST_CHECK(stopsBehindCompile(nullptr, { "threads=4"_s }));
    // The pool never comes, so the build thread runs the check after the units.
    AOT_TEST_CHECK(stopsBehindCompile(&deferringTestPool, { "threads=4"_s }));
    AOT_TEST_CHECK(runDeferredCalls());

    BuildWithChangedPlan sound = buildWithChangedPlan(&poolOfNewThreads, { "threads=4"_s }, leavePlanAsItIs);
    AOT_TEST_CHECK(sound.hasImage && !sound.listsErrorOfCheck && sound.unitsCompiled);
}

static void testPlanThatFailsItsCheckInFrontOfCompileReachesNoWalk()
{
    auto stopsInFrontOfCompile = [](const HostThreadPool* pool, OptionWords words) {
        BuildWithChangedPlan build = buildWithChangedPlan(pool, words, addSelectorOfNoFamily);
        return !build.hasImage && build.listsErrorOfCheck && !build.unitsCompiled;
    };
    AOT_TEST_CHECK(stopsInFrontOfCompile(nullptr, { "threads=1"_s }));
    AOT_TEST_CHECK(stopsInFrontOfCompile(&poolOfNewThreads, { "threads=1"_s }));
    AOT_TEST_CHECK(stopsInFrontOfCompile(&poolOfNewThreads, { "threads=4"_s, "check-only"_s }));

    BuildWithChangedPlan sound = buildWithChangedPlan(nullptr, { "threads=1"_s }, leavePlanAsItIs);
    AOT_TEST_CHECK(sound.hasImage && !sound.listsErrorOfCheck && sound.unitsCompiled);
}

namespace {

template<size_t numberOfOtherWords>
struct SortedRecord {
    uint32_t key;
    std::array<uint32_t, numberOfOtherWords> otherWords;

    friend bool operator==(const SortedRecord&, const SortedRecord&) = default;
};

struct SortedRecordWithList {
    uint32_t key;
    Vector<uint32_t, 2> list;

    friend bool operator==(const SortedRecordWithList&, const SortedRecordWithList&) = default;
};

enum class KeyPattern : uint8_t { Random, Ascending, Descending, RisingThenFalling, AllEqual };

Vector<uint32_t> createKeys(WeakRandom& random, size_t size, KeyPattern pattern, uint32_t numberOfDifferentKeys)
{
    return Vector<uint32_t>(size, [&](size_t index) -> uint32_t {
        switch (pattern) {
        case KeyPattern::Random:
            return random.getUint32(numberOfDifferentKeys);
        case KeyPattern::Ascending:
            return index * numberOfDifferentKeys / size;
        case KeyPattern::Descending:
            return (size - index) * numberOfDifferentKeys / size;
        case KeyPattern::RisingThenFalling:
            return std::min(index, size - index) * numberOfDifferentKeys / size;
        case KeyPattern::AllEqual:
            return 7;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

void forEachListOfKeys(const Invocable<void(const Vector<uint32_t>&)> auto& check)
{
    WeakRandom random(20261022);
    for (size_t size : { 0, 1, 2, 3, 5, 7, 8, 15, 16, 17, 18, 31, 33, 64, 100, 257, 1000, 4099 }) {
        for (KeyPattern pattern : { KeyPattern::Random, KeyPattern::Ascending, KeyPattern::Descending, KeyPattern::RisingThenFalling, KeyPattern::AllEqual }) {
            for (uint32_t numberOfDifferentKeys : { 2u, 3u, 10u, 1000000u })
                check(createKeys(random, size, pattern, numberOfDifferentKeys));
        }
    }
}

template<typename Record>
Vector<Record> createRecords(const Vector<uint32_t>& keys)
{
    return Vector<Record>(keys.size(), [&](size_t index) {
        Record record { };
        record.key = keys[index];
        if constexpr (std::is_same_v<Record, SortedRecordWithList>)
            record.list = { static_cast<uint32_t>(index), 1, 2, 3 };
        else
            record.otherWords[0] = index;
        return record;
    });
}

template<typename Record>
uint32_t indexBeforeSorting(const Record& record)
{
    if constexpr (std::is_same_v<Record, SortedRecordWithList>)
        return record.list[0];
    else
        return record.otherWords[0];
}

template<typename Record>
void checkSortsOf(const Vector<uint32_t>& keys)
{
    Vector<Record> sorted = createRecords<Record>(keys);
    Vector<Record> expected = sorted;
    AOT::sort(sorted, { }, &Record::key);
    // No two records are equal in this order, so it leaves a sort no choice.
    std::ranges::sort(expected, { }, [](const Record& record) {
        return std::pair { record.key, indexBeforeSorting(record) };
    });
    AOT_TEST_CHECK(sorted == expected);
    Vector<Record> sortedInLine = createRecords<Record>(keys);
    AOT::sortInLine(sortedInLine, { }, &Record::key);
    AOT_TEST_CHECK(sortedInLine == sorted);

    sorted = createRecords<Record>(keys);
    expected = sorted;
    auto isGreater = [](const Record& first, const Record& second) {
        return first.key > second.key;
    };
    AOT::stableSort(sorted, isGreater);
    std::ranges::stable_sort(expected, isGreater);
    AOT_TEST_CHECK(sorted == expected);
    sortedInLine = createRecords<Record>(keys);
    AOT::stableSortInLine(sortedInLine, isGreater);
    AOT_TEST_CHECK(sortedInLine == expected);
}

} // namespace

static void testSortLeavesEqualElementsInTheirOrder()
{
    forEachListOfKeys([](const Vector<uint32_t>& keys) {
        checkSortsOf<SortedRecord<1>>(keys);
        checkSortsOf<SortedRecord<2>>(keys);
        checkSortsOf<SortedRecord<5>>(keys);
        checkSortsOf<SortedRecord<20>>(keys);
        checkSortsOf<SortedRecordWithList>(keys);
    });
}

static void testSortOfIntegersAndIDs()
{
    forEachListOfKeys([](const Vector<uint32_t>& keys) {
        Vector<uint32_t> expected = keys;
        std::ranges::sort(expected);
        Vector<uint32_t> integers = keys;
        AOT::sort(integers);
        AOT_TEST_CHECK(integers == expected);
        Vector<FunctionID> ids = keys.map([](uint32_t key) {
            return FunctionID(key);
        });
        AOT::sort(ids);
        AOT_TEST_CHECK(std::ranges::equal(ids, expected, { }, &FunctionID::index));
        AOT::sort(integers, std::ranges::greater { });
        AOT_TEST_CHECK(std::ranges::equal(integers, expected | std::views::reverse));
    });
}

struct CountedValue {
    friend bool operator==(CountedValue a, CountedValue b)
    {
        ++numberOfComparisons;
        return a.value == b.value;
    }
    friend std::strong_ordering operator<=>(CountedValue a, CountedValue b)
    {
        ++numberOfComparisons;
        return a.value <=> b.value;
    }

    static inline size_t numberOfComparisons = 0;

    uint32_t value { 0 };
};

static void testShortSortedListIsLookedForByHalving()
{
    WeakRandom random(1);
    auto createList = [&](size_t size, uint32_t numberOfValues) {
        Vector<uint32_t> values(size, [&](size_t) {
            return random.getUint32(numberOfValues);
        });
        std::ranges::sort(values);
        values.shrink(std::ranges::unique(values).begin() - values.begin());
        return values;
    };
    for (size_t size : { 0, 1, 2, 3, 17, 40 }) {
        for (size_t otherSize : { 0, 1, 2, 16, 33, 700 }) {
            for (uint32_t numberOfValues : { 3, 50, 2000, 100000 }) {
                Vector<uint32_t> list = createList(size, numberOfValues);
                Vector<uint32_t> other = createList(otherSize, numberOfValues);
                auto isInOther = [&](uint32_t value) { return other.contains(value); };
                AOT_TEST_CHECK(sortedListsOverlap(list.span(), other.span()) == std::ranges::any_of(list, isInOther));
                AOT_TEST_CHECK(sortedListsOverlap(other.span(), list.span()) == std::ranges::any_of(list, isInOther));
                AOT_TEST_CHECK(sortedListIncludes(other.span(), list.span()) == std::ranges::all_of(list, isInOther));
            }
        }
    }

    constexpr uint32_t size = 1 << 16;
    Vector<CountedValue> many(size, [](size_t index) {
        return CountedValue { static_cast<uint32_t>(2 * index) };
    });
    for (uint32_t value : { 0u, 1u, size, size + 1, 2 * size }) {
        std::array<CountedValue, 1> one { CountedValue { value } };
        bool isAmongMany = !(value % 2) && value < 2 * size;
        CountedValue::numberOfComparisons = 0;
        AOT_TEST_CHECK(sortedListsOverlap(std::span<const CountedValue>(one), many.span()) == isAmongMany);
        AOT_TEST_CHECK(sortedListsOverlap(many.span(), std::span<const CountedValue>(one)) == isAmongMany);
        AOT_TEST_CHECK(sortedListIncludes(many.span(), std::span<const CountedValue>(one)) == isAmongMany);
        AOT_TEST_CHECK(CountedValue::numberOfComparisons < 3 * 20);
    }
}

static void testAnswersAlongRowsAreThoseOfWalkForEachLayout()
{
    WeakRandom random(1);
    for (uint32_t numberOfLayouts : { 1, 2, 7, 60, 600 }) {
        for (unsigned round = 0; round < 12; ++round) {
            // In the first round one row of all layouts. Then any layout stands above any other, so that there are rings.
            Vector<LayoutID> layoutAbove(numberOfLayouts, [&](size_t index) {
                if (!round)
                    return index ? LayoutID(static_cast<uint32_t>(index - 1)) : LayoutID();
                return random.getUint32(8) ? LayoutID(random.getUint32(numberOfLayouts)) : LayoutID();
            });
            enum : uint8_t { No, Yes };
            Vector<uint8_t> settled(numberOfLayouts, [&](size_t) {
                return static_cast<uint8_t>(random.getUint32(round ? 12 : 4000));
            });
            auto step = [&](LayoutID id) -> StepAlongRow {
                if (settled[id.index()] <= Yes)
                    return { settled[id.index()] == Yes, { } };
                if (!layoutAbove[id.index()])
                    return { true, { } };
                return { std::nullopt, layoutAbove[id.index()] };
            };
            for (bool answerInRing : { false, true }) {
                size_t numberOfSteps = 0;
                BitVector answers = answerAlongRows(numberOfLayouts, answerInRing, [&](LayoutID id) {
                    ++numberOfSteps;
                    return step(id);
                });
                AOT_TEST_CHECK(numberOfSteps == numberOfLayouts);
                for (uint32_t index = 0; index < numberOfLayouts; ++index) {
                    bool expected = answerInRing;
                    LayoutID current(index);
                    for (size_t steps = 0; steps < numberOfLayouts; ++steps) {
                        StepAlongRow taken = step(current);
                        if (taken.answer) {
                            expected = *taken.answer;
                            break;
                        }
                        current = taken.next;
                    }
                    AOT_TEST_CHECK(answers.quickGet(index) == expected);
                }
            }

            auto isWanted = [&](LayoutID id) {
                return settled[id.index()] == No;
            };
            size_t numberOfLooksAbove = 0;
            Vector<LayoutID> nearest = findNearestWantedAbove(numberOfLayouts, [&](LayoutID id) {
                ++numberOfLooksAbove;
                return layoutAbove[id.index()];
            }, isWanted);
            AOT_TEST_CHECK(numberOfLooksAbove == 2 * numberOfLayouts);
            auto wantedAbove = [&](uint32_t index, const Vector<LayoutID>& next) {
                Vector<LayoutID> found;
                BitVector isFound(numberOfLayouts);
                LayoutID current = next[index];
                for (size_t steps = 0; current && steps < numberOfLayouts; ++steps, current = next[current.index()]) {
                    if (isWanted(current) && !isFound.quickSet(current.index()))
                        found.append(current);
                }
                return found;
            };
            for (uint32_t index = 0; index < numberOfLayouts; ++index)
                AOT_TEST_CHECK(wantedAbove(index, nearest) == wantedAbove(index, layoutAbove));
            if (!round) {
                for (uint32_t index = 0; index < numberOfLayouts; ++index)
                    AOT_TEST_CHECK(!nearest[index] || isWanted(nearest[index]));
            }
        }
    }
}

static String textOf(size_t (*describe)(std::span<char>))
{
    std::array<char, 4096> buffer;
    return String::fromUTF8(std::span<const char>(buffer).first(describe(buffer)));
}

static void testCurrentWorkNamesFunctionAndStep()
{
    BuildContext build(makeDefaultCompileOptions());
    std::unique_ptr<Program> program = parseProgramOfSupportTests(build);
    AOT_TEST_CHECK(textOf(describeWhereThisThreadStopped).isEmpty());
    {
        CurrentWorkScope stage("plan"_s);
        AOT_TEST_CHECK(textOf(describeWhereThisThreadStopped) == "Stopped while compiling in plan.\n"_s);
        {
            CurrentWorkScope scope(*program, FunctionID(1u), "walk"_s);
            AOT_TEST_CHECK(textOf(describeWhereThisThreadStopped) == "Stopped while compiling second (a.ts:1) in walk.\n"_s);
            String saidOfAllThreads = textOf(describeWhatThreadsAreCompiling);
            AOT_TEST_CHECK(saidOfAllThreads.contains("compiling second (a.ts:1) in walk"_s));
            AOT_TEST_CHECK(saidOfAllThreads.find('\n') + 1 == saidOfAllThreads.length());
            scope.setStep("encode"_s);
            AOT_TEST_CHECK(textOf(describeWhereThisThreadStopped) == "Stopped while compiling second (a.ts:1) in encode.\n"_s);
        }
        AOT_TEST_CHECK(textOf(describeWhereThisThreadStopped) == "Stopped while compiling in plan.\n"_s);
        {
            // A number that no function has.
            CurrentWorkScope scope(*program, FunctionID(1000u), "walk"_s);
            AOT_TEST_CHECK(textOf(describeWhereThisThreadStopped) == "Stopped while compiling in walk.\n"_s);
        }
        AOT_TEST_CHECK(textOf(describeWhereThisThreadStopped) == "Stopped while compiling in plan.\n"_s);
    }
    AOT_TEST_CHECK(textOf(describeWhereThisThreadStopped).isEmpty());
    {
        PhaseTimes times(build, Stage::PlanSlots);
        AOT_TEST_CHECK(textOf(describeWhereThisThreadStopped).startsWith("Stopped while compiling in "_s));
    }
    AOT_TEST_CHECK(textOf(describeWhereThisThreadStopped).isEmpty());
    AOT_TEST_CHECK(textOf(describeWhatThreadsAreCompiling).isEmpty());
}

static void testCurrentWorkOfPoolThreads()
{
    BuildContext build(optionsWith({ "threads=4"_s }));
    std::unique_ptr<Program> program = parseProgramOfSupportTests(build);
    std::atomic<unsigned> numberOfThreadsAtWork { 0 };
    std::atomic<bool> isDescribed { false };
    String saidOfAllThreads;
    // Both indices are at work at once, so they are on two threads, of which at most one is this one.
    forEachIndexInParallel(build.parallelLoops(), 2, [&](unsigned index) {
        CurrentWorkScope scope(*program, FunctionID(index), "walk"_s);
        numberOfThreadsAtWork.fetch_add(1);
        while (numberOfThreadsAtWork.load() < 2)
            Thread::yield();
        if (index) {
            saidOfAllThreads = textOf(describeWhatThreadsAreCompiling);
            isDescribed.store(true);
        }
        while (!isDescribed.load())
            Thread::yield();
    });
    AOT_TEST_CHECK(saidOfAllThreads.contains("compiling first (a.ts:1) in walk"_s));
    AOT_TEST_CHECK(saidOfAllThreads.contains("compiling second (a.ts:1) in walk"_s));
}

// One behind the other, and more of them than there are records.
static void testCurrentWorkOfThreadsBehindManyThatEnded()
{
    for (unsigned count = 0; count < 2000; ++count) {
        String said;
        runOnBuildThread([&] {
            CurrentWorkScope stage("plan"_s);
            said = textOf(describeWhereThisThreadStopped);
        });
        AOT_TEST_CHECK(said == "Stopped while compiling in plan.\n"_s);
    }
}

static void testCurrentWorkFitsItsBuffer()
{
    BuildContext build(makeDefaultCompileOptions());
    std::unique_ptr<Program> program = parseProgramOfSupportTests(build);
    CurrentWorkScope scope(*program, FunctionID(1u), "walk"_s);
    String whole = textOf(describeWhereThisThreadStopped);
    AOT_TEST_CHECK(!whole.isEmpty());
    for (size_t capacity = 0; capacity <= whole.length(); ++capacity) {
        std::array<char, 256> buffer;
        buffer.fill('#');
        size_t length = describeWhereThisThreadStopped(std::span(buffer).first(capacity));
        AOT_TEST_CHECK(length == capacity && buffer[capacity] == '#');
        AOT_TEST_CHECK(String::fromUTF8(std::span<const char>(buffer).first(length)) == whole.left(capacity));
    }
}

enum class EnumWithGap : uint8_t { First, Second, Last = 4 };
enum class WideEnum : uint16_t { Only };

static void testNamesOfEnumeratorsEndAtTheLastOne()
{
    static_assert(WTF::enumNamesSize<EnumWithGap>() == 256 && WTF::enumNamesSizeUpToLastName<EnumWithGap>() == 5);
    static_assert(WTF::enumNamesSize<WideEnum>() == 511 && WTF::enumNamesSizeUpToLastName<WideEnum>() == 1);
    AOT_TEST_CHECK(WTF::enumName(EnumWithGap::First) == "First"_s && WTF::enumName(EnumWithGap::Second) == "Second"_s && WTF::enumName(EnumWithGap::Last) == "Last"_s);
    for (unsigned value = 0; value < 256; ++value)
        AOT_TEST_CHECK(WTF::enumName(static_cast<EnumWithGap>(value)) == WTF::enumNames<EnumWithGap>()[value]);
    AOT_TEST_CHECK(WTF::enumName(static_cast<EnumWithGap>(3)).isNull() && WTF::enumName(static_cast<EnumWithGap>(5)).isNull() && WTF::enumName(static_cast<EnumWithGap>(255)).isNull());
    AOT_TEST_CHECK(WTF::enumName(WideEnum::Only) == "Only"_s && WTF::enumName(static_cast<WideEnum>(510)).isNull());
    AOT_TEST_CHECK(WTF::enumName(static_cast<WideEnum>(511)) == "enum out of range"_s);
}

std::span<const NativeTest> supportTests()
{
    static constexpr NativeTest tests[] = {
        { "diagnostics/order-by-text-position"_s, testDiagnosticsOrderByTextPosition },
        { "diagnostics/text-position-is-last-context-clause"_s, testTextPositionIsLastContextClause },
        { "diagnostics/two-lists"_s, testDiagnosticsKeepTwoLists },
        { "option-words/deleted-words-are-refused"_s, testDeletedOptionWordsAreRefused },
        { "option-words/refused-word-changes-nothing"_s, testRefusedOptionWordChangesNothing },
        { "option-words/applied-words-reach-the-build"_s, testAppliedOptionWordsReachTheBuild },
        { "statistics/many-threads"_s, testStatisticsFromManyThreads },
        { "threads/sort-on-pool-of-lists-of-every-length"_s, testSortOnPoolOfListsOfEveryLength },
        { "threads/background-jobs-are-left-to-host-pool"_s, testBackgroundJobsAreLeftToHostPool },
        { "threads/background-jobs-run-at-once-without-host-pool"_s, testBackgroundJobsRunAtOnceWithoutHostPool },
        { "threads/stage-results-are-destroyed-in-background"_s, testStageResultsAreDestroyedInBackground },
        { "threads/lanes-that-pool-never-begins-are-the-callers"_s, testLanesThatPoolNeverBeginsAreTheCallers },
        { "threads/lanes-run-side-by-side"_s, testLanesRunSideBySide },
        { "threads/lanes-worker-numbers-never-index-one-state"_s, testWorkerNumbersOfLanesNeverIndexOneState },
        { "threads/lanes-nested-loop-in-helper-keeps-its-number"_s, testNestedLoopInHelperOfLaneKeepsItsNumber },
        { "threads/lanes-started-in-lane-have-loops-of-their-own"_s, testLanesStartedInLaneHaveLoopsOfTheirOwn },
        { "threads/lanes-run-in-order-at-width-of-one-and-in-a-task"_s, testLanesRunInOrderAtWidthOfOneAndInTask },
        { "threads/lanes-leave-image-as-it-is"_s, testLanesLeaveImageAsItIs },
        { "threads/plan-that-fails-its-check-beside-compile-gives-no-image"_s, testPlanThatFailsItsCheckBesideCompileGivesNoImage },
        { "threads/plan-that-fails-its-check-in-front-of-compile-reaches-no-walk"_s, testPlanThatFailsItsCheckInFrontOfCompileReachesNoWalk },
        { "current-work/names-function-and-step"_s, testCurrentWorkNamesFunctionAndStep },
        { "current-work/of-pool-threads"_s, testCurrentWorkOfPoolThreads },
        { "current-work/of-threads-behind-many-that-ended"_s, testCurrentWorkOfThreadsBehindManyThatEnded },
        { "current-work/fits-its-buffer"_s, testCurrentWorkFitsItsBuffer },
        { "sort/equal-elements"_s, testSortLeavesEqualElementsInTheirOrder },
        { "sort/integers-and-ids"_s, testSortOfIntegersAndIDs },
        { "sorted-lists/short-list-is-looked-for-by-halving"_s, testShortSortedListIsLookedForByHalving },
        { "rows-of-layouts/answers-are-those-of-a-walk-for-each-layout"_s, testAnswersAlongRowsAreThoseOfWalkForEachLayout },
        { "enum-names/end-at-the-last-enumerator"_s, testNamesOfEnumeratorsEndAtTheLastOne },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
