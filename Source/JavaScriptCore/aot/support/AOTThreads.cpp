/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTThreads.h"

#if ENABLE(AOT)

#include "AOTPhaseTimes.h"
#include <array>
#include <atomic>
#include <limits>
#include <optional>
#include <wtf/Atomics.h>
#include <wtf/CPUTime.h>
#include <wtf/Deque.h>
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/ParkingLot.h>
#include <wtf/StackAllocation.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Threading.h>
#include <wtf/UniqueArray.h>

namespace JSC { namespace AOT {

static thread_local TaskPriority t_currentPriority = TaskPriority::Foreground;
static thread_local unsigned t_parallelRegionDepth = 0;
static thread_local Seconds t_parallelRegionStart;
static thread_local Seconds t_parallelRegionCPUTime;

TaskPriority ParallelLoops::currentPriority()
{
    return t_currentPriority;
}

ParallelRegionScope::ParallelRegionScope()
    : m_isCounted(InstructionCounts::hasBegun())
{
    if (m_isCounted && !t_parallelRegionDepth++) [[unlikely]]
        t_parallelRegionStart = CPUTime::forCurrentThread();
}

ParallelRegionScope::~ParallelRegionScope()
{
    if (m_isCounted && !--t_parallelRegionDepth) [[unlikely]]
        t_parallelRegionCPUTime += CPUTime::forCurrentThread() - t_parallelRegionStart;
}

Seconds ParallelRegionScope::cpuTimeOfCurrentThread()
{
    return t_parallelRegionCPUTime;
}

static thread_local bool t_isCountedAsBusy = false;
static Lock s_busyThreadsLock;
static unsigned s_numberOfBusyThreads WTF_GUARDED_BY_LOCK(s_busyThreadsLock) = 0;
static MonotonicTime s_busyThreadsChangeTime WTF_GUARDED_BY_LOCK(s_busyThreadsLock);
static BusyThreads::Times s_busyThreadsTimes WTF_GUARDED_BY_LOCK(s_busyThreadsLock);

static void addTimeSinceBusyThreadsChanged() WTF_REQUIRES_LOCK(s_busyThreadsLock)
{
    MonotonicTime now = MonotonicTime::now();
    if (s_busyThreadsChangeTime) {
        unsigned bucket = 0;
        while (s_numberOfBusyThreads > BusyThreads::maxThreadsOfBucket[bucket])
            ++bucket;
        s_busyThreadsTimes[bucket] += now - s_busyThreadsChangeTime;
    }
    s_busyThreadsChangeTime = now;
}

static void setCurrentThreadIsBusy(bool isBusy)
{
    t_isCountedAsBusy = isBusy;
    Locker locker { s_busyThreadsLock };
    addTimeSinceBusyThreadsChanged();
    s_numberOfBusyThreads += isBusy ? 1 : -1;
}

void BusyThreads::countCurrentThread()
{
    if (!t_isCountedAsBusy)
        setCurrentThreadIsBusy(true);
}

BusyThreads::Times BusyThreads::times()
{
    Locker locker { s_busyThreadsLock };
    addTimeSinceBusyThreadsChanged();
    return s_busyThreadsTimes;
}

BusyThreads::WaitScope::WaitScope()
    : m_wasCounted(t_isCountedAsBusy)
{
    if (m_wasCounted) [[unlikely]]
        setCurrentThreadIsBusy(false);
}

BusyThreads::WaitScope::~WaitScope()
{
    if (m_wasCounted) [[unlikely]]
        setCurrentThreadIsBusy(true);
}

static ALWAYS_INLINE void spinHint()
{
#if CPU(X86_64)
    __builtin_ia32_pause();
#elif CPU(ARM64)
    asm volatile("isb" ::: "memory");
#endif
}

// See ParallelLoops::ShortLoopsScope.
static bool spinUntil(const Invocable<bool()> auto& isDone)
{
    constexpr Seconds spinTime = Seconds::fromMicroseconds(100);
    constexpr unsigned spinsBetweenClockReads = 32;
    MonotonicTime end = MonotonicTime::now() + spinTime;
    for (unsigned spins = 1;; ++spins) {
        if (isDone())
            return true;
        spinHint();
        if (!(spins % spinsBetweenClockReads) && MonotonicTime::now() > end)
            return false;
    }
}

// A pool wakes its threads one after the other, each when the one before has found work. That way a loop waits milliseconds for its helpers, and
// on a busy machine a hundred times longer. So a thread of the pool that has run out of work rests here for a while, and a task goes to a resting
// thread first. Whoever has the task wakes that thread itself: a loop whose helpers each ask for two more has them all after a few steps.
// A task that finds no resting thread is pending. A thread that finishes a task takes the pending task with the highest priority.
class ParallelLoops::RestingThreads final : public ThreadSafeRefCounted<RestingThreads> {
public:
    struct Task {
        void (*run)(void* argument) { nullptr };
        void* argument { nullptr };
        TaskPriority priority { TaskPriority::Foreground };
    };

