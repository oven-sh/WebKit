/*
 * Copyright (C) 2012-2021 Apple Inc. All rights reserved.
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
#include "Watchpoint.h"

#include "AdaptiveInferredPropertyValueWatchpointBase.h"
#include "CachedSpecialPropertyAdaptiveStructureWatchpoint.h"
#include "ChainedWatchpoint.h"
#include "CodeBlockJettisoningWatchpoint.h"
#include "DFGAdaptiveStructureWatchpoint.h"
#include "FunctionRareData.h"
#include "HeapInlines.h"
#include "JSThreadsSafepoint.h"
#include "LLIntPrototypeLoadAdaptiveStructureWatchpoint.h"
#include "ObjectAdaptiveStructureWatchpoint.h"
#include "PropertyInlineCacheClearingWatchpoint.h"
#include "StructureRareDataInlines.h"
#include "VM.h"
#include <atomic>
#include <wtf/Lock.h>
#include <wtf/Locker.h>

namespace JSC {

DEFINE_ALLOCATOR_WITH_HEAP_IDENTIFIER(Watchpoint);
DEFINE_ALLOCATOR_WITH_HEAP_IDENTIFIER(WatchpointSet);

Lock g_watchpointMembershipLock;

namespace {

// AB18-G: flag-on-only RAII for g_watchpointMembershipLock (see the
// declaration comment in Watchpoint.h). Flag-off this is a single
// predictable branch and no atomic.
class MembershipLocker {
    WTF_MAKE_NONCOPYABLE(MembershipLocker);
public:
    ALWAYS_INLINE MembershipLocker()
    {
        if (Options::useJSThreads()) [[unlikely]] {
            g_watchpointMembershipLock.lock();
            m_locked = true;
        }
    }
    ALWAYS_INLINE ~MembershipLocker()
    {
        if (m_locked) [[unlikely]]
            g_watchpointMembershipLock.unlock();
    }
private:
    bool m_locked { false };
};

} // anonymous namespace

StringFireDetail::StringFireDetail(ClangVTableWorkaroundTag)
    : m_string(nullptr)
{
}

void StringFireDetail::dump(PrintStream& out) const
{
    out.print(m_string);
}

template<typename Func>
inline void Watchpoint::runWithDowncast(const Func& func)
{
    switch (m_type) {
#define JSC_DEFINE_WATCHPOINT_DISPATCH(type, cast) \
    case Type::type: \
        func(static_cast<cast*>(this)); \
        break;
    JSC_WATCHPOINT_TYPES(JSC_DEFINE_WATCHPOINT_DISPATCH)
#undef JSC_DEFINE_WATCHPOINT_DISPATCH
    }
}

void Watchpoint::operator delete(Watchpoint* watchpoint, std::destroying_delete_t)
{
    watchpoint->runWithDowncast([](auto* derived) {
        std::destroy_at(derived);
        std::decay_t<decltype(*derived)>::freeAfterDestruction(derived);
    });
}

Watchpoint::~Watchpoint()
{
    // AB18-G: the unlink must be serialized against concurrent membership
    // mutation of the same set from other mutators (e.g. another thread's
    // WatchpointSet::add on a SharedJITStubSet-shared stub's set while a
    // retire path destroys a displaced handler's clearing watchpoint, or
    // lazy-sweep ~CodeBlock destruction on a live mutator). The check must
    // run under the lock too: isOnList() reads the node links the racing
    // unlinks mutate.
    MembershipLocker locker;
    if (isOnList()) {
        // This will happen if we get destroyed before the set fires. That's totally a valid
        // possibility. For example:
        //
        // CodeBlock has a Watchpoint on transition from structure S1. The transition never
        // happens, but the CodeBlock gets destroyed because of GC.
        remove();
    }
}

void Watchpoint::fire(VM& vm, const FireDetail& detail)
{
    RELEASE_ASSERT(!isOnList());
    runWithDowncast([&](auto* derived) {
        derived->fireInternal(vm, detail);
    });
}

WatchpointSet::WatchpointSet(WatchpointState state, WatchpointSetClassification classification)
{
    // TSAN wave 5 (triage 12.6, REOPENED family 9): initialize via relaxed
    // STORES, not the Atomic value constructor — the value constructor is a
    // plain (non-atomic) store, and when this set is the fat set allocated by
    // InlineWatchpointSet::inflateSlow it becomes reachable to lock-free
    // compiler-thread readers (state()/isStillValid() through the thin/fat
    // word) the moment the release CAS publishes the pointer; those readers'
    // accesses are atomic, so these construction writes must be too. Ordering
    // against the publish is the release CAS on the writer side plus the
    // consume-ordered fat-pointer read on the reader side
    // (InlineWatchpointSet::consumeFat); relaxed is sufficient here. Relaxed
    // byte stores compile to plain byte stores: flag-off codegen unchanged.
    m_state.storeRelaxed(state);
    m_setIsNotEmpty.storeRelaxed(false);
    m_invalidatesCode.storeRelaxed(classification == WatchpointSetClassification::InvalidatesCode);
    // TSAN r11 (reports 14/15/25/26/27/28): publication choke point for the
    // consume-published fresh set — pairs with the HAPPENS_AFTER in state()
    // and InferredValueWatchpointSet::inferredValue(). The real edge is the
    // release CAS (inflateSlow) / fence-before-pointer-publish on the owner
    // side, which TSAN cannot model; the annotation records "construction
    // happens-before any cross-thread probe", which is trivially true (the
    // probe needs the published pointer). No-op outside TSAN.
    TSAN_ANNOTATE_HAPPENS_BEFORE(this);
}

WatchpointSet::~WatchpointSet()
{
    // FIXME(rdar://165379969): This is here to silence a RefcountDebugger ASSERT. But the
    // ASSERT is correct and our code is incorrect!
    refCountDebugger().willDelete();

    // Remove all watchpoints, so that they don't try to remove themselves. Note that we
    // don't fire watchpoints on deletion. We assume that any code that is interested in
    // watchpoints already also separately has a mechanism to make sure that the code is
    // either keeping the watchpoint set's owner alive, or does some weak reference thing.
    //
    // AB18-G: this destructor can run during lazy sweep on a LIVE mutator
    // (AB18-C) while another mutator destroys one of the member watchpoints
    // (~Watchpoint -> remove()), so the drain takes the membership lock.
    MembershipLocker locker;
    while (!m_set.isEmpty())
        m_set.begin()->remove();
}

bool WatchpointSet::add(Watchpoint* watchpoint)
{
    ASSERT(!isCompilationThread());
    ASSERT(Options::useJSThreads() || state() != IsInvalidated);
    if (!watchpoint)
        return true;
    // AB18-G: flag-on, installs reach the same set from N mutators holding
    // only per-CodeBlock locks (shared-stub watchpointSets via
    // SharedJITStubSet reuse; per-Structure transition sets on the shared
    // object model). Serialize the link against concurrent add/remove.
    MembershipLocker locker;
    if (Options::useJSThreads()) [[unlikely]] {
        // Fires flip m_state outside this lock (the deferred fireAllSlow's
        // claim, a Class-B fireAllNow, invalidate() on a clear set) and then
        // take or drain the members under it, so the state is settled first:
        // the set is armed by CAS, never by a store that could cover an
        // IsInvalidated that landed after a check, and a watchpoint is linked
        // only once the set is known to be IsWatched. A fire that claims the
        // set after that finds the watchpoint in m_set and fires it.
        WatchpointState state = m_state.loadRelaxed();
        while (state != IsWatched) {
            if (state == IsInvalidated)
                return false;
            state = m_state.compareExchangeStrong(ClearWatchpoint, IsWatched);
            if (state == ClearWatchpoint)
                break;
        }
        m_set.push(watchpoint);
        m_setIsNotEmpty.storeRelaxed(true);
        return true;
    }
    m_set.push(watchpoint);
    // Relaxed stores (triage 3.6): concurrent lock-free state()/isBeingWatched()
    // readers tolerate staleness by design; no ordering is implied here beyond
    // what the membership lock already provides to other add/remove paths.
    m_setIsNotEmpty.storeRelaxed(true);
    m_state.storeRelaxed(IsWatched);
    return true;
}

// ===== SPEC-jit section 5.6: central Class-A fire protocol =====
//
// Fire sites span ~20 files including non-owned runtime/** (G6), so the
// interception lives HERE, inside the slow paths every fire funnels through;
// no call-site edits are needed (P2). Direct callers of fireAll/fireAllSlow
// are REQUIRED to be lock-free w.r.t. every SPEC-jit section-7 lock and every
// cell lock (audit table: docs/threads/INTEGRATE-jit.md, Task 11; lock-holding
// sites => manifest M6). An escaped lock-holding caller deadlocks the stop and
// is named by the JSThreadsSafepoint watchdog (annex App. 5.6(d)).
//
// Coalescing (REQUIRED): concurrent Class-A fires enqueue stack-allocated
// records on an intrusive queue; whichever requester's stop runs first drains
// EVERY record of its VM in that one stop. A loser parked inside stopTheWorldAndRun
// (R1.g) finds its record already serviced when its own closure runs (the
// drain re-checks state() == IsWatched per entry, I11, so an already-fired set
// is a no-op). Either way, when fireAllSlow returns the fire is COMPLETE
// (synchronous completion is load-bearing; RELEASE_ASSERTed below).
//
// Queue discipline: records are enqueued BEFORE requesting the stop and the
// drain closure allocates nothing (intrusive stack nodes), keeping the STWR
// closure allocation-free (OM O4). The queue lock is an owned leaf taken only
// around pointer swaps, never across a fire.

namespace {

struct PendingClassAFire {
    WatchpointSet* set;
    const FireDetail* detail; // Caller-owned; the caller blocks in stopTheWorldAndRun until serviced, keeping it alive.
    VM* vm;
    PendingClassAFire* next { nullptr };
    std::atomic<bool> serviced { false };
};

} // anonymous namespace

static Lock s_classAFireQueueLock;
static PendingClassAFire* s_classAFireQueueHead WTF_GUARDED_BY_LOCK(s_classAFireQueueLock) { nullptr };

void WatchpointSet::drainClassAFireQueue(VM& vm)
{
    // Runs world-stopped for `vm`, inside a stopTheWorldAndRun closure. Only
    // this VM's records are unlinked: the stop parks this VM's mutators and no
    // other VM's, so a record enqueued by an independent VM (a Worker, a
    // GIL-on VM beside the gilOff one) would be fired under running code. Its
    // requester is blocked in its own VM's stop, which services it. The relink
    // touches only `next` under the queue lock; every record on the queue is
    // alive because its owner blocks until serviced.
    PendingClassAFire* head = nullptr;
    {
        Locker locker { s_classAFireQueueLock };
        PendingClassAFire** link = &s_classAFireQueueHead;
        while (PendingClassAFire* entry = *link) {
            if (entry->vm != &vm) {
                link = &entry->next;
                continue;
            }
            *link = entry->next;
            entry->next = head;
            head = entry;
        }
    }
    while (head) {
        PendingClassAFire* entry = head;
        head = entry->next; // Read next BEFORE publishing serviced: the owning (parked) requester's stack frame dies once it resumes.
        // Step (3): re-check after the stop (I11) — a fire coalesced earlier in
        // this drain (or a previous winner's drain) may already have
        // invalidated this set; fires are idempotent.
        // B5 audit (precondition 10, docs/threads/cve/map-MC-CODE.md S6): this
        // re-check is a CONSUMER of the deferred-fire fact (a deferred claim
        // CAS may have flipped this set to IsInvalidated on another mutator
        // before this drain runs). The load is relaxed (state()) but the
        // ordering edge is the §A.3 stop barrier we are INSIDE — every
        // mutator's prior writes (including the seq_cst claim CAS) are
        // visible world-stopped. No separate acquire needed.
        if (entry->set->state() == IsWatched) {
            // Step (4): the existing fire body, world stopped. Step (5):
            // jettisons performed by the fired Watchpoints (e.g.
            // CodeBlockJettisoningWatchpoint -> CodeBlock::jettison) run in
            // this SAME closure via jettison's R1.h already-stopped path.
            // Nested Class-A fires reached from a fireInternal enqueue and
            // drain inline through that same path. Fired with the ENQUEUER's
            // VM: entries from
            // different mutators carry their own VM (DeferGCForAWhile etc. are
            // per-VM; deferral-depth bumps are heap-metadata writes, legal
            // without heap access, heap section 10A).
            entry->set->fireAllNow(*entry->vm, *entry->detail);
        }
        entry->serviced.store(true, std::memory_order_release);
        // entry may now dangle (loser's stack) once the world resumes; do not touch it again.
    }
}

void WatchpointSet::fireAllUnderClassAStop(VM& vm, const FireDetail& detail)
{
    ASSERT(Options::useJSThreads());
    ASSERT(invalidatesCompiledCode());

    // Step (1): enqueue first so a concurrent winner can coalesce this fire.
    PendingClassAFire pending { this, &detail, &vm };
    {
        Locker locker { s_classAFireQueueLock };
        pending.next = s_classAFireQueueHead;
        s_classAFireQueueHead = &pending;
    }

    // Step (2): request the stop (lock-free callers only; see the audit note
    // above). A fire reached with the world already stopped (a GC's stopped
    // window, an outer stopTheWorldAndRun closure, a nested fire from a
    // fireInternal, every legacy-GC finalizeUnconditionally/visitWeak fire and
    // every TTL set fire) is served by stopTheWorldAndRun's already-stopped
    // path, which drains inline on this stack under the witness scope. That
    // path, not this function, decides whether the evidence licenses inline
    // patching on this thread: a non-conductor whose only evidence is the
    // thread-granular window fail-stops, and a gilOff caller whose evidence is
    // a GC stop it does not conduct is rerouted to a stop of its own, because
    // that GC's conductor can resume the world underneath it.
    {
        // Watchdog context (annex App. 5.6(d)): if the stop never reaches
        // Mode::Stopped (an escaped lock-holding direct caller wedged a
        // mutator), the M4 wait loop crashes naming this set.
        JSThreadsSafepoint::ClassAStopWatchdogContext watchdogContext(this, "WatchpointSet Class-A fire");
        JSThreadsSafepoint::stopTheWorldAndRun(vm, scopedLambda<void()>([&] {
            drainClassAFireQueue(vm);
        }));
    }

    // Step (6): synchronous completion — by the time ANY requester's
    // stopTheWorldAndRun returns, its queued fire has run (winner's drain or
    // our own; a loser parks for the winner's whole stop, R1.g).
    RELEASE_ASSERT(pending.serviced.load(std::memory_order_acquire));
    RELEASE_ASSERT(hasBeenInvalidated());
}

void WatchpointSet::fireAllNow(VM& vm, const FireDetail& detail)
{
    ASSERT(state() == IsWatched);

    WTF::storeStoreFence();
    m_state.storeRelaxed(IsInvalidated); // Do this first. Needed for adaptive watchpoints. Ordering comes from the surrounding F4 fence pair / STW barrier, as before.
    fireAllWatchpoints(vm, detail);
    WTF::storeStoreFence(); // F4: this fence pair stays; Class-A fires additionally ride the stop entry/exit barrier.
}

void WatchpointSet::fireAllSlow(VM& vm, const FireDetail& detail)
{
    ASSERT(state() == IsWatched);

    // SPEC-jit section 5.6: flag on, Class-A fires ALWAYS run world-stopped —
    // deliberately no ">1 mutator" gate (G7/I10: VM construction does not
    // synchronize with an in-flight inline fire). Class-B sets and data-only
    // FireDetails (rare-site override) fire exactly as today.
    if (Options::useJSThreads() && m_invalidatesCode.loadRelaxed() && !detail.fireIsDataOnly()) [[unlikely]] {
        fireAllUnderClassAStop(vm, detail);
        return;
    }

    fireAllNow(vm, detail);
}

void WatchpointSet::fireAllSlow(VM&, DeferredWatchpointFire* deferredWatchpoints)
{
    // Deferral transfer: as today (SPEC-jit section 5.6 / annex App. 5.6(a)).
    // Callers MAY hold locks here — that is the point of deferring. Only the
    // state flip and list transfer happen now; the code-invalidating FIRE runs
    // at the holder's scope exit (lock-free by construction) through
    // m_watchpointsToFire.fireAll => fireAllSlow above, where the Class-A stop
    // protocol applies. Cross-thread mutation of m_set here is serialized by
    // the same owner-side locks that serialize the watched state itself
    // (e.g. Structure transitions); pre-M4 the GIL stub guarantees a single
    // mutator.
    //
    // ORDERING (GIL-removal precondition 10): a deferring caller COMPLETES
    // its watched-fact mutation (e.g. publishes a new structureID into
    // objects) BEFORE the scope-exit fire stops the world. Under N mutators,
    // optimized code in another mutator that elided a check on this set
    // would otherwise execute against the already-false fact in that window.
    // What this function provides is the claim CAS below (m_state IsWatched
    // -> IsInvalidated, seq_cst, hence release) on the SOURCE set: the single
    // point at which the set becomes observably invalidated, which runs
    // BEFORE any caller publishes its watched-fact mutation. A consumer that
    // decides whether to re-use a not-yet-jettisoned code pointer either
    // acquire-loads the source set's state and observes IsInvalidated, or
    // rides the stop barrier of the scope-exit fire. No deferring site fires
    // before it publishes, so gilOff the window between publication and the
    // scope-exit jettison is still open; GIL-on and flag-off the single
    // running mutator keeps today's adapt-after-publish ordering. See
    // JSThreadsSafepoint::gilRemovalPreconditionsMet().
    // B-relabelrace (SPEC-jit §5.6 deferral row, amended in this change):
    // the owner-side-serialization claim above does NOT hold for every entry.
    // The inline original-array nonPropertyTransition path
    // (StructureInlines.h, reached from relabelIndexingShapeConcurrent and
    // plain array-shape relabels) fires this deferred overload with no
    // m_lock, no allocation and no safepoint: two mutators relabeling
    // DISTINCT arrays that share the SAME original structure both pass the
    // relaxed fireAll precheck (Watchpoint.h) and race here. (The
    // firePropertyReplacementWatchpointSet direct caller has the same
    // lock-free IsWatched pre-check and already documents reliance on an
    // internal re-check.) So flag-on, the claim itself is atomic: exactly
    // one racer CASes IsWatched -> IsInvalidated and owns the membership
    // transfer; losers return with their deferred set untouched
    // (ClearWatchpoint => no scope-exit fire), which is benign because the
    // winner's deferred fire invalidates everything the loser would have,
    // and the loser's caller re-publishes against a set every observer
    // already sees as IsInvalidated. The claim runs BEFORE the transfer
    // (claim-then-splice; the splice itself is serialized by take()'s
    // membership lock), so takeWatchpointsToFire sees the source already
    // IsInvalidated flag-on and take() installs IsWatched into the deferred
    // set explicitly — the state the source held when the claim succeeded.
    // Flag-off: single mutator, today's exact sequence, unchanged.
    if (Options::useJSThreads()) [[unlikely]] {
        WTF::storeStoreFence();
        if (WatchpointState prior = m_state.compareExchangeStrong(IsWatched, IsInvalidated); prior != IsWatched) {
            // The only legitimate loser entry is the lost race documented
            // above: another claimant already CASed IsWatched -> IsInvalidated.
            // States are monotonic, so a ClearWatchpoint prior here can only
            // mean a caller bypassed the IsWatched precheck — trap it, as the
            // pre-claim ASSERT(state() == IsWatched) did.
            ASSERT_UNUSED(prior, prior == IsInvalidated);
            return;
        }
        deferredWatchpoints->takeWatchpointsToFire(this);
        WTF::storeStoreFence();
        return;
    }

    ASSERT(state() == IsWatched);

    WTF::storeStoreFence();
    deferredWatchpoints->takeWatchpointsToFire(this);
    m_state.storeRelaxed(IsInvalidated); // Do after moving watchpoints to deferredWatchpoints so deferredWatchpoints gets our current state.
    WTF::storeStoreFence();
}

void WatchpointSet::fireAllSlow(VM& vm, const char* reason)
{
    fireAllSlow(vm, StringFireDetail(reason));
}

void WatchpointSet::fireAllWatchpoints(VM& vm, const FireDetail& detail)
{
    // In case there are any adaptive watchpoints, we need to make sure that they see that this
    // watchpoint has been already invalidated.
    RELEASE_ASSERT(hasBeenInvalidated());

    // Firing a watchpoint may cause a GC to happen. This GC could destroy various
    // Watchpoints themselves while they're in the process of firing. It's not safe
    // for most Watchpoints to be destructed while they're in the middle of firing.
    // This GC could also destroy us, and we're not in a safe state to be destroyed.
    // The safest thing to do is to DeferGCForAWhile to prevent this GC from happening.
    DeferGCForAWhile deferGC(vm);
    
    while (true) {
        Watchpoint* watchpoint = nullptr;
        {
            // AB18-G: Class-A fires run world-stopped, but Class-B (DataOnly)
            // fires run with mutators live, so the emptiness check, the head
            // read, AND the unlink hold the membership lock as one critical
            // section. The lock is RELEASED before fire(): fire can run
            // arbitrary code, including re-installs that take the lock again
            // (adaptive watchpoints).
            MembershipLocker membershipLocker;
            if (m_set.isEmpty())
                break;
            watchpoint = &*m_set.begin();
            ASSERT(watchpoint->isOnList());

            // Removing the Watchpoint before firing it makes it possible to implement watchpoints
            // that add themselves to a different set when they fire. This kind of "adaptive"
            // watchpoint can be used to track some semantic property that is more fine-graiend than
            // what the set can convey. For example, we might care if a singleton object ever has a
            // property called "foo". We can watch for this by checking if its Structure has "foo" and
            // then watching its transitions. But then the watchpoint fires if any property is added.
            // So, before the watchpoint decides to invalidate any code, it can check if it is
            // possible to add itself to the transition watchpoint set of the singleton object's new
            // Structure.
            watchpoint->remove();
            ASSERT(&*m_set.begin() != watchpoint);
            ASSERT(!watchpoint->isOnList());
        }

        watchpoint->fire(vm, detail);
        // After we fire the watchpoint, the watchpoint pointer may be a dangling pointer. That's
        // fine, because we have no use for the pointer anymore.
    }
}

void WatchpointSet::take(WatchpointSet* other)
{
    ASSERT(state() == ClearWatchpoint);
    // AB18-G: bulk membership transfer — same serialization requirement as
    // add()/remove() (a deferred-fire take can otherwise race a concurrent
    // install on the source set).
    MembershipLocker locker;
    m_set.takeFrom(other->m_set);
    m_setIsNotEmpty.storeRelaxed(other->m_setIsNotEmpty.loadRelaxed());
    if (Options::useJSThreads()) [[unlikely]] {
        // B-relabelrace: the claiming CAS in the deferred fireAllSlow flipped
        // the source to IsInvalidated BEFORE this transfer (claim-then-splice;
        // the deferred fireAllSlow is the sole caller, via
        // DeferredWatchpointFire::takeWatchpointsToFire). The deferred set
        // must still fire at scope exit, so it gets IsWatched — the state the
        // source held when the claim succeeded.
        ASSERT(other->m_state.loadRelaxed() == IsInvalidated);
        m_state.storeRelaxed(IsWatched);
    } else
        m_state.storeRelaxed(other->m_state.loadRelaxed());
    m_invalidatesCode.storeRelaxed(other->m_invalidatesCode.loadRelaxed()); // SPEC-jit section 5.6: a deferred fire keeps the source set's classification.
    other->m_setIsNotEmpty.storeRelaxed(false);
}

bool InlineWatchpointSet::add(Watchpoint* watchpoint)
{
    return inflate()->add(watchpoint);
}

void InlineWatchpointSet::fireAll(VM& vm, const char* reason)
{
    fireAll(vm, StringFireDetail(reason));
}

WatchpointSet* InlineWatchpointSet::inflateSlow()
{
    ASSERT(!isCompilationThread());
    // AB18-G: flag-on, two mutators can race the thin->fat inflation of one
    // shared set (e.g. a Structure's transition set under the shared object
    // model): both would allocate a fat set and one thread's subsequent
    // add() would land on the LOSING set — a silently disarmed watchpoint.
    // Double-check under the membership lock so exactly one fat set wins.
    // (Readers of m_data stay lock-free: the publish below is
    // fence-then-store, as before.)
    MembershipLocker locker;
    uintptr_t data = m_data.loadRelaxed();
    if (Options::useJSThreads() && isFat(data)) [[unlikely]]
        return fat(data);
    ASSERT(isThin(data));
    // Transfer the construction-time classification to the fat set (I10).
    WatchpointSetClassification classification = (data & ClassBFlag) ? WatchpointSetClassification::DataOnly : WatchpointSetClassification::InvalidatesCode;
    WatchpointSet* fat = &WatchpointSet::create(decodeState(data), classification).leakRef();
    // TSAN wave 2 (triage 3.6): publish the fat pointer with a release CAS so
    // the WatchpointSet's initialized contents are ordered before the pointer
    // becomes visible to lock-free relaxed readers of m_data (this replaces
    // the old storeStoreFence + plain store, which was UB against those
    // readers). Flag-off there is a single mutator and the CAS cannot fail.
    // Flag-on the thin->fat transition is serialized by the membership lock
    // (re-checked above), but a thin state store (tryStoreThinState, a
    // lock-free CAS from whichever thread fires or touches the set) can land
    // between the load above and this publish; the fat set then adopts the
    // state that store left and the publish retries, so a fire that landed
    // there makes the caller's add() see IsInvalidated and refuse.
    //
    // The WatchpointSet constructor initializes m_state / m_setIsNotEmpty /
    // m_invalidatesCode via relaxed atomic stores, so the lock-free readers'
    // atomic loads of them are defined; the reader-side edge pairing with this
    // release publish is the address dependency of every dereference on the
    // word loaded from m_data (InlineWatchpointSet::dataLoadOrder).
    uintptr_t prior = m_data.compareExchangeStrong(data, std::bit_cast<uintptr_t>(fat), std::memory_order_release);
    if (Options::useJSThreads()) [[unlikely]] {
        while (prior != data) {
            ASSERT(isThin(prior));
            data = prior;
            fat->m_state.storeRelaxed(decodeState(data));
            prior = m_data.compareExchangeStrong(data, std::bit_cast<uintptr_t>(fat), std::memory_order_release);
        }
        return fat;
    }
    ASSERT_UNUSED(prior, prior == data);
    return fat;
}

void InlineWatchpointSet::freeFat()
{
    ASSERT(isFat());
    fat()->deref();
}

void DeferredWatchpointFire::takeWatchpointsToFire(WatchpointSet* watchpointsToFire)
{
    ASSERT(m_watchpointsToFire.state() == ClearWatchpoint);
    // B-relabelrace re-scope (SPEC-jit §5.6 deferral row amended in the same
    // change): flag-on, the deferred fireAllSlow claims the source via CAS
    // (IsWatched -> IsInvalidated) BEFORE transferring, so the protective
    // invariant here is "source already claimed-invalid by this thread";
    // flag-off, the flip happens after the transfer and the source is still
    // IsWatched. Both arms assert the one exact state their protocol permits.
    ASSERT(watchpointsToFire->state() == (Options::useJSThreads() ? IsInvalidated : IsWatched));
    m_watchpointsToFire.take(watchpointsToFire);
}

} // namespace JSC

namespace WTF {

void printInternal(PrintStream& out, JSC::WatchpointState state)
{
    switch (state) {
    case JSC::ClearWatchpoint:
        out.print("ClearWatchpoint");
        return;
    case JSC::IsWatched:
        out.print("IsWatched");
        return;
    case JSC::IsInvalidated:
        out.print("IsInvalidated");
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} // namespace WTF

