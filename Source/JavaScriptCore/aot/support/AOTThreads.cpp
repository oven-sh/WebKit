/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTThreads.h"

#if ENABLE(AOT)

#include <atomic>
#include <optional>
#include <wtf/Atomics.h>
#include <wtf/MonotonicTime.h>
#include <wtf/ParkingLot.h>
#include <wtf/StackAllocation.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Threading.h>
#include <wtf/UniqueArray.h>

namespace JSC { namespace AOT {

// A pool wakes its threads one after the other, each when the one before has found work. That way a loop waits milliseconds for its helpers, and
// on a busy machine a hundred times longer. So a thread of the pool that has run out of work rests here for a while, and a task goes to a resting
// thread first. Whoever has the task wakes that thread itself: a loop whose helpers each ask for two more has them all after a few steps.
class ParallelLoops::RestingThreads final : public ThreadSafeRefCounted<RestingThreads> {
public:
    struct Task {
        void (*run)(void* argument) { nullptr };
        void* argument { nullptr };
    };

    struct PostedTask {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(PostedTask);

        static void run(void* argument)
        {
            std::unique_ptr<PostedTask> posted(static_cast<PostedTask*>(argument));
            posted->restingThreads->runOnPoolThread(posted->task);
        }

        Ref<RestingThreads> restingThreads;
        Task task;
    };

    static Ref<RestingThreads> create(unsigned numberOfSeats, Seconds restTime)
    {
        return adoptRef(*new RestingThreads(numberOfSeats, restTime));
    }

    bool tryHandOver(Task task)
    {
        return wakeOne(HasTask, task);
    }

    void willPost()
    {
        m_numberOfPostedTasks.fetch_add(1, std::memory_order_seq_cst);
    }

    // A thread may have come to rest since none was found, and the pool may have no other thread for the task.
    void didPost()
    {
        wakeOne(IsSentBack, { });
    }

    void runOnPoolThread(Task task)
    {
        m_numberOfPostedTasks.fetch_sub(1, std::memory_order_seq_cst);
        task.run(task.argument);

        std::optional<uint32_t> index = m_freeSeats.pop(m_seats.get());
        if (!index)
            return;
        Seat& seat = m_seats[*index];
        for (;;) {
            seat.state.store(IsResting);
            m_restingSeats.push(m_seats.get(), *index);
            switch (rest(seat)) {
            case HasTask:
                break;
            case IsSentBack:
                m_freeSeats.push(m_seats.get(), *index);
                return;
            default:
                return;
            }
            Task next = seat.task;
            next.run(next.argument);
        }
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
            result += m_seats[index].state.load() == IsResting;
        return result;
    }

private:
    // The thread of a seat that is resting is in the list of resting seats. Whoever takes the seat out of the list gives it a task or sends it back
    // to the pool. A thread that leaves by itself leaves its seat in the list, for whoever takes it out to find it empty.
    enum State : uint32_t { IsEmpty, IsResting, HasTask, IsSentBack };

    struct Seat {
        Atomic<uint32_t> state { IsEmpty };
        std::atomic<uint32_t> next { 0 };
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
        // Either this thread sees the posted task, or whoever posted it sees this thread in the list.
        bool shouldLeave = m_numberOfPostedTasks.load(std::memory_order_seq_cst) || m_isClosed.load(std::memory_order_seq_cst);
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
    std::atomic<unsigned> m_numberOfPostedTasks { 0 };
    std::atomic<bool> m_isClosed { false };
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

unsigned ParallelLoops::numberOfRestingThreads() const
{
    return m_restingThreads->numberOfRestingThreads();
}

void ParallelLoops::schedule(void (*run)(void* argument), void* argument) const
{
    RestingThreads::Task task { run, argument };
    if (m_restingThreads->tryHandOver(task))
        return;

    m_restingThreads->willPost();
    auto* posted = new RestingThreads::PostedTask { m_restingThreads.copyRef(), task };
    if (m_hostPool)
        m_hostPool->schedule(RestingThreads::PostedTask::run, posted);
    else {
        m_ownPool->postTask([posted] {
            RestingThreads::PostedTask::run(posted);
        });
    }
    m_restingThreads->didPost();
}

namespace {

// Work that its caller runs, and that threads of the pool join while there is something for them to take. Each helper that finds
// some asks for two more, so no thread is woken for work that is gone. A helper that arrives after the work ended only drops its reference.
class SharedWork final : public ThreadSafeRefCounted<SharedWork> {
public:
    static void run(const ParallelLoops& loops, unsigned maxHelpers, NOESCAPE const ScopedLambda<void()>& work, NOESCAPE const ScopedLambda<bool()>& hasWorkToTake)
    {
        adoptRef(*new SharedWork(loops, maxHelpers, work, hasWorkToTake))->runOnCaller();
    }

private:
    SharedWork(const ParallelLoops& loops, unsigned maxHelpers, const ScopedLambda<void()>& work, const ScopedLambda<bool()>& hasWorkToTake)
        : m_loops(loops)
        , m_maxHelpers(maxHelpers)
        , m_work(work)
        , m_hasWorkToTake(hasWorkToTake)
    {
    }

    void runOnCaller()
    {
        askForHelper();
        m_work();
        close();
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
        m_work();
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
        if (m_numberOfHelpersAskedFor.fetch_add(1, std::memory_order_relaxed) >= m_maxHelpers)
            return;
        ref();
        m_loops.schedule([](void* sharedWork) {
            adoptRef(*static_cast<SharedWork*>(sharedWork))->runOnHelper();
        }, this);
    }

    static constexpr uint32_t isClosed = 1;
    static constexpr uint32_t oneHelperAtWork = 2;

    // All but the state belongs to the caller, which stays in runOnCaller() while a helper is at work.
    const ParallelLoops& m_loops;
    const unsigned m_maxHelpers;
    const ScopedLambda<void()>& m_work;
    const ScopedLambda<bool()>& m_hasWorkToTake;
    std::atomic<unsigned> m_numberOfHelpersAskedFor { 0 };
    Atomic<uint32_t> m_state { 0 };
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
    // The last runs are short, so that the workers end together.
    constexpr unsigned longestRun = 256;
    const unsigned maxWorkers = std::min(loops.width(), count);
    const unsigned shares = 4 * maxWorkers;
    alignas(128) std::atomic<unsigned> next { 0 };
    alignas(128) std::atomic<unsigned> numberOfWorkers { 0 };
    SharedWork::run(loops, maxWorkers - 1, [&] {
        unsigned worker = numberOfWorkers.fetch_add(1, std::memory_order_relaxed);
        RELEASE_ASSERT(worker < maxWorkers);
        unsigned run = 1;
        for (unsigned first = next.load(std::memory_order_relaxed); first < count;) {
            unsigned end = first + std::max(std::min(run, (count - first) / shares), 1u);
            if (!next.compare_exchange_weak(first, end, std::memory_order_relaxed))
                continue;
            for (unsigned index = first; index < end; ++index)
                functor(index, worker);
            run = std::min(run * 2, longestRun);
            first = next.load(std::memory_order_relaxed);
        }
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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
