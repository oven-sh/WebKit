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
#include <cstdlib>
#include <mutex>
#include <wtf/DataLog.h>
#include <wtf/Lock.h>
#include <wtf/Locker.h>
#include <wtf/MonotonicTime.h>

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

void WatchpointSet::add(Watchpoint* watchpoint)
{
    ASSERT(!isCompilationThread());
    ASSERT(state() != IsInvalidated);
    if (!watchpoint)
        return;
    // AB18-G: flag-on, installs reach the same set from N mutators holding
    // only per-CodeBlock locks (shared-stub watchpointSets via
    // SharedJITStubSet reuse; per-Structure transition sets on the shared
    // object model). Serialize the link against concurrent add/remove.
    MembershipLocker locker;
    m_set.push(watchpoint);
    // Relaxed stores (triage 3.6): concurrent lock-free state()/isBeingWatched()
    // readers tolerate staleness by design; no ordering is implied here beyond
    // what the membership lock already provides to other add/remove paths.
    m_setIsNotEmpty.storeRelaxed(true);
    m_state.storeRelaxed(IsWatched);
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
// the WHOLE queue in that one stop. A loser parked inside stopTheWorldAndRun
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

// ===== BUGHUNT INSTRUMENTATION (stw-watchdog evidence pack; env-gated; NOT FOR LANDING) =====
// JSC_CLASSA_FIRE_STATS=1: atexit summary (fire counts + wall-clock span) of Class-A fires.
// JSC_CLASSA_FIRE_LOG=1: one line per Class-A fire naming the set and FireDetail.
static std::atomic<uint64_t> s_bhInlineClassAFires { 0 };
static std::atomic<uint64_t> s_bhStopClassAFires { 0 };
static std::atomic<uint64_t> s_bhDrainedFireEntries { 0 };
static std::atomic<double> s_bhFirstFireMs { 0 };
static std::atomic<double> s_bhLastFireMs { 0 };

static bool bhFireStatsEnabled()
{
    static const bool enabled = !!getenv("JSC_CLASSA_FIRE_STATS");
    return enabled;
}

static bool bhFireLogEnabled()
{
    static const bool enabled = !!getenv("JSC_CLASSA_FIRE_LOG");
    return enabled;
}

static void bhDumpFireStats()
{
    double spanMs = s_bhLastFireMs.load() - s_bhFirstFireMs.load();
    dataLogLn("BUGHUNT-CLASSA-STATS stopFires=", s_bhStopClassAFires.load(),
        " inlineFires=", s_bhInlineClassAFires.load(),
        " drainedEntries=", s_bhDrainedFireEntries.load(),
        " spanMs=", spanMs);
}

static void bhNoteFire(bool inlineFire, WatchpointSet* set, const FireDetail& detail)
{
    if (!bhFireStatsEnabled() && !bhFireLogEnabled()) [[likely]]
        return;
    double nowMs = MonotonicTime::now().secondsSinceEpoch().milliseconds();
    double expected = 0;
    s_bhFirstFireMs.compare_exchange_strong(expected, nowMs);
    s_bhLastFireMs.store(nowMs);
    if (inlineFire)
        s_bhInlineClassAFires.fetch_add(1, std::memory_order_relaxed);
    else
        s_bhStopClassAFires.fetch_add(1, std::memory_order_relaxed);
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] { std::atexit(bhDumpFireStats); });
    if (bhFireLogEnabled()) {
        dataLog("BUGHUNT-FIRE ", inlineFire ? "inline" : "stop", " set=", RawPointer(set), " state=", set->state(), " detail=[");
        detail.dump(WTF::dataFile());
        dataLogLn("]");
    }
}

