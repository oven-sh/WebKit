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

#pragma once

#include "DeferredWorkTimer.h"
#include "Options.h"
#include "Strong.h"
#include "Weak.h"
#include <wtf/Condition.h>
#include <wtf/Deque.h>
#include <wtf/FastMalloc.h>
#include <wtf/Function.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/StdLibExtras.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>

namespace JSC {

class JSCell;
class JSGlobalObject;
class JSObject;
class JSPromise;
class VM;
class VMLite;

namespace GCClient {
class Heap;
}

// Master gate for the phase-1 GIL'd shared-memory Thread API
// (docs/threads/SPEC-api.md). The GIL is the shared VM's JSLock and is
// always on in phase 1.
//
// SPEC-api 9.2-1 paired edit (a), landed: this gate reads ONLY the canonical
// Options::useJSThreads(), the same predicate every other workstream
// (objectmodel JSObject.h hooks, jit TID-tag paths, vmstate flag
// implications and its planned GIL-off backstop assert) gates on. The
// prep-stub --useThreads alias must NOT be honored here: it would let a
// --useThreads=1 run spawn real OS threads into the shared heap with all
// flag-gated concurrent-mode machinery (keyed on useJSThreads) switched
// off. The alias OptionsList.h entry itself is now a dead option with zero
// code consumers; its one-line deletion is the only remaining 9.2-1 INT
// action (OptionsList.h is a shared hot file, not api-editable).
ALWAYS_INLINE bool useJSThreadsEnabled()
{
    return Options::useJSThreads();
}

class ThreadState;

// One async ticket type backing asyncJoin / asyncHold / asyncWait /
// property Atomics.waitAsync (SPEC-api 5.5). Wraps a DeferredWorkTimer
// ticket whose target is the promise; the ticket's pending-work
// registration — performed at creation, under the JSLock — is what keeps
// the shell alive until the promise settles (I20/4.6.3), NOT settle time.
class AsyncTicket final : public ThreadSafeRefCounted<AsyncTicket> {
public:
    // Must be called while holding the shared VM's JSLock. Captures the
    // calling thread's ThreadState as the registrant.
    //
    // countsKeepalive (§E.3, U-T9): pass true at the COUNTED registration
    // sites — asyncHold, cond.asyncWait, property Atomics.waitAsync; never
    // asyncJoin — and create arms the keepalive INTERNALLY (the full §E.3
    // protocol: armed-before-visible, inboxLock'd increment, spawned+OPEN
    // assert) iff the VM is gilOff and the registrant is a spawned TS.
    // Main/embedder registrants and GIL-on/flag-off ignore the bit (§E.7:
    // they never touch keepalive). Defaulted false so the unowned call-site
    // TUs compile unchanged; see gate U-T9-INT1 at armKeepalive() below.
    JS_EXPORT_PRIVATE static Ref<AsyncTicket> create(JSGlobalObject*, JSPromise*, Vector<JSCell*>&& dependencies = { }, bool countsKeepalive = false);
    JS_EXPORT_PRIVATE ~AsyncTicket();

    VM& vm() { return m_vm; }

    // The registering thread's ThreadState (SPEC-api 5.5 / 4.6.2). Tickets
    // are process-owned and outlive their registering thread: this Ref keeps
    // the registrant ThreadState alive past its completion sequence, so a
    // finished thread's pending continuation (e.g. its own asyncHold) still
    // settles (I20). In the GIL phase the settling thread is whichever one
    // drains the single shared VM queue (I12 relaxation); post-GIL this is
    // the routing key for the per-ThreadState ticket inbox (5.5).
    ThreadState& registrant() const { return m_registrant.get(); }

    // The promise this ticket settles (SPEC-api 5.5 / 5.10): the Strong is
    // created at registration (host call, under the JSLock) and cleared by
    // the settle task (which runs under the JSLock); a never-settled
    // ticket's pending work is cancelled at DWT VM shutdown (5.5), and its
    // Strong dies with the ticket. Read it only from a settle task or while
    // holding the JSLock.
    Strong<JSPromise>& promise() { return m_promise; }

    // True once DWT VM-shutdown cancelPendingWork ran for the underlying
    // ticket (settle() is then a no-op). Timer tasks check this to bail out
    // before touching VM state after shutdown.
    bool isCancelled() const { return m_ticket->isCancelled(); }

    // For lock-release / asyncWait consumption arbitration (SPEC-api 5.5a).
    bool tryConsume()
    {
        bool expected = false;
        return m_consumed.compare_exchange_strong(expected, true);
    }

    // Lock-grant delivery gate (SPEC-api 4.3(b), tightened — see the
    // "Landed deviations" D6 entry in docs/threads/INTEGRATE-api.md).
    // NativeLockState installs m_asyncHolder (pump / immediate-grant path)
    // BEFORE the grant's settle task runs; until that task delivers the
    // grant to user code (runs the held fn, or resolves the promise with a
    // release fn), the hold is not observable by JS and MUST NOT be
    // consumable by cond.asyncWait's (b) arm — consuming it would unlock
    // m_lock while the with-fn settle task later runs fn believing it holds
    // the lock (mutual-exclusion hole, I6). markGrantDelivered() is called
    // by settleLockGrant's settle tasks (under the JSLock); readers
    // (cond.asyncWait, also under the JSLock in phase 1) load-acquire.
    void markGrantDelivered() { m_grantDelivered.store(true, std::memory_order_release); }
    bool grantDelivered() const { return m_grantDelivered.load(std::memory_order_acquire); }

    // Schedules a settle task on the VM's run loop via
    // DeferredWorkTimer::scheduleWorkSoon (SPEC-api 5.5): never settles
    // synchronously inside the registering call (I12). Thread-safe; only the
    // first call has any effect. The task runs holding the JSLock with the
    // promise as the ticket target; after it returns, the ticket's promise
    // Strong is cleared (5.10).
    JS_EXPORT_PRIVATE void settle(DeferredWorkTimer::Task&&);

