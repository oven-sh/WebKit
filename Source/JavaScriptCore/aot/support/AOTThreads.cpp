/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTThreads.h"

#if ENABLE(AOT)

#include "AOTCurrentWork.h"
#include "AOTPhaseTimes.h"
#include "AOTStageLog.h"
#include "AOTSystemResources.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <array>
#include <atomic>
#include <limits>
#include <optional>
#include <wtf/Atomics.h>
#include <wtf/BitVector.h>
#include <wtf/CPUTime.h>
#include <wtf/Deque.h>
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/ParkingLot.h>
#include <wtf/SetForScope.h>
#include <wtf/StackAllocation.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Threading.h>
#include <wtf/UniqueArray.h>

namespace JSC { namespace AOT {

void joinChunksAsBytes(const ParallelLoops& loops, VectorWords& result, std::span<VectorWords> chunks, size_t elementSize)
{
    size_t size = 0;
    for (auto& chunk : chunks)
        size += chunk.size;
    if (size) {
        allocateVectorBuffer<VectorBufferMalloc>(result, size, elementSize);
        auto bytes = unsafeMakeSpan(static_cast<uint8_t*>(result.buffer), size * elementSize);
        takePageFaultsInParallel(loops, bytes);
        for (auto& chunk : chunks) {
            if (!chunk.size)
                continue;
            memcpySpan(bytes, unsafeMakeSpan(static_cast<const uint8_t*>(chunk.buffer), chunk.size * elementSize));
            bytes = bytes.subspan(chunk.size * elementSize);
            chunk.size = 0;
        }
        result.size = static_cast<unsigned>(size);
    }
    AOT_THRESHOLD(size_t, fewestChunksFreedOnPool, 64);
    if (chunks.size() >= fewestChunksFreedOnPool) {
        forEachIndexInParallel(loops, static_cast<unsigned>(chunks.size()), [&](unsigned index) {
            VectorWords& chunk = chunks[index];
            if (chunk.buffer)
                VectorBufferMalloc::free(chunk.buffer);
            chunk = { };
        });
    }
}

static thread_local TaskPriority t_currentPriority = TaskPriority::Foreground;
static thread_local bool t_isInsideLoop = false;
static thread_local unsigned t_parallelRegionDepth = 0;
static thread_local Seconds t_parallelRegionStart;
static thread_local Seconds t_parallelRegionCPUTime;

TaskPriority ParallelLoops::currentPriority()
{
    return t_currentPriority;
}

bool ParallelLoops::isInsideLoop()
{
    return t_isInsideLoop;
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

// A pool wakes its threads one after the other, each when the one before has found work.
static std::atomic<bool (*)(bool)> s_giveFreeMemoryOfCurrentThreadBack { nullptr };
static std::atomic<uint32_t> s_timesAskedForFreeMemory { 0 };
static thread_local uint32_t t_timesGaveFreeMemoryBack { 0 };

enum class TurnToGiveBack : bool { IsTakenIfFree, IsWaitedFor };

// A thread with work in hand does not wait: it asks again behind its next step.
static void giveFreeMemoryOfCurrentThreadBackIfAsked(TurnToGiveBack turn)
{
    uint32_t timesAsked = s_timesAskedForFreeMemory.load(std::memory_order_seq_cst);
    if (t_timesGaveFreeMemoryBack == timesAsked) [[likely]]
        return;
    if (s_giveFreeMemoryOfCurrentThreadBack.load(std::memory_order_relaxed)(turn == TurnToGiveBack::IsWaitedFor))
        t_timesGaveFreeMemoryBack = timesAsked;
}

class ParallelLoops::RestingThreads final : public ThreadSafeRefCounted<RestingThreads> {
public:
    struct Task {
        void (*run)(void* argument) { nullptr };
        void* argument { nullptr };
        TaskPriority priority { TaskPriority::Foreground };
        InheritedWork work;
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

    // Every seat leaves the list before the first thread is woken. A thread that has run the task and rests again would otherwise get it a second time, and another thread never.
    bool handOverToAll(Task task)
    {
        Vector<uint32_t, 64> seats;
        while (std::optional<uint32_t> index = m_restingSeats.pop(m_seats.get()))
            seats.append(*index);
        bool hasWokenThread = false;
        for (uint32_t index : seats)
            hasWokenThread |= wake(index, HasTask, task);
        return hasWokenThread;
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

    unsigned numberOfRestingThreads() const
    {
        unsigned result = 0;
        for (unsigned index = 0; index < m_numberOfSeats; ++index)
            result += m_seats[index].state.load() == IsResting && m_seats[index].isInRestingList.load(std::memory_order_acquire);
        return result;
    }

private:
    // The thread of a seat that is resting is in the list of resting seats.
    enum State : uint32_t { IsEmpty, IsResting, HasTask, IsSentBack };

    struct Seat {
        Atomic<uint32_t> state { IsEmpty };
        std::atomic<uint32_t> next { 0 };
        std::atomic<bool> isInRestingList { false };
        Task task;
    };

    // The low half of the head is the number of the first seat plus one.
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
        giveFreeMemoryOfCurrentThreadBackIfAsked(TurnToGiveBack::IsTakenIfFree);
        TaskPriority previousPriority = std::exchange(t_currentPriority, task.priority);
        CurrentWorkScope workScope(task.work);
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
            // Either this thread sees that it was asked, or whoever asked sees this thread in the list.
            giveFreeMemoryOfCurrentThreadBackIfAsked(TurnToGiveBack::IsWaitedFor);
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

    // Of a seat that was taken out of the list of resting seats.
    bool wake(uint32_t index, State newState, Task task)
    {
        Seat& seat = m_seats[index];
        seat.task = task;
        if (seat.state.compareExchangeStrong(IsResting, newState) == IsResting) {
            ParkingLot::unparkOne(&seat.state);
            return true;
        }
        m_freeSeats.push(m_seats.get(), index);
        return false;
    }

    bool wakeOne(State newState, Task task)
    {
        while (std::optional<uint32_t> index = m_restingSeats.pop(m_seats.get())) {
            if (wake(*index, newState, task))
                return true;
        }
        return false;
    }

    State rest(Seat& seat)
    {
        // Either this thread sees the posted wakeup, or whoever posted it sees this thread in the list.
        bool shouldLeave = m_numberOfPostedWakeups.load(std::memory_order_seq_cst) || m_isClosed.load(std::memory_order_seq_cst);
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
    alignas(128) std::array<std::atomic<size_t>, numberOfTaskPriorities> m_numberOfPendingTasks { };
    alignas(128) Lock m_pendingTasksLock;
    std::array<Deque<Task>, numberOfTaskPriorities> m_pendingTasks WTF_GUARDED_BY_LOCK(m_pendingTasksLock);
};

namespace {

// How many parallel loops a build ran, by their number of indices and by how long they took. Counted while phase times are logged.
class LoopCounts {
public:
    static void add(unsigned numberOfIndices, Seconds time, unsigned numberOfHelpers)
    {
        unsigned size = 0;
        while (size + 1 < numberOfSizes && numberOfIndices >= leastIndicesOf[size + 1])
            ++size;
        unsigned length = 0;
        while (length + 1 < numberOfLengths && time.microseconds() >= leastMicrosecondsOf[length + 1])
            ++length;
        Cell& cell = s_cells[size][length];
        cell.loops.fetch_add(1, std::memory_order_relaxed);
        cell.nanoseconds.fetch_add(static_cast<uint64_t>(time.nanoseconds()), std::memory_order_relaxed);
        cell.helpers.fetch_add(numberOfHelpers, std::memory_order_relaxed);
    }

    static void logAndReset()
    {
        for (unsigned size = 0; size < numberOfSizes; ++size) {
            for (unsigned length = 0; length < numberOfLengths; ++length) {
                Cell& cell = s_cells[size][length];
                uint64_t loops = cell.loops.exchange(0, std::memory_order_relaxed);
                uint64_t nanoseconds = cell.nanoseconds.exchange(0, std::memory_order_relaxed);
                uint64_t helpers = cell.helpers.exchange(0, std::memory_order_relaxed);
                if (loops)
                    logStageLine("AOT parallel loops: from ", leastIndicesOf[size], " indices | from ", leastMicrosecondsOf[length], " us | ", loops, " loops | ", static_cast<double>(nanoseconds) / 1e6, " ms | ", static_cast<double>(helpers) / loops, " helpers asked for a loop");
            }
        }
    }

private:
    struct Cell {
        std::atomic<uint64_t> loops;
        std::atomic<uint64_t> nanoseconds;
        std::atomic<uint64_t> helpers;
    };

    static constexpr unsigned numberOfSizes = 5;
    static constexpr unsigned numberOfLengths = 6;
    static constexpr unsigned leastIndicesOf[numberOfSizes] { 2, 16, 256, 4096, 65536 };
    static constexpr unsigned leastMicrosecondsOf[numberOfLengths] { 0, 10, 30, 100, 1000, 10000 };
    static constinit Cell s_cells[numberOfSizes][numberOfLengths];
};

constinit LoopCounts::Cell LoopCounts::s_cells[numberOfSizes][numberOfLengths] { };

} // anonymous namespace

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
    if (InstructionCounts::hasBegun()) [[unlikely]]
        LoopCounts::logAndReset();
}

void ParallelLoops::sendRestingThreadsBack() const
{
    m_restingThreads->close();
}

void ParallelLoops::askThreadsAtWorkForFreeMemory() const
{
    if (!m_hostPool || !m_hostPool->giveFreeMemoryOfCurrentThreadBack)
        return;
    s_giveFreeMemoryOfCurrentThreadBack.store(m_hostPool->giveFreeMemoryOfCurrentThreadBack, std::memory_order_relaxed);
    s_timesAskedForFreeMemory.fetch_add(1, std::memory_order_seq_cst);
}

void ParallelLoops::giveFreeMemoryBack() const
{
    askThreadsAtWorkForFreeMemory();
    if (!m_hostPool || !m_hostPool->giveFreeMemoryOfCurrentThreadBack)
        return;
    // They are woken for nothing else.
    RestingThreads::Task task { [](void*) { giveFreeMemoryOfCurrentThreadBackIfAsked(TurnToGiveBack::IsWaitedFor); }, nullptr, TaskPriority::Foreground, InheritedWork::ofCurrentThread() };
    if (!m_restingThreads->handOverToAll(task))
        giveFreeMemoryOfCurrentThreadBackIfAsked(TurnToGiveBack::IsWaitedFor);
}

unsigned ParallelLoops::numberOfRestingThreads() const
{
    return m_restingThreads->numberOfRestingThreads();
}

bool ParallelLoops::shouldYield(TaskPriority priority) const
{
    return m_restingThreads->hasPendingTaskAbove(priority);
}

void ParallelLoops::schedule(TaskPriority priority, void (*run)(void* argument), void* argument) const
{
    RestingThreads::Task task { run, argument, priority, InheritedWork::ofCurrentThread() };
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

// Work that its caller runs, and that threads of the pool join while there is something for them to take.
class SharedWork final : public ThreadSafeRefCounted<SharedWork> {
public:
    using Work = ScopedLambda<WorkResult(Participant)>;

    static unsigned run(const ParallelLoops& loops, unsigned maxHelpers, NOESCAPE const Work& work, NOESCAPE const ScopedLambda<bool()>& hasWorkToTake)
    {
        Ref sharedWork = adoptRef(*new SharedWork(loops, maxHelpers, work, hasWorkToTake));
        sharedWork->runOnCaller();
        return sharedWork->m_numberOfHelpersAskedFor.load(std::memory_order_relaxed);
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
        for (uint32_t state = m_state.load(); state != isClosed; state = m_state.load())
            ParkingLot::compareAndPark(&m_state, state);
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
    startBuildThread("AOT build"_s, [&] {
        functor();
    })->waitForCompletion();
}

NEVER_INLINE void forEachIndexInParallel(const ParallelLoops& loops, unsigned count, NOESCAPE const ScopedLambda<void(unsigned index, unsigned worker)>& functor)
{
    if (count < 2 || loops.width() == 1) {
        SetForScope isInsideLoop(t_isInsideLoop, t_isInsideLoop || count > 1);
        for (unsigned index = 0; index < count; ++index)
            functor(index, 0);
        return;
    }

    // A worker's first run is one index, because callers put the largest work first, and each next run is twice as long.
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
    const bool isCounted = InstructionCounts::hasBegun();
    const MonotonicTime start = isCounted ? MonotonicTime::now() : MonotonicTime();
    unsigned numberOfHelpers = SharedWork::run(loops, maxWorkers - 1, [&](Participant participant) {
        unsigned worker = canYield ? workerNumbers->take() : numberOfWorkers.fetch_add(1, std::memory_order_relaxed);
        RELEASE_ASSERT(worker < maxWorkers);
        SetForScope isInsideLoop(t_isInsideLoop, true);
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
            giveFreeMemoryOfCurrentThreadBackIfAsked(TurnToGiveBack::IsTakenIfFree);
            run = std::min(run * 2, longestRun);
            first = next.load(std::memory_order_relaxed);
        }
        return WorkResult::RanOut;
    }, [&] {
        return next.load(std::memory_order_relaxed) < count;
    });
    if (isCounted) [[unlikely]]
        LoopCounts::add(count, MonotonicTime::now() - start, numberOfHelpers);
}

NEVER_INLINE void forEachIndexInParallel(const ParallelLoops& loops, unsigned count, NOESCAPE const ScopedLambda<void(unsigned index)>& functor)
{
    forEachIndexInParallel(loops, count, [&](unsigned index, unsigned) {
        functor(index);
    });
}

NEVER_INLINE void forEachIndexInParallel(const ParallelLoops& loops, unsigned count, std::span<const uint32_t> longestFirst, NOESCAPE const ScopedLambda<void(unsigned index)>& functor)
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
    forEachIndexInParallel(loops, numberFirst + count, [&](unsigned position, unsigned) {
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

ChunksOfIndices divideIntoChunks(unsigned count, std::span<const uint32_t> longestFirst)
{
    constexpr unsigned chunkSize = 256;
    Vector<uint32_t> ascending(longestFirst);
    AOT::sort(ascending);
    Vector<uint32_t> firstIndexOf;
    firstIndexOf.reserveInitialCapacity(count / chunkSize + 2 * ascending.size() + 2);
    Vector<uint32_t> chunkOfLongIndex;
    chunkOfLongIndex.reserveInitialCapacity(ascending.size());
    uint32_t nextIndex = 0;
    auto addChunksUpTo = [&](uint32_t end) {
        for (; nextIndex < end; nextIndex = std::min(end, nextIndex + chunkSize))
            firstIndexOf.append(nextIndex);
    };
    for (uint32_t longIndex : ascending) {
        addChunksUpTo(longIndex);
        chunkOfLongIndex.append(firstIndexOf.size());
        addChunksUpTo(longIndex + 1);
    }
    addChunksUpTo(count);
    firstIndexOf.append(count);

    Vector<uint32_t> longestChunksFirst(longestFirst.size(), [&](size_t position) {
        size_t rank = std::ranges::lower_bound(ascending, longestFirst[position]) - ascending.begin();
        return chunkOfLongIndex[rank];
    });
    return { WTF::move(firstIndexOf), WTF::move(longestChunksFirst) };
}

void sortRunsAndMerge(const ParallelLoops& loops, const SortSteps& steps, char* list, size_t count)
{
    constexpr size_t numberOfRuns = 16;
    size_t elementSize = steps.elementSize();
    size_t numberOfWords = (count * elementSize + sizeof(uint64_t) - 1) / sizeof(uint64_t);
    Vector<uint64_t> scratchBuffer = createVectorWithCapacityInParallel<uint64_t>(loops, numberOfWords);
    scratchBuffer.grow(numberOfWords);
    char* from = list;
    char* to = reinterpret_cast<char*>(scratchBuffer.mutableSpan().data());
    auto startOf = [&](size_t run, size_t runs) {
        return count * run / runs;
    };
    forEachIndexInParallel(loops, numberOfRuns, [&](unsigned run) {
        size_t start = startOf(run, numberOfRuns);
        sortLongRange(steps, from + start * elementSize, startOf(run + 1, numberOfRuns) - start);
    });
    // The last round merges two runs: one thread would go through the whole list, and two through the round in front of it.
    constexpr size_t partsPerRound = 4 * numberOfRuns;
    for (size_t runs = numberOfRuns; runs > 1; runs /= 2) {
        size_t partsPerMerge = partsPerRound / (runs / 2);
        forEachIndexInParallel(loops, partsPerRound, [&](unsigned index) {
            size_t pair = index / partsPerMerge;
            size_t startOfFirst = startOf(2 * pair, runs);
            size_t startOfSecond = startOf(2 * pair + 1, runs);
            size_t sizeOfFirst = startOfSecond - startOfFirst;
            size_t sizeOfSecond = startOf(2 * pair + 2, runs) - startOfSecond;
            char* first = from + startOfFirst * elementSize;
            char* second = from + startOfSecond * elementSize;
            auto beginOf = [&](size_t part) {
                if (part == partsPerMerge)
                    return std::pair { sizeOfFirst, sizeOfSecond };
                size_t inFirst = sizeOfFirst * part / partsPerMerge;
                size_t inSecond = 0;
                for (size_t length = part ? sizeOfSecond : 0; length;) {
                    size_t half = length / 2;
                    if (steps.isLess(second + (inSecond + half) * elementSize, first + inFirst * elementSize)) {
                        inSecond += half + 1;
                        length -= half + 1;
                    } else
                        length = half;
                }
                return std::pair { inFirst, inSecond };
            };
            auto [firstBegin, secondBegin] = beginOf(index % partsPerMerge);
            auto [firstEnd, secondEnd] = beginOf(index % partsPerMerge + 1);
            steps.merge(first + firstBegin * elementSize, first + firstEnd * elementSize, second + secondBegin * elementSize, second + secondEnd * elementSize, to + (startOfFirst + firstBegin + secondBegin) * elementSize);
        });
        std::swap(from, to);
    }
    static_assert(numberOfRuns == 16, "an even number of rounds of merging leaves the result in the list");
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