    static Ref<RestingThreads> create(unsigned numberOfSeats, Seconds restTime)
    {
        return adoptRef(*new RestingThreads(numberOfSeats, restTime));
    }

    static void runOnPoolThread(void* restingThreads)
    {
        adoptRef(*static_cast<RestingThreads*>(restingThreads))->runOnPoolThread();
    }

    bool tryHandOver(Task task)
    {
        return !hasPendingTaskAbove(task.priority) && wakeOne(HasTask, task);
    }

    void addPendingTask(Task task)
    {
        unsigned priority = static_cast<unsigned>(task.priority);
        Locker locker { m_pendingTasksLock };
        m_pendingTasks[priority].append(task);
        m_numberOfPendingTasks[priority].store(m_pendingTasks[priority].size(), std::memory_order_relaxed);
    }

    bool hasPendingTaskAbove(TaskPriority priority) const
    {
        return hasPendingTaskAmongFirst(static_cast<unsigned>(priority));
    }

    void willPost()
    {
        m_numberOfPostedWakeups.fetch_add(1, std::memory_order_seq_cst);
    }

    // A thread may have come to rest since none was found, and the pool may have no other thread for the task.
    void didPost()
    {
        wakeOne(IsSentBack, { });
    }

    void close()
    {
        m_isClosed.store(true, std::memory_order_seq_cst);
        while (wakeOne(IsSentBack, { })) { }
    }

    void didOpenShortLoopsScope() { m_numberOfShortLoopsScopes.fetch_add(1, std::memory_order_relaxed); }
    void didCloseShortLoopsScope() { m_numberOfShortLoopsScopes.fetch_sub(1, std::memory_order_relaxed); }
    bool expectsShortLoops() const { return m_numberOfShortLoopsScopes.load(std::memory_order_relaxed); }

    unsigned numberOfRestingThreads() const
    {
        unsigned result = 0;
        for (unsigned index = 0; index < m_numberOfSeats; ++index)
            result += m_seats[index].state.load() == IsResting && m_seats[index].isInRestingList.load(std::memory_order_acquire);
        return result;
    }

private:
    // The thread of a seat that is resting is in the list of resting seats. Whoever takes the seat out of the list gives it a task or sends it back
    // to the pool. A thread that leaves by itself leaves its seat in the list, for whoever takes it out to find it empty.
    enum State : uint32_t { IsEmpty, IsResting, HasTask, IsSentBack };

    struct Seat {
        Atomic<uint32_t> state { IsEmpty };
        std::atomic<uint32_t> next { 0 };
        std::atomic<bool> isInRestingList { false };
        Task task;
    };

