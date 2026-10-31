/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCompile.h"
#include "AOTCompileOptionWords.h"
#include "AOTCurrentWork.h"
#include "AOTDerivedFunctions.h"
#include "AOTDiagnostics.h"
#include "AOTFlowResults.h"
#include "AOTFunctionEditing.h"
#include "AOTFunctionQueue.h"
#include "AOTIR.h"
#include "AOTIRParser.h"
#include "AOTInterningPhases.h"
#include "AOTLimits.h"
#include "AOTLosses.h"
#include "AOTNativeTest.h"
#include "AOTPassID.h"
#include "AOTPersistentSortedSet.h"
#include "AOTPhaseTimes.h"
#include "AOTPropertyAccessMembers.h"
#include "AOTSchemaEnums.h"
#include "AOTSharedPrefixList.h"
#include "AOTSort.h"
#include "AOTSortedSet.h"
#include "AOTStatistics.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include "B3PhaseScope.h"
#include "B3Procedure.h"
#include "LineColumn.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <compare>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <wtf/EnumTraits.h>
#include <wtf/FastMalloc.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/VectorTraits.h>
#include <wtf/WeakRandom.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

using namespace Typed;

namespace {

struct ExpectedOrigin {
    uint32_t function;
    FunctionID functionAsRead;
};

struct ExpectedDecline {
    uint32_t function;
    uint32_t position;
};

} // anonymous namespace

static constexpr unsigned numberOfWorkers = 8;

static_assert(sizeof(Vector<void*>) == sizeof(WTF::Vector<void*>) && sizeof(Vector<void*, 4>) == sizeof(WTF::Vector<void*, 4>) && sizeof(Vector<uint8_t, 3>) == sizeof(WTF::Vector<uint8_t, 3>));
static_assert(Vector<void*, 4>::dataMemoryOffset() == WTF::Vector<void*, 4>::dataMemoryOffset() && Vector<void*, 4>::sizeMemoryOffset() == WTF::Vector<void*, 4>::sizeMemoryOffset());

static constexpr ASCIILiteral derivedFunctions = R"IR(module m0 path="/a.ts" displayPath="a.ts"
function f0 name="first" results=[{type=anything}]
function f1 name="second" parameters=[{type=anything}] results=[{type=anything}]
function f2 name="genericVersion" role=GenericVersion results=[{type=anything}] derivation={source=f0} creator=Version{provenVersion=f0}
function f3 name="resultWorkerOfVersion" role=ResultWorker results=[{type=anything}] derivation={source=f2} creator=ResultWorker{origin=f2}
function f4 name="outlinedFromWorkerOfVersion" role=OutlinedBody results=[{type=anything}] derivation={source=f3}
function f5 name="resultWorker" role=ResultWorker parameters=[{type=anything}] results=[{type=anything}] derivation={source=f1} creator=ResultWorker{origin=f1}
function f6 name="serializer" role=Serializer results=[{type=anything}] creator=Serializer{}
function f7 name="outlinedFromSerializer" role=OutlinedBody results=[{type=anything}] derivation={source=f6}
)IR"_s;

static Wire::CompileOptions optionsWith(std::initializer_list<ASCIILiteral> words)
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    for (ASCIILiteral word : words)
        AOT_TEST_CHECK(!applyOptionWord(options, word));
    return options;
}

