/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ThreadManager.h"

#include "Error.h"         // UNGIL §E.5/TERM1.3 (U-T9): the fresh Error("Thread terminated").
#include "GlobalObjectMethodTable.h" // UNGIL §E.2 (U-T9): reportUncaughtExceptionAtEventLoop on the snapshotted realm.
#include "JSCInlines.h"
#include "JSLock.h"        // UNGIL §E.2 (U-T9): per-task token brackets in the E2A loop.
#include "JSPromise.h"
#include "RaceAmplifier.h" // UNGIL EXIT1.8 (U-T6): T5-tail stall points.
#include "ThreadObject.h"
#include "TopExceptionScope.h"
#include "VMLite.h"        // UNGIL §B.1/EXIT1.3 (U-T6): per-thread client lifecycle.
#include "VMLiteShared.h"  // UNGIL EXIT1.9 (U-T6): registry lock + the notifying wrapper.
#include "WeakHandleOwner.h"
#include "WeakInlines.h"
#include <wtf/Atomics.h>
#include <wtf/Condition.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/ThreadSpecific.h>

namespace JSC {

// ---------------- AsyncTicket ----------------

AsyncTicket::AsyncTicket(VM& vm, Ref<DeferredWorkTimer::TicketData>&& ticket, Ref<ThreadState>&& registrant, JSObject* extraDependency)
    : m_vm(vm)
    , m_ticket(WTF::move(ticket))
    , m_registrant(WTF::move(registrant))
    , m_extraDependency(extraDependency)
{
}

// Out of line so the Strong<JSPromise> / Ref<ThreadState> members destruct
// where both types are complete. A never-settled ticket's Strong dies here
// (SPEC-api 5.5: not an error; DWT VM-shutdown cancelPendingWork is the
// liveness backstop). Destroying a still-set Strong mutates the VM's
// HandleSet, which requires the API lock: settle tasks clear the Strong
// under the JSLock, and every other last-ref drop site (DWT shutdown, the
// 5.6 timeout timer task) runs under a JSLockHolder. The assert documents
// that discipline for any future holder of the last Ref.
AsyncTicket::~AsyncTicket()
{
    ASSERT(!m_promise || m_vm.currentThreadIsHoldingAPILock());
}

Ref<AsyncTicket> AsyncTicket::create(JSGlobalObject* globalObject, JSPromise* promise, Vector<JSCell*>&& dependencies, bool countsKeepalive)
{
    VM& vm = globalObject->vm();
    JSObject* extraDependency = nullptr;
    if (!dependencies.isEmpty())
        extraDependency = dynamicDowncast<JSObject>(dependencies[0]);
    // Registration under the JSLock (SPEC-api 5.5): addPendingWork is the
    // shell-liveness point (I20/4.6.3) — a pending asyncJoin/asyncHold/
    // asyncWait/waitAsync keeps the shell alive from HERE, not from settle
    // time. The registrant is the calling thread's ThreadState (4.6.2).
    DeferredWorkTimer::Ticket ticket = vm.deferredWorkTimer->addPendingWork(DeferredWorkTimer::WorkType::AtSomePoint, vm, promise, WTF::move(dependencies));
    Ref<AsyncTicket> result = adoptRef(*new AsyncTicket(vm, Ref { *ticket }, ensureCurrentThreadState(), extraDependency));
    result->m_promise.set(vm, promise);
    // §E.3 INCREMENT, in-set (U-T9): a COUNTED registration (asyncHold /
    // cond.asyncWait / property waitAsync — gate U-T9-INT1's boolean) arms
    // here, BEFORE the ticket is returned and thus before it is visible to
    // any settler. Only gilOff spawned registrants count (§E.7:
    // main/embedder registrations never touch keepalive); armKeepalive
    // asserts spawned+inbox-OPEN (U25), which holds because the counted
    // host calls run inside fn, after openThreadInbox. GIL-on/flag-off:
    // gilOff() is false and this is dead.
    if (countsKeepalive && vm.gilOff() && result->m_registrant->isSpawned) [[unlikely]]
        result->armKeepalive();
    return result;
}

// The landed settle tail (GIL-on/flag-off byte-identical) and the GIL-off
// main-fallback arm: the wrapper keeps the ticket alive through the settle
// task and clears the promise Strong afterwards, under the carrier's token
// (SPEC-api 5.10: the AT::Strong<JSPromise> is created at registration,
// cleared at settle).
void AsyncTicket::scheduleViaDeferredWorkTimer(DeferredWorkTimer::Task&& task)
{
    m_vm.deferredWorkTimer->scheduleWorkSoon(m_ticket.ptr(), [protectedThis = Ref { *this }, task = WTF::move(task)](DeferredWorkTimer::Ticket dwtTicket) mutable {
        task(dwtTicket);
        // §E.4 retirement parity (MC-DOS S4, mc-dos-waiter-table-storm):
        // the task-queue path retires the DWT registration right after the
        // settle task ((a) settle, (b) retire, (c) m_promise clear —
        // runQueuedSettleTaskOnRegistrant below). This tail historically
        // relied on doWork's m_pendingTickets removal alone, which UNROOTS
        // NOTHING: JSGlobalObject::visitChildren marks target+dependencies
        // of every LIVE, un-cancelled TicketData in the realm's
        // m_weakTickets set, and any holder of a stray Ref to the ticket —
        // e.g. the 5.6 finite-timeout timer lambda, which pins its
        // Ref<AsyncTicket> for the FULL timeout — keeps the settled
        // ticket's waited-on cell and promise GC-rooted long after the
        // settle (observed: a notified property waitAsync's cell stays
        // uncollectable for the whole 60s timeout). cancel() — not
        // cancelPendingWork(): doWork already take()s the ticket out of
        // m_pendingTickets before running this task, so the GIL-on
        // contains() assert there would fire — flips the visitor's skip
        // bit; dependencies are NOT freed (cancel, not cancelAndClear), so
        // concurrent mayBeCancelled readers stay safe.
        dwtTicket->cancel();
        protectedThis->m_promise.clear();
    });
}

void AsyncTicket::settle(DeferredWorkTimer::Task&& task)
{
    // PRECONDITION (UNGIL §E.4, r17 F2, BINDING): settle is invoked holding
    // NO api rank-1..3 lock (the frozen 5.5a A/P + F5 asyncJoin sites were
    // re-shaped by U-T8 to decide-under-lock / act-after-drop). The §E.7.3
    // wake fired by the closed-arm fallback below must run with neither
    // m_pendingLock nor any TS::inboxLock held — guaranteed structurally:
    // the fallback runs after the inboxLock drop and DWT takes its own leaf
    // lock internally.
    bool expected = false;
    if (!m_settled.compare_exchange_strong(expected, true))
        return;
    if (m_ticket->isCancelled())
        return;
    // UNGIL §E.4 (U-T9): GIL-off, settlement ROUTES BY REGISTRANT (U22/I12):
    // a spawned registrant with an open inbox gets a ThreadTask hop; a
    // closed/never-opened (main/embedder — §E.1: their inboxes never open)
    // registrant takes the LANDED scheduleWorkSoon path (r17 F6: the api:200
    // "append to MAIN TS inbox" closed arm is SUPERSEDED — the main inbox is
    // structurally dead GIL-off; scheduleWorkSoon composes with §E.7.3-4).
    // GIL-on/flag-off: the routing block vanishes (m_gilOff false) and the
    // landed path runs byte-identically.
    if (m_vm.gilOff() && m_registrant->isSpawned) [[unlikely]] {
        settleViaRegistrantRouting(WTF::move(task));
        return;
    }
    scheduleViaDeferredWorkTimer(WTF::move(task));
}

void AsyncTicket::settleViaRegistrantRouting(DeferredWorkTimer::Task&& task)
{
    ASSERT(m_vm.gilOff());
    ThreadState& registrant = m_registrant.get();
    {
        // Decide under the lock (r18 F2): the open-arm append + rule-1
        // decrement + wake are ATOMIC under inboxLock (U9 — E2A's exit check
        // reads keepaliveCount and taskQueue under the same lock, so no
        // close can interleave between a decrement and its append; the
        // decrementer signals runLoopCondition before unlocking).
        Locker locker { registrant.inboxLock };
        if (registrant.inboxOpen) {
            registrant.taskQueue.append(ThreadTask { WTF::move(task), Ref { *this } });
            // §E.3 rule 1: decrement iff this ticket was ARMED (won the
            // false->true CAS). Never-armed tickets (asyncJoin et al.) lose
            // the CAS and never decrement — no uint64 wrap.
            if (claimKeepaliveRelease()) {
                ASSERT(registrant.keepaliveCount);
                if (registrant.keepaliveCount)
                    registrant.keepaliveCount--;
            }
            registrant.runLoopCondition.notifyOne();
            return;
        }
        // Closed: win the CAS (so a later cancel does nothing) but SKIP the
        // decrement — the counter is DEAD post-close (§E.3 rule 3).
        claimKeepaliveRelease();
    }
    // Act after the drop (r18 F2): inbox closure is MONOTONIC (open exactly
    // once pre-fn, false forever at close), so this post-drop fallback can
    // never race a reopen. No api rank-1..3 lock is held here (the §E.7.3
    // wake-edge contract).
    scheduleViaDeferredWorkTimer(WTF::move(task));
}

void AsyncTicket::runQueuedSettleTaskOnRegistrant(DeferredWorkTimer::Task&& task)
{
    // §E.4 retirement on the task-queue path: (a) the settle task, (b) DWT
    // retirement (cancelPendingWork — internal-arm; fires the §E.7.4 wake if
    // a shell is parked on ticket-emptiness), (c) m_promise clear. Thread
    // keepalive supersedes DWT shell-liveness for spawned registrants (U24).
    // Caller (the E2A loop) holds this thread's §F token.
    ASSERT(m_vm.currentThreadIsHoldingAPILock());
    task(m_ticket.ptr());
    m_vm.deferredWorkTimer->cancelPendingWork(m_ticket.ptr());
    m_promise.clear();
}

void AsyncTicket::routeQueuedTaskToMainFallback(DeferredWorkTimer::Task&& task)
{
    // E2A close residue rule: the ticket already won its m_settled CAS when
    // it was enqueued; the queued task is re-routed to the landed
    // scheduleWorkSoon path and runs at the next carrier drain (4.6.2).
    ASSERT(m_settled.load(std::memory_order_relaxed));
    if (m_ticket->isCancelled())
        return;
    scheduleViaDeferredWorkTimer(WTF::move(task));
}

void AsyncTicket::retireUnsettled()
{
    // §C.3(b) revoked registration (U-T11; see the header comment). The
    // caller dequeued this ticket's only waiter under the list lock while
    // its state was still Waiting, so no notifier ever observed it and no
    // settle path can be in flight — the CAS below must win.
    ASSERT(m_vm.currentThreadIsHoldingAPILock());
    bool expected = false;
    bool won = m_settled.compare_exchange_strong(expected, true);
    RELEASE_ASSERT(won);
    // §E.3: the property-waitAsync registration is never-armed until gate
    // U-T9-INT1 lands (countsKeepalive=false at the call site); if that gate
    // ever flips, this retirement site must perform the rule-1 decrement
    // under the registrant's inboxLock like the open settle arm does.
    bool wasArmed = claimKeepaliveRelease();
    ASSERT_UNUSED(wasArmed, !wasArmed);
    if (!m_ticket->isCancelled())
        m_vm.deferredWorkTimer->cancelPendingWork(m_ticket.ptr());
    m_promise.clear();
}

void AsyncTicket::armKeepalive()
{
    // §E.3 INCREMENT: once, at registration, on the REGISTERING TS — which
    // must be a spawned thread with an OPEN inbox (U25; main/embedder
    // registrations never touch keepalive, §E.7). Stores armed (false)
    // BEFORE the ticket becomes visible to any settler — the caller
    // registers the ticket with its waiter list only after this returns.
    ThreadState& registrant = m_registrant.get();
    RELEASE_ASSERT(registrant.isSpawned);
    Locker locker { registrant.inboxLock };
    RELEASE_ASSERT(registrant.inboxOpen); // U25: increment sites assert spawned+OPEN.
    ASSERT(m_keepaliveReleased.load(std::memory_order_relaxed)); // constructed released; armed at most once.
    m_keepaliveReleased.store(false, std::memory_order_relaxed);
    registrant.keepaliveCount++;
}

// ---------------- current ThreadState ----------------

static ThreadSpecific<RefPtr<ThreadState>>& threadStateSlot()
{
    static LazyNeverDestroyed<ThreadSpecific<RefPtr<ThreadState>>> slot;
    static std::once_flag onceKey;
    std::call_once(onceKey, [&] {
        slot.construct();
    });
    return slot;
}

ThreadState* currentThreadStateIfExists()
{
    return threadStateSlot()->get();
}

Ref<ThreadState> ensureCurrentThreadState()
{
    RefPtr<ThreadState>& slot = *threadStateSlot();
    if (!slot) {
        // Lazy main/embedder ThreadState (SPEC-api 5.1): tid 0, not spawned,
        // created on first access (Thread.current, ThreadLocal storage, lock
        // holder identity, ...) and stable thereafter for this thread.
        // Distinct embedder threads get distinct ThreadStates — identity is
        // this TLS slot / nativeThread, never the tid (all lazy TSs share
        // tid 0, which is why lookups NEVER go through m_threads). The lazy
        // TS is registered nowhere and carries no Strongs yet; whoever
        // creates its FIRST Strong must call ensureJSThreadForState()
        // (ThreadObject.h) first so the 5.10 finalizer hook exists — that
        // hook (not a completion sequence, which lazy TSs never run) is what
        // clears jsThread/threadLocals/result at VM teardown.
        Ref<ThreadState> state = ThreadState::create(ThreadManager::mainThreadTID, false);
        state->nativeThread = &Thread::currentSingleton();
        slot = state.ptr();
    }
    return *slot;
}

void setCurrentThreadState(RefPtr<ThreadState>&& state)
{
    *threadStateSlot() = WTF::move(state);
}

// ---------------- ThreadManager ----------------

ThreadManager& ThreadManager::singleton()
{
    static LazyNeverDestroyed<ThreadManager> manager;
    static std::once_flag onceKey;
    std::call_once(onceKey, [&] {
        manager.construct();
    });
    return manager;
}

uint16_t ThreadManager::currentTID()
{
    // UNGIL §A.3.6 TID supersession (r9 F4; U-T6): GIL-off currentTID()
    // returns the CARRIER TID — the installed lite's tid (it feeds
    // tagging/TTL consumers, never JS; main/embedder ThreadState.tid STAYS
    // 0, so thr.id/Thread.current.id are unchanged). Behavior-identical
    // GIL-on/flag-off: an installed GIL-on lite is either m_mainVMLite
    // (tid 0 == mainThreadTID) or a spawned lite whose tid equals its
    // ThreadState's.
    if (VMLite* lite = VMLite::currentIfExists())
        return lite->tid;
    if (ThreadState* state = currentThreadStateIfExists())
        return state->tid;
    return mainThreadTID;
}

bool ThreadManager::isJSThreadCurrent()
{
    ThreadState* state = currentThreadStateIfExists();
    return state && state->isSpawned;
}

RefPtr<ThreadState> ThreadManager::allocateSpawnedThreadStateInternal()
{
    Locker locker { m_lock };
    // UNGIL §D.1 phase 3 (U-T12): a Restamped snapshot is released HERE —
    // post-resume, on a mutator, under m_lock — strictly before the
    // exhaustion check below, so the SD9 RangeError gate lifts at exactly
    // the first allocation after the in-stop restamp+fire (ANNEX D1:
    // "m_freeTIDs released POST-RESUME under TM::m_lock BEFORE the gate
    // lifts"). GIL-on/flag-off: state is always Idle, the call no-ops.
    if (VM::isGILOffProcess()) [[unlikely]]
        completeRebiasIfPendingLocked();
    if (m_threads.size() >= Options::maxJSThreads())
        return nullptr;
    uint16_t tid;
    if (!m_freeTIDs.isEmpty())
        tid = m_freeTIDs.takeFirst();
    else {
        // UNGIL U-T6 split: spawned TIDs stop at carrierTIDBase — the upper
        // half is the A36 carrier range (see ThreadManager.h; previously the
        // bound was notTTLTID).
        if (m_nextTID >= carrierTIDBase) {
            // SD9 exhaustion window: the caller throws RangeError. U-T12:
            // make sure the trigger is armed and any retired TIDs are
            // sealed so the next full shared collection rebiases and the
            // window closes (GIL-on keeps Dev 10: spawn fails forever).
            if (VM::isGILOffProcess()) [[unlikely]] {
                m_rebiasArmed.store(true, std::memory_order_relaxed);
                maybeArmAndSealRebiasLocked();
            }
            return nullptr;
        }
        tid = m_nextTID++;
    }
    Ref<ThreadState> state = ThreadState::create(tid, true);
    m_threads.add(tid, state.copyRef());
    // UNGIL §D.1 trigger (U-T12): arm at >=75% consumption; seal eagerly if
    // dead TIDs are already waiting (spawn-storm shape: arm + seal happen on
    // the allocating mutator, pre-stop, under m_lock — the D1 phase-1 pass).
    if (VM::isGILOffProcess()) [[unlikely]]
        maybeArmAndSealRebiasLocked();
    return RefPtr<ThreadState> { WTF::move(state) };
}

RefPtr<ThreadState> ThreadManager::allocateSpawnedThreadState()
{
    // UNGIL U0b BACKSTOP (U-T6): under gilOffProcess a spawn may only be
    // licensed by the VM-aware overload below — this VM-blind form cannot
    // prove the spawning VM is the m_gilOff winner (U0c), and a LOSER VM's
    // spawn must be REFUSED (U0b: the host call throws RangeError), never
    // allowed to run GIL'd phase-1 semantics with no per-thread GCClient.
    // AB-11 landed: the spawn host call (ThreadObject.cpp constructThread)
    // now uses the VM-aware overload, so under gilOffProcess this form has
    // no callers — the null return remains as a fail-safe backstop against
    // any FUTURE VM-blind call site (over-refusal: a RangeError, not silent
    // wrong semantics or the lite->clientHeap==null fail-stop a
    // wired-but-clientless spawned entry would hit). Flag-off (every
    // shipping configuration): isGILOffProcess() is false and this is
    // byte-identical to the landed behavior.
    if (VM::isGILOffProcess()) [[unlikely]]
        return nullptr;
    return allocateSpawnedThreadStateInternal();
}

RefPtr<ThreadState> ThreadManager::allocateSpawnedThreadState(VM& vm)
{
    // UNGIL U0b (U-T6): under gilOffProcess exactly ONE VM per process — the
    // m_gilOff winner (U0c) — may hold per-thread clients, hence spawn.
    // A loser VM's spawn returns null and the host call throws RangeError.
    if (VM::isGILOffProcess() && !vm.gilOff()) [[unlikely]]
        return nullptr;
    RefPtr<ThreadState> state = allocateSpawnedThreadStateInternal();
    // UNGIL §D.1 SD9 liveness (U-T12): an exhausted winner-VM spawn left a
    // snapshot Sealed (the internal form armed + sealed under m_lock). The
    // rebias only RUNS inside the next full shared collection — actually
    // request one from this mutator (a host call holding heap access; the
    // shouldDoFullCollection probe makes the granted cycle Full regardless
    // of scope coalescing), so the RangeError window closes without waiting
    // for organic allocation pressure. Pure liveness aid: arming/sealing/
    // restamping are unchanged; gilOffProcess-only, dead flag-off/GIL-on.
    if (!state && VM::isGILOffProcess() && vm.gilOff() && rebiasSnapshotIsSealed()) [[unlikely]]
        vm.heap.collectAsync(CollectionScope::Full);
    return state;
}

void ThreadManager::unregisterThread(ThreadState& state)
{
    Locker locker { m_lock };
    m_threads.remove(state.tid);
    // UNGIL §D.1 (U-T12): under gilOffProcess the TID is retired into the
    // rebias pipeline (dead from here: no m_threads entry, and TM never
    // reissues before the post-resume release — ANNEX D1 soundness). The
    // exiting thread's residual T5 tail still runs with its R5 TLS tag
    // installed, but it executes no JS and installs no tagged state, and
    // the §10.4 access barrier serializes its remaining access brackets
    // against any rebias stop (see the ThreadManager.h banner).
    // GIL-on/flag-off: TIDs stay retired forever (SPEC-api Deviation 10).
    if (VM::isGILOffProcess()) [[unlikely]] {
        m_retiredSpawnedTIDs.append(state.tid);
        maybeArmAndSealRebiasLocked();
    }
}

// ---------------- UNGIL §A.3.6 carrier TIDs (ANNEX A36; U-T6) ----------------

uint16_t ThreadManager::allocateCarrierTIDInternal()
{
    Locker locker { m_lock };
    // UNGIL §D.1 phase 3 (U-T12): release a Restamped snapshot before the
    // exhaustion check — refilled carrier TIDs avert the fail-stop. This is
    // also the carrier-side SD9 gate-lift site. The TM-churn interplay
    // (another VM's embedder thread allocating here while the gilOff VM
    // rebias-stops) is the sole sanctioned §LK negative-edge row: this path
    // only ADDS live TIDs and never touches a Sealed snapshot.
    if (VM::isGILOffProcess()) [[unlikely]]
        completeRebiasIfPendingLocked();
    uint16_t tid;
    if (!m_freeCarrierTIDs.isEmpty())
        tid = m_freeCarrierTIDs.takeFirst();
    else {
        // Carrier allocation runs at first VM entry on a thread (JSLock
        // didAcquireLock / §F.1) — no throw context exists there, so range
        // exhaustion is a fail-stop (I17 accounting; the range covers 16382
        // carriers, far beyond any plausible embedder thread count, and
        // U-T12 rebias recycles retired carrier TIDs ahead of this bound).
        RELEASE_ASSERT(m_nextCarrierTID < notTTLTID);
        tid = m_nextCarrierTID++;
    }
    if (VM::isGILOffProcess()) [[unlikely]]
        maybeArmAndSealRebiasLocked();
    return tid;
}

static uint16_t allocateCarrierTIDHook()
{
    return ThreadManager::singleton().allocateCarrierTIDInternal();
}

static void releaseCarrierTIDHook(uint16_t tid)
{
    // UNGIL §D.1 (U-T12): gilOffProcess retires the carrier TID into the
    // rebias pipeline. GIL-on/flag-off: retired forever (Dev 10 no-reuse —
    // recycling a TID without the rebias protocol could alias a live
    // butterfly TID tag), preserving the landed v1 no-op byte-for-byte.
    if (VM::isGILOffProcess()) [[unlikely]]
        ThreadManager::singleton().retireCarrierTID(tid);
}

void ThreadManager::retireCarrierTID(uint16_t tid)
{
    ASSERT(isCarrierTID(tid));
    if (!VM::isGILOffProcess())
        return; // Dev 10 stands GIL-on/flag-off.
    // Carrier exits are UN-GATED (EXIT1.3): this can run mid-stop. Taking
    // rank-1 m_lock here is legal — every release-hook call site (JSLock.cpp
    // tearDownCarriersAtThreadDeath tail / evictStaleCarrier, the VM.cpp A36
    // walk tail) runs with no lock held — and a mid-stop retire only appends
    // to the retired set or seals a NEW snapshot (state Idle), never touches
    // a Sealed one the conductor may be reading.
    RaceAmplifier::perturb(); // U-T12 TM-churn / D1R.5 stall point: pre-retire.
    Locker locker { m_lock };
    m_retiredCarrierTIDs.append(tid);
    maybeArmAndSealRebiasLocked();
}

// ---------------- UNGIL §D.1 TID rebias state machine (U-T12) ---------------
// See the ThreadManager.h banner for the full three-phase protocol and the
// m_rebiasSnapshot ownership rotation.

void ThreadManager::maybeArmAndSealRebiasLocked()
{
    ASSERT(VM::isGILOffProcess());
    if (!m_rebiasArmed.load(std::memory_order_relaxed)) {
        // Trigger (ANNEX D1: ">=75% of 2^15 arms the next full collection").
        // RECORDED REFINEMENT (U-T12; required by the U-T6 partition, which
        // this task must preserve): the threshold is evaluated PER PARTITION
        // — 75% of the spawned range [1, carrierTIDBase) or 75% of the
        // carrier range [carrierTIDBase, notTTLTID). A literal 75%-of-2^15
        // combined threshold (24576) exceeds either partition's capacity
        // (16383), so a spawn storm could exhaust its half and RangeError
        // forever without ever arming — violating SD9's "until rebias
        // completes" liveness and the U18/D1R.5 amplifier shapes. Per-
        // partition arming is strictly earlier, never later (not a
        // weakening); consumption cursors are monotone, so armed is sticky.
        constexpr uint32_t spawnedCapacity = carrierTIDBase - 1;
        constexpr uint32_t carrierCapacity = notTTLTID - carrierTIDBase;
        uint32_t spawnedConsumed = m_nextTID - 1;
        uint32_t carrierConsumed = m_nextCarrierTID - carrierTIDBase;
        bool pressure = spawnedConsumed * 4 >= spawnedCapacity * 3
            || carrierConsumed * 4 >= carrierCapacity * 3;
        if (!pressure)
            return;
        m_rebiasArmed.store(true, std::memory_order_relaxed);
    }
    if (m_rebiasState.load(std::memory_order_relaxed) != static_cast<uint8_t>(RebiasState::Idle))
        return; // A snapshot is Sealed/Restamped: TIDs retired after that seal wait for the next cycle (D1 soundness — they stay un-reissuable).
    if (m_retiredSpawnedTIDs.isEmpty() && m_retiredCarrierTIDs.isEmpty())
        return; // Nothing to recycle: do NOT seal (an empty snapshot would force full collections for no benefit).
    ASSERT(m_rebiasSnapshot.isEmpty());
    m_rebiasSnapshot.appendVector(m_retiredSpawnedTIDs);
    m_rebiasSnapshot.appendVector(m_retiredCarrierTIDs);
    m_retiredSpawnedTIDs.clear();
    m_retiredCarrierTIDs.clear();
    // Release: publishes the snapshot contents to the conductor's acquire
    // load in rebiasSnapshotForConductor(). From here until Restamped ->
    // Idle, no mutator writes m_rebiasSnapshot.
    m_rebiasState.store(static_cast<uint8_t>(RebiasState::Sealed), std::memory_order_release);
}

void ThreadManager::completeRebiasIfPendingLocked()
{
    if (m_rebiasState.load(std::memory_order_acquire) != static_cast<uint8_t>(RebiasState::Restamped))
        return;
    // Phase 3: every snapshot TID's instance tags and structure transition
    // TIDs were restamped to 0 and the D1R fires jettisoned every body with
    // a baked tid<<48 immediate, all inside the stop that preceded this
    // (the acquire above synchronizes with the conductor's Restamped
    // release; the heap-word restamps themselves are published by the §10
    // resume protocol). Reissue is sound from here (OM I11/I15 by
    // construction, restamp-to-0 = the payload-0/TID-0 main-allocated
    // regime). The U-T6 partition is preserved by the per-range routing.
    for (uint16_t tid : m_rebiasSnapshot) {
        ASSERT(tid && tid < notTTLTID);
        if (isCarrierTID(tid))
            m_freeCarrierTIDs.append(tid);
        else
            m_freeTIDs.append(tid);
    }
    m_rebiasSnapshot.clear();
    m_rebiasState.store(static_cast<uint8_t>(RebiasState::Idle), std::memory_order_release);
    // Continuous-recycling regime: TIDs retired while the completed
    // snapshot was in flight seal the next one immediately.
    maybeArmAndSealRebiasLocked();
}

const Vector<uint16_t>* ThreadManager::rebiasSnapshotForConductor()
{
    // Conductor-side, lock-free (§D.1: the conductor takes NO api lock;
    // TM::m_lock is not excepted). Sound without the lock: the vector is
    // immutable while Sealed, and the acquire pairs with the seal's release.
    if (m_rebiasState.load(std::memory_order_acquire) != static_cast<uint8_t>(RebiasState::Sealed))
        return nullptr;
    return &m_rebiasSnapshot;
}

void ThreadManager::noteRebiasRestampComplete()
{
    ASSERT(m_rebiasState.load(std::memory_order_relaxed) == static_cast<uint8_t>(RebiasState::Sealed));
    // D1R ordering: callers flip this strictly AFTER the in-stop restamps +
    // fireTransitionThreadLocal jettisons, and the phase-3 release (which
    // alone makes reissue possible) is gated on Restamped — so no baked
    // dead-TID immediate survives to any reissue point.
    m_rebiasState.store(static_cast<uint8_t>(RebiasState::Restamped), std::memory_order_release);
}

void ThreadManager::installCarrierTIDHooksIfNeeded()
{
    static std::once_flag onceKey;
    std::call_once(onceKey, [] {
        setCarrierTIDHooks(&allocateCarrierTIDHook, &releaseCarrierTIDHook);
    });
}

// ---------------- UNGIL EXIT1.9 fence + §B.1-2 client lifecycle (U-T6) ------
// See the banner in ThreadManager.h for the recorded placement deviation
// (VMLiteShared.h is outside U-T6's owned-file set) and the post-unlock
// notify soundness argument.

Condition& vmLiteTeardownCondition()
{
    static NeverDestroyed<Condition> condition;
    return condition.get();
}

void unregisterVMLiteAndNotifyTeardown(VMLite& lite)
{
    VMLiteRegistry::singleton().unregisterLite(lite); // takes + drops the registry lock
    vmLiteTeardownCondition().notifyAll();            // both waiters are predicate loops (EXIT1.9)
}

void attachSpawnedThreadGCClient(VM& vm, VMLite& lite)
{
    // §B.1: after lite registration/setCurrent + TID-tag handshake, BEFORE
    // any allocation. Spawned threads exist only in the m_gilOff VM (U0b).
    RELEASE_ASSERT(vm.gilOff());
    RELEASE_ASSERT(lite.vm == &vm);     // registered (vmstate §6.5.1 sole writer)
    RELEASE_ASSERT(lite.tid && lite.tid < ThreadManager::carrierTIDBase); // spawned-range tid
    RELEASE_ASSERT(!lite.clientHeap);   // EXIT1.4(b): written ONCE per registration epoch

    auto* client = new GCClient::Heap(vm.heap); // §B.1: the thread's OWN client (heap Dev 8); ctor registers with the server's HeapClientSet (may run the §10B.4 sticky switch)
    {
        // EXIT1.4(b) release-publish: the fence orders the client's
        // construction before the pointer store; the registry-lock hold
        // publishes it to every sampler (samplers read lite fields only
        // under VMLiteRegistry::lock, EXIT1.2). Never nulled or repointed
        // while the lite is registered.
        WTF::storeStoreFence();
        Locker locker { VMLiteRegistry::singleton().lock };
        RELEASE_ASSERT(lite.state == VMLite::State::Live);
        lite.clientHeap = client;
    }
    // ACT (heap I4(a)-(c)): §10A.1 slot stamp + conservative-scan
    // registration + §A.3.2b/§A.3.8-gated access acquisition (parks across
    // any pending stop; the ISB1.2 generation sync runs on the acquire
    // success path). This is the thread's FIRST access acquisition —
    // strictly after the clientHeap publish above (EXIT1.4(b)).
    client->attachCurrentThread();
}

void tearDownSpawnedThreadForExit(VM& vm, VMLite& lite)
{
    // EXIT1.3 spawned T5 tail as AMENDED r31 (caller: Strong clears +
    // ThreadManager::unregisterThread + the E2A close already ran; caller
    // frees the lite AFTER this returns — the M12 default-queue removal in
    // that free is covered by the EXIT1.9 residual-tail rule, not by the
    // fence). Exit is UN-GATED: nothing below polls a stop bit or parks.
    RELEASE_ASSERT(vm.gilOff());
    RELEASE_ASSERT(lite.vm == &vm);
    GCClient::Heap* client = lite.clientHeap;
    RELEASE_ASSERT(client);

    // (1) Access release (seq_cst RHA, F8) — ordered BEFORE the TEARDOWN
    // mark so a conductor that samples the mark (or misses the lite) has
    // this thread's NoAccess release ordered before its sample (EXIT1.4(a)
    // soundness).
    if (client->hasHeapAccess())
        client->releaseHeapAccess();
    RaceAmplifier::perturb(); // EXIT1.8 exit-storm stall point: post-release.

    // (2) TEARDOWN mark under the registry lock (LOGICAL removal: conductors
    // count this lite EXITED from here on and never dereference its client;
    // access re-acquisition is now FORBIDDEN — asserted in
    // GCClient::Heap::acquireHeapAccess). The lite stays PHYSICALLY
    // registered through the whole server-touching tail (the EXIT1.9 fence
    // counts it, making join-then-destroy-VM safe).
    {
        Locker locker { VMLiteRegistry::singleton().lock };
        RELEASE_ASSERT(lite.state == VMLite::State::Live);
        lite.state = VMLite::State::Teardown;
    }
    RaceAmplifier::perturb(); // EXIT1.8 stall point: post-mark, pre-DCT.

    // TLS uninstall BEFORE the destroy step (I20: no thread's TLS may
    // dangle into a destroyed lite; also keeps the EXIT1.4(a)
    // "TEARDOWN lite never re-acquires" debug assert in
    // GCClient::Heap::acquireHeapAccess keyed on a CURRENT lite from firing
    // on ~Heap's own sanctioned teardown access bracket below — that
    // bracket is the landed review-round-1 T5 order, not a JS re-entry).
    // The tag clears through the setCurrent hook.
    if (VMLite::currentIfExists() == &lite)
        VMLite::setCurrent(nullptr);

    // (3) DCT: park the local epoch at MAX and clear the §10A.1 slot.
    // attachSpawnedThreadGCClient stamped this thread's slot, but an E2A
    // close that already detached leaves it cleared — both shapes legal.
    if (GCClient::Heap::currentThreadClient() == client)
        client->detachCurrentThread();
    RaceAmplifier::perturb(); // EXIT1.8 stall point: mid-tail, pre-destroy.

    // (4) Destroy the GCClient::Heap (the live-path dtor: access bracket +
    // lastChanceToFinalize under MSPL + clientSet().remove against the
    // still-alive server — the EXIT1.9 fence is what keeps the server alive
    // through this). lite.clientHeap is left DANGLING, not nulled
    // (EXIT1.4(b): never nulled while registered; EXIT1.4(a): samplers never
    // dereference a TEARDOWN lite's client).
    delete client;
    RaceAmplifier::perturb(); // EXIT1.8 stall point: post-destroy, pre-unregister.

    // (5) PHYSICAL removal LAST, via the notifying wrapper (U20 r31). The
    // EXIT1.9 ~VM wait can return from here on; the caller's lite free (with
    // its M12 default-queue removal) is the residual tail outside the fence.
    unregisterVMLiteAndNotifyTeardown(lite);
}

// ============================================================================
// UNGIL §E.2/§E.3/§E.5 — the per-thread event loop (ANNEXES E2A + E3,
// BINDING; U-T9). See the header banner for the threadMain integration shape
// (ThreadObject.cpp is outside U-T9's owned set — recorded obligation).
// ============================================================================

void openThreadInbox(ThreadState& state)
{
    // §E.1: exactly once, on the owning spawned thread, post-§B.1 attach,
    // BEFORE fn — the inboxLock hold gives the r22 happens-before edge vs
    // every later registration against this TS.
    RELEASE_ASSERT(state.isSpawned);
    RELEASE_ASSERT(state.nativeThread.get() == &Thread::currentSingleton());
    Locker locker { state.inboxLock };
    RELEASE_ASSERT(!state.inboxOpen);
    RELEASE_ASSERT(state.taskQueue.isEmpty());
    ASSERT(!state.keepaliveCount);
    state.inboxOpen = true;
}

void addThreadWaitDeadline(ThreadState& state, MonotonicTime deadline, Function<bool()>&& tryDequeue, Function<void()>&& settleTimedOut)
{
    // §C.3/§E.7.5 (r12 F3): finite-timeout PROPERTY waitAsync deadlines park
    // on the spawned registrant TS; the E2A loop expires them locally (SD16
    // — a registrant that never reaches its drain loop never times out; the
    // §E.5 close harvest is the other expiry edge).
    RELEASE_ASSERT(state.isSpawned);
    Locker locker { state.inboxLock };
    RELEASE_ASSERT(state.inboxOpen); // registration sites gate on spawned+open (U25 shape).
    state.waitDeadlines.append(ThreadWaitDeadline { deadline, WTF::move(tryDequeue), WTF::move(settleTimedOut) });
    // An idle E2A wait recomputes its sleep bound on wake.
    state.runLoopCondition.notifyOne();
}

// §E.2 EXPIRE step (r12; the landed 5.6 timeout, inline): repeatedly take the
// earliest due deadline under inboxLock, DROP inboxLock, dequeue under the
// waiter's listLock inside tryDequeue (already-dequeued => skip — the
// in-flight settle wins), then §E.4 settle "timed-out" (rule-1 decrement
// applies inside the settle routing). Rank-3 locks are NEVER held together
// (§LK): inboxLock is dropped before tryDequeue takes the list lock.
static void expireDueThreadWaitDeadlines(ThreadState& state)
{
    while (true) {
        std::optional<ThreadWaitDeadline> due;
        {
            Locker locker { state.inboxLock };
            MonotonicTime now = MonotonicTime::now();
            size_t dueIndex = notFound;
            for (size_t i = 0; i < state.waitDeadlines.size(); ++i) {
                if (state.waitDeadlines[i].deadline > now)
                    continue;
                if (dueIndex == notFound || state.waitDeadlines[i].deadline < state.waitDeadlines[dueIndex].deadline)
                    dueIndex = i;
            }
            if (dueIndex == notFound)
                break;
            due.emplace(WTF::move(state.waitDeadlines[dueIndex]));
            state.waitDeadlines.removeAt(dueIndex);
        }
        if (due->tryDequeue())
            due->settleTimedOut();
    }
}

// The earliest pending deadline, or infinity. Requires inboxLock.
static MonotonicTime earliestThreadWaitDeadlineLocked(ThreadState& state) WTF_REQUIRES_LOCK(state.inboxLock)
{
    MonotonicTime earliest = MonotonicTime::infinity();
    for (auto& entry : state.waitDeadlines) {
        if (entry.deadline < earliest)
            earliest = entry.deadline;
    }
    return earliest;
}

// The §E.5/E2A close block + the F5 completion protocol + the EXIT1.3
// teardown handoff. `terminated` selects the TERM1.3 Failed publication.
static void closeThreadInboxAndComplete(VM& vm, VMLite& lite, ThreadState& state, ThreadState::Phase phase, bool terminated)
{
    GCClient::Heap* client = lite.clientHeap;
    RELEASE_ASSERT(client);

    // Close harvest, access-released (E2A close; lock/access rule: no
    // heap-access transition while holding a rank-3 lock).
    if (client->hasHeapAccess())
        client->releaseHeapAccess();
    Deque<ThreadTask> residue;
    Vector<ThreadWaitDeadline> deadlines;
    {
        Locker locker { state.inboxLock };
        state.inboxOpen = false; // keepalive DEAD from here (E.3 rule 3).
        residue = std::exchange(state.taskQueue, { });
        deadlines = std::exchange(state.waitDeadlines, { }); // r16 F5
    }
    // Post-drop §A.3.2b poll + re-acquisition: acquireHeapAccess is the
    // gated form (parks across any pending stop).
    if (!client->hasHeapAccess())
        client->acquireHeapAccess();

    // Deadline harvest (r16 F5; SD8 ext: finite waitAsync never hangs —
    // early "timed-out" at owner close/termination is the declared SD8
    // EXTENSION). Each entry: dequeue under its listLock (already-dequeued
    // => skip, the in-flight settle wins), drop the listLock, §E.4 settle
    // "timed-out" — which takes the MAIN fallback since the inbox is closed;
    // the rule-1 decrement skip is the existing exactly-once story.
    for (auto& entry : deadlines) {
        if (entry.tryDequeue())
            entry.settleTimedOut();
    }

    // Residue: route to main (E.4 dead rule) — the tickets are already
    // settled (CAS won at enqueue); their tasks run at the next carrier
    // drain. DWT registration is still live, so the landed retirement
    // applies there.
    while (!residue.isEmpty()) {
        ThreadTask task = residue.takeFirst();
        task.ticket->routeQueuedTaskToMainFallback(WTF::move(task.task));
    }

    // SD17 (r24 F3): per-lite microtask residue is DROPPED at close — never
    // drained, never adopted (I11); it dies with ~VMLite. Published
    // settlements remain visible (they were published cross-thread before
    // any reaction job was enqueued here).

    // F1/F5 completion (under this thread's token): Phase release-store +
    // joiner settle + 5.10 Strong clears. Thread completes — and
    // join/asyncJoin settle — ONLY here (U7, SD1).
    {
        JSLockHolder locker(vm);
        JSValue terminationResult;
        if (terminated) {
            // TERM1.3 (SD8 ext2): Failed publishes a FRESH ordinary
            // Error("Thread terminated") — NEVER the sticky
            // m_terminationException. join() rethrows it NORMALLY (the
            // joiner is not re-terminated); asyncJoin rejects with it.
            phase = ThreadState::Phase::Failed;
            JSValue threadCell = state.jsThread.get();
            if (threadCell.isObject()) {
                JSGlobalObject* globalObject = asObject(threadCell)->globalObject();
                auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
                terminationResult = createError(globalObject, "Thread terminated"_s);
                if (scope.exception()) [[unlikely]] {
                    scope.clearExceptionExceptTermination();
                    terminationResult = jsUndefined();
                }
            } else
                terminationResult = jsUndefined();
            state.result.set(vm, terminationResult);
        }

        Vector<Ref<AsyncTicket>> joiners;
        {
            Locker joinLocker { state.joinLock };
            state.phase.store(phase, std::memory_order_release);
            state.joinCondition.notifyAll();
            joiners = std::exchange(state.asyncJoiners, { });
        }
        bool failed = phase == ThreadState::Phase::Failed;
        for (auto& joiner : joiners) {
            // §E.4 routes each joiner by ITS registrant (mutual/self
            // asyncJoin settles via the joiner's own inbox or the main
            // fallback; SD12 — asyncJoin never counted keepalive, so no
            // decrement fires here).
            joiner->settle([protectedTicket = joiner.copyRef(), protectedState = Ref { state }, failed](DeferredWorkTimer::Ticket) {
                JSPromise* promise = protectedTicket->promise().get();
                if (!promise)
                    return;
                JSGlobalObject* globalObject = promise->realm();
                VM& promiseVM = globalObject->vm();
                JSValue result = protectedState->result.get();
                if (!result)
                    result = jsUndefined();
                if (failed)
                    promise->reject(promiseVM, result);
                else
                    promise->resolve(globalObject, promiseVM, result);
            });
        }

        // 5.10 Strong clears (TS::result is NOT cleared — the finalizer hook
        // is its sole clearer; join()/asyncJoin() readers still need it).
        state.threadLocals.clear();
        state.jsThread.clear();
        ThreadManager::singleton().unregisterThread(state);
    }

    // Access release at the landed T5 point + the EXIT1.3 teardown order
    // (as AMENDED r31) — tearDownSpawnedThreadForExit owns both. The caller
    // frees the lite after we return (EXIT1.9 residual rule).
    tearDownSpawnedThreadForExit(vm, lite);
}

void runSpawnedThreadDrainLoopAndClose(VM& vm, VMLite& lite, ThreadState& state, ThreadState::Phase phase)
{
    // ANNEX E2A, verbatim shape. GIL-off only; the caller is the owning
    // spawned thread, after fn returned/threw, holding no lock and no token.
    RELEASE_ASSERT(vm.gilOff());
    RELEASE_ASSERT(state.isSpawned);
    RELEASE_ASSERT(lite.vm == &vm);
    GCClient::Heap* client = lite.clientHeap;
    RELEASE_ASSERT(client);

    static constexpr Seconds drainQuantum = 10_ms; // D9 quanta bound (§A.2.4).
    bool terminated = false;

    while (true) {
        // drainMicrotasks(own): VM::drainMicrotasks reroutes to the CURRENT
        // lite's queue GIL-off (I11; the U-T9 VM.cpp reroute).
        {
            JSLockHolder locker(vm);
            vm.drainMicrotasks();
        }

        // releaseClientHeapAccess() BEFORE the rank-3 hold (lock/access
        // rule: heap-access transitions are not leaf).
        if (client->hasHeapAccess())
            client->releaseHeapAccess();

        std::optional<ThreadTask> task;
        bool doClose = false;
        {
            Locker locker { state.inboxLock };
            if (vm.hasTerminationRequest()) [[unlikely]] {
                // §E.5/TERM1: the trap takes the landed Failed path VIA the
                // close block (incl. its deadline harvest).
                terminated = true;
                doClose = true;
            } else if (!state.taskQueue.isEmpty())
                task.emplace(state.taskQueue.takeFirst());
            else if (!state.keepaliveCount)
                doClose = true; // E2A exit predicate: queues empty + keepalive == 0 (U9: read under the same lock as the decrement+append).
            else {
                // Bounded wait: min(10ms quantum, earliest waitDeadline).
                // Wakeups: task append, stop/termination (quantum-bounded
                // poll), deadline registration.
                MonotonicTime bound = MonotonicTime::now() + drainQuantum;
                MonotonicTime earliestDeadline = earliestThreadWaitDeadlineLocked(state);
                if (earliestDeadline < bound)
                    bound = earliestDeadline;
                state.runLoopCondition.waitUntil(state.inboxLock, bound);
            }
        }

        // Post-wake §A.3.2b poll + reacquire: acquireHeapAccess is the gated
        // form — it parks across any pending stop before returning.
        if (!client->hasHeapAccess())
            client->acquireHeapAccess();

        if (doClose)
            break;

        // EXPIRE deadlines (r12): inline, on the registrant (SD16).
        expireDueThreadWaitDeadlines(state);

        if (task) {
            // Run the ThreadTask (arbitrary JS) under this thread's §F
            // token; uncaught exceptions report at the event loop, matching
            // the DWT doWork contract.
            JSLockHolder locker(vm);
            auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
            // Snapshot the report realm BEFORE the body runs — retirement
            // step (c) clears the promise Strong.
            JSGlobalObject* reportGlobalObject = nullptr;
            if (JSPromise* promise = task->ticket->promise().get())
                reportGlobalObject = promise->realm();
            task->ticket->runQueuedSettleTaskOnRegistrant(WTF::move(task->task));
            if (Exception* exception = scope.exception()) [[unlikely]] {
                if (scope.clearExceptionExceptTermination() && reportGlobalObject)
                    reportGlobalObject->globalObjectMethodTable()->reportUncaughtExceptionAtEventLoop(reportGlobalObject, exception);
            }
        }
    }

    closeThreadInboxAndComplete(vm, lite, state, phase, terminated);
}

// Per-insert Weak<JSObject> finalizer (SPEC-api 5.7.2): prunes the affinity
// table when a restricted cell dies. finalize() runs on a thread holding the
// JSLock (GC finalization / lastChanceToFinalize), with no rank 1-3 lock
// held, so taking the rank-2 affinity lock here is 5.9-legal.
class ThreadAffinityWeakHandleOwner final : public WeakHandleOwner {
public:
    void finalize(Handle<Unknown> handle, void* context) final
    {
        // context is the ThreadAffinityEntry this Weak was created for (set
        // at Weak construction in restrictObject); pruneRestrictedObject uses
        // it as an identity check so a stale finalizer never removes a
        // successor entry installed at a recycled cell address.
        ThreadManager::singleton().pruneRestrictedObject(handle.get().asCell(), context);
    }
};

static WeakHandleOwner& threadAffinityWeakHandleOwner()
{
    static NeverDestroyed<ThreadAffinityWeakHandleOwner> owner;
    return owner.get();
}

ThreadManager::Affinity ThreadManager::objectAffinity(JSObject* object)
{
    if (!anyRestrictedObjects())
        return Affinity::None;
    RefPtr<ThreadState> owner = restrictionOwner(object);
    if (!owner)
        return Affinity::None;
    // Owner identity is the restricting thread's ThreadState/nativeThread,
    // never its TID (5.7.2; lazy main/embedder TSs all share tid 0). The
    // nativeThread pointer is compared, never dereferenced.
    return owner->nativeThread.get() == &Thread::currentSingleton() ? Affinity::Owner : Affinity::Foreign;
}

// Deleted-slot-reuse hazard, applied to the affinity table: the table is
// keyed by raw JSCell*, and a dead restricted cell's entry is pruned by its
// Weak's finalizer — which runs when the containing WeakBlock is swept and
// can therefore LAG the MarkedBlock sweep that recycles the dead cell's
// memory. Until the finalizer runs, a NEW object allocated at the recycled
// address aliases the stale key. Every reader/writer below therefore treats
// an entry whose Weak is not Live-and-equal-to-the-probed-object as ABSENT
// (Weak<T>::get() returns null for any non-Live impl, so a dead-but-not-yet-
// finalized Weak can never report a match).

static std::unique_ptr<ThreadAffinityEntry> makeAffinityEntry(JSObject* object, ThreadState& owner)
{
    // The Weak is created under the GIL (Thread.restrict is a host call)
    // and carries the pruning finalizer; the table holds no Strong on the
    // restricted object (5.7.2: entries pruned by per-insert Weak<JSObject>
    // finalizers). The entry's own address is the Weak's context, giving the
    // finalizer an identity check (see pruneRestrictedObject).
    auto entry = makeUnique<ThreadAffinityEntry>(Ref { owner }, Weak<JSObject> { });
    entry->weak = Weak<JSObject>(object, &threadAffinityWeakHandleOwner(), entry.get());
    return entry;
}

RefPtr<ThreadState> ThreadManager::restrictionOwner(JSObject* object)
{
    Locker locker { m_affinityLock };
    auto it = m_affinityTable.find(object);
    if (it == m_affinityTable.end())
        return nullptr;
    // Stale-key check (header comment above): a recycled-address alias must
    // never make a never-restricted object appear restricted.
    if (it->value->weak.get() != object)
        return nullptr;
    return RefPtr<ThreadState> { it->value->owner.copyRef() };
}

bool ThreadManager::restrictObject(JSObject* object, ThreadState& owner)
{
    Locker locker { m_affinityLock };
    auto result = m_affinityTable.ensure(object, [&] {
        return makeAffinityEntry(object, owner);
    });
    if (result.isNewEntry) {
        m_restrictedCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (result.iterator->value->weak.get() == object) {
        // Live entry for THIS object. GIL-on, only an owner-idempotent
        // re-restrict can reach here (the caller's affinity step (0) and
        // this insert are one atomic host call). GIL-off, step (0) and this
        // section are SEPARATE m_affinityLock sections: two threads can both
        // observe Affinity::None and race here, and the loser must not be
        // told it owns an object the table assigns to its rival (MC-HAND S6
        // phantom ownership claim, docs/threads/cve/map-MC-HAND.md; frozen
        // SPEC-api 4.1 requires ConcurrentAccessError for a re-restrict from
        // another thread). The claim verdict is therefore re-derived under
        // THIS lock section: owner identity is the ThreadState pointer
        // (5.7.2), compared, never dereferenced.
        return result.iterator->value->owner.ptr() == &owner;
    }
    // Stale entry: the previous occupant of this address died and its
    // finalizer has not run yet. Replace the entry so the NEW object's
    // restriction is recorded with the correct owner. The count is already
    // accounted (one entry before, one after). Destroying the old entry
    // deallocates its dead WeakImpl, so its finalizer will not run; if it
    // already started racing us, its identity check (context != entry)
    // makes it a no-op.
    result.iterator->value = makeAffinityEntry(object, owner);
    return true;
}

void ThreadManager::pruneRestrictedObject(JSCell* cell, void* expectedEntry)
{
    Locker locker { m_affinityLock };
    auto it = m_affinityTable.find(cell);
    if (it == m_affinityTable.end())
        return;
    // Identity check: only the finalizer belonging to THIS entry may remove
    // it. A late finalizer for a dead predecessor at a recycled address must
    // not evict the live successor entry restrictObject installed.
    if (it->value.get() != expectedEntry)
        return;
    m_affinityTable.remove(it);
    m_restrictedCount.fetch_sub(1, std::memory_order_relaxed);
}

// NOTE for reviewers: in THIS tree the only caller is threadFuncRestrict's
// affinity step — that is by design, not an omission. The 14 generic-path
// hook sites (JSObject.h / JSObjectInlines.h / JSObject.cpp) are
// INTEGRATOR-applied after the obj-model merge, per the frozen SPEC-api I14
// ("INT gate via 9.2-6; //@ skipped until then") and the exact ready-to-apply
// diff in docs/threads/INTEGRATE-api.md 9.2-6. Those files belong to the
// objectmodel workstream's merge surface, so the api workstream must not
// land the hooks itself; JSTests/threads/api/thread-restrict.js stays
// //@ skip'ped until the integrator applies the hook and deletes the skip
// line (the 9.2-6 apply checklist).
bool threadRestrictCheck(JSGlobalObject* globalObject, JSObject* object)
{
    auto& manager = ThreadManager::singleton();
    if (!manager.anyRestrictedObjects()) [[likely]]
        return true;
    switch (manager.objectAffinity(object)) {
    case ThreadManager::Affinity::None:
    case ThreadManager::Affinity::Owner:
        return true;
    case ThreadManager::Affinity::Foreign:
        break;
    }
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    throwConcurrentAccessError(globalObject, scope, "Concurrent access to a thread-restricted object"_s);
    return false;
}

} // namespace JSC