    // asyncHold with-fn arity support: the function cell, rooted by the
    // underlying ticket's dependency vector.
    JSObject* extraDependency() const { return m_extraDependency; }
    bool grantWithFunction { false };

    std::atomic<uint8_t> state { 0 }; // Waiting = 0, Notified = 1, TimedOut = 2.

    // ========================================================================
    // UNGIL §E.3 keepalive accounting (ANNEX E3, BINDING; U-T9).
    //
    // m_keepaliveReleased is CONSTRUCTED true (= released; r6 F1 — the safe
    // default mirrors the landed m_settled CAS): a never-armed ticket
    // (asyncJoin, TA waitAsync, main/embedder registrations, any future
    // non-counted registration) loses every claim CAS and never decrements,
    // so the uint64 keepalive counter can never wrap below zero.
    //
    // armKeepalive() is the SOLE site that stores false (= armed): called
    // once at registration (I20 addPendingWork time), on the REGISTERING
    // (spawned, inbox-OPEN) thread, BEFORE the ticket is visible to any
    // settler — every spawned-TS AsyncTicket EXCEPT asyncJoin (asyncHold,
    // cond.asyncWait, property Atomics.waitAsync; §E.3 INCREMENT rule).
    // Increments the registrant's keepaliveCount under its inboxLock and
    // asserts spawned+OPEN (U25).
    //
    // claimKeepaliveRelease() is the exactly-once gate every DECREMENT site
    // must win first (false->true CAS); losers do nothing. Decrement sites
    // (§E.3): (1) settle-enqueue, in the SAME inboxLock section as the
    // ThreadTask append, iff inboxOpen; (2) VM-shutdown cancelPendingWork.
    // Inbox-close performs NO claim step: a post-close settle wins its CAS,
    // observes inboxOpen == false, SKIPS the decrement (the counter is dead)
    // and takes the main fallback.
    //
    // ==== NAMED INTEGRATION GATE U-T9-INT1 (blocks the §E ladder arms) ====
    // The arming PROTOCOL is fully in-set: AsyncTicket::create takes
    // `bool countsKeepalive` and arms internally (gilOff + spawned
    // registrant). The RESIDUAL obligation is exactly four one-token edits
    // in TUs outside U-T9's owned set, to be landed TOGETHER with the
    // threadMain E2A wiring (openThreadInbox / runSpawnedThreadDrainLoop-
    // AndClose, see the integration-point banner below):
    //   LockObject.cpp:492      asyncHold            -> countsKeepalive=true
    //   ConditionObject.cpp:287 cond.asyncWait       -> countsKeepalive=true
    //   ThreadAtomics.cpp:639   property waitAsync   -> countsKeepalive=true
    //   ThreadObject.cpp:414    asyncJoin            -> stays false (§E.3)
    // plus the §C.3 finite-timeout addThreadWaitDeadline call (SD16). Until
    // U-T9-INT1 lands, every ticket stays never-armed: keepalive reads 0, a
    // spawned thread's E2A loop exits at fn-return + queue-empty, and late
    // settles take the declared main fallback (the api 4.6.2 class) — safe
    // (no hang, no wrap) but the §E.3 liveness semantics (never-notified
    // asyncHold/waitAsync keeps the loop alive) and the SD16 timing arms are
    // NOT in force; the §E.2/§E.3/SD16/SD17 ladder arms cannot be claimed
    // green before this gate closes. GIL-on/flag-off: armKeepalive is never
    // called and nothing here runs.
    // ========================================================================
    JS_EXPORT_PRIVATE void armKeepalive();
    bool claimKeepaliveRelease()
    {
        bool expected = false;
        return m_keepaliveReleased.compare_exchange_strong(expected, true);
    }

    // §E.4 "DWT retirement on the task-queue path": runs a queued ThreadTask
    // body on the registrant under its §F token — (a) the settle task,
    // (b) cancelPendingWork (fires the §E.7.4 wake), (c) m_promise clear.
    // E2A-loop only.
    void runQueuedSettleTaskOnRegistrant(DeferredWorkTimer::Task&&);

    // E2A close residue rule (§E.4 dead=>main): re-routes a harvested,
    // already-CAS-settled ThreadTask to the landed scheduleWorkSoon path.
    // Called with no api rank-1..3 lock held.
    void routeQueuedTaskToMainFallback(DeferredWorkTimer::Task&&);

    // §C.3(b) revoked-registration retirement (U-T11; MC-DOS S4): the
    // under-listLock SVZ re-validation dequeued this ticket's waiter before
    // ANY settler could observe it (the node was published and revoked while
    // its state was still Waiting, both under the list lock), so no settle
    // task will ever run. Retire the registration synchronously: win the
    // settle CAS (a racing settle/cancel then no-ops), cancel the pending
    // DWT work (un-roots target + dependencies from the realm's weak-ticket
    // visitor; fires the §E.7.4 emptiness wake), and clear the promise
    // Strong. Must run under the JSLock with no api rank-1..3 lock held;
    // caller guarantees exclusivity (RELEASE_ASSERTed via the CAS).
    void retireUnsettled();

private:
    AsyncTicket(VM&, Ref<DeferredWorkTimer::TicketData>&&, Ref<ThreadState>&& registrant, JSObject* extraDependency);

    // §E.4 helper: the post-CAS settle body (open-arm inbox append with the
    // rule-1 decrement, or the r17 F6 main fallback after the drop).
    void settleViaRegistrantRouting(DeferredWorkTimer::Task&&);

    // The landed GIL-on/flag-off settle tail (and the GIL-off main-fallback
    // arm): wraps the task to keep `this` alive and clear m_promise, then
    // DWT scheduleWorkSoon.
    void scheduleViaDeferredWorkTimer(DeferredWorkTimer::Task&&);

