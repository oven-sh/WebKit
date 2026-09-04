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

#include "config.h"
#include "TerminationDeadline.h"

#include "VM.h"
#include "VMLite.h"
#include <wtf/TZoneMallocInlines.h>

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(TerminationDeadline);
WTF_MAKE_TZONE_ALLOCATED_IMPL(TerminationDeadlineSet);

void TerminationDeadline::cancel(VM& vm)
{
    ASSERT(vm.currentThreadIsHoldingAPILock());
    if (RefPtr set = vm.m_terminationDeadlines)
        set->remove(*this);
}

Ref<TerminationDeadline> TerminationDeadlineSet::add(MonotonicTime at)
{
    Locker locker { m_lock };
    ASSERT(m_vm);
    VMLite* lite = nullptr;
    if (m_vm->gilOff()) [[unlikely]] {
        lite = VMLite::currentIfExists();
        if (lite && lite->vm != m_vm)
            lite = nullptr;
    }
    Ref deadline = adoptRef(*new TerminationDeadline(at, lite));
    size_t index = 0;
    while (index < m_deadlines.size() && m_deadlines[index]->m_deadline <= at)
        ++index;
    m_deadlines.insert(index, deadline.copyRef());
    armTimerIfNeeded();
    return deadline;
}

void TerminationDeadlineSet::remove(TerminationDeadline& deadline)
{
    Locker locker { m_lock };
    m_deadlines.removeFirstMatching([&](auto& d) { return d.ptr() == &deadline; });
}

void TerminationDeadlineSet::willDestroyVM()
{
    Locker locker { m_lock };
    m_vm = nullptr;
    m_deadlines.clear();
}

void TerminationDeadlineSet::armTimerIfNeeded()
{
    if (m_deadlines.isEmpty())
        return;
    MonotonicTime earliest = m_deadlines.first()->m_deadline;
    if (m_timerArmedFor <= earliest) // Also: never arm for a deadline at infinity.
        return;
    m_timerArmedFor = earliest;
    VMTraps::queue().dispatchAfter(std::max(earliest - MonotonicTime::now(), 0_s), [protectedThis = Ref { *this }, earliest] {
        protectedThis->timerFired(earliest);
    });
}

void TerminationDeadlineSet::timerFired(MonotonicTime armedFor)
{
    Locker locker { m_lock };
    if (m_timerArmedFor == armedFor)
        m_timerArmedFor = MonotonicTime::infinity();
    if (!m_vm)
        return;
    MonotonicTime now = MonotonicTime::now();
    bool dueForVM = false;
    Vector<VMLite*, 2> dueForThreads;
    while (!m_deadlines.isEmpty() && m_deadlines.first()->m_deadline <= now) {
        m_deadlines.first()->m_fired.store(true, std::memory_order_release);
        if (VMLite* lite = m_deadlines.first()->m_lite)
            dueForThreads.append(lite);
        else
            dueForVM = true;
        m_deadlines.removeAt(0);
    }
    if (dueForVM)
        m_vm->notifyNeedTermination();
    // A lite that is no longer registered left the VM without cancelling, and there is nothing to stop.
    for (VMLite* lite : dueForThreads)
        m_vm->traps().fireTargetedTermination(*lite);
    armTimerIfNeeded();
}

} // namespace JSC
