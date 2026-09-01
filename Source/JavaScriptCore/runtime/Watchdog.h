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

#pragma once

#include "JSExportMacros.h"
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/Ref.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeRefCounted.h>

namespace JSC {

class CallFrame;
class JSGlobalObject;
class VM;

// UNGIL §A.2.8 (ANNEX W + W ext, BINDING; SD14): v1 GIL-off the watchdog is
// CARRIER-ONLY — per-thread CPU deadlines are post-ungil.
//   W0 (accounting): arms/measures on main/embedder carriers only. Spawned
//      Thread entry/exit toggles neither carrier-entered state nor the timer
//      (enteredVM/exitedVM early-return on a spawned thread — the W0
//      enforcement point); spawned CPU never advances the CPU budget; the
//      watchdog-check trap bit is carrier-serviced only (rule-3 exemption,
//      VMTraps::CarrierOnlyServicedEvents).
//   W1 (parked-carrier service): a parked carrier polls its captured lite's
//      trap bits each park quantum; on the watchdog-check bit it runs the
//      full exit reacquisition early and services shouldTerminate() under
//      its token on its own thread (its entry scope is live, so it still
//      counts in m_carrierEnteredDepth) with CallerState::ParkedCarrier:
//      stale-timer rejection and the embedder callback are those of an
//      entered carrier, but the CPU budget is not consulted — a parked
//      carrier accrues no CPU, so the CPU re-arm could never trip and the
//      park would be unkillable; the wall-clock deadline is authoritative.
//      Terminate => VM-wide termination (rule 3) and the park fails; else
//      the bit is cleared and the carrier disposes of its old wait node and
//      re-parks as a new acquisition episode. This class supplies the
//      service step (serviceCheckFromReacquiredParkedCarrier() below); every
//      park site (LockObject, ConditionObject, ThreadObject, ThreadAtomics,
//      WaiterListManager) drives it through
//      reacquireParkedCarrierAndServiceWatchdogCheck (JSLock.cpp), and the
//      park predicate parkLitePollTerminationRequested (VMTraps.cpp) tests
//      NeedTermination only GIL-off, with the check bit read separately by
//      parkLitePollWatchdogCheckRequested.
//   W2 (exit deferral): GIL-off m_hasEnteredVM splits into
//      m_carrierEnteredDepth + m_wallClockArmed. exitedVM() on the LAST
//      carrier clears the (carrier-scoped) CPU budget but PRESERVES
//      m_deadline and the pending dispatched timer: the watchdog stays armed
//      for wall-clock purposes while spawned execution may continue. A
//      re-entering carrier re-arms the CPU budget as landed.
//   W3 (no-carrier enforcement): the dispatched timer callback
//      (timerDidFire), under m_lock: any carrier entered-or-parked =>
//      notifyNeedWatchdogCheck() as landed. Else (spawned-only execution)
//      the WALL-CLOCK deadline is evaluated on the timer thread itself;
//      expired => VM-wide termination via the rule-3 fan-out
//      (terminate-by-default — the embedder callback is NOT consulted: it
//      needs a JSGlobalObject, the token and carrier thread identity) and
//      full disarm. The CPU budget is never evaluated in W3. With nothing
//      entered at all (last spawned lite gone, no carrier), the expired
//      timer disarms fully without raising termination.
//   W4 (W ext): the four API-lock asserts (setTimeLimit/shouldTerminate/
//      startTimer/stopTimer) are §F.2 EXCLUSIVITY CONSUMERS whose named
//      serializer is the REAL JSLock m_lock; GIL-off they assert
//      JSLock::currentThreadIsHoldingLock() && !spawned (spawned threads
//      never reach them — watchdog-unobserved v1, SD14). GIL-on/flag-off
//      byte-identical.
// GIL-on (useJSThreads without gilOff) and flag-off: m_gilOff is latched
// false and every path below keeps the landed single-carrier behavior. The
// one difference from the pre-threads code is that the carrier paths also
// take the uncontended m_lock around deadline-state updates (see m_lock
// below); the trap-bit protocol and generated code are unchanged.
//   Known GIL-on limitation: the CPU budget is armed from the clock of the
//   thread that entered (startTimer), but NeedWatchdogCheck is serviced by
//   whichever thread holds the JSLock at the poll — with useJSThreads that
//   can be a spawned Thread, as it can be any other OS thread an embedder
//   alternates on one JSLock. shouldTerminate() then compares that thread's
//   own CPU clock against the foreign budget: a servicer with a younger
//   clock is granted up to the arming thread's accumulated CPU time beyond
//   the limit (the re-arm then rebases the budget onto the servicer's
//   clock), one with an older clock is terminated at the wall-clock
//   deadline. Per-thread CPU budgets are not implemented.
class Watchdog : public ThreadSafeRefCounted<Watchdog> {
    WTF_MAKE_TZONE_ALLOCATED(Watchdog);
public:
    class Scope;

