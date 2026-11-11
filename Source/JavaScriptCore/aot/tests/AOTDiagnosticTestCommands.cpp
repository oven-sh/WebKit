/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSupportTestCommands.h"

#if ENABLE(AOT)

#include "AOTDiagnostics.h"
#include "AOTGroupingOnPool.h"
#include "AOTLimits.h"
#include "AOTSchemaEnums.h"
#include "AOTSort.h"
#include "AOTSystemResources.h"
#include "AOTTestFile.h"
#include "AOTTestHarness.h"
#include "AOTThreads.h"
#include "AOTTypeTable.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <iterator>
#include <ranges>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/OptionSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>
#include <wtf/Variant.h>
#include <wtf/WeakRandom.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct RecordWord {
    StringView kind { };
    FunctionID function { };
    BlockID block { };
    ValueID value { };
    LayoutID layout { };
    LimitID limit { LimitID::None };
    uint32_t count { 0 };
    SourceLocation location { noSourceLocation };
};

enum class RecordPart : uint8_t {
    Function = 1 << 0,
    Block = 1 << 1,
    Value = 1 << 2,
    Layout = 1 << 3,
    Limit = 1 << 4,
    Count = 1 << 5,
    Location = 1 << 6,
};

struct RecordPartSyntax {
    ASCIILiteral prefix;
    RecordPart part;
};

struct IDText {
    char letter;
    uint32_t index;

    void dump(PrintStream& out) const
    {
        if (index == Wire::none)
            out.print('-');
        else
            out.print(letter, index);
    }
};

struct LocationText {
    SourceLocation location;

    void dump(PrintStream& out) const
    {
        if (!hasModule(location))
            out.print('-');
        else
            out.print(location.module, ':', location.position);
    }
};

using AnyDiagnostic = Variant<UserDiagnostic, InternalError>;

struct DiagnosticList {
    CommonTestWords commonWords;
    const Program* program { nullptr };
    Vector<AnyDiagnostic> records;
};

} // anonymous namespace

static constexpr RecordPartSyntax recordPartSyntaxes[] = {
    { "f"_s, RecordPart::Function },
    { "b"_s, RecordPart::Block },
    { "v"_s, RecordPart::Value },
    { "L"_s, RecordPart::Layout },
    { "limit="_s, RecordPart::Limit },
    { "count="_s, RecordPart::Count },
    { "at="_s, RecordPart::Location },
};

template<typename Target, typename T>
static bool assignIfParsed(Target& target, const std::optional<T>& parsed)
{
    if (!parsed)
        return false;
    target = *parsed;
    return true;
}

template<typename IDType>
static std::optional<IDType> parseID(StringView text)
{
    auto index = parseTestNumber<uint32_t>(text);
    if (!index || *index == Wire::none)
        return std::nullopt;
    return IDType(*index);
}

static std::optional<SourceLocation> parseLocation(StringView text)
{
    size_t colon = text.find(':');
    if (colon == notFound)
        return std::nullopt;
    auto module = parseTestNumber<uint32_t>(text.left(colon));
    auto position = parseTestNumber<uint32_t>(text.substring(colon + 1));
    if (!module || *module == Wire::none || !position)
        return std::nullopt;
    return SourceLocation { *module, *position };
}