    // The low half of the head is the number of the first seat plus one. The high half changes with every push and pop, so that a head that has
    // the same first seat again is not taken for unchanged.
    class SeatList {
    public:
        void push(Seat* seats, uint32_t index)
        {
            uint64_t head = m_head.load(std::memory_order_relaxed);
            do
                seats[index].next.store(static_cast<uint32_t>(head), std::memory_order_relaxed);
            while (!m_head.compare_exchange_weak(head, headAfter(head, index + 1), std::memory_order_seq_cst, std::memory_order_relaxed));
        }

        std::optional<uint32_t> pop(Seat* seats)
        {
            uint64_t head = m_head.load(std::memory_order_seq_cst);
            for (;;) {
                uint32_t first = static_cast<uint32_t>(head);
                if (!first)
                    return std::nullopt;
                uint32_t next = seats[first - 1].next.load(std::memory_order_relaxed);
                if (m_head.compare_exchange_weak(head, headAfter(head, next), std::memory_order_seq_cst, std::memory_order_seq_cst))
                    return first - 1;
            }
        }

    private:
        static uint64_t headAfter(uint64_t head, uint32_t first) { return ((head >> 32) + 1) << 32 | first; }

        std::atomic<uint64_t> m_head { 0 };
    };

    RestingThreads(unsigned numberOfSeats, Seconds restTime)
        : m_numberOfSeats(restTime ? numberOfSeats : 0)
        , m_restTime(restTime)
        , m_seats(makeUniqueArray<Seat>(m_numberOfSeats))
    {
        for (unsigned index = 0; index < m_numberOfSeats; ++index)
            m_freeSeats.push(m_seats.get(), index);
    }

    bool hasPendingTaskAmongFirst(unsigned numberOfPriorities) const
    {
        for (unsigned priority = 0; priority < numberOfPriorities; ++priority) {
            if (m_numberOfPendingTasks[priority].load(std::memory_order_relaxed))
                return true;
        }
        return false;
    }

    std::optional<Task> takePendingTask()
    {
        if (!hasPendingTaskAmongFirst(numberOfTaskPriorities))
            return std::nullopt;
        Locker locker { m_pendingTasksLock };
        for (unsigned priority = 0; priority < numberOfTaskPriorities; ++priority) {
            if (m_pendingTasks[priority].isEmpty())
                continue;
            Task task = m_pendingTasks[priority].takeFirst();
            m_numberOfPendingTasks[priority].store(m_pendingTasks[priority].size(), std::memory_order_relaxed);
            return task;
        }
        return std::nullopt;
    }

    static void run(Task task)
    {
        TaskPriority previousPriority = std::exchange(t_currentPriority, task.priority);
        bool isCounted = InstructionCounts::hasBegun() && !t_isCountedAsBusy;
        if (isCounted) [[unlikely]]
            setCurrentThreadIsBusy(true);
        task.run(task.argument);
        if (isCounted) [[unlikely]]
            setCurrentThreadIsBusy(false);
        t_currentPriority = previousPriority;
    }

    void runPendingTasks()
    {
        while (std::optional<Task> task = takePendingTask())
            run(*task);
    }

    void runOnPoolThread()
    {
        m_numberOfPostedWakeups.fetch_sub(1, std::memory_order_seq_cst);
        if (InstructionCounts::hasBegun()) [[unlikely]]
            InstructionCounts::countOnCurrentThread();
        runPendingTasks();

        std::optional<uint32_t> index = m_freeSeats.pop(m_seats.get());
        if (!index)
            return;
        Seat& seat = m_seats[*index];
        for (;;) {
            seat.isInRestingList.store(false, std::memory_order_relaxed);
            seat.state.store(IsResting);
            m_restingSeats.push(m_seats.get(), *index);
            seat.isInRestingList.store(true, std::memory_order_release);
            switch (rest(seat)) {
            case HasTask:
                break;
            case IsSentBack:
                m_freeSeats.push(m_seats.get(), *index);
                return;
            default:
                return;
            }
            run(seat.task);
            runPendingTasks();
        }
    }