    VM& m_vm;
    Ref<DeferredWorkTimer::TicketData> m_ticket;
    Ref<ThreadState> m_registrant;
    Strong<JSPromise> m_promise;
    JSObject* m_extraDependency { nullptr };
    std::atomic<bool> m_settled { false };
    std::atomic<bool> m_consumed { false };
    std::atomic<bool> m_grantDelivered { false };
    std::atomic<bool> m_keepaliveReleased { true }; // §E.3: constructed RELEASED; armKeepalive() alone stores false.
};

// One queued cross-thread settlement (§E.1: "ThreadTask = settle task +
// Ref<AsyncTicket>"). The task body runs on the registrant under its §F
// token: (a) the settle task, (b) DWT retirement (cancelPendingWork — fires
// the §E.7.4 wake), (c) m_promise clear (§E.4 "DWT retirement on the
// task-queue path"; thread keepalive supersedes DWT shell-liveness for
// spawned registrants).
struct ThreadTask {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ThreadTask);

    DeferredWorkTimer::Task task;
    Ref<AsyncTicket> ticket;
};

// One §C.3/§E.7.5 finite-timeout deadline parked on a spawned registrant TS
// (SD16: the 5.6 run-loop timer becomes a registrant-local deadline; the
// §E.2 wait sleeps min(quantum, earliest deadline) and expires it locally).
// Guarded by the registrant's inboxLock.
//
// tryDequeue: dequeues the waiter under ITS list lock (api rank 3) and
// returns whether this expirer won — an already-notified/dequeued waiter
// returns false (the in-flight settle wins; §E.5 harvest rule). Called with
// NO lock held; takes and DROPS the list lock internally (rank-3 locks are
// never held together, §LK).
// settleTimedOut: the §E.4 settle "timed-out" (rule-1 decrement applies).
// Called strictly AFTER tryDequeue returned true, with NO api rank-1..3
// lock held (r17 F2).
struct ThreadWaitDeadline {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ThreadWaitDeadline);

    MonotonicTime deadline;
    Function<bool()> tryDequeue;
    Function<void()> settleTimedOut;
};

// Native state of one JS thread (SPEC-api 5.1). Main/embedder threads get a
// lazily created ThreadState (tid 0, isSpawned = false) on first use.
class ThreadState final : public ThreadSafeRefCounted<ThreadState> {
public:
    enum class Phase : uint8_t { Running, Finished, Failed };

    static Ref<ThreadState> create(uint16_t tid, bool isSpawned)
    {
        return adoptRef(*new ThreadState(tid, isSpawned));
    }

    ~ThreadState()
    {
        // SPEC-api 5.10: every Strong must have been cleared (under the
        // JSLock) before the last reference drops.
        RELEASE_ASSERT(!result);
        RELEASE_ASSERT(!fnSlot);
        RELEASE_ASSERT(argSlots.isEmpty());
        RELEASE_ASSERT(threadLocals.isEmpty());
        RELEASE_ASSERT(!jsThread);
        // UNGIL §E.2/§E.5 (U-T9): the E2A close block routed every residue
        // ThreadTask to main and harvested every waitDeadline before the
        // last ref can drop; a lazy main/embedder TS never opens its inbox
        // (§E.1), so both stay empty there.
        {
            Locker inboxLocker { inboxLock };
            RELEASE_ASSERT(!inboxOpen);
            RELEASE_ASSERT(taskQueue.isEmpty());
            RELEASE_ASSERT(waitDeadlines.isEmpty());
        }
        // asyncJoiners must have been drained too: by the completion
        // sequence (spawned threads) or by the 5.10 finalizer hook
        // (never-completing lazy/embedder ThreadStates, VM teardown) —
        // otherwise this destructor, which can run off the JSLock (TLS slot
        // teardown of an exiting embedder thread, possibly after VM death),
        // would drop the last refs to unsettled AsyncTickets whose
        // Strong<JSPromise> is still set (5.10 violation / VM UAF).
        {
            Locker joinLocker { joinLock };
            RELEASE_ASSERT(asyncJoiners.isEmpty());
        }
    }

    uint16_t tid { 0 };
    bool isSpawned { false };

    // Compared, never dereferenced for identity purposes.
    RefPtr<WTF::Thread> nativeThread;

    std::atomic<Phase> phase { Phase::Running };

    // The thread's result (fn's return value, or the thrown exception value
    // when phase == Failed). F1: written under the JSLock in the completion
    // sequence, release-fenced by the subsequent Phase store; read under the
    // JSLock by join() and the asyncJoin settle tasks. Cleared ONLY by the
    // 5.10 finalizer hook registered at TS::jsThread creation.
    Strong<Unknown> result;

    Lock joinLock;
    Condition joinCondition;
    Vector<Ref<AsyncTicket>> asyncJoiners WTF_GUARDED_BY_LOCK(joinLock);

    // Owner-thread-only ThreadLocal storage (SPEC-api 5.8).
    UncheckedKeyHashMap<uint64_t, Strong<Unknown>> threadLocals;

    // Roots the JSThread cell while the thread is alive (Thread.current
    // identity); cleared (under the JSLock) in the completion sequence.
    Strong<Unknown> jsThread;

    // Root fn/args between spawn and invocation (cleared right after the
    // call, under the JSLock, on the spawned thread).
    Strong<Unknown> fnSlot;
    Vector<Strong<Unknown>> argSlots;

