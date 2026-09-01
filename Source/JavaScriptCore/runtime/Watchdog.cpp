/*
 * Copyright (C) 2013-2023 Apple Inc. All rights reserved.
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
#include "Watchdog.h"

#include "JSLock.h"
#include "ThreadManager.h"
#include "VM.h"
#include "VMEntryScopeInlines.h"
#include "VMLite.h"  // UNGIL annex W W1 (U-T11): clear the check bit on the carrier lite's word too.
#include "VMTraps.h" // perThreadTrapsIfExists.
#include <wtf/CPUTime.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(Watchdog);

// UNGIL annex W ext (W4): the four watchdog API-lock asserts guard state with
// no serializer other than today's GIL; under §F.2's redefined token meaning,
// N spawned threads would satisfy them simultaneously. GIL-off they become
// the mutex-literal predicate (the REAL JSLock m_lock — §F.1 keeps
// main/embedder mutual exclusion, so at most one carrier is between the
// asserts at a time) plus the spawned-unreachability lint (spawned JS is
// watchdog-unobserved in v1 — SD14). GIL-on/flag-off byte-identical.
static ALWAYS_INLINE void assertWatchdogStateAccess(VM* vm)
{
#if ASSERT_ENABLED
    ASSERT(vm);
    if (vm->gilOff()) {
        ASSERT(vm->apiLock().currentThreadIsHoldingLock());
        ASSERT(!ThreadManager::isJSThreadCurrent());
        return;
    }
    ASSERT(vm->currentThreadIsHoldingAPILock());
#else
    UNUSED_PARAM(vm);
#endif
}

// W0 enforcement point: spawned Thread entry/exit must toggle neither
// carrier-entered state nor the timer (annex W; SD14).
static ALWAYS_INLINE bool isSpawnedThreadGILOff(bool gilOff)
{
    return gilOff && ThreadManager::isJSThreadCurrent();
}

Watchdog::Watchdog(VM* vm)
    : m_vm(vm)
    , m_gilOff(vm->gilOff()) // U0c: computed at the top of the VM ctor; immutable.
{
}

void Watchdog::setTimeLimit(Seconds limit, ShouldTerminateCallback callback, void* data1, void* data2)
{
    assertWatchdogStateAccess(m_vm); // W4 (was: currentThreadIsHoldingAPILock).

    Locker locker { m_lock };

    m_timeLimit = limit;
    m_callback = callback;
    m_callbackData1 = data1;
    m_callbackData2 = data2;

    bool entered = m_gilOff ? !!m_carrierEnteredDepth : m_hasEnteredVM;
    if (entered && hasTimeLimit())
        startTimer(m_timeLimit);
}

bool Watchdog::shouldTerminate(JSGlobalObject* globalObject, CallerState callerState)
{
    assertWatchdogStateAccess(m_vm); // W4. W1 service from a reacquired parked carrier satisfies this too.

    {
        Locker locker { m_lock };

        Seconds epsilon;
#if OS(WINDOWS)
        // We can reach this point as much as 15ms before the deadline on Windows,
        // in which case the watchdog will never get to do its job.
        // Adding this leeway shouldn't cause a problem for other platforms
        // (since the "deadline is infinity" case should be the crucial one),
        // but it is a fact that only Windows is experiencing the issue.
        epsilon = 20_ms;
#endif
        if (MonotonicTime::timePointFromNow(epsilon) < m_deadline)
            return false; // Just a stale timer firing. Nothing to do.

        // Set m_deadline to MonotonicTime::infinity() here so that we can reject all future
        // spurious wakes.
        m_deadline = MonotonicTime::infinity();
        m_wallClockArmed = false;

        // W1 parked-carrier verdict (see Watchdog.h CallerState): a parked
        // carrier's CPU clock does not advance, so consulting the CPU budget
        // here would re-arm and return no-terminate on EVERY W1 episode —
        // the carrier re-parks, the timer re-fires after the (constant)
        // remaining CPU budget worth of wall time, and the park never fails:
        // an unkillable park / livelock (reproduced GIL-off by
        // api/condition-wait-termination.js and
        // atomics/property-wait-termination.js before this arm existed).
        // The wall-clock deadline (already validated above) is authoritative
        // for a parked carrier; fall through to the embedder callback, which
        // can still grant an extension (annex-W shape (a)).
        if (callerState != CallerState::ParkedCarrier) {
            auto cpuTime = CPUTime::forCurrentThread();
            if (cpuTime < m_cpuDeadline) {
                auto remainingCPUTime = m_cpuDeadline - cpuTime;
                startTimer(remainingCPUTime);
                return false;
            }
        } else {
            // The parked verdict consumes this arming: clear the (bypassed)
            // CPU budget so the case-3 restart below ("callback did nothing
            // — allow another cycle") sees callbackAlreadyStartedTimer ==
            // false and actually re-arms; otherwise a do-nothing extension
            // callback would leave the watchdog permanently disarmed against
            // a still-parked carrier (the livelock's quieter twin).
            m_cpuDeadline = noTimeLimit;
        }
    }

    // Note: we should not be holding the lock while calling the callbacks. The callbacks may
    // call setTimeLimit() which will try to lock as well.

    // If m_callback is not set, then we terminate by default.
    // Else, we let m_callback decide if we should terminate or not.
    bool needsTermination = !m_callback
        || m_callback(globalObject, m_callbackData1, m_callbackData2);
    if (needsTermination)
        return true;

    // If we get here, then the callback above did not want to terminate execution. As a
    // result, the callback may have done one of the following:
    //   1. cleared the time limit (i.e. watchdog is disabled),
    //   2. set a new time limit via Watchdog::setTimeLimit(), or
    //   3. did nothing (i.e. allow another cycle of the current time limit).
    //
    // In the case of 1, we don't have to do anything.
    // In the case of 2, Watchdog::setTimeLimit() would already have started the timer.
    // In the case of 3, we need to re-start the timer here.

    Locker locker { m_lock };

    // W2 re-point: GIL-off the entered predicate is the carrier depth (a
    // §J.3-parked W1 servicer still counts — its entry scope is live).
    ASSERT(m_gilOff ? !!m_carrierEnteredDepth : m_hasEnteredVM);

    bool callbackAlreadyStartedTimer = (m_cpuDeadline != noTimeLimit);
    if (hasTimeLimit() && !callbackAlreadyStartedTimer)
        startTimer(m_timeLimit);

    return false;
}

bool Watchdog::serviceCheckFromReacquiredParkedCarrier(VM& vm)
{
    // W1 (annex W): the caller is a main/embedder carrier that was parked
    // under §J.3, observed the watchdog-check bit at a D9 quantum
    // (parkLitePollWatchdogCheckRequested, VMTraps.cpp — the GIL-off W1
    // split of the landed D9 predicate), and has already performed the FULL
    // exit reacquisition — so the W4 predicate (real JSLock held, not
    // spawned) holds here exactly as for an entered carrier.
    //
    // WIRING (U-T11; updated at AB-17): driven through the reacquire/
    // service/re-release episode helper
    // reacquireParkedCarrierAndServiceWatchdogCheck (JSLock.cpp). ALL §J.3
    // park sites are now re-pointed consumers of this W1 episode: the SD6
    // per-wait TA sync park (WaiterListManager.cpp, the first consumer,
    // including the r15 F2 old-node disposition on the no-terminate return),
    // join (ThreadObject.cpp), cond.wait (ConditionObject.cpp), lock.hold
    // (LockObject.cpp), and property Atomics.wait (ThreadAtomics.cpp) — the
    // AB-17 W1/D9 park-site split. The landed GIL-on fold-into-termination
    // shape no longer runs GIL-off at any park site; the verdict below uses
    // the parked-carrier mode of shouldTerminate() (wall clock authoritative
    // — a parked carrier accrues no CPU, so the CPU-budget arm would
    // livelock; see Watchdog.h CallerState).
    ASSERT(vm.gilOff());
    ASSERT(!ThreadManager::isJSThreadCurrent()); // SD14: spawned threads never service the watchdog.
    ASSERT(vm.apiLock().currentThreadIsHoldingLock());

    // Take the bit first (mirrors takeTopPriorityTrap's take-then-service):
    // a false return means a racing entered service already consumed it —
    // nothing to do, the caller just re-parks. CLEAR ON BOTH WORDS: the
    // predicate that triggered this episode
    // (parkLitePollWatchdogCheckRequested) reads the CAPTURED park lite's
    // word via perThreadTrapsIfExists — under today's §A.2.1 interim alias
    // that IS the VM word, but once the alias flips to per-lite words a
    // VM-word-only clear would leave the lite bit set forever and the parked
    // carrier would re-run this W1 episode every D9 quantum (reacquisition +
    // embedder callback hammered — an unbounded livelock with an
    // extension-granting callback). The §J.3 reacquisition the caller just
    // performed restored that same carrier lite as current, so clearing the
    // CURRENT lite's word here clears exactly the word the predicate read;
    // under the alias the two clears hit one word and the second is a no-op.
    // This keeps the VMTraps.cpp banner promise that the alias flip stays a
    // VMLite.cpp-only change — no Watchdog.cpp re-point needed at flip time.
    bool tookBit = vm.traps().clearTrap(VMTraps::NeedWatchdogCheck);
    if (VMLite* carrierLite = VMLite::currentIfExists(); carrierLite && carrierLite->vm == &vm) {
        if (VMTraps* liteTraps = perThreadTrapsIfExists(*carrierLite); liteTraps && liteTraps != &vm.traps())
            tookBit |= liteTraps->clearTrap(VMTraps::NeedWatchdogCheck);
    }
    if (!tookBit)
        return false;

    auto* watchdog = vm.watchdog();
    if (!watchdog || !watchdog->isActive())
        return false;

    // A parked carrier's entry scope is live (it never tore it down — that is
    // also why it still counts in m_carrierEnteredDepth, making the W3 timer
    // branch route to notifyNeedWatchdogCheck() instead of firing directly).
    VMEntryScope* entryScope = vm.currentThreadEntryScope();
    RELEASE_ASSERT(entryScope);

    // Callback semantics identical to an entered carrier — stale-timer
    // rejection, embedder callback with extension support (shape (a) of the
    // U-T2 corpus), timer restart on no-terminate — EXCEPT the CPU-budget
    // re-arm, which is skipped via CallerState::ParkedCarrier: a parked
    // carrier accrues no CPU, so the budget arm would no-terminate + re-arm
    // on every episode forever (unkillable park). Wall clock is
    // authoritative for the parked verdict.
    if (!watchdog->shouldTerminate(entryScope->globalObject(), CallerState::ParkedCarrier))
        return false; // Caller: r15 F2 old-node disposition, then re-park (new episode).

    // Terminate: VM-wide (rule 3) — the looping spawned threads observe it at
    // poll sites / D9 quanta, parked siblings via their park predicates. The
    // caller fails its own park per SD8/§E.5.
    vm.traps().fireTerminationVMWideAfterParkedCarrierService();
    return true;
}

bool Watchdog::hasTimeLimit()
{
    return (m_timeLimit != noTimeLimit);
}

bool Watchdog::isActive() const
{
    if (!m_gilOff)
        return m_hasEnteredVM;
    // GIL-off "active" = a carrier is entered-or-parked, OR the wall-clock
    // deadline is still armed for spawned execution (W2).
    Locker locker { m_lock };
    return m_carrierEnteredDepth || m_wallClockArmed;
}

void Watchdog::enteredVM()
{
    if (isSpawnedThreadGILOff(m_gilOff))
        return; // W0: spawned entry toggles neither carrier state nor the timer.

    if (!m_gilOff) {
        m_hasEnteredVM = true;
        if (hasTimeLimit()) {
            Locker locker { m_lock };
            startTimer(m_timeLimit);
        }
        return;
    }

    Locker locker { m_lock };
    ++m_carrierEnteredDepth;
    // A carrier (re-)entering re-arms the CPU budget as landed (W2).
    if (hasTimeLimit())
        startTimer(m_timeLimit);
}

void Watchdog::exitedVM()
{
    if (isSpawnedThreadGILOff(m_gilOff))
        return; // W0.

    if (!m_gilOff) {
        ASSERT(m_hasEnteredVM);
        Locker locker { m_lock };
        stopTimer();
        m_hasEnteredVM = false;
        return;
    }

    Locker locker { m_lock };
    ASSERT(m_carrierEnteredDepth);
    if (!--m_carrierEnteredDepth) {
        // W2 (exit deferral): the CPU budget is carrier-scoped — clear it.
        // PRESERVE m_deadline and the pending dispatched timer: while
        // spawned execution may continue, the watchdog stays armed for
        // wall-clock purposes (m_wallClockArmed). Full disarm happens in
        // timerDidFire() once nothing is entered anymore.
        m_cpuDeadline = noTimeLimit;
    }
}

void Watchdog::startTimer(Seconds timeLimit)
{
    ASSERT(m_gilOff ? !!m_carrierEnteredDepth : m_hasEnteredVM); // W2 re-point.
    assertWatchdogStateAccess(m_vm); // W4.
    ASSERT(hasTimeLimit());
    ASSERT(timeLimit <= m_timeLimit);

    m_cpuDeadline = CPUTime::forCurrentThread() + timeLimit;
    auto now = MonotonicTime::now();
    auto deadline = now + timeLimit;

    if ((now < m_deadline) && (m_deadline <= deadline))
        return; // Wait for the current active timer to expire before starting a new one.

    // Else, the current active timer won't fire soon enough. So, start a new timer.
    m_deadline = deadline;
    m_wallClockArmed = true;

    // We need to ensure that the Watchdog outlives the timer.
    // For the same reason, the timer may also outlive the VM that the Watchdog operates on.
    // So, we always need to null check m_vm before using it. The VM will notify the Watchdog
    // via willDestroyVM() before it goes away.
    VMTraps::queue().dispatchAfter(timeLimit, [protectedThis = Ref { *this }] {
        protectedThis->timerDidFire();
    });
}

void Watchdog::timerDidFire()
{
    Locker locker { m_lock };
    if (!m_vm)
        return;

    if (!m_gilOff || m_carrierEnteredDepth) {
        // Landed shape, and the W3 carrier branch: an entered carrier
        // services the check at its next poll site; a §J.3-parked carrier
        // (still counted in the depth) observes the bit at a D9 quantum and
        // runs the W1 early-service episode. The bit is carrier-serviced
        // only (VMTraps::CarrierOnlyServicedEvents).
        m_vm->notifyNeedWatchdogCheck();
        return;
    }

    // W3: no carrier entered-or-parked — spawned-only execution (or nothing
    // at all). Evaluate the WALL-CLOCK deadline on this (timer) thread, with
    // the same stale-timer rejection as shouldTerminate(). The CPU budget is
    // NOT evaluated here: spawned-only execution is governed by wall clock
    // only. The embedder callback is NOT consulted (it needs a
    // JSGlobalObject, the token, and carrier thread identity):
    // terminate-by-default, matching the !m_callback default (SD14).
    if (!m_wallClockArmed)
        return;

    Seconds epsilon;
#if OS(WINDOWS)
    epsilon = 20_ms; // See shouldTerminate().
#endif
    if (MonotonicTime::timePointFromNow(epsilon) < m_deadline)
        return; // Stale timer; a fresher dispatch is pending.

    // Disarm fully (W2 tail + W3): whether we terminate spawned execution or
    // find nothing left entered (the last spawned lite already unregistered
    // with no carrier entered), the watchdog is done with this arming.
    m_deadline = MonotonicTime::infinity();
    m_wallClockArmed = false;
    m_cpuDeadline = noTimeLimit;

    if (!m_vm->isAnyThreadEntered())
        return; // Nothing to terminate; plain full disarm.

    // Rule-3 VM-wide termination (TERM1.2), raised directly from the timer
    // thread — the async-delivery path already runs tokenless GIL-off
    // (§A.2.5); the registry lock is the only lock the fan-out takes while
    // we hold m_lock (leaf — rank-clean).
    m_vm->traps().fireTrapVMWide(VMTraps::NeedTermination);
}

void Watchdog::stopTimer()
{
    ASSERT(m_gilOff ? !!m_carrierEnteredDepth : m_hasEnteredVM); // W2 re-point (GIL-off callers gate on the depth pre-decrement).
    assertWatchdogStateAccess(m_vm); // W4.
    m_cpuDeadline = noTimeLimit;
}

void Watchdog::willDestroyVM(VM* vm)
{
    Locker locker { m_lock };
    ASSERT_UNUSED(vm, m_vm == vm);
    m_vm = nullptr;
}

} // namespace JSC
