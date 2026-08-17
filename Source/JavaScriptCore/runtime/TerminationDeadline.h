/*
 * Copyright (C) 2026 Anthropic PBC.
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

#pragma once

#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Vector.h>

namespace JSC {

class VM;

// A request that a VM terminate its current execution once a wall-clock deadline passes: an embedder's
// time limit on one bounded call (see VM::addTerminationDeadline). Not the Watchdog, which is a CPU-time
// budget for a whole VM entry, one per VM, that every later VM entry keeps paying for. When the deadline
// passes, VM::notifyNeedTermination() is called from a timer thread — unless the deadline was cancelled
// first — and that is safe against the VM having been destroyed by then.
class TerminationDeadline final : public ThreadSafeRefCounted<TerminationDeadline> {
    WTF_MAKE_TZONE_ALLOCATED(TerminationDeadline);
public:
    // The deadline passed and termination has been requested — VM::notifyNeedTermination() had already
    // been called by the time this became true — whether or not the VM has acted on it yet.
    bool didFire() const { return m_fired.load(std::memory_order_acquire); }
    // It does not fire from here on (didFire() is final once this returns). VM's thread, API lock held.
    JS_EXPORT_PRIVATE void cancel(VM&);

private:
    friend class TerminationDeadlineSet;
    explicit TerminationDeadline(MonotonicTime deadline)
        : m_deadline(deadline)
    {
    }

    const MonotonicTime m_deadline;
    std::atomic<bool> m_fired { false };
};

// A VM's pending deadlines, earliest first, and the timer serving them: one WorkQueue timer for the earliest
// deadline (plus superseded ones after earlier deadlines are added), the Watchdog's shape, so that cancelled
// deadlines do not pile up as timers.
class TerminationDeadlineSet final : public ThreadSafeRefCounted<TerminationDeadlineSet> {
    WTF_MAKE_TZONE_ALLOCATED(TerminationDeadlineSet);
public:
    static Ref<TerminationDeadlineSet> create(VM& vm) { return adoptRef(*new TerminationDeadlineSet(vm)); }

    Ref<TerminationDeadline> add(MonotonicTime);
    void remove(TerminationDeadline&);
    void willDestroyVM();

private:
    explicit TerminationDeadlineSet(VM& vm)
        : m_vm(&vm)
    {
    }
    void armTimerIfNeeded() WTF_REQUIRES_LOCK(m_lock);
    void timerFired(MonotonicTime armedFor);

    Lock m_lock;
    VM* m_vm WTF_GUARDED_BY_LOCK(m_lock);
    Vector<Ref<TerminationDeadline>, 2> m_deadlines WTF_GUARDED_BY_LOCK(m_lock);
    MonotonicTime m_timerArmedFor WTF_GUARDED_BY_LOCK(m_lock) { MonotonicTime::infinity() };
};

} // namespace JSC