    // ========================================================================
    // UNGIL §E.1 per-thread macrotask queue (U-T9; supersedes the inert
    // phase-1 inbox vector — "the landed inbox vector IS the task queue").
    // ALL fields below are guarded by the EXISTING inboxLock (api rank 3).
    //
    //  - inboxOpen: set true EXACTLY ONCE, on the owning spawned thread,
    //    under inboxLock, post-§B.1 attach, BEFORE fn (openThreadInbox below;
    //    happens-before any registration vs this TS — r22). Main/embedder
    //    NEVER open theirs (§E.4's closed arm routes their settles through
    //    the landed scheduleWorkSoon path, r17 F6). Monotonic: true exactly
    //    once pre-fn, false forever at close — the r18 F2 act-after-drop
    //    fallback is sound only because of this monotonicity.
    //  - taskQueue: appended by §E.4 open-arm settles; drained ONLY by the
    //    owner's E2A loop.
    //  - keepaliveCount: §E.3/ANNEX E3 — outstanding armed registrations
    //    that may still enqueue a task here; transitions under inboxLock,
    //    exactly-once via AsyncTicket::m_keepaliveReleased. DEAD once
    //    inboxOpen flips false (close): later claims skip the decrement.
    //  - runLoopCondition: the E2A wait/wake edge (wakeups: task append,
    //    stop, termination, quantum — §E.2).
    //  - waitDeadlines: §C.3/§E.7.5 deadline-ordered-by-scan list of
    //    finite-timeout PROPERTY waitAsync deadlines registered while this
    //    spawned TS was open (SD16); expired by the owner's E2A EXPIRE step
    //    or the §E.5 close harvest (r16 F5).
    //
    // GIL-on/flag-off: nothing reads or writes any of these (the E2A loop
    // only runs gilOff; §E.4 routing is gilOff-gated) — flag-off identity.
    // ========================================================================
    Lock inboxLock; // rank 3
    bool inboxOpen WTF_GUARDED_BY_LOCK(inboxLock) { false };
    Deque<ThreadTask> taskQueue WTF_GUARDED_BY_LOCK(inboxLock);
    uint64_t keepaliveCount WTF_GUARDED_BY_LOCK(inboxLock) { 0 };
    Condition runLoopCondition;
    Vector<ThreadWaitDeadline> waitDeadlines WTF_GUARDED_BY_LOCK(inboxLock);

private:
    ThreadState(uint16_t tid, bool isSpawned)
        : tid(tid)
        , isSpawned(isSpawned)
    {
    }
};

// One Thread.restrict affinity-table entry (SPEC-api 5.7.2). The owner is the
// restricting thread's Ref<ThreadState> (NEVER its TID: lazy main/embedder
// ThreadStates all share tid 0); ownership checks compare
// owner->nativeThread.get() against &WTF::Thread::currentSingleton(). The
// per-insert Weak<JSObject> carries a WeakHandleOwner finalizer that prunes
// this entry (and decrements the restricted-object count) when the restricted
// cell dies, so the table never roots restricted objects and never grows
// unboundedly.
struct ThreadAffinityEntry {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ThreadAffinityEntry);

    ThreadAffinityEntry(Ref<ThreadState>&& owner, Weak<JSObject>&& weak)
        : owner(WTF::move(owner))
        , weak(WTF::move(weak))
    {
    }

    Ref<ThreadState> owner;
    Weak<JSObject> weak;
};

class ThreadManager final {
    WTF_MAKE_NONCOPYABLE(ThreadManager);
public:
    JS_EXPORT_PRIVATE static ThreadManager& singleton();

    static constexpr uint16_t mainThreadTID = 0;
    static constexpr uint16_t notTTLTID = 0x7fff; // reserved

    // UNGIL §A.3.6 / ANNEX A36 (U-T6): GIL-off every main/embedder carrier
    // lite needs a TM-allocated unique nonzero TID from the SAME 2^15 space
    // as spawned threads (I17 exhaustion accounting includes carriers; r9
    // F4 — main/embedder ThreadState.tid STAYS 0, the carrier TID is a
    // separate allocation).
    //
    // RECORDED REFINEMENT (U-T6; not a weakening): carriers allocate from a
    // FIXED upper sub-range [carrierTIDBase, notTTLTID) while spawned
    // threads keep [1, carrierTIDBase). The split exists so the ~VM A36
    // carrier-collection walk — which runs under VMLiteRegistry::lock and
    // therefore may acquire NO other lock (§LK.6/I7) — can discriminate
    // carrier lites from spawned lites with a pure range check on the
    // immutable lite tid instead of probing m_threads under m_lock. Both
    // halves still exhaust against the one 2^15 space (the spawn cap is
    // Options::maxJSThreads, far below either half); the §D.1 TID-rebias
    // protocol (U-T12, below) PRESERVES the partition — retired TIDs are
    // recycled into per-range free lists, never across the split.
    static constexpr uint16_t carrierTIDBase = 0x4000;
    static bool isCarrierTID(uint16_t tid) { return tid >= carrierTIDBase && tid < notTTLTID; }

    // Installs the VMLite.h carrier-TID provider pair (setCarrierTIDHooks).
    // Idempotent; called by the GIL-off carrier registration path
    // (JSLock.cpp) before its first allocateCarrierTID(). U-T12: under
    // gilOffProcess a released carrier TID is RETIRED into the §D.1 rebias
    // pipeline below (Dev 10 lifted); GIL-on/flag-off the release hook stays
    // the v1 no-op (retired forever — Deviation 10 stands there, U19).
    JS_EXPORT_PRIVATE static void installCarrierTIDHooksIfNeeded();
    // The provider body (the hook target; public so the file-scope hook
    // function in ThreadManager.cpp can reach it). Takes m_lock; RELEASE_
    // ASSERTs on range exhaustion — carrier allocation happens at VM entry,
    // where there is no throw context (I17).
    uint16_t allocateCarrierTIDInternal();

    static uint16_t currentTID();
    JS_EXPORT_PRIVATE static bool isJSThreadCurrent(); // true iff spawned Thread