static std::unique_ptr<Program> parseDerivedFunctions(BuildContext& build)
{
    std::unique_ptr<Program> program = parseProgram(derivedFunctions, IRForm::Prepared, build);
    AOT_TEST_CHECK(program);
    AOT_TEST_CHECK(program->numberOfFunctionsAsRead() == 2);
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
    diagnostics.add(InternalError { .kind = InternalErrorKind::IRTextError, .function = FunctionID(1u), .textPosition = LineColumn { 4, 12 }, .pass = PassID::VerifyProgram });
    diagnostics.finish(nullptr);
    AOT_TEST_CHECK(diagnostics.messageOfInternalError(0) == "the IR text does not parse; function f1, after pass VerifyProgram, line 4, column 12 of the IR text"_s);
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

static void testFunctionAsReadEndsDerivationChain()
{
    static constexpr ExpectedOrigin cases[] = {
        { 0, FunctionID(0u) },
        { 1, FunctionID(1u) },
        { 2, FunctionID(0u) },
        { 3, FunctionID(0u) },
        { 4, FunctionID(0u) },
        { 5, FunctionID(1u) },
        { 6, FunctionID() },
        { 7, FunctionID() },
    };

    BuildContext build(makeDefaultCompileOptions());
    std::unique_ptr<Program> program = parseDerivedFunctions(build);
    AOT_TEST_CHECK(program->functions.size() == std::size(cases));
    for (const ExpectedOrigin& expected : cases)
        AOT_TEST_CHECK(findFunctionAsRead(program.get(), FunctionID(expected.function)) == expected.functionAsRead);

    AOT_TEST_CHECK(!findFunctionAsRead(program.get(), FunctionID()));
    AOT_TEST_CHECK(!findFunctionAsRead(program.get(), FunctionID(program->functions.size())));
    AOT_TEST_CHECK(!findFunctionAsRead(nullptr, FunctionID(0u)));
}

static void testFunctionAsReadOfInvalidChain()
{
    BuildContext build(makeDefaultCompileOptions());
    std::unique_ptr<Program> program = parseDerivedFunctions(build);

    program->functions[7].derivation.source = FunctionID(program->functions.size());
    AOT_TEST_CHECK(!findFunctionAsRead(program.get(), FunctionID(7u)));

    program->functions[7].derivation.source = FunctionID(6u);
    program->functions[6].derivation.source = FunctionID(7u);
    program->functions[4].derivation.source = FunctionID(6u);
    for (uint32_t function : { 4u, 6u, 7u })
        AOT_TEST_CHECK(!findFunctionAsRead(program.get(), FunctionID(function)));
}

static void testDiagnosticsNameFunctionsAsRead()
{
    BuildContext build(makeDefaultCompileOptions());
    std::unique_ptr<Program> program = parseDerivedFunctions(build);

    Diagnostics diagnostics;
    diagnostics.add(InternalError { .kind = InternalErrorKind::OperandMissing, .function = FunctionID(4u), .block = BlockID(1u), .value = ValueID(2u) });
    diagnostics.add(InternalError { .kind = InternalErrorKind::OperandMissing, .function = FunctionID(1u), .block = BlockID(1u), .value = ValueID(2u) });
    diagnostics.add(InternalError { .kind = InternalErrorKind::OperandMissing, .function = FunctionID(7u), .block = BlockID(1u), .value = ValueID(2u) });
    diagnostics.finish(program.get());

    const Vector<InternalError>& errors = diagnostics.internalErrors();
    AOT_TEST_CHECK(errors.size() == 3);
    AOT_TEST_CHECK(errors[0].function == FunctionID(1u) && errors[0].block == BlockID(1u) && errors[0].value == ValueID(2u));
    AOT_TEST_CHECK(errors[1].function == FunctionID(0u) && !errors[1].block && !errors[1].value);
    AOT_TEST_CHECK(!errors[2].function && !errors[2].block && !errors[2].value);
}

static Wire::DeclineRecord declineIn(uint32_t function, uint32_t position)
{
    return {
        .kind = Wire::DeclineKind::HotCallNotInlined,
        .limit = LimitID::None,
        .count = 0,
        .witness = { .function = function, .value = Wire::none, .location = { 0, position } },
    };
}

static void expectDeclines(const Vector<Wire::DeclineRecord>& declines, std::initializer_list<ExpectedDecline> expectedDeclines)
{
    AOT_TEST_CHECK(std::ranges::equal(declines, expectedDeclines, [](const Wire::DeclineRecord& decline, const ExpectedDecline& expected) {
        return decline.witness.function == expected.function && decline.witness.location.position == expected.position;
    }));
}

static void testDeclinesNameFunctionsAsRead()
{
    BuildContext build(optionsWith({ "report=Declines"_s }));
    std::unique_ptr<Program> program = parseDerivedFunctions(build);

    build.addDecline(declineIn(Wire::none, 40));
    build.addDecline(declineIn(6, 30));
    build.addDecline(declineIn(5, 20));
    build.addDecline(declineIn(3, 10));
    build.addDecline(declineIn(2, 10));
    expectDeclines(build.takeDeclines(program.get()), { { 0, 10 }, { 1, 20 }, { Wire::none, 30 }, { Wire::none, 40 } });

    build.addDecline(declineIn(3, 10));
    build.addDecline(declineIn(2, 10));
    expectDeclines(build.takeDeclines(nullptr), { { 2, 10 }, { 3, 10 } });
    AOT_TEST_CHECK(build.takeDeclines(program.get()).isEmpty());
}

static void testDeclinesAreCollectedOnRequest()
{
    BuildContext build(makeDefaultCompileOptions());
    build.addDecline(declineIn(0, 10));
    AOT_TEST_CHECK(build.takeDeclines(nullptr).isEmpty());
}

static void testDerivedFunctionIDsFromManyThreads()
{
    constexpr unsigned numberOfKeys = 20000;
    constexpr unsigned numberOfRequests = 8 * numberOfKeys;
    constexpr unsigned shuffleFactor = 7919;
    static_assert(std::gcd(numberOfKeys, shuffleFactor) == 1 && !(numberOfKeys % 2));
    auto keyOf = [](unsigned request) {
        return derivedFunctionKey(FunctionRole::BoundFunction, FunctionID(request % 2), request * shuffleFactor % numberOfKeys);
    };

    BuildContext build(optionsWith({ "threads=8"_s }));
    std::unique_ptr<Program> program = parseDerivedFunctions(build);
    FlowResults flow;
    DerivedFunctions table(build);
    DerivedFunctionKey recorded = derivedFunctionKey(FunctionRole::GenericEntrypoint, FunctionID(0u));
    table.record(recorded, FunctionID(2u));
    {
        InterningPhases interning(*program, program->functions.size());
        DerivedFunctionPhase phase(table, *program, flow, interning);

        Vector<FunctionID> ids(numberOfRequests);
        forEachIndexInParallel(build.parallelLoops(), numberOfRequests, [&](unsigned request) {
            ids[request] = table.ensure(keyOf(request));
            AOT_TEST_CHECK(table.find(keyOf(request)) == ids[request]);
            AOT_TEST_CHECK(table.ensure(recorded) == FunctionID(2u));
        });

        AOT_TEST_CHECK(phase.numberOfRequests() == numberOfKeys);
        BitVector isTaskTaken;
        BitVector isSeen(numberOfKeys);
        for (unsigned request = 0; request < numberOfRequests; ++request) {
            FunctionID id = ids[request];
            AOT_TEST_CHECK(ProvisionalFunctions::contains(id) && ProvisionalFunctions::ordinalOf(id) < numberOfKeys);
            AOT_TEST_CHECK(table.keyOf(id) == keyOf(request));
            if (isSeen.set(ProvisionalFunctions::ordinalOf(id)))
                continue;
            AOT_TEST_CHECK(phase.taskOf(id) > program->functions.size());
            AOT_TEST_CHECK(!isTaskTaken.set(phase.taskOf(id)) && !isTaskTaken.set(phase.taskOf(id) - 1));
        }
        AOT_TEST_CHECK(isSeen.bitCount() == numberOfKeys);
        interning.discard();
    }

    AOT_TEST_CHECK(!table.find(keyOf(0)) && !table.find(keyOf(numberOfRequests - 1)));
    AOT_TEST_CHECK(table.find(recorded) == FunctionID(2u));
    table.record(recorded, FunctionID(3u));
    AOT_TEST_CHECK(table.find(recorded) == FunctionID(3u));
    table.recordFunctionsOf(*program);
    AOT_TEST_CHECK(table.find(recorded) == FunctionID(3u));
}

static void testCachesOfProgramLastAsLongAsWhatRunsBesideEdits()
{
    BuildContext build(optionsWith({ "threads=8"_s }));
    std::unique_ptr<Program> program = parseDerivedFunctions(build);
    ProgramEditor editor(*program);
    auto hasCachesAndOpenPhases = [&] {
        return program->propertyAccessCache() && program->sharedFieldTypes() && program->types.hasOpenPhase() && program->strings.hasOpenPhase();
    };
    auto hasNone = [&] {
        return !program->propertyAccessCache() && !program->sharedFieldTypes() && !program->types.hasOpenPhase() && !program->strings.hasOpenPhase();
    };
    std::array functions { FunctionID(0u), FunctionID(1u) };
    for (size_t numberOfFunctions = 0; numberOfFunctions <= functions.size(); ++numberOfFunctions) {
        std::atomic<unsigned> numberOfEdits { 0 };
        unsigned numberOfRunsBeside = 0;
        AOT_TEST_CHECK(hasNone());
        editFunctionsInParallel(editor, build, std::span<const FunctionID>(functions).first(numberOfFunctions), [&](unsigned, FunctionID, Function&, TaskArenas&) {
            AOT_TEST_CHECK(hasCachesAndOpenPhases());
            numberOfEdits.fetch_add(1, std::memory_order_relaxed);
        }, [&] {
            AOT_TEST_CHECK(numberOfFunctions ? hasCachesAndOpenPhases() : hasNone());
            ++numberOfRunsBeside;
        });
        AOT_TEST_CHECK(hasNone());
        AOT_TEST_CHECK(numberOfEdits.load() == numberOfFunctions && numberOfRunsBeside == 1);
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
        statistics.count(StatisticID::FlowAllocationSites, index);
        statistics.count(StatisticID::None);
    });

    Vector<uint64_t> values = statistics.values();
    AOT_TEST_CHECK(values.size() == numberOfStatistics);
    AOT_TEST_CHECK(values[static_cast<size_t>(StatisticID::TypesInterned)] == numberOfCalls);
    AOT_TEST_CHECK(values[static_cast<size_t>(StatisticID::FlowAllocationSites)] == sumOfIndexes);
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
}

