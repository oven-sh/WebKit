/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTThreads.h"

#if ENABLE(AOT)

#include "AOTCurrentWork.h"
#include "AOTEndOfThread.h"
#include "AOTPhaseTimes.h"
#include "AOTSystemResources.h"
#include <atomic>
#include <limits>
#include <optional>
#include <wtf/Atomics.h>
#include <wtf/MonotonicTime.h>
#include <wtf/ParkingLot.h>
#include <wtf/Seconds.h>
#include <wtf/SetForScope.h>
#include <wtf/StackAllocation.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Threading.h>

namespace JSC { namespace AOT {

static thread_local bool t_isInsideLoop = false;
static thread_local const ParallelLoops* t_currentLoops = nullptr;
static thread_local unsigned t_workerNumber = 0;
static thread_local const ParallelLoops* t_loopsOfLane = nullptr;
constinit thread_local void* t_whatHelpersInherit = nullptr;

namespace {

struct WorkAtEndOfThread {
    ~WorkAtEndOfThread()
    {
        while (!functions.isEmpty())
            functions.takeLast()();
    }

    Vector<void (*)()> functions;
};

} // anonymous namespace

void runAtEndOfThread(void (*function)())
{
    WTF_THREAD_LOCAL_ALWAYS_DESTROY static thread_local WorkAtEndOfThread work;
    work.functions.append(function);
}

bool ParallelLoops::isInsideLoop()
{
    return t_isInsideLoop;
}

const ParallelLoops& ParallelLoops::ofCurrentThread() const
{
    if (t_currentLoops && t_currentLoops->m_loopsOfBuild == this)
        return *t_currentLoops;
    if (t_loopsOfLane && t_loopsOfLane->m_loopsOfBuild == this)
        return *t_loopsOfLane;
    return *this;
}

ParallelLoops::ParallelLoops(unsigned width, const HostThreadPool* hostPool)
    : m_width(width)
    , m_hostPool(hostPool)
    , m_loopsOfBuild(this)
{
    RELEASE_ASSERT(width);
    if (width == 1 || hostPool)
        return;
    m_ownPool = WorkerPool::create("AOT worker"_s, width - 1, Seconds::infinity(), StackAllocationSpecification::RequestSize(workerStackSize));
}

ParallelLoops::ParallelLoops(LaneTag, const ParallelLoops& loopsOfStarter)
    : m_width(loopsOfStarter.m_width)
    , m_hostPool(loopsOfStarter.m_hostPool)
    , m_loopsOfBuild(loopsOfStarter.m_loopsOfBuild)
    , m_ownPool(loopsOfStarter.m_ownPool)
{
    RELEASE_ASSERT(m_hostPool || m_ownPool);
}

void ParallelLoops::schedule(void (*run)(void* argument), void* argument) const
{
    if (m_hostPool) {
        m_hostPool->schedule(run, argument);
        return;
    }
    m_ownPool->postTask([run, argument] {
        run(argument);
    });
}

namespace {

enum class LoopParticipant : bool { Caller, Helper };

class SharedWork final : public ThreadSafeRefCounted<SharedWork> {
public:
    using Work = ScopedLambda<void(LoopParticipant)>;

    static void run(const ParallelLoops& loops, unsigned maxHelpers, NOESCAPE const Work& work, NOESCAPE const ScopedLambda<bool()>& hasWorkToTake)
    {
        Ref sharedWork = adoptRef(*new SharedWork(loops, maxHelpers, work, hasWorkToTake));
        sharedWork->runOnCaller();
    }

private:
    SharedWork(const ParallelLoops& loops, unsigned maxHelpers, const Work& work, const ScopedLambda<bool()>& hasWorkToTake)
        : m_loops(loops)
        , m_maxHelpers(maxHelpers)
        , m_work(work)
        , m_hasWorkToTake(hasWorkToTake)
        , m_whatHelpersInherit(t_whatHelpersInherit)
    {
    }

