/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSupportTestCommands.h"

#if ENABLE(AOT)

#include "AOTBackgroundTask.h"
#include "AOTSort.h"
#include "AOTStoragePool.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <ranges>
#include <utility>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/StackAllocation.h>
#include <wtf/Threading.h>
#include <wtf/WeakRandom.h>

namespace JSC { namespace AOT { namespace Typed {

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

// On one thread the order is the order of the visits.
static bool visitsLongestIndicesFirst(const ParallelLoops& loops, const ParallelLoops& one)
{
    constexpr unsigned count = 1000;
    static constexpr uint32_t longestFirst[] { 700, 3, 999, 0, 256 };
    Vector<std::atomic<uint8_t>> visits(count);
    for (auto& visit : visits)
        visit.store(0, std::memory_order_relaxed);
    forEachIndexInParallel(loops, count, longestFirst, [&](unsigned index) {
        visits[index].fetch_add(1, std::memory_order_relaxed);
    });
    bool visitsEachOnce = std::ranges::all_of(visits, [](const std::atomic<uint8_t>& visit) {
        return visit.load(std::memory_order_relaxed) == 1;
    });

    Vector<uint32_t> order;
    forEachIndexInParallel(one, count, longestFirst, [&](unsigned index) {
        order.append(index);
    });
    return visitsEachOnce && order.size() == count && std::ranges::equal(order.span().first(std::size(longestFirst)), longestFirst) && order[std::size(longestFirst)] == 1;
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

static bool collectsInIndexOrder(const ParallelLoops& loops)
{
    constexpr unsigned count = 1000;
    auto collect = [](unsigned index, Vector<uint32_t>& list) {
        if (index % 3)
            return;
        list.append(index);
        list.append(index + 1);
    };
    Vector<uint32_t> expected;
    for (unsigned index = 0; index < count; ++index)
        collect(index, expected);
    static constexpr uint32_t longestFirst[] { 513, 0, 999, 255, 256, 257 };
    return collectInParallel<uint32_t>(loops, count, collect) == expected && collectInParallel<uint32_t>(loops, count, longestFirst, collect) == expected;
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

static bool sortsLikeStandardSort(const ParallelLoops& loops, size_t count)
{
    WeakRandom random(1);
    Vector<uint32_t> list(count, [&](size_t) {
        return random.getUint32();
    });
    Vector<uint32_t> expected = list;
    AOT::sort(expected);
    parallelSort(loops, list.mutableSpan(), std::less<uint32_t>());
    return list == expected;
}

static Vector<std::pair<uint32_t, uint32_t>> sortByFirst(const ParallelLoops& loops, size_t count)
{
    constexpr unsigned numberOfKeys = 16;
    WeakRandom random(1);
    Vector<std::pair<uint32_t, uint32_t>> list(count, [&](size_t index) {
        return std::pair<uint32_t, uint32_t> { random.getUint32(numberOfKeys), static_cast<uint32_t>(index) };
    });
    parallelSort(loops, list.mutableSpan(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    return list;
}

static bool sortOrderIgnoresWidth(const ParallelLoops& loops, const ParallelLoops& one, size_t count)
{
    return sortByFirst(loops, count) == sortByFirst(one, count);
}

// The parts that a merge is cut into give what one merge of the two whole runs gives, also where many elements compare equal and where one run lies in front of the other.
static bool sortsLikeMergesOfWholeRuns(const ParallelLoops& loops, size_t count, const Invocable<uint32_t(size_t)> auto& keyAt)
{
    using Element = std::pair<uint32_t, uint32_t>;
    constexpr size_t numberOfRuns = 16;
    auto isLess = [](const Element& a, const Element& b) {
        return a.first < b.first;
    };
    Vector<Element> list(count, [&](size_t index) {
        return Element { keyAt(index), static_cast<uint32_t>(index) };
    });
    Vector<Element> expected = list;
    Vector<Element> scratch(count);
    std::span<Element> from = expected.mutableSpan();
    std::span<Element> to = scratch.mutableSpan();
    auto runOf = [&](std::span<Element> all, size_t run, size_t runs) {
        size_t start = count * run / runs;
        return all.subspan(start, count * (run + 1) / runs - start);
    };
    for (size_t run = 0; run < numberOfRuns; ++run)
        AOT::sort(runOf(from, run, numberOfRuns), isLess);
    for (size_t runs = numberOfRuns; runs > 1; runs /= 2) {
        for (size_t pair = 0; pair < runs / 2; ++pair)
            std::ranges::merge(runOf(from, 2 * pair, runs), runOf(from, 2 * pair + 1, runs), runOf(to, pair, runs / 2).begin(), isLess);
        std::swap(from, to);
    }
    parallelSort(loops, list.mutableSpan(), isLess);
    return list == expected;
}

static bool sortsLikeMergesOfWholeRuns(const ParallelLoops& loops, size_t count)
{
    WeakRandom random(1);
    Vector<uint32_t> fewKeys(count, [&](size_t) {
        return random.getUint32(16);
    });
    return sortsLikeMergesOfWholeRuns(loops, count, [&](size_t index) { return fewKeys[index]; })
        && sortsLikeMergesOfWholeRuns(loops, count, [&](size_t index) { return static_cast<uint32_t>((count - index) / 1000); })
        && sortsLikeMergesOfWholeRuns(loops, count, [&](size_t index) { return static_cast<uint32_t>(index / 1000); })
        && sortsLikeMergesOfWholeRuns(loops, count, [](size_t) { return 7u; });
}

static bool sortsAndRemovesDuplicates(const ParallelLoops& loops, unsigned count)
{
    WeakRandom random(1);
    Vector<uint32_t> list(count, [&](size_t) {
        return random.getUint32(count / 4);
    });
    Vector<uint32_t> expected = list;
    AOT::sort(expected);
    expected.shrink(std::unique(expected.begin(), expected.end()) - expected.begin());
    sortAndRemoveDuplicates(loops, list);
    return list == expected;
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
    // On a busy machine a yield takes milliseconds, so the caller waits for the first pool thread and no longer.
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

// A host pool has no argument of its own, and one test command runs at a time.
struct HeldCalls {
    Lock lock;
    Vector<ScheduledCall> calls WTF_GUARDED_BY_LOCK(lock);
};

struct NumberedStorage {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(NumberedStorage);

    explicit NumberedStorage(uint32_t number)
        : number(number)
    {
    }

    uint32_t number;
    std::atomic<bool> isLeased { true };
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

static constexpr HostThreadPool poolOfNewThreads { [](void (*run)(void*), void* argument) {
    Thread::create("AOT test helper"_s, [run, argument] {
        run(argument);
    }, ThreadType::Unknown, Thread::defaultQOS, Thread::defaultSchedulingPolicy, StackAllocationSpecification::RequestSize(ParallelLoops::workerStackSize))->detach();
} };

static bool runsOnHostPool(unsigned width)
{
    ParallelLoops loops(width, &poolOfNewThreads);
    return visitsEachIndexOnce(loops, 100000) && sumsNestedLoops(loops) && handsOutWorkerNumbers(loops);
}

static bool asksForNoHelperForOneIndex()
{
    ParallelLoops loops(4, &poolThatHoldsCalls, 0_s);
    return visitsEachIndexOnce(loops, 0) && visitsEachIndexOnce(loops, 1) && takeHeldCalls().isEmpty();
}

static bool lateHelperDoesNothing()
{
    ParallelLoops loops(4, &poolThatHoldsCalls, 0_s);
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
    ParallelLoops loops(16, &poolThatHoldsCalls, 0_s);
    std::atomic<unsigned> visits { 0 };
    unsigned numberOfCalls = 0;
    forEachIndexInParallel(loops, count, [&](unsigned) {
        if (visits.fetch_add(1))
            return;
        // The caller is in its first index: whoever is asked for now finds the others unclaimed.
        for (Vector<ScheduledCall> calls = takeHeldCalls(); !calls.isEmpty(); calls = takeHeldCalls()) {
            numberOfCalls += calls.size();
            for (ScheduledCall call : calls)
                call.run(call.argument);
        }
    });
    return visits.load() == count && numberOfCalls && numberOfCalls <= count - 1 && takeHeldCalls().isEmpty();
}

static std::atomic<unsigned>& numberOfCountedThreads()
{
    static std::atomic<unsigned> number { 0 };
    return number;
}

static constexpr HostThreadPool poolThatCountsNewThreads { [](void (*run)(void*), void* argument) {
    numberOfCountedThreads().fetch_add(1);
    poolOfNewThreads.schedule(run, argument);
} };

// Every index waits for all the others, so the loop ends only when it has as many threads as indices.
static void runLoopThatNeedsEveryThread(const ParallelLoops& loops)
{
    std::atomic<unsigned> numberArrived { 0 };
    forEachIndexInParallel(loops, loops.width(), [&](unsigned) {
        numberArrived.fetch_add(1);
        while (numberArrived.load() < loops.width())
            Thread::yield();
    });
}

static void waitForRestingThreads(const ParallelLoops& loops, unsigned number)
{
    while (loops.numberOfRestingThreads() != number)
        Thread::yield();
}

static bool restingThreadsTakeTheNextLoop()
{
    constexpr unsigned width = 8;
    numberOfCountedThreads() = 0;
    ParallelLoops loops(width, &poolThatCountsNewThreads, Seconds::infinity());
    runLoopThatNeedsEveryThread(loops);
    bool askedPoolOncePerHelper = numberOfCountedThreads().load() == width - 1;
    for (unsigned round = 0; round < 20; ++round) {
        waitForRestingThreads(loops, width - 1);
        runLoopThatNeedsEveryThread(loops);
    }
    return askedPoolOncePerHelper && numberOfCountedThreads().load() == width - 1;
}

struct TimesMemoryWasGivenBack {
    std::atomic<unsigned> byAnyThread { 0 };
    std::atomic<unsigned> byCaller { 0 };
    std::atomic<Thread*> caller { nullptr };
};

static TimesMemoryWasGivenBack& timesMemoryWasGivenBack()
{
    static NeverDestroyed<TimesMemoryWasGivenBack> times;
    return times;
}

static constexpr HostThreadPool poolThatCountsMemoryGivenBack { poolOfNewThreads.schedule, [] {
    timesMemoryWasGivenBack().byAnyThread.fetch_add(1);
    if (&Thread::currentSingleton() == timesMemoryWasGivenBack().caller.load())
        timesMemoryWasGivenBack().byCaller.fetch_add(1);
} };

static bool callerGivesMemoryBackBesideTheOthersWork()
{
    constexpr unsigned width = 4;
    TimesMemoryWasGivenBack& times = timesMemoryWasGivenBack();
    times.byAnyThread = 0;
    times.byCaller = 0;
    times.caller = &Thread::currentSingleton();
    ParallelLoops loops(width, &poolThatCountsMemoryGivenBack, Seconds::infinity());
    loops.giveFreeMemoryBack();
    bool doesItAtOnceWhereNobodyRests = times.byAnyThread.load() == 1 && times.byCaller.load() == 1;
    runLoopThatNeedsEveryThread(loops);
    waitForRestingThreads(loops, width - 1);
    unsigned before = times.byAnyThread.load();
    loops.giveFreeMemoryBack();
    bool leavesItForLater = times.byCaller.load() == 1;
    while (times.byAnyThread.load() < before + width - 1)
        Thread::yield();
    bool othersDidItAtOnce = times.byCaller.load() == 1;
    runLoopThatNeedsEveryThread(loops);
    return doesItAtOnceWhereNobodyRests && leavesItForLater && othersDidItAtOnce && times.byCaller.load() == 2 && times.byAnyThread.load() == before + width;
}

static bool restingThreadsGoBackToThePool()
{
    constexpr unsigned width = 4;
    ParallelLoops loops(width, &poolOfNewThreads, 1_ms);
    runLoopThatNeedsEveryThread(loops);
    waitForRestingThreads(loops, 0);
    runLoopThatNeedsEveryThread(loops);
    waitForRestingThreads(loops, 0);
    return true;
}

static bool pendingTasksRunByPriority()
{
    ParallelLoops loops(4, &poolThatHoldsCalls, 0_s);
    struct Log {
        Vector<TaskPriority> scheduled;
        Vector<TaskPriority> current;
    } log;
    struct Entry {
        Log& log;
        TaskPriority priority;
    };
    Entry entries[] { { log, TaskPriority::Idle }, { log, TaskPriority::Foreground }, { log, TaskPriority::Background }, { log, TaskPriority::Foreground } };
    for (Entry& entry : entries) {
        loops.schedule(entry.priority, [](void* argument) {
            Entry& entry = *static_cast<Entry*>(argument);
            entry.log.scheduled.append(entry.priority);
            entry.log.current.append(ParallelLoops::currentPriority());
        }, &entry);
    }
    bool yieldsToHigherPriority = !loops.shouldYield(TaskPriority::Foreground) && loops.shouldYield(TaskPriority::Background) && loops.shouldYield(TaskPriority::Idle);
    Vector<ScheduledCall> calls = takeHeldCalls();
    for (ScheduledCall call : calls)
        call.run(call.argument);
    Vector<TaskPriority> expected { TaskPriority::Foreground, TaskPriority::Foreground, TaskPriority::Background, TaskPriority::Idle };
    return yieldsToHigherPriority && calls.size() == std::size(entries) && log.scheduled == expected && log.current == expected
        && !loops.shouldYield(TaskPriority::Idle) && ParallelLoops::currentPriority() == TaskPriority::Foreground;
}

static bool backgroundLoopYieldsToForegroundLoop(unsigned width)
{
    ParallelLoops loops(width, &poolOfNewThreads);
    constexpr unsigned count = 20000;
    std::atomic<unsigned> backgroundVisits { 0 };
    std::atomic<bool> workerNumbersAreValid { true };
    Ref<BackgroundTask> task = BackgroundTask::start(loops, [&] {
        forEachIndexInParallel(loops, count, [&](unsigned, unsigned worker) {
            if (worker >= loops.width())
                workerNumbersAreValid = false;
            backgroundVisits.fetch_add(1, std::memory_order_relaxed);
        });
    });
    bool foregroundLoopsAreRight = true;
    for (unsigned round = 0; round < 50; ++round)
        foregroundLoopsAreRight &= visitsEachIndexOnce(loops, 1000);
    task->finish();
    return foregroundLoopsAreRight && workerNumbersAreValid && backgroundVisits.load() == count;
}

static bool tasksRunAfterTheirDependencies(const ParallelLoops& loops)
{
    std::atomic<unsigned> numberFinished { 0 };
    std::array<unsigned, 4> position { };
    auto record = [&](unsigned task) {
        return [&position, &numberFinished, task] {
            position[task] = numberFinished.fetch_add(1);
        };
    };
    Ref<BackgroundTask> first = BackgroundTask::start(loops, record(0));
    Ref<BackgroundTask> second = BackgroundTask::start(loops, TaskPriority::Idle, { first.ptr() }, record(1));
    Ref<BackgroundTask> third = BackgroundTask::start(loops, TaskPriority::Background, { first.ptr() }, record(2));
    Ref<BackgroundTask> last = BackgroundTask::start(loops, TaskPriority::Background, { second.ptr(), third.ptr() }, record(3));
    last->finish();
    return numberFinished.load() == 4 && !position[0] && position[3] == 3;
}

static bool parallelArrayCreatesAndDestroysEachElementOnce(const ParallelLoops& loops)
{
    struct Element {
        Element(unsigned number, std::atomic<int>& numberAlive)
            : numbers(FillWith { }, 3, number)
            , numberAlive(&numberAlive)
        {
            numberAlive.fetch_add(1, std::memory_order_relaxed);
        }

        Element(Element&& other)
            : numbers(WTF::move(other.numbers))
            , numberAlive(other.numberAlive)
        {
            numberAlive->fetch_add(1, std::memory_order_relaxed);
        }

        ~Element() { numberAlive->fetch_sub(1, std::memory_order_relaxed); }

        Vector<unsigned> numbers;
        std::atomic<int>* numberAlive;
    };

    constexpr unsigned size = 5000;
    std::atomic<int> numberAlive { 0 };
    bool isRight = true;
    {
        ParallelArray<Element> array(loops, size, [&](unsigned index) {
            return Element(index, numberAlive);
        });
        isRight &= array.size() == size && numberAlive.load() == static_cast<int>(size);
        for (unsigned index = 0; index < size; ++index)
            isRight &= array[index].numbers.size() == 3 && array[index].numbers[2] == index;

        ParallelArray<Element> moved = WTF::move(array);
        isRight &= !array.size() && moved.size() == size && numberAlive.load() == static_cast<int>(size);
        moved = ParallelArray<Element>(loops, 7, [&](unsigned index) {
            return Element(index, numberAlive);
        });
        isRight &= moved.size() == 7 && numberAlive.load() == 7;
        isRight &= !ParallelArray<Element>(loops, 0, [&](unsigned index) {
            return Element(index, numberAlive);
        }).size();
    }
    return isRight && !numberAlive.load();
}

static bool storagePoolGivesThreadItsStorageBack()
{
    StoragePool<NumberedStorage, 2047> pool;
    NumberedStorage& first = pool.lease();
    NumberedStorage& nested = pool.lease();
    uint32_t numberOnOtherThread = first.number;
    Thread::create("AOT test lease"_s, [&] {
        NumberedStorage& other = pool.lease();
        numberOnOtherThread = other.number;
        pool.giveBack(other);
    })->waitForCompletion();
    bool areDistinct = &first != &nested && numberOnOtherThread != first.number && numberOnOtherThread != nested.number;
    bool findsByNumber = pool.at(first.number) == &first && pool.at(nested.number) == &nested;
    bool hadLeases = pool.hasLeases();
    pool.giveBack(nested);
    pool.giveBack(first);
    bool hasNoLeases = !pool.hasLeases();
    bool isSameAgain = &pool.lease() == &first;
    pool.giveBack(first);
    return areDistinct && findsByNumber && hadLeases && hasNoLeases && isSameAgain;
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
    printCheck(out, "the longest indices first"_s, visitsLongestIndicesFirst(loops, one));
    printCheck(out, "collect"_s, collectsInIndexOrder(loops));
    printCheck(out, "find"_s, findsInIndexOrder(loops));
    printCheck(out, "sort"_s, sortsLikeStandardSort(loops, 1000) && sortsLikeStandardSort(loops, 20000) && sortOrderIgnoresWidth(loops, one, 20000) && sortsLikeMergesOfWholeRuns(loops, 20000) && sortsLikeMergesOfWholeRuns(loops, 100003));
    printCheck(out, "sort and remove duplicates"_s, sortsAndRemovesDuplicates(loops, 1000) && sortsAndRemovesDuplicates(loops, 20000));
    printCheck(out, "tasks"_s, runsEveryTask(loops));
    printCheck(out, "a thread knows whether it is inside a loop"_s, knowsWhetherThreadIsInsideLoop(loops) && knowsWhetherThreadIsInsideLoop(one));
    printCheck(out, "width one is serial"_s, runsSeriallyOnCaller(one));
    printCheck(out, "worker numbers are below the width and unique at one time"_s, handsOutWorkerNumbers(loops));
    printCheck(out, "build thread has the worker stack size"_s, buildThreadHasWorkerStack());
    printCheck(out, "pool threads have the worker stack size"_s, poolThreadsHaveWorkerStack(loops));
    printCheck(out, "loops run on a host pool"_s, runsOnHostPool(words->threads));
    printCheck(out, "one index asks for no helper"_s, asksForNoHelperForOneIndex());
    printCheck(out, "a helper that comes late does nothing"_s, lateHelperDoesNothing());
    printCheck(out, "no more helpers than indices"_s, asksForNoMoreHelpersThanIndices());
    printCheck(out, "a thread gets its storage back"_s, storagePoolGivesThreadItsStorageBack());
    printCheck(out, "an array creates and destroys each element once"_s, parallelArrayCreatesAndDestroysEachElementOnce(loops));
    printCheck(out, "resting threads take the next loop"_s, restingThreadsTakeTheNextLoop());
    printCheck(out, "resting threads go back to the pool"_s, restingThreadsGoBackToThePool());
    printCheck(out, "the caller gives memory back beside the others' work"_s, callerGivesMemoryBackBesideTheOthersWork());
    printCheck(out, "pending tasks run by priority"_s, pendingTasksRunByPriority());
    printCheck(out, "a background loop yields to foreground loops"_s, backgroundLoopYieldsToForegroundLoop(words->threads));
    printCheck(out, "tasks run after their dependencies"_s, tasksRunAfterTheirDependencies(loops));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