static void testLossesKeepLowestSubject()
{
    constexpr unsigned numberOfHits = 10000;
    constexpr unsigned valuesPerFunction = 100;
    constexpr unsigned shuffleFactor = 7919;
    constexpr uint32_t lowestFunction = 3;
    constexpr uint32_t lowestValue = 5;
    constexpr LimitID limit = LimitID::MaxOptimizationRounds;
    static_assert(std::gcd(numberOfHits, shuffleFactor) == 1);

    BuildContext build(optionsWith({ "threads=8"_s }));
    AOT_TEST_CHECK(build.threadCount() == numberOfWorkers);
    forEachIndexInParallel(build.parallelLoops(), numberOfHits, [&](unsigned index) {
        unsigned shuffledIndex = index * shuffleFactor % numberOfHits;
        GaveUp result = build.gaveUp(limit, { .function = FunctionID(lowestFunction + shuffledIndex / valuesPerFunction), .value = ValueID(lowestValue + shuffledIndex % valuesPerFunction) });
        AOT_TEST_CHECK(result.limit == limit);
    });

    Vector<Wire::LossRecord> losses = build.takeLosses(nullptr);
    AOT_TEST_CHECK(losses.size() == 1);
    AOT_TEST_CHECK(losses[0].limit == limit);
    AOT_TEST_CHECK(losses[0].stage == stageOf(limit));
    AOT_TEST_CHECK(losses[0].count == numberOfHits);
    AOT_TEST_CHECK(losses[0].layout == Wire::none);
    AOT_TEST_CHECK(losses[0].witness == witnessOf({ .function = FunctionID(lowestFunction), .value = ValueID(lowestValue) }));
    AOT_TEST_CHECK(build.statistics().values()[static_cast<size_t>(hitsOf(limit))] == numberOfHits);
    AOT_TEST_CHECK(!build.diagnostics().hasErrors());
    AOT_TEST_CHECK(build.takeLosses(nullptr).isEmpty());
}

static void testLossesPreferSubjectWithLocation()
{
    static_assert(LimitID::MaxOptimizationRounds < LimitID::MaxSinkingRounds);

    Subject withLocation { .function = FunctionID(9u), .value = ValueID(1u), .location = { 0, 4 } };
    BuildContext build(makeDefaultCompileOptions());
    build.gaveUp(LimitID::MaxSinkingRounds, { .function = FunctionID(1u) });
    build.gaveUp(LimitID::MaxSinkingRounds, withLocation);
    build.gaveUp(LimitID::MaxSinkingRounds, { .function = FunctionID(2u), .location = { 0, 5 } });
    AOT_TEST_CHECK(!build.exceeds(LimitID::MaxOptimizationRounds, build.limit(LimitID::MaxOptimizationRounds)));
    AOT_TEST_CHECK(build.exceeds(LimitID::MaxOptimizationRounds, build.limit(LimitID::MaxOptimizationRounds) + 1, { .layout = LayoutID(7u) }));

    Vector<Wire::LossRecord> losses = build.takeLosses(nullptr);
    AOT_TEST_CHECK(losses.size() == 2);
    AOT_TEST_CHECK(losses[0].limit == LimitID::MaxOptimizationRounds);
    AOT_TEST_CHECK(losses[0].count == 1);
    AOT_TEST_CHECK(losses[0].layout == 7);
    AOT_TEST_CHECK(losses[0].witness == Wire::Witness { });
    AOT_TEST_CHECK(losses[1].limit == LimitID::MaxSinkingRounds);
    AOT_TEST_CHECK(losses[1].count == 3);
    AOT_TEST_CHECK(losses[1].witness == witnessOf(withLocation));
}

// Function n leads to 2n + 1 and 2n + 2, and each to the provisional function of its number. Every function is found twice or more.
static void checkFunctionQueue(unsigned width)
{
    constexpr uint32_t count = 5000;
    ParallelLoops loops(width);
    FunctionQueue queue(loops, count + 1);
    Vector<std::atomic<uint8_t>> visits(2 * count);
    for (auto& visit : visits)
        visit.store(0, std::memory_order_relaxed);

    const QueuedFunction roots[] { { FunctionID(0u), 1 }, { FunctionID(0u), 1 } };
    queue.processEach(roots, [&](FunctionID id, FunctionQueue::FoundFunctions& found) {
        if (ProvisionalFunctions::contains(id)) {
            visits[count + ProvisionalFunctions::ordinalOf(id)].fetch_add(1, std::memory_order_relaxed);
            return;
        }
        uint32_t index = id.index();
        visits[index].fetch_add(1, std::memory_order_relaxed);
        for (uint32_t next : { 2 * index + 1, 2 * index + 2, index / 2 }) {
            if (next < count)
                found.append({ FunctionID(next), next % 7 });
        }
        found.append({ ProvisionalFunctions::idOf(index), 0 });
        found.append({ ProvisionalFunctions::idOf(index / 3), 0 });
    });

    for (auto& visit : visits)
        AOT_TEST_CHECK(visit.load(std::memory_order_relaxed) == 1);
    AOT_TEST_CHECK(queue.wasQueued(FunctionID(count - 1)) && !queue.wasQueued(FunctionID(count)));
    AOT_TEST_CHECK(queue.wasQueued(ProvisionalFunctions::idOf(count - 1)) && !queue.wasQueued(ProvisionalFunctions::idOf(count)));
    AOT_TEST_CHECK(!queue.wasQueued(ProvisionalFunctions::idOf(1u << 30)));
    BitVector ordinals = queue.takeQueuedProvisionalOrdinals();
    AOT_TEST_CHECK(ordinals.bitCount() == count && ordinals.get(0) && ordinals.get(count - 1));
}