    // Allocates a TID and registers a new spawned ThreadState. Returns null
    // on TID exhaustion or when the live-thread cap is exceeded (the caller
    // throws RangeError). UNGIL U0b BACKSTOP (U-T6): under gilOffProcess
    // this VM-blind form ALWAYS returns null — it cannot prove the spawning
    // VM is the m_gilOff winner, and a loser-VM spawn must be refused
    // (U0b), not silently run GIL'd phase-1 semantics. Callers under
    // gilOffProcess must use the VM-aware overload; the spawn host call
    // (ThreadObject.cpp, outside U-T6's owned set) migrating to it is a
    // recorded integration obligation — until then gilOffProcess
    // OVER-refuses (winner spawns throw RangeError too), which is
    // fail-safe and changes nothing GIL-on/flag-off.
    RefPtr<ThreadState> allocateSpawnedThreadState();
    // UNGIL U0b (U-T6): the VM-aware overload — under gilOffProcess only
    // the m_gilOff VM may spawn; any other (loser) VM returns null so the
    // caller throws its RangeError (api 5.1 shape; the loser keeps the
    // GIL-on single-migrating-client + real m_lock protocol for
    // multi-embedder entry). Flag-off: identical to the unparameterized
    // form. The U0b refusal is enforced today even at the unmigrated
    // ThreadObject.cpp call site via the VM-blind overload's gilOffProcess
    // backstop above.
    RefPtr<ThreadState> allocateSpawnedThreadState(VM&);
    void unregisterThread(ThreadState&);

    // ========================================================================
    // UNGIL §D.1 TID rebias (ANNEXES D1 + D1R, BINDING; U-T12) — lifts Dev 10
    // under gilOffProcess. GIL-on/flag-off: every member below is inert and
    // TIDs stay retired forever (U19 keeps the old expectations).
    //
    // Protocol (two-phase vs §LK, r9 F2 — the conductor takes NO api lock and
    // TM::m_lock is NOT excepted from that prohibition):
    //
    //   PHASE 1 (mutator-side, PRE-STOP, under m_lock): dead TIDs accumulate
    //   in m_retiredSpawnedTIDs / m_retiredCarrierTIDs (unregisterThread /
    //   retireCarrierTID). Once the rebias trigger arms (>=75% range
    //   consumption — see maybeArmAndSealRebiasLocked for the recorded
    //   per-partition refinement), the next mutator pass under m_lock SEALS
    //   the retired sets into m_rebiasSnapshot (Idle -> Sealed, release
    //   store). A sealed snapshot arms the shared server's next conducted
    //   cycle as a FULL collection (Heap::shouldDoFullCollection probe).
    //
    //   PHASE 2 (conductor-side, IN-STOP, lock-free): inside the full
    //   shared-GC stop (heap §10 barrier, WSAC set — NOT a §A.3 stop, jit
    //   R1.h), the conductor reads the snapshot via
    //   rebiasSnapshotForConductor() (acquire), restamps every live
    //   butterfly tag and Structure::m_transitionThreadLocalTID holding a
    //   snapshotted TID to 0, fires fireTransitionThreadLocal on every such
    //   structure (D1R item 1: jettisons every DFG/FTL/IC body holding a
    //   baked tid<<48 immediate BEFORE reissue becomes possible), then flips
    //   Sealed -> Restamped (noteRebiasRestampComplete, release). The walk
    //   lives in Heap.cpp (conductSharedCollection).
    //
    //   PHASE 3 (mutator-side, POST-RESUME, under m_lock): the next TID
    //   allocation runs completeRebiasIfPendingLocked(): snapshot TIDs move
    //   to the per-range free lists (Restamped -> Idle) and the SD9
    //   RangeError exhaustion gate lifts AT THAT ALLOCATION — i.e. the
    //   release is ordered before the gate lift by construction, and no
    //   dead TID is reissuable before the in-stop restamp+fire completed.
    //
    // Soundness of late retires (D1 soundness paragraph): a TID retired
    // AFTER the seal goes to the retired sets, never the sealed snapshot —
    // it waits for the next cycle. Concurrent lazy-carrier creation in
    // OTHER VMs (TM is process-global; their threads are NOT stopped) only
    // ADDS live TIDs and cannot resurrect a snapshotted-dead TID: a dead
    // TID has no lite, no TLS map entry, and TM never reissues before the
    // post-resume release. An exiting thread's residual T5/A36 tail (which
    // runs after its TID was retired, un-gated) never installs new tagged
    // state: its last heap-access bracket is teardown-only finalization,
    // and the §10.4 access barrier orders any such bracket entirely before
    // or after the in-stop restamp.
    //
    // m_rebiasSnapshot ownership rotates with m_rebiasState: mutators write
    // it under m_lock only while Idle (seal) or Restamped (release+clear);
    // the conductor reads it lock-free only while Sealed. The acquire/
    // release pairs on m_rebiasState publish the vector across the
    // hand-offs; no state ever has two writers (Idle->Sealed and
    // Restamped->Idle are mutator-only under m_lock; Sealed->Restamped is
    // conductor-only, and at most one shared-GC conductor exists — not
    // merely per-heap via GCL: shared-server-ness itself is PROCESS-unique.
    // I13's s_stickySharedServer CAS (Heap.cpp; RELEASE_ASSERTed in
    // noteSharedServerSticky, the sole m_isSharedServer=true site) admits at
    // most one shared server per process, ever, and under gilOffProcess the
    // U0c winner heap took that CAS in its VM ctor — so the one heap whose
    // conductor can reach rebiasSnapshotForConductor()/
    // noteRebiasRestampComplete() IS the gilOff winner's heap (RELEASE_
    // ASSERTed at the consume site in Heap::conductSharedCollection). A
    // loser VM cannot grow a second server: gilOffProcess is OPTION-derived
    // and immutable for the process (VM::isGILOffProcess — it never "flips"
    // mid-life, so no GIL-on phase-1 spawn epoch can precede it), U0b spawn
    // refusal keeps loser clientSet() <= 1, and any path that tried
    // noteSharedServerSticky on a second heap fail-stops on the I13 CAS
    // assert rather than consuming a snapshot.
    //
    // RECORDED DEFERRAL (U-T12 verification arms; this amendment's owned
    // file set is {ThreadManager.h, ThreadManager.cpp, heap/Heap.cpp} —
    // JSTests/threads/** and Tools/** are NOT writable from it): the three
    // handout-listed U-T12 arms — (1) the spawn-storm corpus test (TID
    // exhaustion through SD9 RangeError -> rebias -> recovery, U18 shape),
    // (2) the two-VM TM-churn amplifier run (rebias in VM A while an
    // embedder lazily enters VM B, driving the retireCarrierTID and
    // conductTIDRebiasUnderSharedStop stall points), and (3) the ANNEX D1R
    // item-5 reissue/jettison arm (E4-specialized transition code vs a
    // dying thread's structure; exit, force rebias + TID reissue,
    // transition storm vs a foreign locked transitioner; instrumented I15
    // assert + assert the specialized CodeBlock was jettisoned during the
    // rebias stop) — land with the thread-ungil verification-ladder phase
    // against the RaceAmplifier::perturb hooks already planted here and in
    // Heap.cpp. They are still U-T12 deliverables (gate ledger), not
    // dropped. Arm (2) is ADDITIONALLY blocked on the JSThreadsSafepoint
    // R2-4 tripwire obligation recorded in Heap.cpp's rebias banner: until
    // that lands, the D1R fire path RELEASE_ASSERTs in any multi-VM
    // gilOffProcess process. Arms (1) and (3) are single-VM and unblocked.
    // ========================================================================