void WatchpointSet::drainClassAFireQueue()
{
    // Runs world-stopped, inside a stopTheWorldAndRun closure.
    PendingClassAFire* head;
    {
        Locker locker { s_classAFireQueueLock };
        head = s_classAFireQueueHead;
        s_classAFireQueueHead = nullptr;
    }
    while (head) {
        PendingClassAFire* entry = head;
        head = entry->next; // Read next BEFORE publishing serviced: the owning (parked) requester's stack frame dies once it resumes.
        // Step (3): re-check after the stop (I11) — a fire coalesced earlier in
        // this drain (or a previous winner's drain) may already have
        // invalidated this set; fires are idempotent.
        s_bhDrainedFireEntries.fetch_add(1, std::memory_order_relaxed); // BUGHUNT (always-on counter; read only when env-gated dump runs).
        if (entry->set->state() == IsWatched) {
            // Step (4): the existing fire body, world stopped. Step (5):
            // jettisons performed by the fired Watchpoints (e.g.
            // CodeBlockJettisoningWatchpoint -> CodeBlock::jettison) run in
            // this SAME closure via jettison's R1.h already-stopped path.
            // Nested Class-A fires reached from a fireInternal take branch (1)
            // below and run inline. Fired with the ENQUEUER's VM: entries from
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

    // Step (1): a fire reached with the world already stopped (a GC's stopped
    // window, an outer stopTheWorldAndRun closure, or the pre-M4 stub witness)
    // runs inline without re-requesting (R1.h). This is also the branch every
    // legacy-GC finalizeUnconditionally/visitWeak fire and every TTL set fire
    // takes (SPEC-jit section 5.6; Structure::fireThreadLocalSetsWithChainUnderStop
    // asserts butterflyWorldIsStopped before calling fireAll).
    if (JSThreadsSafepoint::worldIsStopped(vm)) {
        // Review round 3 (R3-1): this inline fire may be reached on PER-HEAP
        // already-stopped evidence (legacy per-VM GC stop) that the VM-less
        // worldIsStopped() consumers cannot see. The witness scope (a) runs
        // the R2-4 entered-VMs tripwire when no process-global witness holds
        // — so a fire reached from VM A's legacy GC stop while VM B's mutator
        // runs (flag-on + Workers, pre-M4) crashes here instead of patching
        // under a live foreign mutator — and (b) raises the process-global
        // stub witness across the whole fire, so the VM-less patching asserts
        // (DFG::CommonData::invalidateLinkedCode, DFG::JumpReplacement::fire)
        // see the stop window even when no jettison (with its own R1.h scope)
        // is reached. Nests freely under an outer scope/stop.
        JSThreadsSafepoint::AlreadyStoppedWorldWitnessScope witnessScope(vm);
        if (state() == IsWatched) { // I11.
            bhNoteFire(true, this, detail); // BUGHUNT
            fireAllNow(vm, detail);
        }
        return;
    }

    // Step (2): request the stop (lock-free callers only; see the audit note
    // above). Enqueue first so a concurrent winner can coalesce this fire.
    bhNoteFire(false, this, detail); // BUGHUNT
    PendingClassAFire pending { this, &detail, &vm };
    {
        Locker locker { s_classAFireQueueLock };
        pending.next = s_classAFireQueueHead;
        s_classAFireQueueHead = &pending;
    }

    {
        // Watchdog context (annex App. 5.6(d)): if the stop never reaches
        // Mode::Stopped (an escaped lock-holding direct caller wedged a
        // mutator), the M4 wait loop crashes naming this set.
        JSThreadsSafepoint::ClassAStopWatchdogContext watchdogContext(this, "WatchpointSet Class-A fire");
        JSThreadsSafepoint::stopTheWorldAndRun(vm, scopedLambda<void()>([] {
            drainClassAFireQueue();
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
    // ORDERING CAVEAT (review round 1; GIL-removal precondition, recorded in
    // docs/threads/INTEGRATE-jit.md): a deferring caller COMPLETES its watched-
    // fact mutation (e.g. publishes a new structureID into objects) BEFORE the
    // scope-exit fire stops the world. Under N mutators, any optimized code in
    // ANOTHER mutator that elided a check based on this set executes against
    // the already-false fact until the stop lands — THREAD.md forbids exactly
    // that ("it will not actually perform the write until that optimized code
    // reaches a safepoint and gets invalidated"). This deferral form is
    // therefore sound only when (a) a single mutator runs (phase-1 GIL — the
    // case today), (b) the world is already stopped around the whole
    // mutation+fire (the OM's TTL-set pattern:
    // Structure::fireThreadLocalSetsWithChainUnderStop publishes INSIDE the
    // stop), or (c) the specific watched fact is re-checked dynamically by all
    // compiled consumers. Before GIL removal, every deferred Class-A site must
    // be classified (a)/(b)/(c) in the Task-11 audit's "fact published before
    // fire?" column or restructured onto the TTL-set pattern; see
    // JSThreadsSafepoint::gilRemovalPreconditionsMet().
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
    m_state.storeRelaxed(other->m_state.loadRelaxed());
    m_invalidatesCode.storeRelaxed(other->m_invalidatesCode.loadRelaxed()); // SPEC-jit section 5.6: a deferred fire keeps the source set's classification.
    other->m_setIsNotEmpty.storeRelaxed(false);
}

void InlineWatchpointSet::add(Watchpoint* watchpoint)
{
    inflate()->add(watchpoint);
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
    // readers). The CAS cannot fail: flag-on, the thin->fat transition is
    // serialized by the membership lock (re-checked above) and thin-state
    // stores require the owner's serialization; flag-off there is a single
    // mutator. Asserted below.
    //
    // TSAN wave 5 (triage 12.6, REOPENED family 9): the release CAS alone was
    // not enough — WatchpointSet::create above ran the Atomic value
    // constructors (plain stores), and the lock-free readers load m_data
    // RELAXED, so there was no reader-side edge ordering the construction
    // writes before the dereference. Both halves are now fixed at their
    // source: the WatchpointSet constructor initializes m_state /
    // m_setIsNotEmpty / m_invalidatesCode via relaxed atomic stores, and every
    // fat-pointer dereference goes through the consume-ordered
    // InlineWatchpointSet::consumeFat, which pairs with this release publish.
    uintptr_t prior = m_data.compareExchangeStrong(data, std::bit_cast<uintptr_t>(fat), std::memory_order_release);
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
    ASSERT(watchpointsToFire->state() == IsWatched);
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

