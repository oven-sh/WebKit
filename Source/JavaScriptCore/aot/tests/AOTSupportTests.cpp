/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCompileOptionWords.h"
#include "AOTDerivedFunctions.h"
#include "AOTDiagnostics.h"
#include "AOTFlowResults.h"
#include "AOTFunctionQueue.h"
#include "AOTIR.h"
#include "AOTIRParser.h"
#include "AOTInterningPhases.h"
#include "AOTLimits.h"
#include "AOTLosses.h"
#include "AOTNativeTest.h"
#include "AOTPassID.h"
#include "AOTSchemaEnums.h"
#include "AOTStatistics.h"
#include "AOTThreads.h"
#include "AOTWire.h"
#include "LineColumn.h"
#include <algorithm>
#include <initializer_list>
#include <memory>
#include <numeric>
#include <optional>
#include <wtf/StdLibExtras.h>

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

// The first function finds one that takes long and many small ones. The long one begins first, on the thread that holds a part of the small ones,
// and does not end before all of them have been processed.
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

std::span<const NativeTest> supportTests()
{
    static constexpr NativeTest tests[] = {
        { "function-queue/one-thread"_s, testFunctionQueueOnOneThread },
        { "function-queue/many-threads"_s, testFunctionQueueOnManyThreads },
        { "function-queue/behind-long-function"_s, testFunctionQueueBehindLongFunction },
        { "derived-functions/ids-from-many-threads"_s, testDerivedFunctionIDsFromManyThreads },
        { "diagnostics/order-by-text-position"_s, testDiagnosticsOrderByTextPosition },
        { "diagnostics/text-position-is-last-context-clause"_s, testTextPositionIsLastContextClause },
        { "diagnostics/two-lists"_s, testDiagnosticsKeepTwoLists },
        { "diagnostics/function-as-read-ends-derivation-chain"_s, testFunctionAsReadEndsDerivationChain },
        { "diagnostics/function-as-read-of-invalid-chain"_s, testFunctionAsReadOfInvalidChain },
        { "diagnostics/name-functions-as-read"_s, testDiagnosticsNameFunctionsAsRead },
        { "losses/declines-name-functions-as-read"_s, testDeclinesNameFunctionsAsRead },
        { "losses/declines-are-collected-on-request"_s, testDeclinesAreCollectedOnRequest },
        { "statistics/many-threads"_s, testStatisticsFromManyThreads },
        { "losses/lowest-subject"_s, testLossesKeepLowestSubject },
        { "losses/subject-with-location"_s, testLossesPreferSubjectWithLocation },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
