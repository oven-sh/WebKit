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

#include "JSDestructibleObject.h"
#include "ThreadManager.h"

namespace JSC {

class CallFrame;
class EntryFrame;
class ExceptionScope;

// Native state backing a JS Lock (SPEC-api 5.3). Non-recursive; sync holds
// are identified by WTF::Thread*, async holds by an AsyncTicket.
class NativeLockState final : public ThreadSafeRefCounted<NativeLockState> {
public:
    static Ref<NativeLockState> create() { return adoptRef(*new NativeLockState()); }

    WTF::Lock m_lock; // rank 4 leaf
    std::atomic<WTF::Thread*> m_holder { nullptr }; // sync holder; compared, never dereferenced
    std::atomic<bool> m_asyncHeld { false };

    Lock m_queueLock; // rank 3 (protects the next three)
    RefPtr<AsyncTicket> m_asyncHolder WTF_GUARDED_BY_LOCK(m_queueLock);
    Deque<Ref<AsyncTicket>> m_asyncWaiters WTF_GUARDED_BY_LOCK(m_queueLock);
    bool m_pumpPending WTF_GUARDED_BY_LOCK(m_queueLock) { false };

    // Identity of the thread currently running an asyncHold(fn) delivered
    // function for the LIVE async grant (landed deviation D10,
    // docs/threads/INTEGRATE-api.md). Async holds are invisible to m_holder;
    // without this, a sync lock.hold(g) on the same lock from inside the
    // delivered fn passes the heldByCurrentThread() recursion check, fails
    // tryLock (m_lock is held by the grant), and parks in m_lock — whose
    // only release point is this very fn's post-fn epilogue (E):
    // a guaranteed self-deadlock from straightforward user code. Set by
    // settleLockGrant's with-fn settle task around JSC::call(fn); cleared by
    // asyncReleaseInternal (covers E and cond.asyncWait's 4.3(b)
    // consumption, after which the lock is genuinely free and a sync hold
    // from the rest of fn is again legal). Compared, never dereferenced.
    std::atomic<WTF::Thread*> m_asyncGrantRunner { nullptr };

    bool heldByCurrentThread() const { return m_holder.load(std::memory_order_relaxed) == &Thread::currentSingleton(); }

    // True iff the calling thread is inside an asyncHold(fn) delivered fn
    // whose grant is still live (see m_asyncGrantRunner). lock.hold treats
    // this as "held by current thread" and throws the 4.2 "Lock is not
    // recursive" Error. lockProtoFuncAsyncHold deliberately does NOT use
    // this: per the frozen 4.2 text, async-held is not recursive for
    // asyncHold — callers queue, and the post-fn implicit release (E) lets
    // the queued ticket settle later, so no deadlock arises there.
    bool asyncGrantRunByCurrentThread() const
    {
        return m_asyncGrantRunner.load(std::memory_order_relaxed) == &Thread::currentSingleton()
            && m_asyncHeld.load(std::memory_order_acquire);
    }

    // Release the sync hold the current thread owns (no pump).
    void releaseSyncHold()
    {
        m_holder.store(nullptr, std::memory_order_relaxed);
        m_lock.unlock();
    }

    // R: after any m_lock release, schedule the async-acquirer pump if needed.
    void releasePump(VM&);

    // A-failure path: enqueue an async acquirer FIFO and schedule the pump.
    void enqueueAsyncAcquirer(Ref<AsyncTicket>&&, VM&);

    // Async release: clear the live async hold and unlock (callable from any
    // thread). Caller must have consumed the ticket already.
    void asyncReleaseInternal(AsyncTicket&, VM&);

    // P: run-loop pump task (SPEC-api 5.5a). GIL-INDEPENDENT: clears
    // pump-pending FIRST (under m_queueLock), then tryLocks; on failure does
    // nothing (the holder's release runs R with pump-pending false and
    // reschedules).
    void pump();

private:
    NativeLockState() = default;

    void schedPumpLocked(VM&) WTF_REQUIRES_LOCK(m_queueLock);
};

class JSLockObject final : public JSDestructibleObject {
public:
    using Base = JSDestructibleObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags;
    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm)
    {
        return &vm.destructibleObjectSpace();
    }

    static JSLockObject* create(VM&, Structure*);
    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
    {
        return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
    }

    DECLARE_EXPORT_INFO;

    NativeLockState& lockState() { return m_state.get(); }

private:
    JSLockObject(VM&, Structure*);

    Ref<NativeLockState> m_state;
};

// Settles a granted async-acquisition ticket (resolve with a release
// function, or run the held function then implicitly release). Shared with
// ConditionObject.cpp for asyncWait re-acquisition.
void settleLockGrant(NativeLockState&, AsyncTicket&);