    bool wakeOne(State newState, Task task)
    {
        while (std::optional<uint32_t> index = m_restingSeats.pop(m_seats.get())) {
            Seat& seat = m_seats[*index];
            seat.task = task;
            if (seat.state.compareExchangeStrong(IsResting, newState) == IsResting) {
                ParkingLot::unparkOne(&seat.state);
                return true;
            }
            m_freeSeats.push(m_seats.get(), *index);
        }
        return false;
    }

    State rest(Seat& seat)
    {
        // Either this thread sees the posted wakeup, or whoever posted it sees this thread in the list.
        bool shouldLeave = m_numberOfPostedWakeups.load(std::memory_order_seq_cst) || m_isClosed.load(std::memory_order_seq_cst);
        if (!shouldLeave && expectsShortLoops()) {
            spinUntil([&] {
                return seat.state.load(std::memory_order_relaxed) != IsResting;
            });
        }
        MonotonicTime end = MonotonicTime::now() + m_restTime;
        for (;;) {
            uint32_t state = seat.state.load();
            if (state != IsResting)
                return static_cast<State>(state);
            if (shouldLeave) {
                if (seat.state.compareExchangeStrong(IsResting, IsEmpty) == IsResting)
                    return IsEmpty;
                continue;
            }
            ParkingLot::parkConditionally(&seat.state, [&] {
                return seat.state.load() == IsResting;
            }, [] { }, end);
            shouldLeave = MonotonicTime::now() >= end;
        }
    }

    const unsigned m_numberOfSeats;
    const Seconds m_restTime;
    const UniqueArray<Seat> m_seats;
    SeatList m_freeSeats;
    SeatList m_restingSeats;
    std::atomic<unsigned> m_numberOfPostedWakeups { 0 };
    std::atomic<bool> m_isClosed { false };
    std::atomic<unsigned> m_numberOfShortLoopsScopes { 0 };
    alignas(128) std::array<std::atomic<size_t>, numberOfTaskPriorities> m_numberOfPendingTasks { };
    alignas(128) Lock m_pendingTasksLock;
    std::array<Deque<Task>, numberOfTaskPriorities> m_pendingTasks WTF_GUARDED_BY_LOCK(m_pendingTasksLock);
};

ParallelLoops::ParallelLoops(unsigned width, const HostThreadPool* hostPool, Seconds restTime)
    : m_width(width)
    , m_hostPool(hostPool)
    , m_restingThreads(RestingThreads::create(std::max(width, 1u) - 1, restTime))
{
    RELEASE_ASSERT(width);
    if (width == 1 || hostPool)
        return;
    m_ownPool = WorkerPool::create("AOT worker"_s, width - 1, Seconds::infinity(), StackAllocationSpecification::RequestSize(workerStackSize));
}

ParallelLoops::~ParallelLoops()
{
    m_restingThreads->close();
}

void ParallelLoops::sendRestingThreadsBack() const
{
    m_restingThreads->close();
}

unsigned ParallelLoops::numberOfRestingThreads() const
{
    return m_restingThreads->numberOfRestingThreads();
}

bool ParallelLoops::shouldYield(TaskPriority priority) const
{
    return m_restingThreads->hasPendingTaskAbove(priority);
}

static constexpr unsigned narrowestLoopsThatSpin = 8;

ParallelLoops::ShortLoopsScope::ShortLoopsScope(const ParallelLoops& loops)
    : m_loops(loops)
{
    if (m_loops.width() >= narrowestLoopsThatSpin)
        m_loops.m_restingThreads->didOpenShortLoopsScope();
}

ParallelLoops::ShortLoopsScope::~ShortLoopsScope()
{
    if (m_loops.width() >= narrowestLoopsThatSpin)
        m_loops.m_restingThreads->didCloseShortLoopsScope();
}

bool ParallelLoops::expectsShortLoops() const
{
    return m_restingThreads->expectsShortLoops();
}

void ParallelLoops::schedule(TaskPriority priority, void (*run)(void* argument), void* argument) const
{
    RestingThreads::Task task { run, argument, priority };
    if (m_restingThreads->tryHandOver(task))
        return;

    m_restingThreads->addPendingTask(task);
    m_restingThreads->willPost();
    RestingThreads* restingThreads = &m_restingThreads.copyRef().leakRef();
    if (m_hostPool)
        m_hostPool->schedule(RestingThreads::runOnPoolThread, restingThreads);
    else {
        m_ownPool->postTask([restingThreads] {
            RestingThreads::runOnPoolThread(restingThreads);
        });
    }
    m_restingThreads->didPost();
}

namespace {

enum class Participant : bool { Caller, Helper };
enum class WorkResult : bool { RanOut, Yielded };

// Work that its caller runs, and that threads of the pool join while there is something for them to take. Each helper that finds
// some asks for two more, so no thread is woken for work that is gone. A helper that arrives after the work ended only drops its reference.
// A helper that yields asks for another in its place, which comes once no task with a higher priority is pending.
class SharedWork final : public ThreadSafeRefCounted<SharedWork> {
public:
    using Work = ScopedLambda<WorkResult(Participant)>;