    void runOnCaller()
    {
        askForHelper();
        m_work(LoopParticipant::Caller);
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
        {
            SetForScope whatHelpersInherit(t_whatHelpersInherit, m_whatHelpersInherit);
            m_work(LoopParticipant::Helper);
        }
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
        m_loops.schedule([](void* sharedWork) {
            if (InstructionCounts::hasBegun()) [[unlikely]]
                InstructionCounts::countOnCurrentThread();
            adoptRef(*static_cast<SharedWork*>(sharedWork))->runOnHelper();
        }, this);
    }

    static constexpr uint32_t isClosed = 1;
    static constexpr uint32_t oneHelperAtWork = 2;

    const ParallelLoops& m_loops;
    const unsigned m_maxHelpers;
    const Work& m_work;
    const ScopedLambda<bool()>& m_hasWorkToTake;
    void* const m_whatHelpersInherit;
    std::atomic<unsigned> m_numberOfHelpersAskedFor { 0 };
    Atomic<uint32_t> m_state { 0 };
};

} // anonymous namespace

class Lanes final : public ThreadSafeRefCounted<Lanes> {
public:
    using Lane = ScopedLambda<void(unsigned lane, const ParallelLoops& loopsOfLane)>;

    static void run(const ParallelLoops& loops, unsigned count, NOESCAPE const Lane& lane)
    {
        Ref lanes = adoptRef(*new Lanes(loops, count, lane));
        lanes->runOnCaller();
    }

private:
    Lanes(const ParallelLoops& loops, unsigned count, const Lane& lane)
        : m_loops(loops)
        , m_count(count)
        , m_lane(lane)
        , m_stepOfCaller(currentWorkOfThread().step.load(std::memory_order_relaxed))
        , m_whatHelpersInherit(t_whatHelpersInherit)
        , m_numberOfLanesLeft(count - 1)
    {
    }

    void runOnCaller()
    {
        for (unsigned lane = 1; lane < m_count; ++lane) {
            ref();
            m_loops.schedule([](void* lanes) {
                if (InstructionCounts::hasBegun()) [[unlikely]]
                    InstructionCounts::countOnCurrentThread();
                adoptRef(*static_cast<Lanes*>(lanes))->runOnPoolThread();
            }, this);
        }
        m_lane(0, m_loops);
        while (std::optional<unsigned> lane = takeLane()) {
            m_lane(*lane, m_loops);
            m_numberOfLanesLeft.exchangeSub(1);
        }
        for (uint32_t left = m_numberOfLanesLeft.load(); left; left = m_numberOfLanesLeft.load())
            ParkingLot::compareAndPark(&m_numberOfLanesLeft, left);
    }

    void runOnPoolThread()
    {
        while (std::optional<unsigned> lane = takeLane()) {
            ASSERT(Thread::currentSingleton().stack().size() >= ParallelLoops::workerStackSize / 2);
            {
                ParallelLoops loopsOfLane(ParallelLoops::ForLane, m_loops);
                SetForScope laneOfThread(t_loopsOfLane, &loopsOfLane);
                SetForScope isInsideLoop(t_isInsideLoop, false);
                SetForScope currentLoops(t_currentLoops, nullptr);
                SetForScope workerNumber(t_workerNumber, 0u);
                SetForScope whatHelpersInherit(t_whatHelpersInherit, m_whatHelpersInherit);
                std::optional<CurrentWorkScope> workOfCaller;
                if (m_stepOfCaller)
                    workOfCaller.emplace(ASCIILiteral::fromLiteralUnsafe(m_stepOfCaller));
                m_lane(*lane, loopsOfLane);
            }
            if (m_numberOfLanesLeft.exchangeSub(1) == 1)
                ParkingLot::unparkAll(&m_numberOfLanesLeft);
        }
    }

    std::optional<unsigned> takeLane()
    {
        unsigned lane = m_nextLane.exchangeAdd(1);
        if (lane >= m_count)
            return std::nullopt;
        return lane;
    }