// D9 park-poll predicate, shared by every GIL-dropped park site. A parked
// thread can never observe vm.hasTerminationRequest() alone: that flag is
// only set by VMTraps::handleTraps(), which runs on a mutator EXECUTING JS
// under the GIL — watchdog firing (NeedWatchdogCheck), VM::notifyNeedTermination,
// and the shell's SIGTERM path only set trap BITS. Polling the flag at a
// park site therefore never observes a termination raised while everyone is
// parked (the watchdog-hang / SIGTERM-immune failure mode). Poll the trap
// bits directly. NeedWatchdogCheck is deliberately treated as terminal at a
// park: Watchdog::shouldTerminate()'s CPU-deadline deferral is meaningless
// for a parked thread (it burns ~no CPU, so deferral == unkillable park),
// the wall timer only fires at/after the deadline, and shouldTerminate()
// itself asserts the API lock so it cannot be consulted from a dropped
// section. Recorded as a D9 amendment in docs/threads/INTEGRATE-api.md.
JS_EXPORT_PRIVATE bool jsThreadParkTerminationRequested(VM&);

// Phase-1 GIL stub fairness primitive: release the GIL, yield, and
// reacquire it, WITHOUT JSLock::DropAllLocks. DropAllLocks participates in
// JSLock's strict-LIFO m_lockDropDepth protocol (JSLock::grabAllLocks spins
// until all deeper droppers have unwound). A thread that repeatedly
// drops/regrabs via droppers therefore only ever leaves the GIL free while
// the drop depth is ABOVE the depths recorded by already-parked waiters, so
// those waiters can never finish unwinding: livelock, observed live in
// JSTests/threads/sync/condition-notify-all-shared-lock.js. This helper
// bypasses the depth bookkeeping (legal for LIFO purposes: the
// release/reacquire pair is fully contained in one host-function frame, so
// per-thread LIFO is trivially preserved) while mirroring grabAllLocks'
// stack-pointer restoration so conservative scan still covers the original
// VM entry frames. NOTE: routes through GILDroppedSection, so it currently
// shares its D11 microtask-drain-at-unlock deviation — see constraint (3)
// on the GILDroppedSection comment below; fixed by the same 9.2-9 hunk.
void jsThreadGILHandoffYield(VM&);

// Phase-1 completion-ordering primitives (the 5.2 yield-point contract vs
// the SPEC-api 4.6.1 completion drain; regression test
// JSTests/threads/api/park-no-microtask-drain.js): a process-global count of
// threads inside a "park -> resumed" window of the GIL stub's
// lock-reacquisition parks — contended sync lock.hold parkers (counted from
// park entry) and sync cond.wait waiters (counted from the notify()-side
// Notified flip, which is what makes the window visible BEFORE the notifier
// can reach its completion sequence). A completing thread must not run its
// 4.6.1 shared-queue microtask drain while such a window is open: the woken
// parker would observe the drain inside its blocking host call, the exact
// D11 violation the test asserts against. The completion sequence calls
// jsThreadYieldForPendingParkResumptions, which yields the GIL until the
// count drains to zero — bounded by a progress-reset deadline (see the
// definition) so a parker pinned behind an unrelated long-lived holder can
// only delay, never deadlock, completion. Process-global is correct for
// phase 1: a single shared VM (5.2).
void jsThreadNoteParkResumptionPending();
void jsThreadNoteParkResumptionDone();
void jsThreadYieldForPendingParkResumptions(VM&);

// Saves vm.topCallFrame/vm.topEntryFrame at construction and restores them at
// destruction. Required around every place a JS thread parks with the GIL
// dropped (cond.wait, lock.hold blocking, join, Atomics.wait) once GIL
// handoffs are not strictly LIFO: those two fields are VM-global and are
// normally kept consistent by the prev-pointer chain in VMEntryRecords, which
// assumes threads leave the VM in reverse order of entry. A resuming thread
// must put ITS frames back or the next SlowPathFrameTracer asserts
// (callFrame < vm.topEntryFrame) against a foreign thread's stack. Mirrors
// the SPEC-vmstate §0 JSLock hand-off treatment of lastStackTop /
// stackPointerAtVMEntry, scoped to the phase-1 stub's park sites.
class GILParkSavedExecutionState {
    WTF_MAKE_NONCOPYABLE(GILParkSavedExecutionState);
public:
    JS_EXPORT_PRIVATE explicit GILParkSavedExecutionState(VM&);
    JS_EXPORT_PRIVATE ~GILParkSavedExecutionState();

