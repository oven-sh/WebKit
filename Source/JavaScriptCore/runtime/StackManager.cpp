/*
 * Copyright (C) 2025 Apple Inc. All rights reserved.
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
#include "StackManager.h"

#include "StackManagerInlines.h"

namespace JSC {

CONCURRENT_SAFE void StackManager::requestStop()
{
    Locker lock { m_mirrorLock };
    m_trapAwareSoftStackLimit.storeRelaxed(stopRequestMarker());
    for (auto& mirror : m_mirrors)
        mirror.m_trapAwareSoftStackLimit.storeRelaxed(stopRequestMarker());
}

// Note (V7-1, C_LOOP): cancelStop restores m_trapAwareSoftStackLimit from
// m_softStackLimit, which on CLoop builds may be null/stale until the next
// setCLoopStackLimit republish (next CLoopStack publish hook, i.e. the next
// native/slow-path call). This is the same self-correcting residual the
// carrier slot already has flag-on; the per-lite publish reroute
// (CLoopStack::publishStackLimit) does not widen it.
CONCURRENT_SAFE void StackManager::cancelStop()
{
    if (Options::forceTrapAwareStackChecks()) [[unlikely]]
        return;

    Locker lock { m_mirrorLock };
    void* softStackLimit = m_softStackLimit.loadRelaxed();
    m_trapAwareSoftStackLimit.storeRelaxed(softStackLimit);
    for (auto& mirror : m_mirrors)
        mirror.m_trapAwareSoftStackLimit.storeRelaxed(softStackLimit);
}

// V7: the m_softStackLimit store moved under m_mirrorLock (it previously
// preceded the Locker) so it pairs with the locked readers in cancelStop and
// registerMirror, and is a relaxed atomic store so the lock-free readers
// (the no-lite C++ fallback chain — softStackLimitForCurrentThread,
// softStackLimitForCurrentThreadGilOffSlow, VM::updateStackLimits' pre-read —
// and generated code's raw loads) are race-free too. Relaxed suffices: every
// reader tolerates a stale limit by design (the AB-17 dual-publish residual),
// and ordering against the m_trapAwareSoftStackLimit publishes is provided by
// m_mirrorLock for the stop-protocol participants — lock-free readers never
// derive control flow from the PAIR of words, only from one or the other.
void StackManager::setStackSoftLimit(void* newStackLimit)
{
    Locker lock { m_mirrorLock };
    m_softStackLimit.storeRelaxed(newStackLimit);
#if !ENABLE(C_LOOP)
    if (!hasStopRequest())
        m_trapAwareSoftStackLimit.storeRelaxed(newStackLimit);
    void* newTrapAwareSoftStackLimit = trapAwareSoftStackLimit();
#endif
    for (auto& mirror : m_mirrors) {
#if !ENABLE(C_LOOP)
        mirror.m_trapAwareSoftStackLimit.storeRelaxed(newTrapAwareSoftStackLimit);
#endif
        mirror.m_softStackLimit.storeRelaxed(newStackLimit);
    }
}

#if ENABLE(C_LOOP)
// V7: m_cloopStackLimit itself stays a plain word — per the header comment it
// is single-writer/single-reader per lite (the owning thread via the
// CLoopStack publish routing), and C_LOOP is outside the V7 TSAN rung (the
// TSAN build is non-CLoop). The write is moved under m_mirrorLock anyway so
// the carrier-instance write is ordered like setStackSoftLimit's; the mirror
// fanout uses the relaxed-atomic mirror word as above. The V7-1 cancelStop
// residual note (top of file) remains valid: cancelStop restores from
// m_softStackLimit, which on CLoop builds is republished here.
void StackManager::setCLoopStackLimit(void* newStackLimit)
{
    Locker lock { m_mirrorLock };
    m_cloopStackLimit = newStackLimit;
    if (!hasStopRequest())
        m_trapAwareSoftStackLimit.storeRelaxed(newStackLimit);

    void* newTrapAwareSoftStackLimit = trapAwareSoftStackLimit();
    for (auto& mirror : m_mirrors) {
        mirror.m_trapAwareSoftStackLimit.storeRelaxed(newTrapAwareSoftStackLimit);
        mirror.m_softStackLimit.storeRelaxed(newStackLimit);
    }
}
#endif

void StackManager::registerMirror(StackManager::Mirror& mirror)
{
    Locker lock { m_mirrorLock };
    mirror.m_trapAwareSoftStackLimit.storeRelaxed(trapAwareSoftStackLimit());
    mirror.m_softStackLimit.storeRelaxed(m_softStackLimit.loadRelaxed());
    m_mirrors.append(&mirror);
}

void StackManager::unregisterMirror(StackManager::Mirror& mirror)
{
    Locker lock { m_mirrorLock };
    m_mirrors.remove(&mirror);
}

} // namespace JSC