    enum class RebiasState : uint8_t { Idle, Sealed, Restamped };

    // Mutator-side: retire a released A36 carrier TID into the rebias
    // pipeline (gilOffProcess; otherwise a no-op — Dev 10 stands). Takes
    // m_lock; every release-hook call site (JSLock.cpp carrier teardown,
    // VM.cpp A36 walk tail) runs with NO lock held, so the rank-1 acquire is
    // §LK-legal even when the exit tail runs mid-stop (exits are un-gated).
    void retireCarrierTID(uint16_t);

    // Conductor-side, lock-free: the sealed dead-TID snapshot if one is
    // pending, else null. Read ONLY inside the full shared-GC stop.
    const Vector<uint16_t>* rebiasSnapshotForConductor();
    // Conductor-side, lock-free: Sealed -> Restamped. Call ONLY after every
    // snapshot TID's instance tags + structure transition TIDs were
    // restamped AND the D1R fires ran, still inside the stop.
    void noteRebiasRestampComplete();

    // Trigger probe for Heap::shouldDoFullCollection (D1: a sealed snapshot
    // "arms the next full collection"). Lock-free.
    bool rebiasSnapshotIsSealed() const
    {
        return m_rebiasState.load(std::memory_order_acquire) == static_cast<uint8_t>(RebiasState::Sealed);
    }

    uint64_t allocateThreadLocalKey() { return ++m_nextThreadLocalKey; }

    // Thread.restrict affinity table (SPEC-api 5.7.2).
    enum class Affinity : uint8_t { None, Owner, Foreign };
    Affinity objectAffinity(JSObject*);
    // Returns true iff the CALLER is the recorded owner after the call (new
    // claim, idempotent re-restrict, or stale-entry replacement). False =
    // another thread's claim won the m_affinityLock race between the
    // caller's affinity step (0) and this insert — GIL-off the two sections
    // are not atomic (MC-HAND S6, docs/threads/cve/map-MC-HAND.md), so the
    // lose arm must surface Foreign for the frozen SPEC-api 4.1
    // "re-restrict from another thread => ConcurrentAccessError" contract.
    [[nodiscard]] bool restrictObject(JSObject*, ThreadState& owner);
    // Mandatory 5.7.2 fast path: one relaxed load, zero => no restricted
    // objects anywhere => no lock, no table probe.
    bool anyRestrictedObjects() const { return m_restrictedCount.load(std::memory_order_relaxed); }
    RefPtr<ThreadState> restrictionOwner(JSObject*);
    // Called by the per-insert Weak finalizer (5.7.2) when a restricted cell
    // dies; removes the entry and decrements the restricted-object count.
    // expectedEntry is the finalizing Weak's context (the ThreadAffinityEntry
    // it was created for): the entry is removed only if it is still THAT
    // entry, so a finalizer that runs late — after the dead cell's address
    // was recycled and the new object at that address was itself restricted
    // (which replaces the table entry) — cannot evict the new object's
    // restriction (deleted-slot-reuse hazard, cf. THREAD.md regime 3).
    void pruneRestrictedObject(JSCell*, void* expectedEntry);

    // Frozen signature (SPEC-api 7); diag/future N-mutator. NOT a GC root
    // source.
    void forEachThreadState(const Invocable<void(ThreadState&)> auto& functor)
    {
        Locker locker { m_lock };
        for (auto& entry : m_threads)
            functor(entry.value.get());
    }

public:
    ThreadManager() = default; // for LazyNeverDestroyed only; use singleton()

private:
    // The shared allocation body behind both allocateSpawnedThreadState
    // overloads (cap check, TID allocation, m_threads registration). The
    // U0b gilOffProcess gating lives in the public overloads only.
    RefPtr<ThreadState> allocateSpawnedThreadStateInternal();

    // §D.1 phase-1 arming + seal. Requires m_lock; gilOffProcess-only
    // callers (no-ops otherwise are enforced at the call sites).
    void maybeArmAndSealRebiasLocked() WTF_REQUIRES_LOCK(m_lock);
    // §D.1 phase-3 release: Restamped -> Idle, snapshot TIDs to the
    // per-range free lists. The SD9 gate-lift site (run at the top of every
    // TID allocation under m_lock, post-resume on a mutator).
    void completeRebiasIfPendingLocked() WTF_REQUIRES_LOCK(m_lock);