    // A freshly spawned JS thread inherits whatever execution state the
    // previous GIL holder left in the VM (possibly pointers into a now-dead
    // thread's stack, e.g. the EXCEPTION_SCOPE_VERIFICATION scope chain).
    // Call right after the thread's first GIL acquisition to start from a
    // clean slate; every park site then restores this thread's own state.
    JS_EXPORT_PRIVATE static void resetForFreshThread(VM&);

private:
    VM& m_vm;
    // UNGIL §J.2 (U-T11): the save/restore is dead GIL-off — Group-3
    // execution state is per-lite (§A.1.3), so there is nothing another
    // mutator could clobber while this thread parks. Keyed on vm.gilOff()
    // ALONE (not gilOff && spawned, unlike GILDroppedSection below): for a
    // gilOff MAIN carrier the raw VM-block words are ITS live per-lite
    // storage and no other GIL-off thread writes them (spawned/embedder
    // carriers write their own lites — mainThreadCarrierMap /
    // threadLocalCarrierMap are per-thread, JSLock.cpp), and for spawned
    // threads they are inert spare storage shared by every concurrently
    // parking thread — a restore through them would be a data race. The
    // ctor/dtor check the premise under ASSERT_ENABLED instead of assuming
    // it. Asserts move from the mutex meaning to the token meaning GIL-off
    // (JSLock.cpp IU row 28).
    bool m_gilOff { false };
    CallFrame* m_topCallFrame { nullptr };
    EntryFrame* m_topEntryFrame { nullptr };
#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    ExceptionScope* m_topExceptionScope { nullptr };
    bool m_needExceptionCheck { false };
#endif
};

// Depth-free replacement for JSLock::DropAllLocks at the phase-1 stub's
// park sites (join, cond.wait, Atomics.wait, lock.hold blocking).
// DropAllLocks enrolls the dropper in JSLock's strict-LIFO m_lockDropDepth
// protocol: grabAllLocks spins until every deeper dropper has unwound. With
// N threads parking and waking in arbitrary (non-LIFO) order, a woken
// shallow dropper spins forever while the deeper dropper it implicitly
// waits on can only be woken by JS the spinner was about to run: livelock
// (observed in join chains, JSTests/threads/lifecycle/join-semantics.js,
// and in condition-notify-all-shared-lock.js). This scope releases the GIL
// completely at construction and reacquires it at destruction with no depth
// bookkeeping; per-thread LIFO is trivially preserved by C++ scoping, and
// the stack-entry bookkeeping (stackPointerAtVMEntry, lastStackTop,
// topCallFrame, topEntryFrame) is saved in locals and restored exactly as
// grabAllLocks would.
//
// LANDED DEVIATION from frozen SPEC-api 5.2/5.3/5.4/F4/F5, which name
// JSLock::DropAllLocks (DAL) as the park-site GIL-release mechanism
// (recorded in docs/threads/INTEGRATE-api.md "Landed deviations"; escalated
// there for a rev-15 amendment naming this class normatively). Three
// constraints follow until that amendment lands:
// (1) The saved/restored state set above is a parallel implementation of
//     JSLock::willReleaseLock/grabAllLocks invariants and MUST be kept in
//     sync with them by hand (the vmstate workstream also touches that
//     surface, SPEC-vmstate §0).
// (2) Interaction with genuine DropAllLocks users on the same JSLock:
//     threads parked here hold no m_lockDropDepth slot, so they are
//     invisible to DAL's depth bookkeeping. That is SOUND for grabAllLocks
//     (the JSLock is simply free or held; depth ordering only constrains
//     DAL droppers among themselves), but phase 1 assumes the embedder does
//     not interleave DropAllLocks on the shared VM's JSLock while spawned
//     Threads are live — the jsc shell does not, and Bun must not until the
//     rev-15 analysis covers it.
// (3) FIXED by JSLock::unlockAllForThreadParking (the applied INTEGRATE-api.md
//     9.2-9 hunk): the constructor releases the lock through that member,
//     which bumps and restores m_lockDropDepth while m_lock is still held —
//     willReleaseLock()'s VM::drainMicrotasks() is suppressed (no user JS
//     runs inside the parking host call; D11 closed), every other
//     willReleaseLock side effect matches dropAllLocks(), and the transient
//     depth never escapes into the DAL protocol, so the livelock fix is
//     preserved. Regression test: JSTests/threads/api/park-no-microtask-drain.js.
// UNGIL §J.3 spawned arm (U-T11): pImpl wrapping the §F.4/DAL2 heap-access
// bracket (JSLock::DropAllLocks, a nested type — not forward-declarable
// here). Defined in LockObject.cpp; GILDroppedSection's out-of-line dtor
// sees the complete type.
struct GILDroppedSectionSpawnedArm;

class GILDroppedSection {
    WTF_MAKE_NONCOPYABLE(GILDroppedSection);
public:
    JS_EXPORT_PRIVATE explicit GILDroppedSection(VM&);
    JS_EXPORT_PRIVATE ~GILDroppedSection();

private:
    VM& m_vm;
    GILParkSavedExecutionState m_savedExecutionState;
    void* m_stackPointerAtVMEntry;
    unsigned m_lockCount { 0 };
    // Non-null exactly when the §J.3 GIL-off-by-caller split took the
    // spawned arm (LockObject.cpp ctor); the dtor early-returns on it.
    std::unique_ptr<GILDroppedSectionSpawnedArm> m_spawnedArm;
};

} // namespace JSC
