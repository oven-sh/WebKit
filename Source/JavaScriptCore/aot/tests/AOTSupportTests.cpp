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

static void testRefusedOptionWordChangesNothing()
{
    const Wire::CompileOptions defaults = makeDefaultCompileOptions();
    for (ASCIILiteral word : { "report=Flow,NoSuchReport"_s, "threads=0"_s, "threads"_s, "obfuscate=1"_s, ""_s }) {
        Wire::CompileOptions options = defaults;
        AOT_TEST_CHECK(applyOptionWord(options, word));
        AOT_TEST_CHECK(options == defaults);
    }
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

namespace {

struct DeferredCall {
    void (*run)(void* argument);
    void* argument;
};

struct DeferredCalls {
    Lock lock;
    Vector<DeferredCall> calls WTF_GUARDED_BY_LOCK(lock);
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

static constexpr HostThreadPool deferringTestPool { [](void (*run)(void*), void* argument) {
    Locker locker { deferredCalls().lock };
    deferredCalls().calls.append({ run, argument });
} };

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

static size_t numberOfJobsLeftByVerifying(const HostThreadPool* pool)
{
    BuildContext build(optionsWith({ "threads=4"_s, "verify-only"_s }), pool);
    compileWithMechanisms(parseProgramWithImage(build), build, mechanismsOfProcess());
    return build.backgroundJobs().numberOfJobsWaiting();
}

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
            while (!hasSeenHelper.load())
                Thread::yield();
        });
    });
}

static Vector<uint8_t> buildImageOn(const HostThreadPool* pool, ASCIILiteral threads)
{
    BuildContext build(optionsWith({ threads }), pool);
    std::unique_ptr<CompileOutput> output = compileWithMechanisms(parseProgramWithImage(build), build, mechanismsOfProcess());
    AOT_TEST_CHECK(output->hasImage());
    return Vector<uint8_t>(output->section(Wire::ResultSection::Image));
}

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

} // namespace

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

std::span<const NativeTest> supportTests()
{
    static constexpr NativeTest tests[] = {
        { "diagnostics/two-lists"_s, testDiagnosticsKeepTwoLists },
        { "option-words/refused-word-changes-nothing"_s, testRefusedOptionWordChangesNothing },
        { "statistics/many-threads"_s, testStatisticsFromManyThreads },
        { "threads/stage-results-are-destroyed-in-background"_s, testStageResultsAreDestroyedInBackground },
        { "threads/lanes-nested-loop-in-helper-keeps-its-number"_s, testNestedLoopInHelperOfLaneKeepsItsNumber },
        { "threads/lanes-leave-image-as-it-is"_s, testLanesLeaveImageAsItIs },
        { "threads/plan-that-fails-its-check-in-front-of-compile-reaches-no-walk"_s, testPlanThatFailsItsCheckInFrontOfCompileReachesNoWalk },
        { "current-work/names-function-and-step"_s, testCurrentWorkNamesFunctionAndStep },
        { "sorted-lists/short-list-is-looked-for-by-halving"_s, testShortSortedListIsLookedForByHalving },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