    Lock m_lock; // rank 1 (SPEC-api 5.9)
    UncheckedKeyHashMap<uint16_t, Ref<ThreadState>> m_threads WTF_GUARDED_BY_LOCK(m_lock); // spawned only
    Deque<uint16_t> m_freeTIDs WTF_GUARDED_BY_LOCK(m_lock); // spawned-range recycle list; fed ONLY by §D.1 rebias phase 3 (U-T12; empty GIL-on/flag-off — Dev 10)
    Deque<uint16_t> m_freeCarrierTIDs WTF_GUARDED_BY_LOCK(m_lock); // carrier-range recycle list, same protocol (the U-T6 partition is preserved)
    uint16_t m_nextTID WTF_GUARDED_BY_LOCK(m_lock) { 1 }; // 0 = main, 0x7fff = notTTLTID; spawned range tops out at carrierTIDBase (U-T6 split)
    uint16_t m_nextCarrierTID WTF_GUARDED_BY_LOCK(m_lock) { carrierTIDBase }; // A36 carrier range [carrierTIDBase, notTTLTID)

    // §D.1 retired-TID sets (dead: no thread, no lite, no TLS entry). Fed by
    // unregisterThread / retireCarrierTID under gilOffProcess; drained into
    // m_rebiasSnapshot at seal. GIL-on/flag-off: never appended to.
    Vector<uint16_t> m_retiredSpawnedTIDs WTF_GUARDED_BY_LOCK(m_lock);
    Vector<uint16_t> m_retiredCarrierTIDs WTF_GUARDED_BY_LOCK(m_lock);
    // The sealed dead-TID snapshot (see the class banner for the ownership
    // rotation; deliberately NOT m_lock-annotated — the conductor reads it
    // lock-free while Sealed, licensed by the m_rebiasState acquire/release
    // hand-off).
    Vector<uint16_t> m_rebiasSnapshot;
    std::atomic<uint8_t> m_rebiasState { static_cast<uint8_t>(RebiasState::Idle) };
    // Sticky once set (range consumption is monotone): from then on every
    // retire/allocate pass under m_lock may seal a fresh snapshot — the
    // continuous-recycling regime past the 75% threshold.
    std::atomic<bool> m_rebiasArmed { false };

    std::atomic<uint64_t> m_nextThreadLocalKey { 0 };

    Lock m_affinityLock; // rank 2 (never held together with the PWT lock, 5.9)
    UncheckedKeyHashMap<JSCell*, std::unique_ptr<ThreadAffinityEntry>> m_affinityTable WTF_GUARDED_BY_LOCK(m_affinityLock);
    std::atomic<size_t> m_restrictedCount { 0 };
};

// The sole current-thread ThreadState lookup (SPEC-api 5.1). Never goes
// through ThreadManager::m_threads (tid-0 collision with embedder threads).
ThreadState* currentThreadStateIfExists();
JS_EXPORT_PRIVATE Ref<ThreadState> ensureCurrentThreadState();
void setCurrentThreadState(RefPtr<ThreadState>&&);

// 5.7 choke point; returns true if the access is allowed, otherwise throws
// ConcurrentAccessError and returns false. Callers gate on
// isUncacheableDictionary() first.
JS_EXPORT_PRIVATE bool threadRestrictCheck(JSGlobalObject*, JSObject*);

// ============================================================================
// UNGIL ANNEX EXIT1.9 / §B.1-2 (U-T6) — per-thread GCClient lifecycle + the
// ~VM completion-fence condition.
//
// PLACEMENT DEVIATION (recorded; spec letter vs file ownership): EXIT1.9
// words the condition as "VMLiteRegistry gains one WTF::Condition beside
// lock; unregisterLite — already under the lock — notifyAll()s it after
// removing the lite". VMLiteRegistry lives in VMLiteShared.h/.cpp, which is
// OUTSIDE U-T6's owned-file set, so:
//   - the Condition is the process-lifetime NeverDestroyed singleton below,
//     paired exclusively with VMLiteRegistry::singleton().lock (it is THE
//     EXIT1.9 vmTeardownCondition; relocating it into VMLiteRegistry is a
//     mechanical follow-up once VMLiteShared.h is editable);
//   - the notify lives in the unregisterVMLiteAndNotifyTeardown() wrapper,
//     which every U-T6-owned GIL-off teardown path uses as its sole physical
//     removal primitive (the r31 U20 "every physical removal is the
//     notifying function" rule, discharged at the wrapper).
// SOUNDNESS of notifying AFTER unregisterLite's internal lock hold drops
// (instead of inside it): both EXIT1.9 waiters are predicate loops under
// VMLiteRegistry::lock, and WTF::Condition::wait enqueues the waiter in the
// parking lot BEFORE releasing the lock — a remover can only mutate the
// registry after acquiring the lock, i.e. after any in-flight waiter that
// observed the pre-removal state is already queued, so its post-unlock
// notifyAll cannot be missed. GIL-on spawned teardown (ThreadObject.cpp)
// keeps calling plain unregisterLite — the fence is armed only for m_gilOff
// VMs, whose ~VM serializes against nothing GIL-on.
// ============================================================================

// The EXIT1.9 vmTeardownCondition: signaled by every GIL-off unregisterLite
// (via the wrapper below) AND by the ~VM A36 walk's COLLECTED->DETACHED
// flips (VM.cpp). Waiters: the EXIT1.9 step-(3) ~VM wait and the r31
// carrier-TLS-death COLLECTED wait (JSLock.cpp) — both predicate loops
// under VMLiteRegistry::singleton().lock, so cross-wakeups are benign.
JS_EXPORT_PRIVATE Condition& vmLiteTeardownCondition();

// unregisterLite + notifyAll (see the banner). The sole physical-removal
// call on U-T6's GIL-off teardown paths (EXIT1.3 spawned T5 + carrier
// TLS-death, the A36 collection, ~VM's m_mainVMLite removal).
JS_EXPORT_PRIVATE void unregisterVMLiteAndNotifyTeardown(VMLite&);