static void testFunctionQueueBehindLongFunction()
{
    constexpr uint32_t numberOfSmallFunctions = 64;
    constexpr uint32_t longFunction = numberOfSmallFunctions + 1;
    ParallelLoops loops(4);
    FunctionQueue queue(loops, longFunction + 1);
    std::atomic<uint32_t> numberProcessed { 0 };
    bool sawAllProcessed = false;

    const QueuedFunction roots[] { { FunctionID(0u), 1 } };
    queue.processEach(roots, [&](FunctionID id, FunctionQueue::FoundFunctions& found) {
        if (!id.index()) {
            for (uint32_t index = 1; index <= numberOfSmallFunctions; ++index)
                found.append({ FunctionID(index), 1 });
            found.append({ FunctionID(longFunction), 1000 });
            return;
        }
        if (id.index() != longFunction) {
            numberProcessed.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        MonotonicTime deadline = MonotonicTime::now() + 20_s;
        while (numberProcessed.load(std::memory_order_relaxed) < numberOfSmallFunctions && MonotonicTime::now() < deadline)
            Thread::yield();
        sawAllProcessed = numberProcessed.load(std::memory_order_relaxed) == numberOfSmallFunctions;
    });
    AOT_TEST_CHECK(sawAllProcessed);
}

static void testFunctionQueueOnOneThread()
{
    checkFunctionQueue(1);
}

static void testFunctionQueueOnManyThreads()
{
    for (unsigned round = 0; round < 20; ++round)
        checkFunctionQueue(8);
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
static_assert(movesAsBytes<SortedRecord<4>> && movesAsBytes<std::pair<FunctionID, FunctionID>> && !movesAsBytes<SortedRecordWithList>);

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
void checkSortsOf(const Vector<uint32_t>& keys)
{
    Vector<Record> sorted = createRecords<Record>(keys);
    Vector<Record> expected = sorted;
    AOT::sort(sorted, { }, &Record::key);
    AOT_TEST_CHECK(std::ranges::is_sorted(sorted, { }, &Record::key));
#if defined(__GLIBCXX__)
    std::ranges::sort(expected, { }, &Record::key);
    AOT_TEST_CHECK(sorted == expected);
#endif
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

static void testSortLeavesEqualElementsWhereTheStandardLibraryDoes()
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

static void testPersistentSetsHoldWhatSortedSetsHold()
{
    WeakRandom random(1);
    auto valuesOf = [](const PersistentSortedSet<uint32_t>& set) {
        Vector<uint32_t> values;
        set.forEach([&](uint32_t value) {
            values.append(value);
        });
        return values;
    };
    auto holdSame = [&](const PersistentSortedSet<uint32_t>& set, const SortedSet<uint32_t>& expected) {
        return set.size() == expected.size() && std::ranges::equal(valuesOf(set), expected.span());
    };
    for (uint32_t numberOfValues : { 1, 4, 40, 3000 }) {
        Vector<std::pair<PersistentSortedSet<uint32_t>, SortedSet<uint32_t>>> sets(1);
        for (unsigned step = 0; step < 600; ++step) {
            auto [set, expected] = sets[random.getUint32(sets.size())];
            const auto& [other, expectedOther] = sets[random.getUint32(sets.size())];
            switch (random.getUint32(6)) {
            case 0:
                AOT_TEST_CHECK(set.unite(other) == expected.unite(expectedOther));
                break;
            case 1:
                set.intersect(other);
                expected.intersect(expectedOther);
                break;
            case 2:
                set.subtract(other);
                expected.subtract(expectedOther);
                break;
            default:
                for (unsigned count = random.getUint32(8); count--;) {
                    uint32_t value = random.getUint32(numberOfValues);
                    AOT_TEST_CHECK(set.contains(value) == expected.contains(value));
                    AOT_TEST_CHECK(set.add(value) == expected.add(value));
                }
                break;
            }
            AOT_TEST_CHECK(holdSame(set, expected));
            AOT_TEST_CHECK(holdSame(other, expectedOther));
            AOT_TEST_CHECK(set.overlaps(other) == expected.overlaps(expectedOther));
            AOT_TEST_CHECK((set == other) == (expected == expectedOther));
            PersistentSortedSet<uint32_t> builtAnew;
            for (uint32_t value : expected)
                builtAnew.add(value);
            AOT_TEST_CHECK(builtAnew == set);
            sets.append({ WTF::move(set), WTF::move(expected) });
        }
    }

    constexpr uint32_t numberOfSets = 300000;
    Vector<PersistentSortedSet<uint32_t>> row(numberOfSets);
    for (uint32_t index = 1; index < numberOfSets; ++index) {
        row[index] = row[index - 1];
        row[index].add(index);
    }
    PersistentSortedSet<uint32_t> common = row.last();
    for (uint32_t index : { numberOfSets - 1, numberOfSets / 2, 7u }) {
        common.intersect(row[index]);
        AOT_TEST_CHECK(common == row[index] && common.size() == index && common.contains(index) && !common.contains(index + 1));
    }
}

static void testPackedPartitionUnpacksToWhatWasPacked()
{
    auto isSame = [](const MemberPartition& a, const MemberPartition& b) {
        return a.rest == b.rest && std::ranges::equal(a.arms, b.arms, [](const MemberArm& a, const MemberArm& b) {
            return a.answer == b.answer && a.layouts == b.layouts;
        });
    };
    AOT_TEST_CHECK(isSame(PackedMemberPartition(MemberPartition { }).unpack(), MemberPartition { }));

    MemberCondition unchanged { MemberCondition::Kind::MemberUnchanged, LayoutID(4u), ClassSide::Prototype, PropertyKeyID(9u) };
    MemberCondition evaluatedOnce { MemberCondition::Kind::ClassEvaluatedOnce, LayoutID(70000u), ClassSide::Static, { } };
    MemberPartition partition;
    partition.rest = { LayoutID(3u), LayoutID(900u) };
    partition.arms.append({ { .kind = MemberKind::Method, .level = KnowledgeLevel::Proven, .holder = LayoutID(7u), .method = FunctionID(0u) }, { LayoutID(7u) } });
    partition.arms.append({ { .kind = MemberKind::Accessor, .level = KnowledgeLevel::Expected, .holder = LayoutID(8u), .getter = FunctionID(12u), .setter = FunctionID(13u), .receiverProofs = { Proof::PrototypeLinkUnchanged, Proof::NoExtraProperties }, .conditions = { unchanged, evaluatedOnce }, .cause = UnprovenCause::PropertyRedefined }, { LayoutID(8u), LayoutID(9u) } });
    partition.arms.append({ { .kind = MemberKind::Field, .level = KnowledgeLevel::Proven, .holder = LayoutID(0u), .field = 5 }, { LayoutID(10u), LayoutID(11u), LayoutID(12u), LayoutID(13u), LayoutID(14u), LayoutID(15u) } });
    partition.arms.append({ { .kind = MemberKind::Field, .level = KnowledgeLevel::Proven }, { LayoutID(16u) } });
    partition.arms.append({ { .kind = MemberKind::Variable, .level = KnowledgeLevel::Proven, .variable = VariableID(2u) }, { LayoutID(17u) } });
    partition.arms.append({ { .kind = MemberKind::Namespace, .level = KnowledgeLevel::Proven, .module = ModuleID(1u) }, { LayoutID(18u) } });
    partition.arms.append({ { .kind = MemberKind::Builtin, .level = KnowledgeLevel::Expected, .builtin = BuiltinID(21u), .conditions = { unchanged } }, { LayoutID(19u) } });
    partition.arms.append({ { .kind = MemberKind::Absent, .level = KnowledgeLevel::Proven }, { LayoutID(20u) } });
    AOT_TEST_CHECK(isSame(PackedMemberPartition(partition).unpack(), partition));

    // A thousand classes with a method each.
    MemberPartition methods;
    for (uint32_t index = 0; index < 1000; ++index)
        methods.arms.append({ { .kind = MemberKind::Method, .level = KnowledgeLevel::Proven, .holder = LayoutID(index), .method = FunctionID(index) }, { LayoutID(index) } });
    PackedMemberPartition packed(methods);
    AOT_TEST_CHECK(isSame(packed.unpack(), methods));
    AOT_TEST_CHECK(packed.sizeInBytes() * 4 <= methods.arms.size() * sizeof(MemberArm));
}

static void testListsShareWhatTheyHeldWhenTheyWereCopied()
{
    WeakRandom random(1);
    Vector<std::pair<SharedPrefixList<uint32_t>, Vector<uint32_t>>> lists(1);
    for (unsigned step = 0; step < 400; ++step) {
        auto [list, expected] = lists[random.getUint32(lists.size())];
        for (unsigned count = random.getUint32(4); count--;) {
            uint32_t value = random.getUint32(3);
            list.append(value);
            expected.append(value);
        }
        if (!random.getUint32(3)) {
            const auto& [appended, expectedAppended] = lists[random.getUint32(lists.size())];
            list.appendList(appended);
            expected.appendVector(expectedAppended);
            if (random.getUint32(2)) {
                list.append(1);
                expected.append(1);
            }
        }
        AOT_TEST_CHECK(list.size() == expected.size() && list.isEmpty() == expected.isEmpty());
        size_t first = random.getUint32(expected.size() + 2);
        Vector<uint32_t> seen;
        AOT_TEST_CHECK(list.allOfFrom(first, [&](uint32_t value) {
            seen.append(value);
            return true;
        }));
        AOT_TEST_CHECK(std::ranges::equal(seen, expected.span().subspan(std::min(first, expected.size()))));
        AOT_TEST_CHECK(list.allOf([](uint32_t value) { return value < 2; }) == !expected.contains(2u));
        for (const auto& [other, expectedOther] : lists)
            AOT_TEST_CHECK((list == other) == (expected == expectedOther));
        lists.append({ WTF::move(list), WTF::move(expected) });
    }

    SharedPrefixList<uint32_t> longest;
    for (uint32_t index = 0; index < 1000000; ++index) {
        SharedPrefixList<uint32_t> copy = longest;
        longest.append(index);
    }
    AOT_TEST_CHECK(longest.size() == 1000000);

    SharedPrefixList<uint32_t> deepest;
    for (uint32_t index = 0; index < 1000000; ++index) {
        SharedPrefixList<uint32_t> outer;
        outer.append(index);
        outer.appendList(deepest);
        deepest = WTF::move(outer);
    }
    uint32_t next = 1000000;
    AOT_TEST_CHECK(deepest.size() == next && deepest.allOf([&](uint32_t value) { return value == --next; }));
    SharedPrefixList<uint32_t> sameValues;
    sameValues.append(999999);
    deepest.allOfFrom(1, [&](uint32_t value) {
        sameValues.append(value);
        return true;
    });
    AOT_TEST_CHECK(deepest == sameValues);
}

static void testHeapSortBehindTooManyPartitions()
{
    using Record = SortedRecord<2>;
    forEachListOfKeys([](const Vector<uint32_t>& keys) {
        if (keys.size() < 2)
            return;
        Vector<Record> sorted = createRecords<Record>(keys);
        Vector<Record> expected = sorted;
        std::ranges::less isLess;
        auto projection = &Record::key;
        PartitioningSteps<Record, std::ranges::less, decltype(projection)> steps(isLess, projection);
        char* first = reinterpret_cast<char*>(sorted.mutableSpan().data());
        heapSort(steps, first, first + sorted.size() * sizeof(Record));
        AOT_TEST_CHECK(std::ranges::is_sorted(sorted, { }, &Record::key));
#if defined(__GLIBCXX__)
        std::ranges::partial_sort(expected, expected.end(), { }, &Record::key);
        AOT_TEST_CHECK(sorted == expected);
#endif
    });

    // An order that answers so that every partition cuts off as little as it can (M. D. McIlroy, "A Killer Adversary for Quicksort").
    constexpr uint32_t size = 20000;
    constexpr uint32_t undecided = size - 1;
    Vector<uint32_t> values;
    values.fill(undecided, size);
    uint32_t numberDecided = 0;
    uint32_t candidate = 0;
    size_t numberOfQuestions = 0;
    Vector<uint32_t> indices(size, [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    AOT::sort(indices, [&](uint32_t first, uint32_t second) {
        ++numberOfQuestions;
        if (values[first] == undecided && values[second] == undecided)
            values[first == candidate ? first : second] = numberDecided++;
        if (values[first] == undecided)
            candidate = first;
        else if (values[second] == undecided)
            candidate = second;
        return values[first] < values[second];
    });
    AOT_TEST_CHECK(numberOfQuestions < 100 * size);
    Vector<Record> sorted = createRecords<Record>(values);
    Vector<Record> expected = sorted;
    AOT::sort(sorted, { }, &Record::key);
    AOT_TEST_CHECK(std::ranges::is_sorted(sorted, { }, &Record::key));
#if defined(__GLIBCXX__)
    std::ranges::sort(expected, { }, &Record::key);
    AOT_TEST_CHECK(sorted == expected);
#endif
}

static String textOf(size_t (*describe)(std::span<char>))
{
    std::array<char, 4096> buffer;
    return String::fromUTF8(std::span<const char>(buffer).first(describe(buffer)));
}

static void testCurrentWorkNamesFunctionAndStep()
{
    BuildContext build(makeDefaultCompileOptions());
    std::unique_ptr<Program> program = parseDerivedFunctions(build);
    AOT_TEST_CHECK(textOf(describeWhereThisThreadStopped).isEmpty());
    {
        PhaseTimes times(build, Stage::Optimize);
        AOT_TEST_CHECK(textOf(describeWhereThisThreadStopped) == "Stopped while compiling in Optimize.\n"_s);
        {
            CurrentWorkScope scope(*program, FunctionID(1u));
            currentWork().setPass(PassID::EliminateDeadCode);
            AOT_TEST_CHECK(textOf(describeWhereThisThreadStopped) == "Stopped while compiling second (a.ts:1) in EliminateDeadCode.\nTo build without that step, add --aot-disable=pass:EliminateDeadCode.\n"_s);
            AOT_TEST_CHECK(textOf(describeWhatThreadsAreCompiling) == "Still compiling second (a.ts:1) in EliminateDeadCode for 0 s.\n"_s);
            currentWork().startInSeconds.store(MonotonicTime::now().secondsSinceEpoch().value() - 90.5, std::memory_order_relaxed);
            AOT_TEST_CHECK(textOf(describeWhatThreadsAreCompiling) == "Still compiling second (a.ts:1) in EliminateDeadCode for 90 s.\nA build that stays this long in one function is stuck, which is a bug in Bun. Please report it with these lines: https://github.com/oven-sh/bun/issues\nTo build without that step, add --aot-disable=pass:EliminateDeadCode.\n"_s);
            // A pass that every build needs has no way around it.
            currentWork().setPass(PassID::GenerateCode);
            AOT_TEST_CHECK(textOf(describeWhereThisThreadStopped) == "Stopped while compiling second (a.ts:1) in GenerateCode.\n"_s);
            // A number that no function has.
            currentWork().setFunction(FunctionID(1000u));
            AOT_TEST_CHECK(textOf(describeWhereThisThreadStopped) == "Stopped while compiling in GenerateCode.\n"_s);
        }
        times.didFinish("the first part"_s);
        AOT_TEST_CHECK(textOf(describeWhereThisThreadStopped) == "Stopped while compiling in Optimize (after: the first part).\n"_s);
        {
            PhaseTimes timesOfPass(build, PassID::AnalyzeFlow);
            AOT_TEST_CHECK(textOf(describeWhereThisThreadStopped).startsWith("Stopped while compiling in AnalyzeFlow.\n"_s));
        }
        AOT_TEST_CHECK(textOf(describeWhereThisThreadStopped) == "Stopped while compiling in Optimize (after: the first part).\n"_s);
    }
    AOT_TEST_CHECK(textOf(describeWhereThisThreadStopped).isEmpty());
    AOT_TEST_CHECK(textOf(describeWhatThreadsAreCompiling).isEmpty());
}

static void testCurrentWorkNamesPhaseOfBackEnd()
{
    BuildContext build(makeDefaultCompileOptions());
    std::unique_ptr<Program> program = parseDerivedFunctions(build);
    B3::Procedure first(B3::Policy::fromProcessOptions());
    B3::Procedure second(B3::Policy::fromProcessOptions());
    first.setSubjectOfClient(1);
    second.setSubjectOfClient(2);
    CurrentWorkScope scope(*program, FunctionID());
    currentWork().setPass(PassID::GenerateCode);
    {
        B3::PhaseScope phase(second, "someB3Phase"_s);
        AOT_TEST_CHECK(textOf(describeWhereThisThreadStopped) == "Stopped while compiling second (a.ts:1) in GenerateCode (someB3Phase).\n"_s);
    }
    {
        B3::PhaseScope phase(first, "anotherB3Phase"_s);
        AOT_TEST_CHECK(textOf(describeWhereThisThreadStopped) == "Stopped while compiling first (a.ts:1) in GenerateCode (anotherB3Phase).\n"_s);
    }
    currentWork().leaveBackEnd();
    AOT_TEST_CHECK(textOf(describeWhereThisThreadStopped) == "Stopped while compiling first (a.ts:1) in GenerateCode.\n"_s);
}

static void testCurrentWorkOfPoolThreads()
{
    BuildContext build(optionsWith({ "threads=4"_s }));
    std::unique_ptr<Program> program = parseDerivedFunctions(build);
    PhaseTimes times(build, PassID::AnalyzeFlow);
    std::atomic<unsigned> numberOfThreadsAtWork { 0 };
    std::atomic<bool> isDescribed { false };
    std::array<String, 2> saidByThread;
    String saidOfAllThreads;
    // Both indices are at work at once, so they are on two threads, of which at most one is this one.
    forEachIndexInParallel(build.parallelLoops(), 2, [&](unsigned index) {
        saidByThread[index] = textOf(describeWhereThisThreadStopped);
        CurrentWorkScope scope(*program, FunctionID(index));
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
    for (const String& said : saidByThread)
        AOT_TEST_CHECK(said.startsWith("Stopped while compiling in AnalyzeFlow.\n"_s));
    AOT_TEST_CHECK(saidOfAllThreads.contains("Still compiling first (a.ts:1) in AnalyzeFlow for 0 s.\n"_s));
    AOT_TEST_CHECK(saidOfAllThreads.contains("Still compiling second (a.ts:1) in AnalyzeFlow for 0 s.\n"_s));
}

// One behind the other, and more of them than there are records.
static void testCurrentWorkOfThreadsBehindManyThatEnded()
{
    BuildContext build(makeDefaultCompileOptions());
    for (unsigned count = 0; count < 2000; ++count) {
        String said;
        runOnBuildThread([&] {
            PhaseTimes times(build, Stage::Optimize);
            said = textOf(describeWhereThisThreadStopped);
        });
        AOT_TEST_CHECK(said == "Stopped while compiling in Optimize.\n"_s);
    }
}

static void testCurrentWorkFitsItsBuffer()
{
    BuildContext build(makeDefaultCompileOptions());
    std::unique_ptr<Program> program = parseDerivedFunctions(build);
    CurrentWorkScope scope(*program, FunctionID(1u));
    currentWork().setPass(PassID::EliminateDeadCode);
    String whole = textOf(describeWhereThisThreadStopped);
    for (size_t capacity = 0; capacity <= whole.length(); ++capacity) {
        std::array<char, 256> buffer;
        buffer.fill('#');
        size_t length = describeWhereThisThreadStopped(std::span(buffer).first(capacity));
        AOT_TEST_CHECK(length == capacity && buffer[capacity] == '#');
        AOT_TEST_CHECK(String::fromUTF8(std::span<const char>(buffer).first(length)) == whole.left(capacity));
    }
}

namespace {

struct MallocOfMarkedBuffers {
    static constexpr uint64_t inUse = 0x6275666665722121;
    static constexpr size_t wordsInFront = 2;

    static void* malloc(size_t size)
    {
        auto* words = static_cast<uint64_t*>(FastMalloc::malloc(size + wordsInFront * sizeof(uint64_t)));
        words[0] = inUse;
        ++numberInUse;
        return words + wordsInFront;
    }
    static void* tryMalloc(size_t size) { return malloc(size); }
    static void* realloc(void* buffer, size_t size)
    {
        auto* words = static_cast<uint64_t*>(buffer) - wordsInFront;
        AOT_TEST_CHECK(words[0] == inUse);
        return static_cast<uint64_t*>(FastMalloc::realloc(words, size + wordsInFront * sizeof(uint64_t))) + wordsInFront;
    }
    static void free(void* buffer)
    {
        auto* words = static_cast<uint64_t*>(buffer) - wordsInFront;
        AOT_TEST_CHECK(words[0] == inUse);
        words[0] = 0;
        --numberInUse;
        FastMalloc::free(words);
    }
    static constexpr size_t nextCapacity(size_t capacity) { return FastMalloc::nextCapacity(capacity); }

    static thread_local size_t numberInUse;
};
thread_local size_t MallocOfMarkedBuffers::numberInUse = 0;

struct RecordWithPadding {
    uint64_t number;
    uint8_t byte;

    friend bool operator==(const RecordWithPadding&, const RecordWithPadding&) = default;
};

struct alignas(16) RecordAlignedToTwoWords {
    uint64_t number;

    friend bool operator==(const RecordAlignedToTwoWords&, const RecordAlignedToTwoWords&) = default;
};

struct RecordThatKnowsWhereItIs {
    explicit RecordThatKnowsWhereItIs(unsigned number)
        : number(number)
        , self(this)
    {
    }
    RecordThatKnowsWhereItIs(const RecordThatKnowsWhereItIs& other)
        : number(other.number)
        , self(this)
    {
        AOT_TEST_CHECK(other.self == &other);
    }
    ~RecordThatKnowsWhereItIs()
    {
        AOT_TEST_CHECK(self == this);
        self = nullptr;
    }
    RecordThatKnowsWhereItIs& operator=(const RecordThatKnowsWhereItIs& other)
    {
        AOT_TEST_CHECK(self == this && other.self == &other);
        number = other.number;
        return *this;
    }

    friend bool operator==(const RecordThatKnowsWhereItIs& a, const RecordThatKnowsWhereItIs& b) { return a.self == &a && b.self == &b && a.number == b.number; }

    unsigned number;
    RecordThatKnowsWhereItIs* self;
};

static_assert(movesAsBytes<uint8_t> && movesAsBytes<FunctionID> && movesAsBytes<RecordWithPadding> && movesAsBytes<RecordAlignedToTwoWords> && movesAsBytes<SortedRecord<9>> && movesAsBytes<String>);
static_assert(!VectorTraits<FunctionID>::canMoveWithMemcpy && !VectorTraits<RecordWithPadding>::canMoveWithMemcpy);
static_assert(!movesAsBytes<RecordThatKnowsWhereItIs> && !movesAsBytes<Vector<unsigned, 2>> && !movesAsBytes<std::pair<unsigned, RecordThatKnowsWhereItIs>>);

struct RecordOfLists {
    unsigned number;
    Vector<unsigned> list;
    std::array<Vector<uint8_t>, 2> more;

    friend bool operator==(const RecordOfLists&, const RecordOfLists&) = default;
};

struct RecordWithInlineList {
    unsigned number;
    Vector<unsigned, 2> list;

    friend bool operator==(const RecordWithInlineList&, const RecordWithInlineList&) = default;
};

struct RecordOfRecordThatKnowsWhereItIs {
    Vector<unsigned> list;
    RecordThatKnowsWhereItIs record;
};

static_assert(movesAsBytes<Vector<unsigned>> && movesAsBytes<Vector<RecordThatKnowsWhereItIs>> && !movesAsBytes<RecordWithInlineList> && !movesAsBytes<RecordOfRecordThatKnowsWhereItIs>);
#if COMPILER(CLANG) && __has_builtin(__builtin_structured_binding_size)
static_assert(movesAsBytes<RecordOfLists> && movesAsBytes<std::pair<unsigned, RecordOfLists>> && movesAsBytes<Vector<RecordOfLists>>);
#endif

struct RecordOfRecordsWithInlineLists {
    uint8_t flag : 1;
    Vector<RecordWithInlineList, 2> records;
    std::pair<unsigned, Vector<uint8_t, 3>> pair;
    Vector<unsigned> list;

    friend bool operator==(const RecordOfRecordsWithInlineLists&, const RecordOfRecordsWithInlineLists&) = default;
};

static_assert(!isMendedAfterMovingAsBytes<unsigned> && !isMendedAfterMovingAsBytes<RecordOfLists> && !isMendedAfterMovingAsBytes<RecordThatKnowsWhereItIs> && !isMendedAfterMovingAsBytes<Vector<RecordThatKnowsWhereItIs, 2>> && !isMendedAfterMovingAsBytes<RecordOfRecordThatKnowsWhereItIs>);
#if !ASAN_ENABLED && COMPILER(CLANG) && __has_builtin(__builtin_structured_binding_size)
static_assert(isMendedAfterMovingAsBytes<Vector<unsigned, 2>> && isMendedAfterMovingAsBytes<RecordWithInlineList> && isMendedAfterMovingAsBytes<RecordOfRecordsWithInlineLists> && isMendedAfterMovingAsBytes<Vector<Vector<unsigned, 2>, 3>>);
#endif

template<typename T, size_t inlineCapacity, size_t minCapacity>
void checkGrowthOfVector(const Invocable<T(unsigned)> auto& create)
{
    using VectorUnderTest = Vector<T, inlineCapacity, CrashOnOverflow, minCapacity, MallocOfMarkedBuffers>;
    using ExpectedVector = WTF::Vector<T, inlineCapacity, CrashOnOverflow, minCapacity>;
    constexpr unsigned numberOfElements = 2000;

    auto isInline = [](const VectorUnderTest& vector) {
        auto* start = reinterpret_cast<const char*>(&vector);
        auto* buffer = reinterpret_cast<const char*>(vector.span().data());
        return buffer >= start && buffer < start + sizeof(VectorUnderTest);
    };
    auto checkEqual = [&](const VectorUnderTest& vector, const ExpectedVector& expected) {
        AOT_TEST_CHECK(vector.size() == expected.size() && vector.capacity() >= vector.size());
        AOT_TEST_CHECK(std::ranges::equal(vector, expected));
        AOT_TEST_CHECK(isInline(vector) == (inlineCapacity && vector.capacity() == inlineCapacity));
    };
    auto onBoth = [&](VectorUnderTest& vector, ExpectedVector& expected, const auto& change) {
        change(vector);
        change(expected);
        checkEqual(vector, expected);
    };

    {
        VectorUnderTest vector;
        ExpectedVector expected;
        unsigned numberOfFullVectors = 0;
        for (unsigned number = 0; vector.size() < numberOfElements; ++number) {
            if (vector.size() < vector.capacity() || !vector.size()) {
                vector.append(T(create(number)));
                expected.append(T(create(number)));
                AOT_TEST_CHECK(vector.size() == expected.size() && vector.last() == expected.last());
                // The first buffer has room for minCapacity elements or 32 bytes, whichever is less.
                if (!inlineCapacity && vector.size() == 1)
                    AOT_TEST_CHECK(vector.capacity() == std::min<size_t>(minCapacity, std::max<size_t>(32 / sizeof(T), 1)));
                continue;
            }
            onBoth(vector, expected, [&](auto& list) {
                T outside(create(number));
                switch (numberOfFullVectors % 8) {
                case 0:
                    list.append(list.first());
                    break;
                case 1:
                    list.append(list.last());
                    break;
                case 2:
                    list.append(outside);
                    break;
                case 3:
                    list.append(WTF::move(outside));
                    break;
                case 4:
                    list.insert((list.size() + 1) / 2, list.first());
                    break;
                case 5:
                    list.append(ExpectedVector(list.span().subspan(list.size() / 2)).span());
                    break;
                case 6:
                    list.constructAndAppend(outside);
                    break;
                case 7:
                    list.insertFill(1, outside, 3);
                    break;
                }
            });
            ++numberOfFullVectors;
        }
        AOT_TEST_CHECK(numberOfFullVectors >= 8);

        VectorUnderTest copy = vector;
        AOT_TEST_CHECK(copy == vector && copy.capacity() == vector.size());
        VectorUnderTest moved = WTF::move(copy);
        AOT_TEST_CHECK(moved == vector && copy.isEmpty());
        auto shiftAndHandOver = [&](auto& list) {
            list.insert(0, T(create(3)));
            list.insert(list.size() / 2, T(create(4)));
            list.insert(list.size(), T(create(5)));
            list.removeAt(list.size() / 2);
            list.removeAt(0);
            list.insertFill(0, T(create(6)), 3);
            list.removeAt(1, 2);
            std::remove_reference_t<decltype(list)> taken = WTF::move(list);
            std::remove_reference_t<decltype(list)> copied = taken;
            list = WTF::move(copied);
            copied = taken;
            list.swap(copied);
            list.appendVector(taken);
            list.removeAt(list.size() - taken.size(), taken.size());
        };
        onBoth(vector, expected, shiftAndHandOver);
        onBoth(vector, expected, [&](auto& list) {
            list.reserveCapacity(list.capacity() + 1);
        });
        onBoth(vector, expected, [&](auto& list) {
            list.reserveCapacity(list.size());
        });
        onBoth(vector, expected, [&](auto& list) {
            list.shrink(inlineCapacity / 2 + 1);
            list.shrinkToFit();
        });
        onBoth(vector, expected, shiftAndHandOver);
        onBoth(vector, expected, [&](auto& list) {
            list.fill(T(create(7)), 3 * inlineCapacity + 40);
        });
        onBoth(vector, expected, [&](auto& list) {
            list.clear();
        });
        onBoth(vector, expected, [&](auto& list) {
            list.reserveInitialCapacity(inlineCapacity + 5);
            list.append(T(create(1)));
        });
    }
    AOT_TEST_CHECK(!MallocOfMarkedBuffers::numberInUse);
}

template<typename T>
void checkGrowthOfVectorsOf(const Invocable<T(unsigned)> auto& create)
{
    checkGrowthOfVector<T, 0, 16>(create);
    checkGrowthOfVector<T, 0, 1>(create);
    checkGrowthOfVector<T, 1, 16>(create);
    checkGrowthOfVector<T, 3, 2>(create);
    checkGrowthOfVector<T, 20, 16>(create);
}

} // anonymous namespace

static void testVectorsHoldWhatThoseOfWTFHold()
{
    auto createInteger = [](unsigned number) {
        return number;
    };
    checkGrowthOfVectorsOf<uint8_t>(createInteger);
    checkGrowthOfVectorsOf<uint16_t>(createInteger);
    checkGrowthOfVectorsOf<uint32_t>(createInteger);
    checkGrowthOfVectorsOf<uint64_t>(createInteger);
    checkGrowthOfVectorsOf<double>(createInteger);
    checkGrowthOfVectorsOf<FunctionID>([](unsigned number) {
        return FunctionID(number);
    });
    checkGrowthOfVectorsOf<std::pair<uint64_t, uint64_t>>([](unsigned number) {
        return std::pair<uint64_t, uint64_t> { number, ~static_cast<uint64_t>(number) };
    });
    checkGrowthOfVectorsOf<RecordWithPadding>([](unsigned number) {
        return RecordWithPadding { number, static_cast<uint8_t>(number) };
    });
    checkGrowthOfVectorsOf<RecordAlignedToTwoWords>([](unsigned number) {
        return RecordAlignedToTwoWords { number };
    });
    checkGrowthOfVectorsOf<SortedRecord<9>>([](unsigned number) {
        return SortedRecord<9> { number, { number + 1, number + 2, 0, 0, 0, 0, 0, 0, number + 9 } };
    });
    checkGrowthOfVectorsOf<String>([](unsigned number) {
        return String::number(number);
    });
    checkGrowthOfVectorsOf<RecordThatKnowsWhereItIs>([](unsigned number) {
        return RecordThatKnowsWhereItIs(number);
    });
    checkGrowthOfVectorsOf<Vector<unsigned>>([](unsigned number) {
        return Vector<unsigned> { number, number + 1 };
    });
    checkGrowthOfVectorsOf<RecordOfLists>([](unsigned number) {
        return RecordOfLists { number, { number, number + 1 }, { { { static_cast<uint8_t>(number) }, { } } } };
    });
    checkGrowthOfVectorsOf<RecordWithInlineList>([](unsigned number) {
        return RecordWithInlineList { number, Vector<unsigned, 2>(number % 4, [&](size_t index) {
            return number + static_cast<unsigned>(index);
        }) };
    });
    checkGrowthOfVectorsOf<RecordOfRecordsWithInlineLists>([](unsigned number) {
        auto createRecord = [&](size_t index) {
            unsigned first = number + static_cast<unsigned>(index);
            return RecordWithInlineList { first, Vector<unsigned, 2>(first % 4, [&](size_t position) {
                return first + static_cast<unsigned>(position);
            }) };
        };
        return RecordOfRecordsWithInlineLists { static_cast<uint8_t>(number & 1), Vector<RecordWithInlineList, 2>(number % 5, createRecord), { number, Vector<uint8_t, 3>(number % 6, [&](size_t position) {
            return static_cast<uint8_t>(number + position);
        }) }, { number } };
    });
    checkGrowthOfVectorsOf<Vector<unsigned, 2>>([](unsigned number) {
        return Vector<unsigned, 2>(number % 4, [&](size_t index) {
            return number + static_cast<unsigned>(index);
        });
    });
    checkGrowthOfVectorsOf<std::pair<unsigned, Vector<unsigned>>>([](unsigned number) {
        return std::pair<unsigned, Vector<unsigned>> { number, { number } };
    });
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
        { "function-queue/one-thread"_s, testFunctionQueueOnOneThread },
        { "function-queue/many-threads"_s, testFunctionQueueOnManyThreads },
        { "function-queue/behind-long-function"_s, testFunctionQueueBehindLongFunction },
        { "derived-functions/ids-from-many-threads"_s, testDerivedFunctionIDsFromManyThreads },
        { "function-editing/caches-last-as-long-as-what-runs-beside-edits"_s, testCachesOfProgramLastAsLongAsWhatRunsBesideEdits },
        { "diagnostics/order-by-text-position"_s, testDiagnosticsOrderByTextPosition },
        { "diagnostics/text-position-is-last-context-clause"_s, testTextPositionIsLastContextClause },
        { "diagnostics/two-lists"_s, testDiagnosticsKeepTwoLists },
        { "diagnostics/function-as-read-ends-derivation-chain"_s, testFunctionAsReadEndsDerivationChain },
        { "diagnostics/function-as-read-of-invalid-chain"_s, testFunctionAsReadOfInvalidChain },
        { "diagnostics/name-functions-as-read"_s, testDiagnosticsNameFunctionsAsRead },
        { "losses/declines-name-functions-as-read"_s, testDeclinesNameFunctionsAsRead },
        { "losses/declines-are-collected-on-request"_s, testDeclinesAreCollectedOnRequest },
        { "statistics/many-threads"_s, testStatisticsFromManyThreads },
        { "threads/sort-on-pool-of-lists-of-every-length"_s, testSortOnPoolOfListsOfEveryLength },
        { "current-work/names-function-and-step"_s, testCurrentWorkNamesFunctionAndStep },
        { "current-work/names-phase-of-back-end"_s, testCurrentWorkNamesPhaseOfBackEnd },
        { "current-work/of-pool-threads"_s, testCurrentWorkOfPoolThreads },
        { "current-work/of-threads-behind-many-that-ended"_s, testCurrentWorkOfThreadsBehindManyThatEnded },
        { "current-work/fits-its-buffer"_s, testCurrentWorkFitsItsBuffer },
        { "losses/lowest-subject"_s, testLossesKeepLowestSubject },
        { "losses/subject-with-location"_s, testLossesPreferSubjectWithLocation },
        { "sort/equal-elements"_s, testSortLeavesEqualElementsWhereTheStandardLibraryDoes },
        { "sort/integers-and-ids"_s, testSortOfIntegersAndIDs },
        { "sort/heap-sort-behind-too-many-partitions"_s, testHeapSortBehindTooManyPartitions },
        { "sorted-lists/short-list-is-looked-for-by-halving"_s, testShortSortedListIsLookedForByHalving },
        { "shared-collections/persistent-sets-hold-what-sorted-sets-hold"_s, testPersistentSetsHoldWhatSortedSetsHold },
        { "shared-collections/lists-share-what-they-held-when-they-were-copied"_s, testListsShareWhatTheyHeldWhenTheyWereCopied },
        { "property-access/packed-partition-unpacks-to-what-was-packed"_s, testPackedPartitionUnpacksToWhatWasPacked },
        { "vector/holds-what-that-of-wtf-holds"_s, testVectorsHoldWhatThoseOfWTFHold },
        { "enum-names/end-at-the-last-enumerator"_s, testNamesOfEnumeratorsEndAtTheLastOne },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