static bool parseRecordPart(RecordPart part, StringView text, RecordWord& record)
{
    switch (part) {
    case RecordPart::Function:
        return assignIfParsed(record.function, parseID<FunctionID>(text));
    case RecordPart::Block:
        return assignIfParsed(record.block, parseID<BlockID>(text));
    case RecordPart::Value:
        return assignIfParsed(record.value, parseID<ValueID>(text));
    case RecordPart::Layout:
        return assignIfParsed(record.layout, parseID<LayoutID>(text));
    case RecordPart::Limit:
        return assignIfParsed(record.limit, parseEnum<LimitID>(text));
    case RecordPart::Count:
        return assignIfParsed(record.count, parseTestNumber<uint32_t>(text));
    case RecordPart::Location:
        return assignIfParsed(record.location, parseLocation(text));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<RecordWord> parseRecordWord(StringView word)
{
    RecordWord record;
    OptionSet<RecordPart> seenParts;
    bool isKind = true;
    for (StringView text : word.splitAllowingEmptyEntries(',')) {
        if (std::exchange(isKind, false)) {
            record.kind = text;
            continue;
        }
        auto* syntax = std::ranges::find_if(recordPartSyntaxes, [&](const RecordPartSyntax& candidate) {
            return text.startsWith(candidate.prefix);
        });
        if (syntax == std::end(recordPartSyntaxes) || seenParts.contains(syntax->part))
            return std::nullopt;
        seenParts.add(syntax->part);
        if (!parseRecordPart(syntax->part, text.substring(syntax->prefix.length()), record))
            return std::nullopt;
    }
    return record;
}

static UserDiagnostic makeUserDiagnostic(UserDiagnosticKind kind, const RecordWord& record)
{
    return {
        .kind = kind,
        .function = record.function,
        .block = record.block,
        .value = record.value,
        .layout = record.layout,
        .limit = record.limit,
        .count = record.count,
        .location = record.location,
    };
}

static InternalError makeInternalError(InternalErrorKind kind, const RecordWord& record)
{
    return {
        .kind = kind,
        .function = record.function,
        .block = record.block,
        .value = record.value,
        .layout = record.layout,
        .limit = record.limit,
        .count = record.count,
        .location = record.location,
    };
}

static std::optional<AnyDiagnostic> parseDiagnostic(StringView word)
{
    auto record = parseRecordWord(word);
    if (!record)
        return std::nullopt;
    auto name = parseDiagnosticName(record->kind);
    if (!name)
        return std::nullopt;
    return WTF::switchOn(*name,
        [&](UserDiagnosticKind kind) -> AnyDiagnostic {
            return makeUserDiagnostic(kind, *record);
        },
        [&](InternalErrorKind kind) -> AnyDiagnostic {
            return makeInternalError(kind, *record);
        });
}

static std::optional<DiagnosticList> parseDiagnosticList(TestCommandContext& context)
{
    DiagnosticList list;
    list.program = context.program.get();
    for (const String& word : context.arguments) {
        if (takeCommonTestWord(word, list.commonWords))
            continue;
        if (word == "no-program"_s) {
            list.program = nullptr;
            continue;
        }
        auto record = parseDiagnostic(word);
        if (!record) {
            printBadArgument(context.out, word);
            return std::nullopt;
        }
        list.records.append(*record);
    }
    shuffle(list.records, list.commonWords.seed);
    return list;
}

template<typename Record>
static void printDiagnosticLine(PrintStream& out, const Record& record, ASCIILiteral lastColumn, const String& message)
{
    out.println(nameOf(record.kind), ' ',
        IDText { 'f', record.function.index() }, ' ',
        IDText { 'b', record.block.index() }, ' ',
        IDText { 'v', record.value.index() }, ' ',
        IDText { 'L', record.layout.index() }, ' ',
        nameOf(record.limit), ' ', record.count, ' ',
        LocationText { record.location }, ' ',
        lastColumn, " | "_s, message);
}

static void printDiagnostic(PrintStream& out, const UserDiagnostic& diagnostic, const String& message)
{
    printDiagnosticLine(out, diagnostic, nameOf(severityOf(diagnostic.kind)), message);
}

static void printDiagnostic(PrintStream& out, const InternalError& error, const String& message)
{
    printDiagnosticLine(out, error, error.operation.isNull() ? "-"_s : error.operation, message);
}

static void checkTemplate(PrintStream& out, ASCIILiteral kind, const String& message, unsigned& numberOfBadTemplates)
{
    if (!message.isEmpty() && !message.contains('{') && !message.contains('}'))
        return;
    ++numberOfBadTemplates;
    out.println("bad template "_s, kind, ": "_s, message);
}

static void runDiagnosticTemplates(PrintStream& out)
{
    out.println("empty sink has errors: "_s, yesOrNo(Diagnostics().hasErrors()));

    const RecordWord fullRecord {
        .function = FunctionID(0u),
        .block = BlockID(0u),
        .value = ValueID(0u),
        .layout = LayoutID(0u),
        .limit = LimitID::MaxWorkers,
        .count = 1,
        .location = { 0, 0 },
    };
    unsigned numberOfBadTemplates = 0;
    bool errorsMatchSeverity = true;
    for (UserDiagnosticKind kind : allMembersOf<UserDiagnosticKind>) {
        Diagnostics sink;
        sink.add(makeUserDiagnostic(kind, fullRecord));
        if (sink.hasErrors() != failsBuild(kind))
            errorsMatchSeverity = false;
        sink.finish(nullptr);
        checkTemplate(out, nameOf(kind), sink.messageOfUserDiagnostic(0), numberOfBadTemplates);
    }
    for (InternalErrorKind kind : allMembersOf<InternalErrorKind>) {
        Diagnostics sink;
        sink.add(makeInternalError(kind, fullRecord));
        if (!sink.hasErrors())
            errorsMatchSeverity = false;
        sink.finish(nullptr);
        checkTemplate(out, nameOf(kind), sink.messageOfInternalError(0), numberOfBadTemplates);
    }
    out.println("templates: "_s, numberOfEnumerators<UserDiagnosticKind>, " user kinds, "_s, numberOfEnumerators<InternalErrorKind>, " internal kinds, "_s, numberOfBadTemplates, " bad"_s);
    printCheck(out, "hasErrors agrees with severity"_s, errorsMatchSeverity);
}

void runDiagnosticsTestCommand(TestCommandContext& context)
{
    if (isOnlyWord(context.arguments, "templates"_s))
        return runDiagnosticTemplates(context.out);

    auto list = parseDiagnosticList(context);
    if (!list)
        return;

    Diagnostics sink;
    ParallelLoops loops(list->commonWords.threads);
    forEachIndexInParallel(loops, static_cast<unsigned>(list->records.size()), [&](unsigned index) {
        WTF::switchOn(list->records[index], [&]<typename Record>(const Record& record) {
            if (index % 2)
                sink.append(Vector<Record> { record });
            else
                sink.add(record);
        });
    });

    PrintStream& out = context.out;
    out.println("has errors: "_s, yesOrNo(sink.hasErrors()));
    sink.finish(list->program);

    const Vector<UserDiagnostic>& userDiagnostics = sink.userDiagnostics();
    out.println("user records "_s, userDiagnostics.size());
    for (size_t index = 0; index < userDiagnostics.size(); ++index)
        printDiagnostic(out, userDiagnostics[index], sink.messageOfUserDiagnostic(index));

    const Vector<InternalError>& internalErrors = sink.internalErrors();
    out.println("internal records "_s, internalErrors.size());
    for (size_t index = 0; index < internalErrors.size(); ++index)
        printDiagnostic(out, internalErrors[index], sink.messageOfInternalError(index));

    out.println("of "_s, list->records.size(), " added"_s);
}

static bool visitsEachIndexOnce(const ParallelLoops& loops, unsigned count)
{
    Vector<std::atomic<uint8_t>> visits(count);
    for (auto& visit : visits)
        visit.store(0, std::memory_order_relaxed);
    forEachIndexInParallel(loops, count, [&](unsigned index) {
        visits[index].fetch_add(1, std::memory_order_relaxed);
    });
    return std::ranges::all_of(visits, [](const std::atomic<uint8_t>& visit) {
        return visit.load(std::memory_order_relaxed) == 1;
    });
}

static bool sumsNestedLoops(const ParallelLoops& loops)
{
    constexpr unsigned outerCount = 8;
    constexpr unsigned innerCount = 1000;
    std::atomic<unsigned> sum { 0 };
    forEachIndexInParallel(loops, outerCount, [&](unsigned) {
        forEachIndexInParallel(loops, innerCount, [&](unsigned) {
            sum.fetch_add(1, std::memory_order_relaxed);
        });
    });
    return sum.load() == outerCount * innerCount;
}

static bool collectsInIndexOrder(const ParallelLoops& loops, unsigned count)
{
    auto collect = [](unsigned index, Vector<uint32_t>& list) {
        if (index % 3)
            return;
        list.append(index);
        list.append(index + 1);
    };
    Vector<uint32_t> expected;
    for (unsigned index = 0; index < count; ++index)
        collect(index, expected);
    return collectInParallel<uint32_t>(loops, count, collect) == expected;
}

static bool findsInIndexOrder(const ParallelLoops& loops)
{
    constexpr unsigned count = 1000;
    auto matches = [](unsigned index) {
        return !(index % 7);
    };
    Vector<uint32_t> expected;
    for (unsigned index = 0; index < count; ++index) {
        if (matches(index))
            expected.append(index);
    }
    return findIndicesInParallel(loops, count, matches) == expected;
}

static bool sortsAsOnOneThread(const ParallelLoops& loops, size_t count, const Invocable<uint32_t(size_t)> auto& keyAt)
{
    using Element = std::pair<uint32_t, uint32_t>;
    auto isLess = [](const Element& a, const Element& b) {
        return a.first < b.first;
    };
    Vector<Element> list(count, [&](size_t index) {
        return Element { keyAt(index), static_cast<uint32_t>(index) };
    });
    Vector<Element> expected = list;
    AOT::sort(expected, isLess);
    parallelSort(loops, list.mutableSpan(), isLess);
    return list == expected;
}

static bool sortsAsOnOneThread(const ParallelLoops& loops, size_t count)
{
    WeakRandom random(1);
    Vector<uint32_t> keys(count, [&](size_t) {
        return random.getUint32();
    });
    return sortsAsOnOneThread(loops, count, [&](size_t index) { return keys[index]; })
        && sortsAsOnOneThread(loops, count, [&](size_t index) { return keys[index] % 16; })
        && sortsAsOnOneThread(loops, count, [&](size_t index) { return static_cast<uint32_t>((count - index) / 1000); })
        && sortsAsOnOneThread(loops, count, [](size_t index) { return static_cast<uint32_t>(index / 1000); })
        && sortsAsOnOneThread(loops, count, [](size_t) { return 7u; });
}

static bool sortsAndRemovesDuplicates(const ParallelLoops& loops, unsigned count)
{
    WeakRandom random(1);
    Vector<uint32_t> list(count, [&](size_t) {
        return random.getUint32(count / 4);
    });
    Vector<uint32_t> expected = list;
    AOT::sort(expected);
    expected.shrink(std::ranges::unique(expected).begin() - expected.begin());
    sortAndRemoveDuplicates(loops, list);
    return list == expected;
}

static bool sortsRowsByGroupAsOnOneThread(const ParallelLoops& loops, size_t count, uint32_t numberOfGroups)
{
    WeakRandom random(1);
    Vector<uint64_t> rows(count, [&](size_t index) {
        uint32_t group = index % 4 ? random.getUint32(numberOfGroups) : numberOfGroups / 2;
        return static_cast<uint64_t>(group) << 32 | index;
    });
    Vector<uint64_t> expected = rows;
    AOT::sort(expected);
    sortRowsByGroup(loops, rows, numberOfGroups);
    if (rows != expected)
        return false;

    Vector<uint32_t> expectedFirstRows;
    size_t row = 0;
    for (uint32_t group = 0; group <= numberOfGroups; ++group) {
        while (row < count && rows[row] >> 32 < group)
            ++row;
        expectedFirstRows.append(static_cast<uint32_t>(row));
    }
    return expectedFirstRows == firstRowsOfGroups(loops, numberOfGroups, count, [&](size_t index) {
        return static_cast<uint32_t>(rows[index] >> 32);
    });
}

static bool runsEveryTask(const ParallelLoops& loops)
{
    constexpr unsigned count = 1000;
    std::array<std::atomic<unsigned>, 3> sums { };
    auto countInto = [&](std::atomic<unsigned>& sum) {
        forEachIndexInParallel(loops, count, [&](unsigned) {
            sum.fetch_add(1, std::memory_order_relaxed);
        });
    };
    runTasksInParallel(loops, {
        [&] { countInto(sums[0]); },
        [&] { countInto(sums[1]); },
        [&] { countInto(sums[2]); },
    });
    return std::ranges::all_of(sums, [](const std::atomic<unsigned>& sum) {
        return sum.load() == count;
    });
}

static bool knowsWhetherThreadIsInsideLoop(const ParallelLoops& loops)
{
    constexpr unsigned count = 1000;
    std::atomic<bool> isRight { true };
    auto expect = [&](bool isInsideLoop) {
        if (ParallelLoops::isInsideLoop() != isInsideLoop)
            isRight = false;
    };
    auto expectInLoop = [&](unsigned numberOfIndices, bool isInsideLoop) {
        forEachIndexInParallel(loops, numberOfIndices, [&](unsigned) {
            expect(isInsideLoop);
        });
    };
    expect(false);
    expectInLoop(1, false);
    expectInLoop(count, true);
    expect(false);
    forEachIndexInParallel(loops, count, [&](unsigned) {
        expectInLoop(1, true);
        expect(true);
    });
    expect(false);
    runTasksInParallel(loops, {
        [&] {
            expectInLoop(count, true);
            expect(true);
        },
        [&] { expect(true); },
    });
    expect(false);
    runTasksInParallel(loops, {
        [&] { expect(false); },
    });
    return isRight;
}

static bool runsSeriallyOnCaller(const ParallelLoops& loops)
{
    constexpr unsigned count = 100000;
    const Thread& caller = Thread::currentSingleton();
    std::atomic<bool> staysOnCaller { true };
    Vector<unsigned> visited;
    visited.reserveInitialCapacity(count);
    forEachIndexInParallel(loops, count, [&](unsigned index) {
        if (&Thread::currentSingleton() != &caller) {
            staysOnCaller = false;
            return;
        }
        visited.append(index);
    });
    return loops.width() == 1 && staysOnCaller && std::ranges::equal(visited, std::views::iota(0u, count));
}

static bool handsOutWorkerNumbers(const ParallelLoops& loops)
{
    constexpr unsigned count = 256;
    constexpr unsigned numberOfSpins = 1000;

    Vector<std::atomic<bool>> inUse(loops.width());
    for (auto& isInUse : inUse)
        isInUse.store(false, std::memory_order_relaxed);
    std::atomic<bool> numbersStayBelowWidth { true };
    std::atomic<bool> numbersAreUnique { true };
    forEachIndexInParallel(loops, count, [&](unsigned, unsigned worker) {
        if (worker >= loops.width()) {
            numbersStayBelowWidth = false;
            return;
        }
        std::atomic<bool>& isInUse = inUse[worker];
        if (isInUse.exchange(true))
            numbersAreUnique = false;
        for (unsigned spin = 0; spin < numberOfSpins; ++spin) {
            if (!isInUse.load())
                numbersAreUnique = false;
        }
        isInUse.store(false);
    });
    return numbersStayBelowWidth && numbersAreUnique;
}

static bool nestedLoopKeepsWorkerNumber(const ParallelLoops& loops)
{
    constexpr unsigned outerCount = 256;
    constexpr unsigned innerCount = 4;
    const Thread& caller = Thread::currentSingleton();
    std::atomic<bool> callerAloneIsWorkerZero { true };
    std::atomic<bool> numbersAreKept { true };
    forEachIndexInParallel(loops, outerCount, [&](unsigned, unsigned worker) {
        if ((&Thread::currentSingleton() == &caller) != !worker)
            callerAloneIsWorkerZero = false;
        forEachIndexInParallel(loops, innerCount, [&](unsigned, unsigned workerOfNestedLoop) {
            if (workerOfNestedLoop != worker)
                numbersAreKept = false;
        });
    });
    return callerAloneIsWorkerZero && numbersAreKept;
}

static bool currentThreadHasWorkerStack()
{
    return Thread::currentSingleton().stack().size() >= ParallelLoops::workerStackSize / 2;
}

static bool buildThreadHasWorkerStack()
{
    bool hasWorkerStack = false;
    runOnBuildThread([&] {
        hasWorkerStack = currentThreadHasWorkerStack();
    });
    return hasWorkerStack;
}

static bool poolThreadsHaveWorkerStack(const ParallelLoops& loops)
{
    const Thread& caller = Thread::currentSingleton();
    std::atomic<bool> haveWorkerStack { true };
    std::atomic<bool> hasSeenPoolThread { false };
    std::atomic<int> yieldsLeft { 400 };
    forEachIndexInParallel(loops, 4 * loops.width(), [&](unsigned) {
        if (&Thread::currentSingleton() != &caller) {
            hasSeenPoolThread = true;
            if (!currentThreadHasWorkerStack())
                haveWorkerStack = false;
            return;
        }
        while (loops.width() > 1 && !hasSeenPoolThread && yieldsLeft.fetch_sub(1) > 0)
            Thread::yield();
    });
    return haveWorkerStack;
}

namespace {

struct ScheduledCall {
    void (*run)(void* argument);
    void* argument;
};

struct HeldCalls {
    Lock lock;
    Vector<ScheduledCall> calls WTF_GUARDED_BY_LOCK(lock);
};

} // anonymous namespace

static HeldCalls& heldCalls()
{
    static NeverDestroyed<HeldCalls> calls;
    return calls;
}

static Vector<ScheduledCall> takeHeldCalls()
{
    Locker locker { heldCalls().lock };
    return std::exchange(heldCalls().calls, { });
}

static constexpr HostThreadPool poolThatHoldsCalls { [](void (*run)(void*), void* argument) {
    Locker locker { heldCalls().lock };
    heldCalls().calls.append({ run, argument });
} };

constinit const HostThreadPool poolOfNewThreads { [](void (*run)(void*), void* argument) {
    startBuildThread("AOT test helper"_s, [run, argument] {
        run(argument);
    })->detach();
} };

static bool runsOnHostPool(unsigned width)
{
    ParallelLoops loops(width, &poolOfNewThreads);
    return visitsEachIndexOnce(loops, 100000) && sumsNestedLoops(loops) && handsOutWorkerNumbers(loops) && nestedLoopKeepsWorkerNumber(loops);
}

static bool asksForNoHelperForOneIndex()
{
    ParallelLoops loops(4, &poolThatHoldsCalls);
    return visitsEachIndexOnce(loops, 0) && visitsEachIndexOnce(loops, 1) && takeHeldCalls().isEmpty();
}

static bool lateHelperDoesNothing()
{
    ParallelLoops loops(4, &poolThatHoldsCalls);
    std::atomic<unsigned> visits { 0 };
    forEachIndexInParallel(loops, 1000, [&](unsigned) {
        visits.fetch_add(1, std::memory_order_relaxed);
    });
    Vector<ScheduledCall> calls = takeHeldCalls();
    for (ScheduledCall call : calls)
        call.run(call.argument);
    return calls.size() == 1 && visits.load() == 1000 && takeHeldCalls().isEmpty();
}

static bool asksForNoMoreHelpersThanIndices()
{
    constexpr unsigned count = 3;
    std::atomic<unsigned> visits { 0 };
    unsigned numberOfCalls = 0;
    runOnBuildThread([&] {
        ParallelLoops loops(16, &poolThatHoldsCalls);
        forEachIndexInParallel(loops, count, [&](unsigned) {
            if (visits.fetch_add(1))
                return;
            for (Vector<ScheduledCall> calls = takeHeldCalls(); !calls.isEmpty(); calls = takeHeldCalls()) {
                numberOfCalls += static_cast<unsigned>(calls.size());
                for (ScheduledCall call : calls)
                    call.run(call.argument);
            }
        });
    });
    return visits.load() == count && numberOfCalls && numberOfCalls <= count - 1 && takeHeldCalls().isEmpty();
}

static bool lanesRunSideBySide(const ParallelLoops& loops)
{
    if (loops.width() == 1)
        return true;
    constexpr Seconds longestWait = 30_s;
    std::array<std::atomic<bool>, 2> hasBegun { };
    std::array<bool, 2> hasMetOther { };
    auto meet = [&](unsigned lane) {
        hasBegun[lane].store(true);
        MonotonicTime deadline = MonotonicTime::now() + longestWait;
        while (!hasBegun[1 - lane].load() && MonotonicTime::now() < deadline)
            Thread::yield();
        hasMetOther[lane] = hasBegun[1 - lane].load();
    };
    runLanes(loops, {
        [&] { meet(0); },
        [&] { meet(1); },
    });
    return hasMetOther[0] && hasMetOther[1];
}

static bool lanesHaveLoopsOfTheirOwn(const ParallelLoops& loops)
{
    constexpr unsigned count = 20000;
    constexpr unsigned numberOfLanes = 4;
    const Thread& starter = Thread::currentSingleton();
    std::array<std::atomic<unsigned>, numberOfLanes> visits { };
    std::atomic<bool> isRight { true };
    runLanes(loops, numberOfLanes, [&](unsigned lane, const ParallelLoops& loopsOfLane) {
        bool isOnStarter = &Thread::currentSingleton() == &starter;
        if (&loops.ofCurrentThread() != &loopsOfLane || isOnStarter != (&loopsOfLane == &loops) || loopsOfLane.width() != loops.width() || ParallelLoops::isInsideLoop())
            isRight = false;
        forEachIndexInParallel(loopsOfLane, count, [&](unsigned, unsigned worker) {
            if (worker >= loopsOfLane.width() || &loops.ofCurrentThread() != &loopsOfLane)
                isRight = false;
            visits[lane].fetch_add(1, std::memory_order_relaxed);
        });
        runOnBuildThread([&] {
            if (&loops.ofCurrentThread() != &loopsOfLane)
                isRight = false;
        });
    });
    return isRight && std::ranges::all_of(visits, [](const std::atomic<unsigned>& visitsOfLane) {
        return visitsOfLane.load() == count;
    });
}

static bool lanesRunInOrderAtWidthOne(const ParallelLoops& one)
{
    Vector<unsigned, 3> order;
    runLanes(one, {
        [&] { order.append(0); },
        [&] { order.append(1); },
        [&] { order.append(2); },
    });
    return order == Vector<unsigned, 3> { 0u, 1u, 2u };
}

static bool callerRunsLanesThatNoThreadTakes()
{
    ParallelLoops loops(4, &poolThatHoldsCalls);
    std::atomic<unsigned> runs { 0 };
    runLanes(loops, {
        [&] { runs.fetch_add(1); },
        [&] { runs.fetch_add(1); },
        [&] { runs.fetch_add(1); },
    });
    Vector<ScheduledCall> calls = takeHeldCalls();
    bool everyLaneHasRun = runs.load() == 3;
    for (ScheduledCall call : calls)
        call.run(call.argument);
    return everyLaneHasRun && calls.size() == 2 && runs.load() == 3 && takeHeldCalls().isEmpty();
}

void runParallelLoopsTestCommand(TestCommandContext& context)
{
    static constexpr unsigned loopCounts[] = { 0, 1, 2, 255, 256, 257, 100000 };

    auto words = parseCommonTestWords(context.out, context.arguments);
    if (!words)
        return;

    PrintStream& out = context.out;
    ParallelLoops loops(words->threads);
    ParallelLoops one(1);
    printCheck(out, "each index once"_s, std::ranges::all_of(loopCounts, [&](unsigned count) {
        return visitsEachIndexOnce(loops, count);
    }));
    printCheck(out, "nested loops"_s, sumsNestedLoops(loops));
    printCheck(out, "collect"_s, collectsInIndexOrder(loops, 1000) && collectsInIndexOrder(loops, 20000));
    printCheck(out, "find"_s, findsInIndexOrder(loops));
    printCheck(out, "sort"_s, sortsAsOnOneThread(loops, 1000) && sortsAsOnOneThread(loops, 20000) && sortsAsOnOneThread(loops, 100003));
    printCheck(out, "sort and remove duplicates"_s, sortsAndRemovesDuplicates(loops, 1000) && sortsAndRemovesDuplicates(loops, 20000));
    printCheck(out, "rows by group"_s, sortsRowsByGroupAsOnOneThread(loops, 0, 1) && sortsRowsByGroupAsOnOneThread(loops, 1000, 7) && sortsRowsByGroupAsOnOneThread(loops, 20000, 1) && sortsRowsByGroupAsOnOneThread(loops, 20000, 2048)
        && sortsRowsByGroupAsOnOneThread(loops, 100003, 2049) && sortsRowsByGroupAsOnOneThread(loops, 100003, 5000000) && sortsRowsByGroupAsOnOneThread(loops, 1500000, 70000));
    printCheck(out, "tasks"_s, runsEveryTask(loops));
    printCheck(out, "a thread knows whether it is inside a loop"_s, knowsWhetherThreadIsInsideLoop(loops) && knowsWhetherThreadIsInsideLoop(one));
    printCheck(out, "width one is serial"_s, runsSeriallyOnCaller(one));
    printCheck(out, "worker numbers are below the width and unique at one time"_s, handsOutWorkerNumbers(loops));
    printCheck(out, "a nested loop keeps the worker number"_s, nestedLoopKeepsWorkerNumber(loops) && nestedLoopKeepsWorkerNumber(one));
    printCheck(out, "build thread has the worker stack size"_s, buildThreadHasWorkerStack());
    printCheck(out, "pool threads have the worker stack size"_s, poolThreadsHaveWorkerStack(loops));
    printCheck(out, "loops run on a host pool"_s, runsOnHostPool(words->threads));
    printCheck(out, "one index asks for no helper"_s, asksForNoHelperForOneIndex());
    printCheck(out, "a helper that comes late does nothing"_s, lateHelperDoesNothing());
    printCheck(out, "no more helpers than indices"_s, asksForNoMoreHelpersThanIndices());
    ParallelLoops onHostPool(words->threads, &poolOfNewThreads);
    printCheck(out, "lanes run side by side"_s, lanesRunSideBySide(loops) && lanesRunSideBySide(onHostPool));
    printCheck(out, "a lane has loops of its own"_s, lanesHaveLoopsOfTheirOwn(loops) && lanesHaveLoopsOfTheirOwn(onHostPool) && lanesHaveLoopsOfTheirOwn(one));
    printCheck(out, "lanes run in order at a width of one"_s, lanesRunInOrderAtWidthOne(one));
    printCheck(out, "the caller runs the lanes that no thread takes"_s, callerRunsLanesThatNoThreadTakes());
}

static std::optional<Vector<uint32_t>> parseHashes(StringView word)
{
    Vector<uint32_t> hashes;
    if (word == "-"_s)
        return hashes;
    for (StringView text : word.splitAllowingEmptyEntries(',')) {
        auto hash = parseTestNumber<uint32_t>(text);
        if (!hash)
            return std::nullopt;
        hashes.append(*hash);
    }
    return hashes;
}

static bool findsEveryInternedListAgain(InternedWordLists& lists, std::span<const uint32_t> hashes)
{
    for (uint32_t position = 0; position < hashes.size(); ++position) {
        std::span<const uint32_t> words = singleElementSpan(position);
        if (lists.find(hashes[position], words) != position || lists.findOrAdd(hashes[position], words) != position || !std::ranges::equal(lists.at(position), words))
            return false;
    }
    return true;
}

void runHashedIndexTestCommand(TestCommandContext& context)
{
    constexpr size_t numberOfWords = 3;

    std::span<const String> words = context.arguments;
    if (words.size() < numberOfWords)
        return printMissingArgument(context.out);
    if (words.size() > numberOfWords)
        return printBadArgument(context.out, words[numberOfWords]);
    auto hashes = parseHashes(words[0]);
    if (!hashes)
        return printBadArgument(context.out, words[0]);
    auto hash = parseTestNumber<uint32_t>(words[1]);
    if (!hash)
        return printBadArgument(context.out, words[1]);
    auto wantedPosition = parseTestNumber<uint32_t>(words[2]);
    if (!wantedPosition)
        return printBadArgument(context.out, words[2]);

    InternedWordLists lists;
    bool positionsFollowArrival = true;
    for (uint32_t position = 0; position < hashes->size(); ++position)
        positionsFollowArrival &= lists.findOrAdd(hashes->at(position), singleElementSpan(position)) == position;

    if (auto found = lists.find(*hash, singleElementSpan(*wantedPosition)))
        context.out.println("position "_s, *found);
    else
        context.out.println("none"_s);
    printCheck(context.out, "positions follow the order of arrival"_s, positionsFollowArrival);
    printCheck(context.out, "every list is found again"_s, findsEveryInternedListAgain(lists, hashes->span()));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