    const ParallelLoops& m_loops;
    const unsigned m_count;
    const Lane& m_lane;
    const char* const m_stepOfCaller;
    void* const m_whatHelpersInherit;
    Atomic<unsigned> m_nextLane { 1 };
    Atomic<uint32_t> m_numberOfLanesLeft;
};

void runLanes(const ParallelLoops& loops, unsigned count, NOESCAPE const ScopedLambda<void(unsigned lane, const ParallelLoops& loopsOfLane)>& lane)
{
    if (count < 2 || loops.width() == 1 || t_isInsideLoop) {
        for (unsigned index = 0; index < count; ++index)
            lane(index, loops);
        return;
    }
    RELEASE_ASSERT(loops.areThoseOfCurrentThread());
    Lanes::run(loops, count, lane);
}

void runLanes(const ParallelLoops& loops, std::initializer_list<ScopedLambda<void()>> lanes)
{
    runLanes(loops, static_cast<unsigned>(lanes.size()), [&](unsigned lane, const ParallelLoops&) {
        lanes.begin()[lane]();
    });
}

void runOnBuildThread(NOESCAPE const ScopedLambda<void()>& functor)
{
    const CurrentWork& caller = currentWorkOfThread();
    const Typed::Program* programOfCaller = caller.program.load(std::memory_order_relaxed);
    uint32_t functionPlusOneOfCaller = caller.functionPlusOne.load(std::memory_order_relaxed);
    const char* stepOfCaller = caller.step.load(std::memory_order_relaxed);
    const ParallelLoops* loopsOfCaller = t_currentLoops ? t_currentLoops : t_loopsOfLane;
    void* inheritedFromCaller = t_whatHelpersInherit;
    startBuildThread("AOT build"_s, [&] {
        if (InstructionCounts::hasBegun()) [[unlikely]]
            InstructionCounts::countOnCurrentThread();
        SetForScope laneOfThread(t_loopsOfLane, loopsOfCaller);
        SetForScope whatHelpersInherit(t_whatHelpersInherit, inheritedFromCaller);
        std::optional<CurrentWorkScope> workOfCaller;
        if (stepOfCaller && programOfCaller && functionPlusOneOfCaller)
            workOfCaller.emplace(*programOfCaller, FunctionID(functionPlusOneOfCaller - 1), ASCIILiteral::fromLiteralUnsafe(stepOfCaller));
        else if (stepOfCaller)
            workOfCaller.emplace(ASCIILiteral::fromLiteralUnsafe(stepOfCaller));
        functor();
    })->waitForCompletion();
}

NEVER_INLINE void forEachIndexInParallel(const ParallelLoops& loops, unsigned count, NOESCAPE const ScopedLambda<void(unsigned index, unsigned worker)>& functor)
{
    if (count < 2 || loops.width() == 1 || t_isInsideLoop) {
        unsigned worker = t_currentLoops == &loops ? t_workerNumber : 0;
        SetForScope isInsideLoop(t_isInsideLoop, t_isInsideLoop || count > 1);
        for (unsigned index = 0; index < count; ++index)
            functor(index, worker);
        return;
    }
    ASSERT(loops.areThoseOfCurrentThread());

    constexpr unsigned longestRun = 256;
    constexpr Seconds longestTimeOfRunThatGrows = Seconds::fromMicroseconds(50);
    const unsigned maxWorkers = std::min(loops.width(), count);
    RELEASE_ASSERT(count <= std::numeric_limits<unsigned>::max() - maxWorkers * longestRun);
    const unsigned shares = 4 * maxWorkers;
    alignas(128) std::atomic<unsigned> next { 0 };
    alignas(128) std::atomic<unsigned> numberOfHelpers { 0 };
    const char* stepOfCaller = currentWorkOfThread().step.load(std::memory_order_relaxed);
    std::optional<uint64_t> instructionsInFrontOfLoop = InstructionCounts::hasBegun() ? InstructionCounts::ofCurrentThread() : std::nullopt;
    SharedWork::run(loops, maxWorkers - 1, [&](LoopParticipant participant) {
        std::optional<CurrentWorkScope> workOfCaller;
        if (participant == LoopParticipant::Helper && stepOfCaller)
            workOfCaller.emplace(ASCIILiteral::fromLiteralUnsafe(stepOfCaller));
        unsigned worker = participant == LoopParticipant::Caller ? 0 : 1 + numberOfHelpers.fetch_add(1, std::memory_order_relaxed);
        RELEASE_ASSERT(worker < maxWorkers);
        SetForScope isInsideLoop(t_isInsideLoop, true);
        SetForScope currentLoops(t_currentLoops, &loops);
        SetForScope workerNumber(t_workerNumber, worker);
        unsigned run = 1;
        MonotonicTime startOfRun = MonotonicTime::now();
        for (unsigned taken = next.load(std::memory_order_relaxed); taken < count;) {
            unsigned length = std::max({ std::min(run, (count - taken) / shares), run / 8, 1u });
            unsigned first = next.fetch_add(length, std::memory_order_relaxed);
            if (first >= count)
                break;
            unsigned end = first + std::min(length, count - first);
            for (unsigned index = first; index < end; ++index)
                functor(index, worker);
            MonotonicTime endOfRun = MonotonicTime::now();
            run = endOfRun - startOfRun <= longestTimeOfRunThatGrows ? std::min(run * 2, longestRun) : std::max(run / 2, 1u);
            startOfRun = endOfRun;
            taken = end;
        }
    }, [&] {
        return next.load(std::memory_order_relaxed) < count;
    });
    if (instructionsInFrontOfLoop) [[unlikely]] {
        if (std::optional<uint64_t> instructionsBehindLoop = InstructionCounts::ofCurrentThread())
            InstructionCounts::noteSpentInsideLoop(*instructionsBehindLoop - *instructionsInFrontOfLoop);
    }
}

NEVER_INLINE void forEachIndexInParallel(const ParallelLoops& loops, unsigned count, NOESCAPE const ScopedLambda<void(unsigned index)>& functor)
{
    forEachIndexInParallel(loops, count, [&](unsigned index, unsigned) {
        functor(index);
    });
}

void runTasksInParallel(const ParallelLoops& loops, std::initializer_list<ScopedLambda<void()>> tasks)
{
    forEachIndexInParallel(loops, static_cast<unsigned>(tasks.size()), [&](unsigned index) {
        tasks.begin()[index]();
    });
}

Ref<BackgroundJobs> BackgroundJobs::create(const ParallelLoops& loops)
{
    return adoptRef(*new BackgroundJobs(loops));
}

BackgroundJobs::BackgroundJobs(const ParallelLoops& loops)
    : m_buildLoops(loops.hostPool() && loops.width() > 1 ? nullptr : &loops)
    , m_hostPool(m_buildLoops ? HostThreadPool { } : *loops.hostPool())
    , m_jobLoops(m_buildLoops ? 1 : loops.width(), m_buildLoops ? nullptr : &m_hostPool)
{
}

void BackgroundJobs::add(Job&& job)
{
    if (m_buildLoops) {
        job(m_buildLoops->ofCurrentThread());
        return;
    }

    {
        Locker locker { m_lock };
        m_jobs.append(WTF::move(job));
        if (m_isRunning)
            return;
        m_isRunning = true;
    }
    ref();
    m_jobLoops.schedule([](void* jobs) {
        if (InstructionCounts::hasBegun()) [[unlikely]]
            InstructionCounts::countOnCurrentThread();
        adoptRef(*static_cast<BackgroundJobs*>(jobs))->runJobs();
    }, this);
}

size_t BackgroundJobs::numberOfJobsWaiting() const
{
    Locker locker { m_lock };
    return m_jobs.size();
}

void BackgroundJobs::waitUntilAllHaveRun()
{
    Locker locker { m_lock };
    while (m_isRunning)
        m_allHaveRun.wait(m_lock);
}

void BackgroundJobs::runJobs()
{
    for (;;) {
        Job job;
        {
            Locker locker { m_lock };
            if (m_jobs.isEmpty()) {
                m_isRunning = false;
                m_allHaveRun.notifyAll();
                return;
            }
            job = m_jobs.takeFirst();
        }
        job(m_jobLoops);
    }
}

ChunksOfIndices divideIntoChunks(unsigned count)
{
    constexpr unsigned indicesInChunk = 256;
    Vector<unsigned> firstIndexOf;
    firstIndexOf.reserveInitialCapacity(count / indicesInChunk + 2);
    for (unsigned first = 0; first < count; first += std::min(indicesInChunk, count - first))
        firstIndexOf.append(first);
    firstIndexOf.append(count);
    return { WTF::move(firstIndexOf) };
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