    static void run(const ParallelLoops& loops, unsigned maxHelpers, NOESCAPE const Work& work, NOESCAPE const ScopedLambda<bool()>& hasWorkToTake)
    {
        adoptRef(*new SharedWork(loops, maxHelpers, work, hasWorkToTake))->runOnCaller();
    }

private:
    SharedWork(const ParallelLoops& loops, unsigned maxHelpers, const Work& work, const ScopedLambda<bool()>& hasWorkToTake)
        : m_loops(loops)
        , m_maxHelpers(maxHelpers)
        , m_priority(ParallelLoops::currentPriority())
        , m_work(work)
        , m_hasWorkToTake(hasWorkToTake)
    {
    }

    void runOnCaller()
    {
        ParallelRegionScope parallelRegion;
        askForHelper();
        m_work(Participant::Caller);
        close();
        if (m_state.load() == isClosed)
            return;
        BusyThreads::WaitScope waitScope;
        for (uint32_t state = m_state.load(); state != isClosed; state = m_state.load()) {
            bool hasChanged = m_loops.expectsShortLoops() && spinUntil([&] {
                return m_state.load(std::memory_order_relaxed) != state;
            });
            if (!hasChanged)
                ParkingLot::compareAndPark(&m_state, state);
        }
    }

    void runOnHelper()
    {
        for (;;) {
            uint32_t state = m_state.load();
            if (state & isClosed)
                return;
            if (m_state.compareExchangeWeak(state, state + oneHelperAtWork))
                break;
        }
        ASSERT(Thread::currentSingleton().stack().size() >= ParallelLoops::workerStackSize / 2);
        if (m_hasWorkToTake()) {
            askForHelper();
            askForHelper();
        }
        if (m_work(Participant::Helper) == WorkResult::Yielded) {
            m_numberOfHelpersAskedFor.fetch_sub(1, std::memory_order_relaxed);
            askForHelper();
        } else
            close();
        if (m_state.exchangeSub(oneHelperAtWork) == (isClosed | oneHelperAtWork))
            ParkingLot::unparkAll(&m_state);
    }

    void close()
    {
        if (!(m_state.load() & isClosed))
            m_state.exchangeOr(isClosed);
    }

    void askForHelper()
    {
        if (m_numberOfHelpersAskedFor.fetch_add(1, std::memory_order_relaxed) >= m_maxHelpers) {
            m_numberOfHelpersAskedFor.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
        ref();
        m_loops.schedule(m_priority, [](void* sharedWork) {
            adoptRef(*static_cast<SharedWork*>(sharedWork))->runOnHelper();
        }, this);
    }

    static constexpr uint32_t isClosed = 1;
    static constexpr uint32_t oneHelperAtWork = 2;

    // All but the state belongs to the caller, which stays in runOnCaller() while a helper is at work.
    const ParallelLoops& m_loops;
    const unsigned m_maxHelpers;
    const TaskPriority m_priority;
    const Work& m_work;
    const ScopedLambda<bool()>& m_hasWorkToTake;
    std::atomic<unsigned> m_numberOfHelpersAskedFor { 0 };
    Atomic<uint32_t> m_state { 0 };
};

// A worker that yields gives its number back for the helper that comes in its place.
class WorkerNumbers {
public:
    explicit WorkerNumbers(unsigned maxWorkers)
        : m_isTaken(maxWorkers)
    {
        for (auto& isTaken : m_isTaken)
            isTaken.store(false, std::memory_order_relaxed);
    }

