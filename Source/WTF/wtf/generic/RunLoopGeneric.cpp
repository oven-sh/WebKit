/*
 * Copyright (C) 2016 Konstantin Tokavev <annulen@yandex.ru>
 * Copyright (C) 2016 Yusuke Suzuki <utatane.tea@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include <wtf/RunLoop.h>

#include <wtf/DataLog.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/ProcessID.h>

namespace WTF {

static constexpr bool report = false;

class RunLoop::TimerBase::ScheduledTask final : public ThreadSafeRefCounted<ScheduledTask>, public RedBlackTree<ScheduledTask, MonotonicTime>::ThreadSafeNode {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ScheduledTask);
    WTF_MAKE_NONCOPYABLE(ScheduledTask);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(ScheduledTask);
public:
    static Ref<ScheduledTask> create(RunLoop::TimerBase& timer)
    {
        return adoptRef(*new ScheduledTask(timer));
    }

    void fired()
    {
        if (!isActive())
            return;

        if (!m_isRepeating)
            deactivate();

        if (isActive())
            updateReadyTime();

        m_timer.fired();
    }

    MonotonicTime scheduledTimePoint() const
    {
        return m_scheduledTimePoint;
    }

    void updateReadyTime()
    {
        ASSERT(!isScheduled());
        m_scheduledTimePoint = MonotonicTime::now();
        if (!m_fireInterval)
            return;
        m_scheduledTimePoint += m_fireInterval;
    }

    MonotonicTime key() const
    {
        return scheduledTimePoint();
    }

    bool isScheduled() const
    {
        return m_isScheduled;
    }

    void setScheduled(bool flag)
    {
        m_isScheduled = flag;
    }

    bool isActive() const
    {
        return m_isActive.load();
    }

    void activate(Seconds interval, bool repeating)
    {
        m_fireInterval = interval;
        m_isRepeating = repeating;
        updateReadyTime();
        m_isActive.store(true);
    }

    void deactivate()
    {
        m_isActive.store(false);
    }

private:
    ScheduledTask(RunLoop::TimerBase& timer)
        : m_timer(timer)
    {
    }

    RunLoop::TimerBase& m_timer;
    MonotonicTime m_scheduledTimePoint;
    Seconds m_fireInterval;
    std::atomic<bool> m_isActive { };
    bool m_isRepeating { };
    bool m_isScheduled { };
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(RunLoop::TimerBase::ScheduledTask);

#if USE(BUN_EVENT_LOOP)
// This constructor and destructor can't be fixed by the `#define RunLoop RunLoop::RunLoopGenericState`
// because that would make their names include `RunLoop::` twice.
RunLoop::RunLoopGenericState::RunLoopGenericState(RunLoop& parent)
    : m_parent(parent)
{
}
#else
RunLoop::RunLoop() = default;
#endif

#if USE(BUN_EVENT_LOOP)
RunLoop::RunLoopGenericState::~RunLoopGenericState()
#else
RunLoop::~RunLoop()
#endif
{
    Locker locker { m_loopLock };
    m_shutdown = true;
    m_readyToRun.notifyOne();

    // Here is running main loops. Wait until all the main loops are destroyed.
    if (!m_mainLoops.isEmpty())
        m_stopCondition.wait(m_loopLock);
}

#if USE(BUN_EVENT_LOOP)
// Make the following member functions be defined on the right class
#define RunLoop RunLoop::RunLoopGenericState
#endif

inline bool RunLoop::populateTasks(RunMode runMode, Status& statusOfThisLoop, Deque<Ref<TimerBase::ScheduledTask>>& firedTimers)
{
    Locker locker { m_loopLock };

    if (runMode == RunMode::Drain) {
        MonotonicTime sleepUntil = MonotonicTime::infinity();
        if (!m_schedules.isEmpty())
            sleepUntil = m_schedules.first()->scheduledTimePoint();

        m_readyToRun.waitUntil(m_loopLock, sleepUntil, [&] {
            return m_shutdown || m_pendingTasks || statusOfThisLoop == Status::Stopping;
        });
    }

    if (statusOfThisLoop == Status::Stopping || m_shutdown) {
        m_mainLoops.removeLast();
        if (m_mainLoops.isEmpty())
            m_stopCondition.notifyOne();
        return false;
    }
    m_pendingTasks = false;
    if (runMode == RunMode::Iterate)
        statusOfThisLoop = Status::Stopping;

    // Check expired timers.
    MonotonicTime now = MonotonicTime::now();
    while (!m_schedules.isEmpty()) {
        auto task = m_schedules.first();
        if (task->scheduledTimePoint() > now)
            break;
        unscheduleWithLock(*task);
        firedTimers.append(Ref(*task));
    }

    return true;
}

void RunLoop::runImpl(RunMode runMode)
{
#if USE(BUN_EVENT_LOOP)
    // We need to #undef because RunLoopGenericState doesn't have a current()
    // And we need &m_parent instead of `this` because `this` is a RunLoopGenericState,
    // not a RunLoop.
#undef RunLoop
    ASSERT(&m_parent == &RunLoop::currentSingleton());
    // Undo the #undef above
#define RunLoop RunLoop::RunLoopGenericState
#else
    ASSERT(this == &RunLoop::currentSingleton());
#endif

    if constexpr (report) {
        static LazyNeverDestroyed<Timer> reporter;
        static std::once_flag onceKey;
        std::call_once(onceKey, [&] {
#if USE(BUN_EVENT_LOOP)
            // construct() needs a RunLoop, not a RunLoopGenericState
            reporter.construct(m_parent, "RunLoop::runImpl::Reporter"_s, [this] {
#else
            reporter.construct(*this, "RunLoop::runImpl::Reporter"_s, [this] {
#endif
                unsigned count = 0;
                unsigned active = 0;
                for (auto task = m_schedules.first(); task; task = task->successor()) {
                    count++;
                    if (task->isActive())
                        active++;
                }

                dataLogF("[RunLoop][pid %d main %s] m_schedules size %u active %u\n",
                    getCurrentProcessID(),
                    (isMain() ? "true" : "false"),
                    count,
                    active);
            });
            reporter->startRepeating(30_s);
            dataLogF("[RunLoop][pid %d main %s] Reporter installed\n",
                getCurrentProcessID(),
                (isMain() ? "true" : "false"));
        });
    }

    Status statusOfThisLoop = Status::Clear;
    {
        Locker locker { m_loopLock };
        m_mainLoops.append(&statusOfThisLoop);
    }

    Deque<Ref<TimerBase::ScheduledTask>> firedTimers;
    while (true) {
        if (!populateTasks(runMode, statusOfThisLoop, firedTimers))
            return;

        // Dispatch scheduled timers.
        while (!firedTimers.isEmpty()) {
            auto task = firedTimers.takeFirst();
            task->fired();

            Locker locker { m_loopLock };
            // It is possible the task is already scheduled while executing fired().
            if (task->isActive() && !task->isScheduled()) {
                // Reschedule because the timer requires repeating.
                // Since we will query the timers' time points before sleeping,
                // we do not call wakeUp() here.
                scheduleWithLock(task.get());
            }
        }
#if USE(BUN_EVENT_LOOP)
        // This function is defined on RunLoop, not RunLoopGenericState (and it's in RunLoop.cpp so
        // it isn't getting moved to RunLoopGenericState)
        m_parent.performWork();
#else
        performWork();
#endif
    }
}

#if !USE(BUN_EVENT_LOOP)
// RunLoop::run() is defined in RunLoopBun.cpp
// RunLoop::setWakeUpCallback() is left undefined as it only exists for the generic and Windows
// run loops, so the rest of WebKit would never call it with USE(BUN_EVENT_LOOP) defined
void RunLoop::run()
{
    RunLoop::currentSingleton().runImpl(RunMode::Drain);
}

void RunLoop::setWakeUpCallback(WTF::Function<void()>&& function)
{
    RunLoop::currentSingleton().m_wakeUpCallback = WTF::move(function);
}
#endif

// RunLoop operations are thread-safe. These operations can be called from outside of the RunLoop's thread.
// For example, WorkQueue::{dispatch, dispatchAfter} call the operations of the WorkQueue thread's RunLoop
// from the caller's thread.

void RunLoop::stop()
{
    Locker locker { m_loopLock };
    if (m_mainLoops.isEmpty())
        return;

    Status* status = m_mainLoops.last();
    if (*status != Status::Stopping) {
        *status = Status::Stopping;
        m_readyToRun.notifyOne();
    }
}

void RunLoop::wakeUpWithLock()
{
    m_pendingTasks = true;
    m_readyToRun.notifyOne();

    if (m_wakeUpCallback)
        m_wakeUpCallback();
}

void RunLoop::wakeUp()
{
    Locker locker { m_loopLock };
    wakeUpWithLock();
}

#if !USE(BUN_EVENT_LOOP)
RunLoop::CycleResult RunLoop::cycle(RunLoopMode)
{
    RunLoop::currentSingleton().runImpl(RunMode::Iterate);
    return CycleResult::Continue;
}
#endif

void RunLoop::scheduleWithLock(TimerBase::ScheduledTask& task)
{
    if (!task.isScheduled()) {
        m_schedules.insert(&task);
        task.setScheduled(true);
    }
}

void RunLoop::unscheduleWithLock(TimerBase::ScheduledTask& task)
{
    if (task.isScheduled()) {
        m_schedules.remove(&task);
        task.setScheduled(false);
    }
}

#if USE(BUN_EVENT_LOOP)
#undef RunLoop
// Now we have to make the TimerBase implementations work. They all access fields on m_runLoop, but
// those fields won't exist anymore since we moved them to RunLoopGenericState. So we need to make
// them access m_genericState (this is undefined behavior if m_genericState is nullopt, but none
// of these functions should even get called if the run loop is not the generic implementation).
#define m_runLoop (m_runLoop->m_genericState)
// And m_scheduledTask needs to be accessed via the std::variant on TimerBase
#define m_scheduledTask (std::get<Ref<ScheduledTask>>(m_impl))
#endif

// Since RunLoop does not own the registered TimerBase,
// TimerBase and its owner should manage these lifetime.
#if !USE(BUN_EVENT_LOOP)
// For Bun, this is defined in RunLoopBun.cpp
RunLoop::TimerBase::TimerBase(Ref<RunLoop>&& runLoop, ASCIILiteral description)
    : m_runLoop(WTF::move(runLoop))
    , m_description(description)
    , m_scheduledTask(ScheduledTask::create(*this))
{
}
#endif

#if USE(BUN_EVENT_LOOP)
// For Bun, ~TimerBase() is defined in RunLoopBun.cpp and it will call destructGeneric() if it
// needs to.
void RunLoop::TimerBase::destructGeneric()
#else
RunLoop::TimerBase::~TimerBase()
#endif
{
    // An active timer must be stopped/destroyed on its run loop's thread: fired() runs the timer's
    // callback on that thread without holding m_loopLock, and the TimerBase is not ref-counted, so
    // tearing it down from another thread races with the in-flight callback and risks a use-after-free.
    // (Starting a timer cross-thread is safe and supported -- that is how dispatch()/dispatchAfter()
    // schedule work onto another run loop.)
    if (m_scheduledTask->isActive())
#if USE(BUN_EVENT_LOOP)
        assertIsCurrent(m_runLoop->m_parent);
#else
        assertIsCurrent(m_runLoop);
#endif
    Locker locker { m_runLoop->m_loopLock };
    stopWithLock();
}

#if USE(BUN_EVENT_LOOP)
void RunLoop::TimerBase::startGeneric(Seconds interval, bool repeating)
#else
void RunLoop::TimerBase::start(Seconds interval, bool repeating)
#endif
{
    Locker locker { m_runLoop->m_loopLock };
    stopWithLock();
    m_scheduledTask->activate(interval, repeating);
    m_runLoop->scheduleWithLock(m_scheduledTask.get());
    m_runLoop->wakeUpWithLock();
}

void RunLoop::TimerBase::stopWithLock()
{
    m_runLoop->unscheduleWithLock(m_scheduledTask.get());
    m_scheduledTask->deactivate();
}

#if USE(BUN_EVENT_LOOP)
void RunLoop::TimerBase::stopGeneric()
#else
void RunLoop::TimerBase::stop()
#endif
{
    Locker locker { m_runLoop->m_loopLock };
    if (m_scheduledTask->isActive())
#if USE(BUN_EVENT_LOOP)
        assertIsCurrent(m_runLoop->m_parent);
#else
        assertIsCurrent(m_runLoop);
#endif
    stopWithLock();
}

#if USE(BUN_EVENT_LOOP)
bool RunLoop::TimerBase::isActiveGeneric() const
#else
bool RunLoop::TimerBase::isActive() const
#endif
{
    Locker locker { m_runLoop->m_loopLock };
    return isActiveWithLock();
}

bool RunLoop::TimerBase::isActiveWithLock() const
{
    return m_scheduledTask->isActive();
}

#if USE(BUN_EVENT_LOOP)
Seconds RunLoop::TimerBase::secondsUntilFireGeneric() const
#else
Seconds RunLoop::TimerBase::secondsUntilFire() const
#endif
{
    Locker locker { m_runLoop->m_loopLock };
    if (isActiveWithLock())
        return std::max<Seconds>(m_scheduledTask->scheduledTimePoint() - MonotonicTime::now(), 0_s);
    return 0_s;
}

#if USE(BUN_EVENT_LOOP)
#undef m_runLoop
#undef m_scheduledTask
#endif

} // namespace WTF
