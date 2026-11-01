/*
 * Copyright (C) 2016-2020 Apple Inc. All rights reserved.
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
#include <wtf/AutomaticThread.h>

#include <wtf/DataLog.h>
#include <wtf/FastMalloc.h>
#include <wtf/PageBlock.h>
#include <wtf/ParkingLot.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Threading.h>

namespace WTF {

WTF_MAKE_TZONE_ALLOCATED_IMPL(AutomaticThread);

static constexpr bool verbose = false;

#if USE(MIMALLOC)
// How long a thread waits for more work before it releases its allocator's free memory.
static constexpr Seconds idleReleaseDelay = 100_ms;
#endif

Ref<AutomaticThreadCondition> AutomaticThreadCondition::create()
{
    return adoptRef(*new AutomaticThreadCondition);
}

AutomaticThreadCondition::AutomaticThreadCondition() = default;

AutomaticThreadCondition::~AutomaticThreadCondition() = default;

void AutomaticThreadCondition::notifyOne(const AbstractLocker& locker)
{
#if USE(MIMALLOC)
    // A thread that is releasing its free memory only sees the notification when it is done with that.
    for (auto& thread : m_threads) {
        if (thread->isWaiting(locker) && !thread->m_isReleasingFreeMemory) {
            thread->notify(locker);
            return;
        }
    }
#endif

    for (auto& thread : m_threads) {
        if (thread->isWaiting(locker)) {
            thread->notify(locker);
            return;
        }
    }

    for (auto& thread : m_threads) {
        if (!thread->hasUnderlyingThread(locker)) {
            thread->start(locker);
            return;
        }
    }

    m_condition.notifyOne();
}

void AutomaticThreadCondition::notifyAll(const AbstractLocker& locker)
{
    m_condition.notifyAll();

    for (auto& thread : m_threads) {
        if (thread->isWaiting(locker))
            thread->notify(locker);
        else if (!thread->hasUnderlyingThread(locker))
            thread->start(locker);
    }
}

#if ENABLE(AOT)
bool AutomaticThreadCondition::tryNotifyOne(const AbstractLocker& locker)
{
#if USE(MIMALLOC)
    for (auto& thread : m_threads) {
        if (thread->isWaiting(locker) && !thread->m_isReleasingFreeMemory) {
            thread->notify(locker);
            return true;
        }
    }
#endif

    for (auto& thread : m_threads) {
        if (thread->isWaiting(locker)) {
            thread->notify(locker);
            return true;
        }
    }

    for (auto& thread : m_threads) {
        // If this one is refused, the others are refused for the same reason: do not try them.
        if (!thread->hasUnderlyingThread(locker))
            return thread->tryEnsureUnderlyingThread(locker);
    }

    m_condition.notifyOne();
    return true;
}

bool AutomaticThreadCondition::tryNotifyAll(const AbstractLocker& locker)
{
    m_condition.notifyAll();

    bool startedAll = true;
    for (auto& thread : m_threads) {
        if (thread->isWaiting(locker))
            thread->notify(locker);
        else if (startedAll && !thread->hasUnderlyingThread(locker))
            startedAll = thread->tryEnsureUnderlyingThread(locker);
    }
    return startedAll;
}
#endif

void AutomaticThreadCondition::wait(Lock& lock)
{
    m_condition.wait(lock);
}

bool AutomaticThreadCondition::waitFor(Lock& lock, Seconds time)
{
    return m_condition.waitFor(lock, time);
}

void AutomaticThreadCondition::add(const AbstractLocker&, AutomaticThread* thread)
{
    ASSERT(!m_threads.contains(thread));
    m_threads.append(thread);
}

void AutomaticThreadCondition::remove(const AbstractLocker&, AutomaticThread* thread)
{
    m_threads.removeFirstMatching([&](auto& t) { return t.get() == thread; });
    ASSERT(!m_threads.contains(thread));
}

bool AutomaticThreadCondition::contains(const AbstractLocker&, AutomaticThread* thread)
{
    return m_threads.containsIf([&](auto& t) { return t.get() == thread; });
}

AutomaticThread::AutomaticThread(const AbstractLocker& locker, Box<Lock> lock, Ref<AutomaticThreadCondition>&& condition, Seconds timeout)
    : AutomaticThread(locker, lock, WTF::move(condition), ThreadType::Unknown, timeout)
{
}

AutomaticThread::AutomaticThread(const AbstractLocker& locker, Box<Lock> lock, Ref<AutomaticThreadCondition>&& condition, ThreadType type, Seconds timeout)
    : AutomaticThread(locker, lock, WTF::move(condition), type, Thread::defaultQOS, timeout)
{
}

AutomaticThread::AutomaticThread(const AbstractLocker& locker, Box<Lock> lock, Ref<AutomaticThreadCondition>&& condition, ThreadType type, ThreadQOS qos, Seconds timeout)
    : m_lock(lock)
    , m_condition(WTF::move(condition))
    , m_timeout(timeout)
    , m_threadType(type)
    , m_qos(qos)
{
    if (verbose)
        dataLog(RawPointer(this), ": Allocated AutomaticThread.\n");
    m_condition->add(locker, this);
}

AutomaticThread::~AutomaticThread()
{
    if (verbose)
        dataLog(RawPointer(this), ": Deleting AutomaticThread.\n");
    Locker locker { *m_lock };
    
    // It's possible that we're in a waiting state with the thread shut down. This is a goofy way to
    // die, but it could happen.
    m_condition->remove(locker, this);
}

bool AutomaticThread::tryStop(const AbstractLocker&)
{
    if (!m_isRunning)
        return true;
    if (m_hasUnderlyingThread)
        return false;
    m_isRunning = false;
    return true;
}

bool AutomaticThread::isWaiting(const AbstractLocker& locker)
{
    return hasUnderlyingThread(locker) && m_isWaiting;
}

bool AutomaticThread::notify(const AbstractLocker& locker)
{
    ASSERT_UNUSED(locker, hasUnderlyingThread(locker));
    m_isWaiting = false;
    return m_waitCondition.notifyOne();
}

void AutomaticThread::join()
{
    Locker locker { *m_lock };
    while (m_isRunning)
        m_isRunningCondition.wait(*m_lock);
}

#if ENABLE(AOT)
bool AutomaticThread::tryEnsureUnderlyingThread(const AbstractLocker& locker)
{
    if (!m_hasUnderlyingThread)
        start(locker, FailedCreation::IsReported);
    return m_hasUnderlyingThread;
}

bool AutomaticThread::s_threadsSayWhenTheyAreUp { false };

void AutomaticThread::setThreadsSayWhenTheyAreUp()
{
    s_threadsSayWhenTheyAreUp = true;
}

void AutomaticThread::waitUntilIsUp(const AbstractLocker&)
{
    while (s_threadsSayWhenTheyAreUp && m_hasUnderlyingThread && !m_isUp)
        m_isUpCondition.wait(*m_lock);
}

void AutomaticThread::start(const AbstractLocker&, FailedCreation failedCreation)
#else
void AutomaticThread::start(const AbstractLocker&)
#endif
{
    RELEASE_ASSERT(m_isRunning);
    
    RefPtr<AutomaticThread> preserveThisForThread = this;
    
    m_hasUnderlyingThread = true;
#if ENABLE(AOT)
    m_isUp = false;
#endif

    auto stackSpec = stackSpecification();
    switch (stackSpec.kind()) {
    case StackAllocationSpecification::Kind::Default:
        break;
    case StackAllocationSpecification::Kind::SizeAndLocation:
        RELEASE_ASSERT(!(reinterpret_cast<uintptr_t>(stackSpec.stackSpan().data()) % pageSize()));
        [[fallthrough]];
    case StackAllocationSpecification::Kind::SizeOnly:
        RELEASE_ASSERT(!(stackSpec.sizeBytes() % pageSize()));
        break;
    }

#if ENABLE(AOT)
    auto entryPoint =
#else
    Thread::create(
        name(),
#endif
        [=, this] () {
            if (verbose)
                dataLog(RawPointer(this), ": Running automatic thread!\n");
            
            RefPtr<AutomaticThread> thread = preserveThisForThread;
            thread->threadDidStart();

#if ENABLE(AOT)
            if (s_threadsSayWhenTheyAreUp) [[unlikely]] {
                // A park whose validation fails allocates this thread's ParkingLot data and the address's bucket, and does not park.
                for (const void* address : { static_cast<const void*>(m_lock.get()), static_cast<const void*>(&m_waitCondition) })
                    ParkingLot::parkConditionally(address, [] { return false; }, [] { }, TimeWithDynamicClockType(MonotonicTime::infinity()));
                Locker locker { *m_lock };
                m_isUp = true;
                m_isUpCondition.notifyAll();
            }
#endif

            if (ASSERT_ENABLED) {
                Locker locker { *m_lock };
                ASSERT(m_condition->contains(locker, this));
            }
            
            auto stopImpl = [&] (const AbstractLocker& locker) {
                thread->threadIsStopping(locker);
                thread->m_hasUnderlyingThread = false;
#if ENABLE(AOT)
                if (s_threadsSayWhenTheyAreUp) [[unlikely]]
                    m_isUpCondition.notifyAll();
#endif
            };
            
            auto stopPermanently = [&] (const AbstractLocker& locker) {
                m_isRunning = false;
                m_isRunningCondition.notifyAll();
                stopImpl(locker);
            };
            
            auto stopForTimeout = [&] (const AbstractLocker& locker) {
                stopImpl(locker);
            };
            
#if USE(MIMALLOC)
            // Whether this thread released its allocator's free memory since it last worked.
            bool didReleaseFreeMemory = false;
#endif

            for (;;) {
                {
                    Locker locker { *m_lock };
                    for (;;) {
                        PollResult result = poll(locker);
                        if (result == PollResult::Work)
                            break;
                        if (result == PollResult::Stop)
                            return stopPermanently(locker);
                        RELEASE_ASSERT(result == PollResult::Wait);

                        // Shut the thread down after a timeout.
                        m_isWaiting = true;
#if USE(MIMALLOC)
                        // Only the owning thread can return the memory its thread-local heap holds.
                        // A compiler thread frees tens of MB of temporaries after a large compile,
                        // and without this they stay resident until the thread times out and exits.
                        // Wait a little first, so that a thread which is notified again right away
                        // (the usual case between tasks) does not pay for the release.
                        //
                        // poll() runs once per notify: JITWorklistThread counts a thread as active
                        // from the notify until its poll() returns Wait. So the thread stays in the
                        // waiting state through the release, and polls again only if a notify
                        // arrived meanwhile (notify() clears m_isWaiting, which is checked under the
                        // lock, so a notify during the release is not lost).
                        Seconds timeout = m_timeout;
                        if (!didReleaseFreeMemory && m_timeout > idleReleaseDelay) {
                            m_waitCondition.waitFor(*m_lock, idleReleaseDelay);
                            if (!m_isWaiting)
                                continue;
                            didReleaseFreeMemory = true;
                            m_isReleasingFreeMemory = true;
                            {
                                DropLockForScope dropLock { locker };
                                releaseFastMallocFreeMemoryForIdleThread();
                            }
                            m_isReleasingFreeMemory = false;
                            if (!m_isWaiting)
                                continue;
                            timeout -= idleReleaseDelay;
                        }
#else
                        Seconds timeout = m_timeout;
#endif
                        bool awokenByNotify =
                            m_waitCondition.waitFor(*m_lock, timeout);
                        if (verbose && !awokenByNotify && !m_isWaiting)
                            dataLog(RawPointer(this), ": waitFor timed out, but notified via m_isWaiting flag!\n");
                        if (m_isWaiting && shouldSleep(locker)) {
                            m_isWaiting = false;
                            if (verbose)
                                dataLog(RawPointer(this), ": Going to sleep!\n");
                            // It's important that we don't release the lock until we have completely
                            // indicated that the thread is kaput. Otherwise we'll have a a notify
                            // race that manifests as a deadlock on VM shutdown.
                            return stopForTimeout(locker);
                        }
                    }
                }
                
#if USE(MIMALLOC)
                didReleaseFreeMemory = false;
#endif
                WorkResult result = work();
                if (result == WorkResult::Stop) {
                    Locker locker { *m_lock };
                    return stopPermanently(locker);
                }
                RELEASE_ASSERT(result == WorkResult::Continue);
            }
#if ENABLE(AOT)
        };

    if (Thread::survivesFailedCreation() && (failedCreation == FailedCreation::IsReported || isOptional())) {
        if (RefPtr thread = Thread::tryCreate(name(), WTF::move(entryPoint), m_threadType, m_qos, Thread::defaultSchedulingPolicy, stackSpec))
            thread->detach();
        else
            m_hasUnderlyingThread = false;
        return;
    }
    Thread::create(name(), WTF::move(entryPoint), m_threadType, m_qos, Thread::defaultSchedulingPolicy, stackSpec)->detach();
#else
        }, m_threadType, m_qos, Thread::defaultSchedulingPolicy, stackSpec)->detach();
#endif
}

void AutomaticThread::threadDidStart()
{
}

void AutomaticThread::threadIsStopping(const AbstractLocker&)
{
}

} // namespace WTF