// §B.1 spawn-side client creation + ACT (U-T6; the GIL-off threadMain /
// E2A integration point — ThreadObject.cpp is outside U-T6's owned set, so
// U-T9's drain-loop rewrite MUST call this after lite registration +
// setCurrent + TID-tag init and BEFORE the first JSLockHolder/allocation).
// Creates the thread's own GCClient::Heap against vm.heap, stamps
// lite.clientHeap write-once under the registry lock (EXIT1.4(b)
// release-publish), then runs attachCurrentThread() (heap I4(a)-(c):
// §10A.1 slot stamp, conservative-scan registration, §A.3.2b/§A.3.8-gated
// access acquisition).
JS_EXPORT_PRIVATE void attachSpawnedThreadGCClient(VM&, VMLite&);

// EXIT1.3 spawned T5 tail as AMENDED r31 (U-T6): access release (seq_cst
// RHA) -> TEARDOWN mark (under the registry lock) -> DCT -> destroy the
// GCClient::Heap -> unregisterLite (notifying wrapper) LAST. The caller
// performed the Strong clears / E2A close first and frees the lite AFTER
// this returns (EXIT1.9 residual-tail rule: the lite free and its M12
// default-queue removal run outside the registry lock). Exit stays
// UN-GATED: no stop-bit poll, no park point.
JS_EXPORT_PRIVATE void tearDownSpawnedThreadForExit(VM&, VMLite&);

// ============================================================================
// UNGIL §E.2/§E.3/§E.5 — the per-thread event loop (ANNEXES E2A + E3, BINDING;
// U-T9). GIL-off only; nothing below is reachable GIL-on/flag-off.
//
// INTEGRATION POINT (recorded; threadMain lives in ThreadObject.cpp, outside
// U-T9's owned set): the GIL-off threadMain shape is
//
//   register lite + setCurrent + TID-tag init
//   attachSpawnedThreadGCClient(vm, lite)        (U-T6, §B.1)
//   openThreadInbox(state)                        (E.1: BEFORE fn, post-attach)
//   run fn under a JSLockHolder token (as landed)
//   runSpawnedThreadDrainLoopAndClose(vm, lite, state, phase, resultValue-already-stored)
//     -> drains/waits per E2A, then runs the close block (§E.5 shares it),
//        the F5 completion protocol against state, and the EXIT1.3 teardown
//        via tearDownSpawnedThreadForExit. The caller frees the lite after.
//
// Thread completes — and join/asyncJoin settle (F5) — ONLY at close (U7),
// not at fn-return (SD1). Park sites inside fn do NOT service the task queue.
// ============================================================================

// §E.1: flips inboxOpen true, exactly once, under inboxLock. Must run on the
// owning spawned thread, after §B.1 attach, before fn. Asserts spawned and
// not-yet-open.
JS_EXPORT_PRIVATE void openThreadInbox(ThreadState&);

// §C.3/§E.7.5 registration helper: appends a finite-timeout deadline to the
// CURRENT spawned TS's waitDeadlines under inboxLock (asserts spawned+OPEN;
// the §C.3 site calls this only when the registrant TS is spawned and the
// timeout is finite — r12 F3). Signals runLoopCondition so an idle E2A loop
// re-computes its sleep bound.
JS_EXPORT_PRIVATE void addThreadWaitDeadline(ThreadState&, MonotonicTime deadline, Function<bool()>&& tryDequeue, Function<void()>&& settleTimedOut);

// ============================================================================
// UNGIL §E.7 (ANNEX E7 + r17 F3 + r18 F2; U-T9) — cross-thread DWT work under
// embedder hooks. Declared here (this header is U-T9-owned; DeferredWorkTimer.h
// is not — recorded placement deviation), defined in DeferredWorkTimer.cpp.
// ============================================================================

// The FOURTH embedder hook, onCrossThreadWorkEnqueued (REQUIRED with the
// other three under gilOff — boot-checked at the first internal-arm enqueue;
// absent => the vm.runLoop().dispatch fallback drives the work, so a
// parked-main settle still cannot deadlock). Contract: invoked with NO JSC
// lock held; must not reenter JSC; never runs JS — it only wakes the
// embedder's loop so a carrier reaches a §F.1 drain point.
JS_EXPORT_PRIVATE void jsThreadsSetOnCrossThreadWorkEnqueued(VM&, Function<void()>&&);

// §F.1 carrier drain point: flushes the m_pendingLock-guarded handoff queue
// into the DWT task queue and runs it under the calling carrier's token
// (incl. the E.4(b) retire + m_promise clear inside each task). Requires the
// caller to hold the VM's token on a main/embedder carrier. No-op GIL-on/
// flag-off and when the queue is empty.
JS_EXPORT_PRIVATE void jsThreadsFlushCrossThreadDeferredWork(VM&);

// ~VM purge (called by ~VM under the destroying thread's token, before the
// DWT dies): drops queued cross-thread work and the internal-arm marks.
JS_EXPORT_PRIVATE void jsThreadsPurgeCrossThreadDeferredWorkAtVMDestruction(VM&);

// ANNEX E2A: the post-fn drain loop + close + F5 completion + EXIT1.3
// teardown, verbatim against the annex pseudocode (see the .cpp). `phase` is
// the fn outcome (Finished/Failed); a termination trap observed by the loop
// (or already pending from fn) takes the §E.5 path through the SAME close
// block with Phase::Failed. PRECONDITIONS: caller is the owning spawned
// thread, holds NO lock and NO JSLock token (the loop brackets its own token
// per task), state->result was already stored (F1), lite.clientHeap is the
// thread's attached client.
JS_EXPORT_PRIVATE void runSpawnedThreadDrainLoopAndClose(VM&, VMLite&, ThreadState&, ThreadState::Phase);

} // namespace JSC