    Watchdog(VM*);
    void willDestroyVM(VM*);

    typedef bool (*ShouldTerminateCallback)(JSGlobalObject*, void* data1, void* data2);
    void setTimeLimit(Seconds limit, ShouldTerminateCallback = nullptr, void* data1 = nullptr, void* data2 = nullptr);
    Seconds getTimeLimit() const { return m_timeLimit; }

    // W1 verdict mode (GIL-off only): a §J.3-parked carrier accrues ~zero CPU
    // while parked, so the CPU-budget re-arm arm of the entered-carrier
    // verdict (cpuTime < m_cpuDeadline => re-arm + no-terminate) can NEVER
    // trip for it — every W1 episode would re-arm and re-park forever, an
    // unkillable park (the exact failure mode D9/annex W forbids). For a
    // parked carrier the wall-clock deadline is authoritative: skip the
    // CPU-budget arm, keep the stale-timer rejection and the embedder
    // callback (annex-W shape (a) extension semantics preserved).
    enum class CallerState : bool { Entered, ParkedCarrier };

    bool shouldTerminate(JSGlobalObject*, CallerState = CallerState::Entered);

    // W1 (GIL-off only): the parked-carrier service step, called by a park
    // site that observed NeedWatchdogCheck and has ALREADY run the full exit
    // reacquisition (m_lock + token + access) on its own thread, so its entry
    // scope is live. Takes (clears) the check bit on both the VM word and the
    // carrier lite's word, then runs shouldTerminate(CallerState::ParkedCarrier)
    // under the token: the embedder callback is consulted as for an entered
    // carrier, the CPU budget is not (see CallerState). Terminate => raises
    // VM-wide termination (rule 3) and returns true — the caller fails the
    // park; no-terminate (callback granted more time / stale timer) =>
    // returns false — the caller disposes of its old wait node under the
    // owning listLock and re-parks as a NEW acquisition episode. Also false
    // when the bit was already taken by a racing entered service, or when no
    // watchdog is active.
    JS_EXPORT_PRIVATE static bool serviceCheckFromReacquiredParkedCarrier(VM&);

    bool isActive() const; // Out-of-line: GIL-off it reads the W2 split under m_lock.

    bool NODELETE hasTimeLimit();
    void enteredVM();
    void NODELETE exitedVM();

    static constexpr Seconds noTimeLimit = Seconds::infinity();

private:
    void startTimer(Seconds timeLimit) WTF_REQUIRES_LOCK(m_lock);
    void NODELETE stopTimer() WTF_REQUIRES_LOCK(m_lock);
    void timerDidFire(); // The dispatched timer callback (W3 branch lives here).

    bool m_hasEnteredVM { false }; // GIL-on only; GIL-off state is the W2 split below.

    // W2 split (GIL-off only; guarded by m_lock). The depth counts carrier
    // top-level entries (VMEntryScope service hooks); a §J.3-parked carrier
    // has NOT torn down its entry scope, so "entered-or-parked" == depth > 0.
    unsigned m_carrierEnteredDepth WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    bool m_wallClockArmed WTF_GUARDED_BY_LOCK(m_lock) { false };

    // GIL-off, m_lock is the real serializer for ALL deadline state: the W3
    // timer thread reads AND writes m_deadline/m_cpuDeadline concurrently
    // with carrier-side arming (TSAN-clean per the U-T11 arm). GIL-on it
    // guarded only m_vm; taking it on the (uncontended) carrier paths too is
    // behavior-neutral and keeps one code shape. Never held across the
    // embedder callback (which may re-enter setTimeLimit).
    mutable Lock m_lock;
    VM* m_vm { nullptr };

    bool m_gilOff { false }; // Latched from VM::gilOff() at construction; immutable.

    Seconds m_timeLimit { noTimeLimit };
    Seconds m_cpuDeadline { noTimeLimit };
    MonotonicTime m_deadline { MonotonicTime::infinity() };

    ShouldTerminateCallback m_callback { nullptr };
    void* m_callbackData1 { nullptr };
    void* m_callbackData2 { nullptr };
};

} // namespace JSC