    unsigned take()
    {
        for (;;) {
            for (unsigned worker = 0; worker < m_isTaken.size(); ++worker) {
                if (!m_isTaken[worker].load(std::memory_order_relaxed) && !m_isTaken[worker].exchange(true, std::memory_order_acquire))
                    return worker;
            }
        }
    }

    void giveBack(unsigned worker) { m_isTaken[worker].store(false, std::memory_order_release); }

private:
    Vector<std::atomic<bool>, 64> m_isTaken;
};

} // anonymous namespace

void runOnBuildThread(NOESCAPE const ScopedLambda<void()>& functor)
{
    Thread::create("AOT build"_s, [&] {
        functor();
    }, ThreadType::Compiler, Thread::defaultQOS, Thread::defaultSchedulingPolicy, StackAllocationSpecification::RequestSize(ParallelLoops::workerStackSize))->waitForCompletion();
}

static void runLoop(const ParallelLoops& loops, unsigned count, NOESCAPE const ScopedLambda<void(unsigned, unsigned)>& functor)
{
    if (count < 2 || loops.width() == 1) {
        for (unsigned index = 0; index < count; ++index)
            functor(index, 0);
        return;
    }

    // A worker's first run is one index, because callers put the largest work first, and each next run is twice as long.
    // The last runs are short, so that the workers end together. A helper yields only between two runs.
    const TaskPriority priority = ParallelLoops::currentPriority();
    const bool canYield = priority != TaskPriority::Foreground;
    const unsigned longestRun = canYield ? 16 : 256;
    const unsigned maxWorkers = std::min(loops.width(), count);
    const unsigned shares = 4 * maxWorkers;
    alignas(128) std::atomic<unsigned> next { 0 };
    alignas(128) std::atomic<unsigned> numberOfWorkers { 0 };
    std::optional<WorkerNumbers> workerNumbers;
    if (canYield)
        workerNumbers.emplace(maxWorkers);
    SharedWork::run(loops, maxWorkers - 1, [&](Participant participant) {
        unsigned worker = canYield ? workerNumbers->take() : numberOfWorkers.fetch_add(1, std::memory_order_relaxed);
        RELEASE_ASSERT(worker < maxWorkers);
        unsigned run = 1;
        for (unsigned first = next.load(std::memory_order_relaxed); first < count;) {
            if (canYield && participant == Participant::Helper && loops.shouldYield(priority)) {
                workerNumbers->giveBack(worker);
                return WorkResult::Yielded;
            }
            unsigned end = first + std::max(std::min(run, (count - first) / shares), 1u);
            if (!next.compare_exchange_weak(first, end, std::memory_order_relaxed))
                continue;
            for (unsigned index = first; index < end; ++index)
                functor(index, worker);
            run = std::min(run * 2, longestRun);
            first = next.load(std::memory_order_relaxed);
        }
        return WorkResult::RanOut;
    }, [&] {
        return next.load(std::memory_order_relaxed) < count;
    });
}

void forEachIndexInParallel(const ParallelLoops& loops, unsigned count, NOESCAPE const ScopedLambda<void(unsigned index)>& functor)
{
    runLoop(loops, count, [&](unsigned index, unsigned) {
        functor(index);
    });
}

void forEachIndexInParallel(const ParallelLoops& loops, unsigned count, NOESCAPE const ScopedLambda<void(unsigned index, unsigned worker)>& functor)
{
    runLoop(loops, count, functor);
}

void forEachIndexInParallel(const ParallelLoops& loops, unsigned count, std::span<const uint32_t> longestFirst, NOESCAPE const ScopedLambda<void(unsigned index)>& functor)
{
    if (longestFirst.empty()) {
        forEachIndexInParallel(loops, count, functor);
        return;
    }

    BitVector comesFirst(count);
    for (uint32_t index : longestFirst) {
        RELEASE_ASSERT(index < count);
        bool isRepeated = comesFirst.quickSet(index);
        RELEASE_ASSERT(!isRepeated);
    }
    unsigned numberFirst = static_cast<unsigned>(longestFirst.size());
    runLoop(loops, numberFirst + count, [&](unsigned position, unsigned) {
        if (position < numberFirst)
            functor(longestFirst[position]);
        else if (!comesFirst.quickGet(position - numberFirst))
            functor(position - numberFirst);
    });
}

void runTasksInParallel(const ParallelLoops& loops, std::initializer_list<ScopedLambda<void()>> tasks)
{
    forEachIndexInParallel(loops, static_cast<unsigned>(tasks.size()), [&](unsigned index) {
        tasks.begin()[index]();
    });
}

void forEachIndexInParallelWithOrderedCommit(const ParallelLoops& loops, unsigned count, NOESCAPE const ScopedLambda<void(unsigned index)>& produce, NOESCAPE const ScopedLambda<void(unsigned index)>& commit)
{
    if (count < 2 || loops.width() == 1) {
        for (unsigned index = 0; index < count; ++index) {
            produce(index);
            commit(index);
        }
        return;
    }

    constexpr unsigned maxBlockSize = 64;
    constexpr uint32_t noBlock = std::numeric_limits<uint32_t>::max();
    const unsigned blockSize = std::clamp(count / (64 * loops.width()), 1u, maxBlockSize);
    const unsigned numberOfBlocks = (count + blockSize - 1) / blockSize;
    Vector<std::atomic<bool>> isProduced(numberOfBlocks);
    for (auto& flag : isProduced)
        flag.store(false, std::memory_order_relaxed);
    alignas(128) std::atomic<unsigned> nextBlock { 0 };
    alignas(128) Atomic<uint32_t> awaitedBlock { noBlock };

    auto produceNextBlock = [&] {
        unsigned block = nextBlock.fetch_add(1, std::memory_order_relaxed);
        if (block >= numberOfBlocks)
            return false;
        unsigned end = std::min(count, (block + 1) * blockSize);
        for (unsigned index = block * blockSize; index < end; ++index)
            produce(index);
        isProduced[block].store(true, std::memory_order_seq_cst);
        if (awaitedBlock.load(std::memory_order_seq_cst) == block)
            ParkingLot::unparkAll(&awaitedBlock);
        return true;
    };

    SharedWork::run(loops, std::min(loops.width(), numberOfBlocks) - 1, [&](Participant participant) {
        if (participant == Participant::Helper) {
            while (produceNextBlock()) { }
            return WorkResult::RanOut;
        }
        for (unsigned block = 0; block < numberOfBlocks; ++block) {
            while (!isProduced[block].load(std::memory_order_acquire)) {
                if (produceNextBlock())
                    continue;
                awaitedBlock.store(block, std::memory_order_seq_cst);
                BusyThreads::WaitScope waitScope;
                ParkingLot::parkConditionally(&awaitedBlock, [&] {
                    return !isProduced[block].load(std::memory_order_seq_cst);
                }, [] { }, MonotonicTime::infinity());
                awaitedBlock.store(noBlock, std::memory_order_relaxed);
            }
            unsigned end = std::min(count, (block + 1) * blockSize);
            for (unsigned index = block * blockSize; index < end; ++index)
                commit(index);
        }
        return WorkResult::RanOut;
    }, [&] {
        return nextBlock.load(std::memory_order_relaxed) < numberOfBlocks;
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
