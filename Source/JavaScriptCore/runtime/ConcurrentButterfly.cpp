/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
#include "ConcurrentButterfly.h"

// ConcurrentButterfly.cpp - runtime slow paths of the shared-memory-threads
// object model (SPEC-objectmodel.md, frozen rev 14). This translation unit
// owns (Task 5):
//
//   - the §9.3 exported spine/fragment accessors (declared in
//     ConcurrentButterfly.h since Task 2; the layout landed in Butterfly.h with
//     Task 4);
//   - the §10.6 interim stop-the-world veneer + witness flag (integration
//     manifest entry 6): g_jsThreadsStubWorldStopped,
//     jsThreadsStopTheWorldAndRun, butterflyWorldIsStopped;
//   - the §4.2 flat->segmented conversion (convertToSegmentedButterfly):
//     step-0 TTL firing, RESTART discipline, zero-copy slice aliasing,
//     aliased-allocation base/size recording, and the nuke + 128-bit DCAS
//     publication (PA cells: the I36 fenced order instead).
//
// Task 6 (landed below): the §4.3 segmented-transition protocol
// (trySegmentedTransition/segmentedTransition) with the full DCAS-failure
// taxonomy (a)-(d) and RESTART discipline; the N2 locked structure-only
// transition (tryStructureOnlyTransition/structureOnlyTransition); and the
// §9.5 full-§2-dispatch slow paths JSObjectWithButterfly::getDirectConcurrent
// / putDirectConcurrent (M7-conforming, I34 poll-free).
//
// Task 6b (landed below): the §4.5 GC visit (visitSegmentedButterfly,
// explicitly instantiated for SlotVisitor/AbstractSlotVisitor; the segmented
// branches of visitButterflyImpl live in owned JSObject.cpp) and the I25
// barrier audit (recorded at the definition).
//
// Task 7 (landed below): ensureSharedWriteBit - the §3 foreign first write:
// F1 fire-then-DCAS under the §3.0 merge loop (I12), the §4.6 ArrayStorage
// per-event-stop publication, the §4.8/I35 CoW materialize-first path, the
// I36 PA cell-locked 64-bit flip, and the R-DOUBLE (§4.7) no-rebox rule.
//
// Task 8 (landed below): casButterfly + the §4.4 array-CAS plumbing - the
// frozen §9.3 casButterfly with the I27/I17 assert set, the cell-locked §4.6
// AS-COPY publication form (publishArrayStorageButterflyLocked), the T2
// replacement-spine growth (tryGrowSegmentedVectorLength), the flag-on
// ensureLengthSlow/reallocateAndShrinkButterfly drivers (T1/T2; the former T5
// in-place vectorLength growth was REMOVED in review round 1 - flat
// vectorLengths are immutable flag-on), and the §9.5 indexed slow
// paths JSObjectWithButterfly::getIndexConcurrent / putIndexConcurrent
// (AS accesses cell-locked, I31).
//
// Task 9 (landed below): the §6 per-server-heap quarantine-epoch registry
// (ButterflyQuarantineEpochs - Lock + stable Heap* -> Atomic<uint64_t> map),
// its §10.4c Heap::addStopTheWorldSafepointHook adapter, and the exported
// butterflyQuarantineEpochSlot / registerButterflyQuarantineEpochHook
// (declared in runtime/PropertyTable.h, their sole runtime consumer).
//
// Flag-off (I22): nothing here is reachable - no spine is ever published, the
// veneer is only called from flag-on paths, and the witness stays false.

#include "ArrayConventions.h"
#include "ArrayStorage.h"
#include "ButterflyInlines.h"
#include "GCDeferralContextInlines.h"
#include "JSCInlines.h"
#include "JSThreadsSafepoint.h"
#include "PropertyTable.h"
#include <algorithm>
#include <cstring>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

// ===== §9.3 spine/fragment accessors (frozen signatures; layout = Butterfly.h) =====

ButterflyFragment* spineOutOfLineFragment(ButterflySpine* spine, unsigned fragmentIndex)
{
    spine->tsanConsume(); // V7: import the publisher's clock (see Butterfly.h).
    return spine->outOfLineFragment(fragmentIndex);
}

ButterflyFragment* spineIndexedFragment(ButterflySpine* spine, unsigned fragmentIndex)
{
    spine->tsanConsume(); // V7
    return spine->indexedFragment(fragmentIndex);
}

WriteBarrierBase<Unknown>* segmentedOutOfLineSlot(ButterflySpine* spine, PropertyOffset offset)
{
    // Precondition (I33, out-of-line clause): the 0-based out-of-line index
    // < butterflyFragmentSlots * outOfLineFragmentCount; the member ASSERTs it.
    // (offsetInOutOfLineStorage() is the NEGATIVE PropertyStorage index, not
    // the spine index - see outOfLineButterflyIndex in ConcurrentButterfly.h.)
    ASSERT(isOutOfLineOffset(offset));
    spine->tsanConsume(); // V7
    return spine->outOfLineSlot(outOfLineButterflyIndex(offset));
}

// segmentedIndexedSlot / segmentedPublicLength: ALWAYS_INLINE in
// ConcurrentButterflyInlines.h (T2-segmented-accessors-inline).

void setSegmentedPublicLength(ButterflySpine* spine, uint32_t value)
{
    spine->tsanConsume(); // V7
    spine->setPublicLength(value); // RELEASE_ASSERTs indexedFragmentCount (C2).
}

// Bounds-checked consumer variants (segmentedOutOfLineSlotIfWithinBounds /
// segmentedIndexedSlotIfReadable / segmentedIndexedSlotIfWithinVectorLength /
// segmentedVectorLength): ALWAYS_INLINE in ConcurrentButterflyInlines.h
// (T2-segmented-accessors-inline — these were the dominant W>=2 serial-phase
// self% in the SCALEBENCH profile).

// ===== Task 10: O3/I20 cell-lock depth witness =====

namespace {

// Per-thread count of JSCellLocks acquired by THIS translation unit's
// protocols. O3: a thread holds at most one JSCellLock; I20/O2 add that no
// §6-ranked lock is held across a safepoint (hence the veneer-entry assert
// below). The counter observes only locks taken here - a foreign thread's
// locks, GC's visitor locks, and unowned runtime sites are invisible to it
// (other threads, or manifest-7/7b audits) - which is exactly the O3
// obligation these protocols carry: each documents "the only cell lock we
// hold".
thread_local unsigned t_cellLocksHeldByConcurrentButterfly = 0;

ALWAYS_INLINE void lockCellChecked(JSCellLock& lock)
{
    RELEASE_ASSERT(!t_cellLocksHeldByConcurrentButterfly); // O3: max one JSCellLock per thread; also O2 (no nested blocking acquire).
    lock.lock();
    ++t_cellLocksHeldByConcurrentButterfly;
}

ALWAYS_INLINE void unlockCellChecked(JSCellLock& lock)
{
    RELEASE_ASSERT(t_cellLocksHeldByConcurrentButterfly == 1); // Pairing witness (a missed unlock path would trip the next lockCellChecked anyway).
    --t_cellLocksHeldByConcurrentButterfly;
    lock.unlock();
}

// RAII flavor for the Locker { cellLock() } accessor sites (I31/L5).
struct CellLockDepthScope {
    CellLockDepthScope()
    {
        RELEASE_ASSERT(!t_cellLocksHeldByConcurrentButterfly); // O3
        ++t_cellLocksHeldByConcurrentButterfly;
    }
    ~CellLockDepthScope()
    {
        RELEASE_ASSERT(t_cellLocksHeldByConcurrentButterfly == 1);
        --t_cellLocksHeldByConcurrentButterfly;
    }
};

} // anonymous namespace

// ===== §10.6 interim stop-the-world veneer + witness (manifest entry 6) =====

// Pre-M4 witness. std::atomic<bool> (relaxed accesses) because it is read
// cross-thread: SPEC-jit section 5.6 disjunct 4 reads it through
// JSC_OM_PROVIDES_JSTHREADS_STUB_WITNESS (defined next to the declaration in
// ConcurrentButterfly.h), and compiler/GC threads consult
// butterflyWorldIsStopped() - matching the jit side's atomic depth-counter
// discipline (JSThreadsSafepoint.cpp s_stubWorldStoppedDepth). Deleted at M4
// integration.
std::atomic<bool> g_jsThreadsStubWorldStopped { false };

void jsThreadsStopTheWorldAndRun(VM& vm, const ScopedLambda<void()>& work)
{
    // INTERIM STUB until integration manifest M4 (then: real VMManager STWR +
    // CS2 GC-conductor bracket). Per jit CS6's preferred option we DELEGATE to
    // JSThreadsSafepoint::stopTheWorldAndRun (bytecode/JSThreadsSafepoint.cpp,
    // landed by jit Task 1), which RELEASE_ASSERTs the §10.6 single-mutator
    // witness contract - vm.currentThreadIsHoldingAPILock() and at most one
    // entered VM - and runs `work` inline on this stack, raising its own
    // worldIsStopped() depth counter.
    //
    // ORDERING IS LOAD-BEARING (adversarial-review round 1): the owned witness
    // is raised INSIDE the delegated closure, NOT around the delegated call.
    // JSThreadsSafepoint::stopTheWorldAndRun begins with `if (worldIsStopped(vm))
    // run-inline-and-return`, and its worldIsStopped() consults this witness
    // (disjunct 4) - raising the witness before delegating would make EVERY
    // outermost OM stop look "already stopped" and silently skip the delegate's
    // API-lock + <=1-entered-VM RELEASE_ASSERTs. With the raise inside the
    // closure, the delegate's entry checks always execute on the outermost
    // call, while butterflyWorldIsStopped() still holds for everything `work`
    // does. Genuinely nested veneer calls (R1.h) are still inline: the
    // delegate's own depth counter is raised around the closure.
    // // THREADS-INTEGRATE(objectmodel)
    //
    // Caller contract (GT11): entered mutator; no §6-ranked lock (SAL /
    // JSCellLock / Structure::m_lock) held - O2: never block on a safepoint
    // holding them; `work` must not allocate in the GC heap (O4: pre-allocate
    // before requesting the stop, re-validate inside, RESTART on refit).
    RELEASE_ASSERT(!t_cellLocksHeldByConcurrentButterfly); // O2/I20 (Task 10): never block on a stop holding a §6-ranked lock; §4.3(b2) bans converting under the cell lock.
    // Watchdog context (review round): OM transition stops are the dominant
    // requester under property-write storms (e.g. counter-lock); a wedged
    // stop must name this requester class instead of crashing context-nil.
    JSThreadsSafepoint::ClassAStopWatchdogContext watchdogContext(&vm, "OM transition stop");
    JSThreadsSafepoint::stopTheWorldAndRun(vm, scopedLambda<void()>([&] {
        bool savedWitness = g_jsThreadsStubWorldStopped.load(std::memory_order_relaxed); // Nested veneer calls (R1.h) just nest.
        g_jsThreadsStubWorldStopped.store(true, std::memory_order_relaxed);
        work();
        g_jsThreadsStubWorldStopped.store(savedWitness, std::memory_order_relaxed);
    }));
}

bool butterflyWorldIsStopped(VM& vm)
{
    // §9.4 fire-assert predicate (I13): owned stub witness OR the jit
    // workstream's disjuncts (VMManager mode Stopped, legacy per-VM GC stop,
    // shared-server stop once the heap workstream lands). At M4 integration
    // this becomes the jit predicate alone. // THREADS-INTEGRATE(objectmodel)
    return g_jsThreadsStubWorldStopped.load(std::memory_order_relaxed) || JSThreadsSafepoint::worldIsStopped(vm);
}

// ===== Per-event-stop claim table (STW dedup; T1-butterfly-stw-growth) =====
//
// Problem: every per-event stop in this file is requested from a lock-free
// re-dispatch loop, so W threads racing the SAME event (the tail migration of
// one object, the F1/F2 TTL fire of one structure, the §4.6 AS SW flip of one
// object) EACH pay a full stop-the-world rendezvous, only for W-1 of the stop
// closures to observe the winner's publication and no-op. The locksites
// profile measured the mode-(b) grow stop below as the single largest STW
// source on the W=16 scale bench for exactly this reason.
//
// Mechanics: a fixed table of striped claim slots keyed by the event's anchor
// pointer (the object, or the structure whose sets are being fired). The
// first requester CASes its key in, pays the stop, and releases. Racers
// (same key - or, rarely, a stripe collision) wait in a STOP-PARTICIPATING
// loop: parkSitePollAndParkForStopTheWorld every iteration, so the holder's
// rendezvous never waits on a spinner (a spinner parks like any mutator -
// no deadlock), re-evaluating the caller's shouldAbandon() predicate
// (typically "the word / TTL set I planned against moved") between polls.
// Abandon => the caller re-dispatches/RESTARTs WITHOUT stopping the world;
// that is precisely the winner-already-published case. On a pure stripe
// collision the predicate stays false and the loop re-attempts the CAS once
// the unrelated holder releases, so progress is guaranteed.
//
// The claim is an OPTIMIZATION ONLY: every stop closure re-validates its
// preconditions inside the stop exactly as before, so correctness never
// depends on holding a claim. Hence the world-stopped BYPASS below: a caller
// already running under a stop (nested veneer calls run inline) must never
// wait - the holder it would wait on may itself be parked by OUR stop and
// could only release after resume.
//
// Lifetime/safety notes: claims are held only across one (pre-revalidated)
// stop request - never across a return (RAII) and never while a §6-ranked
// lock is held (RELEASE_ASSERTed; the wait parks, O2). The key is used only
// as a hash + busy marker, never dereferenced, and the keyed cell is pinned
// while we spin: the spinner's stack references it and parked stacks are
// conservatively scanned. GIL-on the table is dead weight but harmless:
// claim+stop+release happen within one uninterrupted API-lock tenure (the
// stub stop runs inline and never releases the lock), so a held claim is
// never observable by a second thread and the wait loop is unreachable.
// Flag-off (I22): nothing in this TU is reached.

namespace {

constexpr size_t perEventStopClaimStripeCount = 256; // Power of two; collisions only cost a bounded wait + retry.
Atomic<uintptr_t> s_perEventStopClaimStripes[perEventStopClaimStripeCount]; // Static storage: zero-initialized (0 = free).

thread_local bool t_holdsPerEventStopClaim = false;

ALWAYS_INLINE Atomic<uintptr_t>& perEventStopClaimStripe(const void* key)
{
    uintptr_t hash = reinterpret_cast<uintptr_t>(key);
    hash ^= hash >> 17;
    hash *= 0x9e3779b97f4a7c15ull;
    hash ^= hash >> 32;
    return s_perEventStopClaimStripes[hash & (perEventStopClaimStripeCount - 1)];
}

class PerEventStopClaim {
    WTF_MAKE_NONCOPYABLE(PerEventStopClaim);
public:
    template<typename ShouldAbandonFunctor>
    PerEventStopClaim(VM& vm, const void* key, const ShouldAbandonFunctor& shouldAbandon)
        : m_stripe(perEventStopClaimStripe(key))
    {
        // World already stopped (we are inside a stop closure / a nested
        // veneer call): BYPASS - proceed claim-free. Waiting here could
        // deadlock (see the header comment), and the nested stop runs inline
        // anyway, with every closure re-validating its own preconditions.
        if (butterflyWorldIsStopped(vm)) {
            m_acquired = true;
            m_bypassed = true;
            return;
        }
        RELEASE_ASSERT(!t_holdsPerEventStopClaim); // Claims bracket exactly ONE stop request; nesting outside a stop is a protocol error.
        RELEASE_ASSERT(!t_cellLocksHeldByConcurrentButterfly); // O2/I20: the wait below parks; same contract as the veneer itself.
        uintptr_t keyBits = reinterpret_cast<uintptr_t>(key);
        ASSERT(keyBits); // 0 is the free marker.
        while (true) {
            if (m_stripe.compareExchangeWeak(static_cast<uintptr_t>(0), keyBits)) {
                m_acquired = true;
                t_holdsPerEventStopClaim = true;
                return;
            }
            // Stop-participating wait: park for any in-flight window so the
            // holder's rendezvous never waits on us, then re-test whether the
            // holder's stop already did our work.
            JSThreadsSafepoint::parkSitePollAndParkForStopTheWorld(vm);
            if (shouldAbandon())
                return; // m_acquired stays false: the caller re-dispatches without a stop.
            Thread::yield();
        }
    }

    ~PerEventStopClaim()
    {
        if (!m_acquired || m_bypassed)
            return;
        RELEASE_ASSERT(t_holdsPerEventStopClaim);
        t_holdsPerEventStopClaim = false;
        m_stripe.store(0, std::memory_order_release); // Publication order: the stop's effects precede the release.
    }

    bool acquired() const { return m_acquired; }

private:
    Atomic<uintptr_t>& m_stripe;
    bool m_acquired { false };
    bool m_bypassed { false };
};

} // anonymous namespace

// ===== Task 9: §6 quarantine-epoch registry (ButterflyQuarantineEpochs) =====
//
// §6 release path (r13: PER-SERVER-HEAP, never a process-global counter -
// manifest entry 4c). One epoch counter per JSC::Heap (server heap). The
// counter is bumped by the §10.4c hook adapter below, which the integrator
// registers via Heap::addStopTheWorldSafepointHook at VM/Heap init, BEFORE a
// second client can attach (heap §9): the hook runs once per collection of
// THAT heap, in BOTH protocols (legacy runEndPhase and shared-mode step 7),
// while the world is stopped. PropertyTable entries stamp the OWNING heap's
// epoch at deletion; a stamp strictly below the current epoch proves at least
// one world-stopped window separates the deletion from the reuse attempt, so
// every mutator passed a safepoint and no stale offset/slot pointer survives
// (I18, leaning on I34's no-poll rule). The exported accessors are declared in
// runtime/PropertyTable.h (their sole runtime consumer).

namespace {

struct ButterflyQuarantineEpochs {
    Lock lock;
    // Heap* -> boxed Atomic<uint64_t>. The Atomic is heap-allocated (boxed in
    // a unique_ptr) so its ADDRESS is stable across map rehashes: PropertyTable
    // caches the slot pointer at first quarantine (§6) and reads it lock-free
    // forever after. Entries are NEVER removed - a destroyed Heap's slot is
    // simply retired in place, and a recycled Heap* address re-adopts the old
    // slot, which stays sound: the counter is monotone, and stamps taken from
    // a slot are only ever compared against that same slot's later values.
    UncheckedKeyHashMap<JSC::Heap*, std::unique_ptr<Atomic<uint64_t>>> slots WTF_GUARDED_BY_LOCK(lock);
    UncheckedKeyHashSet<JSC::Heap*> hookRegistered WTF_GUARDED_BY_LOCK(lock);
};

ButterflyQuarantineEpochs& butterflyQuarantineEpochs()
{
    static NeverDestroyed<ButterflyQuarantineEpochs> epochs;
    return epochs.get();
}

// §10.4c hook adapter: runs world-stopped, once per collection of `heap`
// (Heap::runStopTheWorldSafepointHooks contract, heap §9/CR §13.10d). Bumps
// ONLY the collecting heap's slot. Allocation-free after the slot exists; the
// slot is created eagerly at registration below, so the hook itself never
// takes the registry lock on a hot path beyond the get.
void butterflyQuarantineEpochSafepointHook(JSC::Heap& heap)
{
    butterflyQuarantineEpochSlot(heap).exchangeAdd(1);
}

} // anonymous namespace

WTF::Atomic<uint64_t>& butterflyQuarantineEpochSlot(JSC::Heap& heap)
{
    // Leaf lock (§6 lock-context): callers may hold Structure::m_lock (the
    // PropertyTable mutation context, L6) and/or the JSCellLock; this lock is
    // inner to both and is held for a hash lookup only - no allocation in the
    // GC heap, no safepoint, no other lock acquired under it.
    ButterflyQuarantineEpochs& registry = butterflyQuarantineEpochs();
    Locker locker { registry.lock };
    auto result = registry.slots.ensure(&heap, [] {
        return makeUnique<Atomic<uint64_t>>(0);
    });
    return *result.iterator->value;
}

void registerButterflyQuarantineEpochHook(JSC::Heap& heap)
{
    // Idempotent per heap. Heap::addStopTheWorldSafepointHook takes the heap's
    // own hook lock, so it is called OUTSIDE the registry lock (which must
    // remain a leaf). Racing registrants are harmless here in principle, but
    // the manifest-4c call site runs at VM/Heap init (single-threaded for the
    // heap), so the add below completes before any quarantine can occur.
    {
        ButterflyQuarantineEpochs& registry = butterflyQuarantineEpochs();
        Locker locker { registry.lock };
        if (!registry.hookRegistered.add(&heap).isNewEntry)
            return;
        // Create the slot eagerly so the world-stopped hook never populates
        // the map (allocation-free stop windows, O4-adjacent hygiene).
        registry.slots.ensure(&heap, [] {
            return makeUnique<Atomic<uint64_t>>(0);
        });
    }
    heap.addStopTheWorldSafepointHook(&butterflyQuarantineEpochSafepointHook);
}

// ===== §4.2 flat -> segmented conversion =====

namespace {

// --- Cell-word atomics. Bytes [0,16) of the cell = {8B header, 8B tagged
// butterfly word} (GT#3); the header's little-endian lanes are
// structureID [0,4), indexingTypeAndMisc [4], type [5], flags [6],
// cellState [7] (static_asserted in ConcurrentButterfly.h).

ALWAYS_INLINE Atomic<uint64_t>* cellHeaderAtomic(JSCell* cell)
{
    return reinterpret_cast<Atomic<uint64_t>*>(cell);
}

ALWAYS_INLINE Atomic<uint32_t>* structureIDAtomic(JSCell* cell)
{
    static_assert(!JSCell::structureIDOffset());
    return reinterpret_cast<Atomic<uint32_t>*>(cell);
}

ALWAYS_INLINE Atomic<uint64_t>* butterflyWordAtomic(JSObjectWithButterfly* object)
{
    return reinterpret_cast<Atomic<uint64_t>*>(object->butterflyAddress());
}

ALWAYS_INLINE uint8_t headerByte(uint64_t header, unsigned byteOffset)
{
    return static_cast<uint8_t>(header >> (8 * byteOffset));
}

ALWAYS_INLINE uint64_t withHeaderByte(uint64_t header, unsigned byteOffset, uint8_t value)
{
    uint64_t mask = 0xffULL << (8 * byteOffset);
    return (header & ~mask) | (static_cast<uint64_t>(value) << (8 * byteOffset));
}

ALWAYS_INLINE uint64_t withStructureIDLane(uint64_t header, uint32_t idBits)
{
    return (header & ~0xffffffffULL) | idBits;
}

// The DCAS's desired header: `fresh` (whose structureID lane is nuked, and
// whose volatile bytes are the freshest read - I26) with the semantic bytes a
// structure transition owns rewritten: un-nuked new structureID; and, when the
// trigger carries a new structure, the shape bits / m_type / m_flags exactly
// as JSCell::setStructure computes them (lock bits 0xC0 of the indexing byte
// are volatile and copied from fresh - §3.0).
ALWAYS_INLINE uint64_t headerForPublication(uint64_t fresh, StructureID newStructureID, Structure* newStructureOrNull)
{
    uint64_t header = withStructureIDLane(fresh, newStructureID.bits());
    if (newStructureOrNull) {
        uint8_t indexingByte = headerByte(header, JSCell::indexingTypeAndMiscOffset());
        uint8_t newIndexingByte = static_cast<uint8_t>((indexingByte & ~AllArrayTypesAndHistory) | newStructureOrNull->indexingModeIncludingHistory());
        header = withHeaderByte(header, JSCell::indexingTypeAndMiscOffset(), newIndexingByte);
        header = withHeaderByte(header, JSCell::typeInfoTypeOffset(), static_cast<uint8_t>(newStructureOrNull->typeInfo().type()));
        header = withHeaderByte(header, JSCell::typeInfoFlagsOffset(),
            TypeInfo::mergeInlineTypeFlags(newStructureOrNull->typeInfo().inlineTypeFlags(), headerByte(header, JSCell::typeInfoFlagsOffset())));
    }
    return header;
}

// I8 debug validation for a conversion spine whose out-of-line side may carry
// FRESH (non-aliased) tail fragments (the trigger's new structure grew
// out-of-line capacity). validateSpineAliasesFlatButterfly (ButterflyInlines.h)
// covers the exact-aliasing case; this sweeps only the aliased prefix.
void validatePartiallyAliasedSpine(const ButterflySpine* spine, Butterfly* flat, size_t aliasedOutOfLineCapacity, bool hasIndexingHeader)
{
    if (!ASSERT_ENABLED && !verifyConcurrentButterflyEnabled())
        return;

    spine->validateConsistency();
    char* base = flat->pointer();

    for (size_t k = 0; k < aliasedOutOfLineCapacity; ++k)
        RELEASE_ASSERT(reinterpret_cast<char*>(spine->outOfLineSlot(static_cast<unsigned>(k))) == base - 16 - sizeof(EncodedJSValue) * k);

    if (hasIndexingHeader) {
        // C4 holds by ADDRESS identity: the slot-0 assert below proves the
        // spine's live publicLength (fragment 0 slot 0, low half — half
        // layout static_asserted against IndexingHeader in Butterfly.h) and
        // flat->publicLength() are the SAME 4 bytes at B - 8. Do NOT
        // value-compare two reads of that live word: a lock-free in-bounds
        // dense grower (Butterfly::bumpPublicLengthToAtLeast) legally
        // advances it between the reads even while we hold the cell lock —
        // §4.4 in-bounds stores touch neither the butterfly word nor the
        // structureID, so neither the step-3 re-read nor the DCAS-failure
        // taxonomy excludes them. The old equality assert was a TOCTOU; it
        // was also a TSAN-visible data race, since flat->publicLength() is a
        // plain (non-atomic) IndexingHeader load racing the grower's CAS.
        // Note the RETAINED I9b assert below is likewise a same-address
        // double read (high half of B - 8) when aliased; it is sound only
        // because flat vectorLength is immutable flag-on (every bound
        // increase publishes fresh storage, caught by the step-3 word
        // re-read/DCAS) and the grower CAS is a 32-bit low-half-only RMW
        // that cannot tear the high half. If I9b ever flakes under load,
        // apply the same TOCTOU-vs-invariant analysis before suspecting the
        // engine.
        RELEASE_ASSERT(reinterpret_cast<char*>(&spine->indexedFragment(0)->slots[0]) == base - 8);
        RELEASE_ASSERT(spine->frozenFlatVectorLength() == flat->vectorLength()); // I9b: frozen high half vs immutable flat VL — stable, keep.
        for (uint32_t i = 0; i < spine->vectorLengthConcurrent(); ++i)
            RELEASE_ASSERT(reinterpret_cast<char*>(spine->indexedSlot(i)) == base + sizeof(EncodedJSValue) * static_cast<size_t>(i));
    }
}

} // anonymous namespace

// §4.2: zero-copy flat->segmented conversion. ONE publication: when the
// trigger is a transition (newStructureOrNull != nullptr, adding the
// out-of-line property at `offset` with `value`), the single DCAS publishes
// {new structure, fully sized spine} - never an intermediate {old structure,
// undersized spine}. newStructureOrNull == nullptr is the in-place form (T2
// array-resize trigger: the structure is unchanged; offset/value are ignored,
// pass invalidOffset / JSValue()).
//
// Returns the published spine on success. Returns nullptr for RESTART (§4.2):
// the caller must re-enter the WHOLE operation from §2 dispatch on the fresh
// tag + structureID (fresh target computation, fresh F1/F2 checks, fresh
// allocation) - lock-free at restart. nullptr covers: step-0 fired (the world
// changed under the stop), the structure changed before/under the lock, a
// racing conversion already published a spine (re-dispatch lands on §4.3), or
// the butterfly vanished/was replaced incompatibly.
ButterflySpine* convertToSegmentedButterfly(VM& vm, JSObjectWithButterfly* object, Structure* expectedSourceOrNull, Structure* newStructureOrNull, PropertyOffset offset, JSValue value)
{
    RELEASE_ASSERT(Options::useJSThreads());
    ASSERT(vm.currentThreadIsHoldingAPILock());
    RELEASE_ASSERT(offset == invalidOffset || isOutOfLineOffset(offset)); // Inline adds are N2 (structureOnlyTransition), never §4.2 step 4.
    ASSERT(newStructureOrNull || offset == invalidOffset); // A value store needs the transition that exposes it.
    RELEASE_ASSERT(!newStructureOrNull || expectedSourceOrNull); // AB18-S2: a transition trigger must name the source it derived the target from. RELEASE: the stale-parent guard below is load-bearing only when the source is named; a future caller (e.g. JIT-tier E4 emission, SPEC-jit 5.5) passing a transition with a null source would silently reopen the I21 lost-add window in release builds.

    // Planning-time source (§4.2 step 3 compares the re-read structureID
    // against this). A nuked ID here means a racing E4 publication is mid
    // flight - re-dispatch on the settled state.
    StructureID sourceID = object->structureID(); // RAW bits (M5).
    if (sourceID.isNuked())
        return nullptr; // RESTART
    // AB18-S2 stale-parent guard (I21, see the header comment): a transition
    // trigger may publish newStructureOrNull only while the object still has
    // the structure the target was derived FROM. A racing transition that
    // settled between the caller's source check and this capture would
    // otherwise be silently erased - this function would validate (and
    // nuke-CAS) against the racer's fresh structureID while publishing a
    // target whose lineage lacks the racer's add (lost property, I21).
    if (expectedSourceOrNull && sourceID != expectedSourceOrNull->id())
        return nullptr; // RESTART: re-derive the target from the settled source.
    Structure* sourceStructure = sourceID.decode();

    // I31: ArrayStorage never segments (its conversions are per-event STW that
    // publish FLAT, §4.6). I35: CoW materializes a private flat butterfly
    // (§4.8) before any §4.2 protocol; CoW words never reach this function.
    RELEASE_ASSERT(!hasAnyArrayStorage(sourceStructure->indexingType()));
    RELEASE_ASSERT(!isCopyOnWrite(sourceStructure->indexingMode()));
    if (newStructureOrNull) {
        // §4.2 covers property transitions; indexing-SHAPE transitions are full
        // §4.3 (T4) and Double-touching shape changes are per-event STW (§4.7).
        ASSERT(newStructureOrNull->indexingModeIncludingHistory() == sourceStructure->indexingModeIncludingHistory());
        ASSERT(newStructureOrNull->hasIndexingHeader(object) == sourceStructure->hasIndexingHeader(object));
    }

    // ---- Step 0: source/target TTL sets still valid => fire F2 (both sets on
    // S and target) in-closure under the §10.6 veneer, BEFORE any lock
    // (O2/I13/I10b); after the stop returns, RESTART (the closure allocates
    // nothing - O4). I10: foreign butterfly transitions (incl. element
    // resizes) fire both sets under STW before producing a segmented object.
    {
        auto anySetStillValid = [&]() {
            if (sourceStructure->transitionThreadLocalIsStillValid() || sourceStructure->writeThreadLocalIsStillValid())
                return true;
            return newStructureOrNull
                && (newStructureOrNull->transitionThreadLocalIsStillValid() || newStructureOrNull->writeThreadLocalIsStillValid());
        };
        if (anySetStillValid()) {
            // STW dedup (T1-butterfly-stw-growth): W threads converting
            // instances of the same structure race this fire; the sets are
            // monotone, so the winner's single stop does everyone's work.
            // Losers park-wait, observe the fired sets via shouldAbandon, and
            // RESTART without a stop (identical caller contract: every path
            // out of this block is `return nullptr`).
            PerEventStopClaim claim(vm, sourceStructure, [&] { return !anySetStillValid(); });
            if (claim.acquired() && anySetStillValid()) {
                jsThreadsStopTheWorldAndRun(vm, scopedLambda<void()>([&] {
                    // Re-check inside the stop: a racing fire may have got here first.
                    if (sourceStructure->transitionThreadLocalIsStillValid() || sourceStructure->writeThreadLocalIsStillValid())
                        sourceStructure->fireTransitionThreadLocal(vm, "F2: flat->segmented conversion (foreign or shared-write transition)");
                    if (newStructureOrNull
                        && (newStructureOrNull->transitionThreadLocalIsStillValid() || newStructureOrNull->writeThreadLocalIsStillValid()))
                        newStructureOrNull->fireTransitionThreadLocal(vm, "F2: flat->segmented conversion (transition target)");
                }));
            }
            return nullptr; // RESTART (after our stop, or the racing fire that abandoned ours - the world changed either way).
        }
    }

    const bool isPA = object->isPreciseAllocation(); // I36: no 16B DCAS at 8-mod-16 bases.
    JSCellLock& cellLock = object->cellLock();

    // Capacities are structure-determined and stable while the structureID
    // check holds (re-verified under the lock each iteration).
    size_t aliasedOutOfLineCapacity = sourceStructure->outOfLineCapacity();
    size_t newOutOfLineCapacity = newStructureOrNull ? newStructureOrNull->outOfLineCapacity() : aliasedOutOfLineCapacity;
    RELEASE_ASSERT(newOutOfLineCapacity >= aliasedOutOfLineCapacity);
    RELEASE_ASSERT(!(newOutOfLineCapacity % butterflyFragmentSlots)); // C1 for the grown side too.
    bool hasIndexingHeader = sourceStructure->hasIndexingHeader(object);

    uint32_t aliasedOutOfLineFragments = aliasedOutOfLineFragmentCountForConversion(aliasedOutOfLineCapacity); // C1 RELEASE_ASSERT inside.
    uint32_t totalOutOfLineFragments = static_cast<uint32_t>(newOutOfLineCapacity / butterflyFragmentSlots);
    uint32_t freshOutOfLineFragments = totalOutOfLineFragments - aliasedOutOfLineFragments;

    while (true) {
        // Review round 4 (blocker fix): hold DeferGC across the WHOLE
        // allocate-to-publication window (O1's sanctioned pre-lock form, like
        // Task 8's unshiftCountSlowCase). Without it, a collection triggered by
        // a later allocation (or while parked on the cell lock) sweeps fresh
        // fragments that are reachable only from the heap-spilled
        // freshFragments buffer (Vector<,4> spills to fastMalloc beyond 4 -
        // invisible to the conservative scan) or only from the UNPUBLISHED
        // spine (a stack-pinned auxiliary cell's CONTENTS are never traced;
        // spines are visited only via the owning object after publication).
        // The destructor runs at each refit/return boundary, so deferred
        // collections make progress between attempts.
        DeferGC deferGC(vm);

        // ---- Step 1: allocate the spine (+ the fresh fragments the trigger
        // needs) OUTSIDE the lock (O1). Indexed fragment count is planned from
        // an unlocked read and re-validated under the lock (refit => back
        // here). Spines/fragments are GC-auxiliary allocations (I25); until
        // publication they are kept alive by the DeferGC above - stack reach
        // alone is NOT enough (see the round-4 note): fragment pointers spill
        // to fastMalloc and unpublished spine contents are never traced.
        uint32_t plannedVectorLength = 0;
        {
            uint64_t plannedWord = butterflyWordAtomic(object)->load(std::memory_order_seq_cst);
            ButterflyRegime regime = butterflyRegimeForWord(plannedWord);
            if (regime != ButterflyRegime::Flat && regime != ButterflyRegime::FlatShared)
                return nullptr; // RESTART: nothing flat to convert (None => N3 install path; Segmented => §4.3).
            if (hasIndexingHeader)
                plannedVectorLength = untaggedButterfly(plannedWord)->vectorLength();
        }
        uint32_t plannedIndexedFragments = aliasedIndexedFragmentCountForConversion(hasIndexingHeader, plannedVectorLength); // C2
        uint32_t allocatedTotalFragments = totalOutOfLineFragments + plannedIndexedFragments;

        ButterflySpine* spine = static_cast<ButterflySpine*>(
            vm.auxiliarySpace().allocate(vm, ButterflySpine::allocationSize(allocatedTotalFragments), nullptr, AllocationFailureMode::Assert));

        Vector<ButterflyFragment*, 4> freshFragments;
        freshFragments.reserveInitialCapacity(freshOutOfLineFragments);
        for (uint32_t j = 0; j < freshOutOfLineFragments; ++j) {
            auto* fragment = static_cast<ButterflyFragment*>(
                vm.auxiliarySpace().allocate(vm, sizeof(ButterflyFragment), nullptr, AllocationFailureMode::Assert));
            for (size_t slotIndex = 0; slotIndex < butterflyFragmentSlots; ++slotIndex)
                fragment->slots[slotIndex].clear(); // Beyond outOfLineSize: never value-visited (§4.5 step 4), cleared for safety.
            freshFragments.append(fragment);
        }

        // ---- Step 2: acquire the cell lock (L2; O3: the only cell lock we hold).
        lockCellChecked(cellLock); // O3/I20 depth witness (Task 10)

        bool refit = false;
        bool restart = false;
        ButterflySpine* published = nullptr;

        // ---- Step 3 (also the re-entry point of the §4.3 DCAS-failure
        // taxonomy's (c): "goto 3", including the refit escape).
        while (true) {
            uint32_t rawStructureIDBits = structureIDAtomic(object)->load(std::memory_order_seq_cst);
            if (rawStructureIDBits != sourceID.bits()) {
                // A locked transition (or anything else) changed the structure
                // in the window: RESTART (fresh target, fresh F1/F2 checks,
                // fresh allocation). I10b's re-check under the lock.
                restart = true;
                break;
            }
            uint64_t expectedWord = butterflyWordAtomic(object)->load(std::memory_order_seq_cst);
            if (isSegmentedButterfly(expectedWord)) {
                // A racing conversion completed before we locked: unlock,
                // retry segmented (the caller's re-dispatch lands on §4.3).
                restart = true;
                break;
            }
            if (!(expectedWord & butterflyPointerMask)) {
                restart = true; // Butterfly vanished (cannot happen today; defensive RESTART).
                break;
            }
            Butterfly* flat = untaggedButterfly(expectedWord);

            // Re-read flat vectorLength/publicLength under the lock. Flag-on
            // a published flat butterfly's vectorLength is IMMUTABLE (the T5
            // in-place growth was removed in review round 1; every bound
            // increase publishes fresh storage), and the word re-check above
            // pins the payload, so this read is stable until we unlock
            // (§4.2-3/history §16.1).
            uint32_t flatVectorLength = hasIndexingHeader ? flat->vectorLength() : 0;
            uint32_t indexedFragments = aliasedIndexedFragmentCountForConversion(hasIndexingHeader, flatVectorLength); // C2
            if (totalOutOfLineFragments + indexedFragments > allocatedTotalFragments) {
                // Counts no longer fit the step-1 spine: unlock, goto step 1.
                refit = true;
                break;
            }

            // ---- T3-segmented-born-fullcoverage: publish FULL COVERAGE
            // (vectorLength == indexedFragments*4 - 1) so the very first
            // tryGrowSegmentedVectorLength on this spine takes the lock-free
            // mode-(a) CAS instead of a mode-(b) per-event STW. The flag-on
            // optimalContiguousVectorLength alignment (ButterflyInlines.h)
            // sizes every fresh contiguous flat butterfly so (1+flatVL)%4 == 0
            // and there is NO C2 tail to begin with - that is the path the
            // scalebench Phase-A arrays take. The branch below additionally
            // covers butterflies sized by other paths whose size-class
            // rounding left tail slack: it hole-fills the aliased tail
            // [flatVectorLength, coveredVectorLength) ONLY when those bytes
            // provably lie inside the flat butterfly's heap cell
            // (availableContiguousVectorLength re-derives the cell's usable
            // VL from the same MarkedSpace::optimalSizeFor the creator's
            // allocator used; C3 + the structure-pinned outOfLineCapacity
            // make the recorded aliased size the exact requested size). The
            // unconditional fill the locksites note suggested would WRITE
            // PAST THE HEAP CELL whenever the creator went through
            // availableContiguousVectorLength itself (it back-computes VL to
            // fill the size class exactly - zero slack), so it is gated. The
            // fill races nothing: tail addresses are past flatVectorLength so
            // every §4.4 in-bounds store and every flat-side reader (both
            // C4-bounded by flat VL) stays clear, and fragment-0 slot-0 (the
            // I9b frozen flat-era VL high half + the live publicLength low
            // half) is untouched. When neither the alignment nor the slack
            // check applies the spine publishes flatVectorLength exactly as
            // before and the first grow falls into mode-(b) once - now a
            // residual rare path.
            uint32_t publishedVectorLength = flatVectorLength;
            if (hasIndexingHeader && indexedFragments) {
                uint32_t coveredVectorLength = indexedFragments * static_cast<uint32_t>(butterflyFragmentSlots) - 1;
                ASSERT(flatVectorLength <= coveredVectorLength);
                if (flatVectorLength < coveredVectorLength
                    && Butterfly::availableContiguousVectorLength(aliasedOutOfLineCapacity, flatVectorLength) >= coveredVectorLength) {
                    bool fillDouble = hasDouble(sourceStructure->indexingType()); // structureID re-checked above; I28 relabels change it.
                    uint64_t hole = fillDouble ? std::bit_cast<uint64_t>(PNaN) : static_cast<uint64_t>(JSValue::encode(JSValue()));
                    for (uint32_t i = flatVectorLength; i < coveredVectorLength; ++i) {
                        // Direct flat-address lane store (== spine->indexedSlot(i)
                        // by I8 once the spine is built). V7: relaxed atomic so
                        // post-publication segmented readers' relaxed lane loads
                        // pair; tsanPublish below imports the edge.
                        WTF::atomicStore(std::bit_cast<uint64_t*>(flat->pointer()) + i, hole, std::memory_order_relaxed);
                    }
                    publishedVectorLength = coveredVectorLength;
                } else if (flatVectorLength == coveredVectorLength)
                    publishedVectorLength = coveredVectorLength; // == flatVectorLength; the optimalContiguousVectorLength flag-on path.
            }

            // ---- Build the spine (private until publication; immutable
            // after - I6). Slice-aliasing per the §4.1 equations (I8);
            // aliased base/size recorded VERBATIM for GC (§4.5/I7 - the old
            // flat allocation's only references after publication are the
            // spine's interior fragment pointers).
            butterflyConcurrentStore(&spine->outOfLineFragmentCount, totalOutOfLineFragments); // V7: paired with the reader-side relaxed loads (Butterfly.h).
            butterflyConcurrentStore(&spine->indexedFragmentCount, indexedFragments);
            butterflyConcurrentStore(&spine->vectorLength, publishedVectorLength); // Authoritative live VL (== flatVectorLength, or full coverage per T3 above); the flat-era copy stays frozen in fragment 0 slot 0's high half (I9b) regardless.
            butterflyConcurrentStore(&spine->spineEpoch, 1u); // First spine this object publishes; replacements (§4.3-1/T2) copy + increment.
            butterflyConcurrentStore(&spine->aliasedAllocationBase, aliasedAllocationBaseForConversion(flat, 0, aliasedOutOfLineCapacity)); // C3 RELEASE_ASSERT inside.
            butterflyConcurrentStore(&spine->aliasedAllocationSize, aliasedAllocationSizeForConversion(aliasedOutOfLineCapacity, hasIndexingHeader, static_cast<size_t>(flatVectorLength) * sizeof(EncodedJSValue)));

            for (uint32_t j = 0; j < aliasedOutOfLineFragments; ++j)
                butterflyConcurrentStore(&spine->fragments()[j], aliasedOutOfLineFragmentForConversion(flat, j));
            for (uint32_t j = 0; j < freshOutOfLineFragments; ++j)
                butterflyConcurrentStore(&spine->fragments()[aliasedOutOfLineFragments + j], freshFragments[j]);
            for (uint32_t f = 0; f < indexedFragments; ++f)
                butterflyConcurrentStore(&spine->fragments()[totalOutOfLineFragments + f], aliasedIndexedFragmentForConversion(flat, f));

            if (!freshOutOfLineFragments)
                validateSpineAliasesFlatButterfly(spine, flat, 0, aliasedOutOfLineCapacity, hasIndexingHeader); // Full I8 sweep.
            else
                validatePartiallyAliasedSpine(spine, flat, aliasedOutOfLineCapacity, hasIndexingHeader);

            // ---- Step 4: the trigger adds a property => release-store its
            // value into the fragment slot BEFORE the type/structure publish
            // (M2/I9: a reader seeing the new StructureID sees the value).
            if (newStructureOrNull && offset != invalidOffset) {
                uint64_t outOfLineIndex = outOfLineButterflyIndex(offset);
                RELEASE_ASSERT(outOfLineIndex < static_cast<uint64_t>(butterflyFragmentSlots) * totalOutOfLineFragments); // I33 by construction.
                WriteBarrierBase<Unknown>* slot = spine->outOfLineSlot(static_cast<unsigned>(outOfLineIndex));
                reinterpret_cast<Atomic<uint64_t>*>(slot)->store(JSValue::encode(value), std::memory_order_release);
                if (verifyConcurrentButterflyEnabled()) [[unlikely]]
                    RELEASE_ASSERT(reinterpret_cast<Atomic<uint64_t>*>(slot)->load(std::memory_order_relaxed) == JSValue::encode(value)); // I9/M2 witness (Task 10): value lands BEFORE the type publish.
            }

            // ---- Step 5: nuke + publish under the §3.0 discipline.
            // Publication debug-assert (I11/E1, steps 0/3 made this so): never
            // publish (notTTLTID, 1) while transitionThreadLocal is valid.
            ASSERT(!sourceStructure->transitionThreadLocalIsStillValid());
            ASSERT(!newStructureOrNull || !newStructureOrNull->transitionThreadLocalIsStillValid());
            if (verifyConcurrentButterflyEnabled()) [[unlikely]] {
                // I10b/I11 witness (Task 10): the step-0 fire preceded this
                // lock and the step-3 RESTART re-check held - so no segmented
                // word is ever published under a still-valid set.
                RELEASE_ASSERT(!sourceStructure->transitionThreadLocalIsStillValid());
                RELEASE_ASSERT(!newStructureOrNull || !newStructureOrNull->transitionThreadLocalIsStillValid());
                RELEASE_ASSERT(!sourceStructure->writeThreadLocalIsStillValid()); // transitionThreadLocal fire implies writeThreadLocal (§5).
            }

            StructureID newStructureID = newStructureOrNull ? newStructureOrNull->id() : sourceID;
            spine->tsanPublish(); // V7: last pre-publication store done; pairs with tsanConsume() in every segmented* reader (Butterfly.h rationale).
            uint64_t spineWord = encodeSegmentedButterfly(spine); // (notTTLTID, SW=1, spine) - I3.

            // Nuke: 32-bit CAS structureID -> nuke(old) (M5). The lane holds
            // no volatile bytes and we own the semantic bytes under the lock
            // (E4 transitioners are excluded - the source's sets are fired -
            // so failure is a logic error, §3.0 step 4).
            uint32_t previousIDBits = structureIDAtomic(object)->compareExchangeStrong(sourceID.bits(), sourceID.nuke().bits());
            RELEASE_ASSERT(previousIDBits == sourceID.bits());

            bool reenterStep3 = false;

            if (!isPA) {
                // 128-bit DCAS {nuked header, expected flat word} ->
                // {new un-nuked header, spine word}; seq_cst (M3).
                uint64_t nukedHeader = cellHeaderAtomic(object)->load(std::memory_order_seq_cst); // Freshest volatile bytes; ID lane nuked by us.
                CellHeaderAndButterfly expected { nukedHeader, expectedWord };
                CellHeaderAndButterfly desired { headerForPublication(nukedHeader, newStructureID, newStructureOrNull), spineWord };

                while (!dcasHeaderAndButterfly(object, expected, desired)) {
                    // §4.3 DCAS-failure taxonomy (exhaustive); re-read both words.
                    uint64_t freshHeader = cellHeaderAtomic(object)->load(std::memory_order_seq_cst);
                    uint64_t freshWord = butterflyWordAtomic(object)->load(std::memory_order_seq_cst);

                    // (d) guards: the cell lock excludes competing conversions
                    // and locked transitions; the nuked ID excludes lock-free
                    // SW DCASes completing against the header (§3.0 abandons on
                    // semantic divergence). Any semantic header change here is
                    // a logic error.
                    RELEASE_ASSERT(!isSegmentedButterfly(freshWord));
                    RELEASE_ASSERT(headerDiffersOnlyInVolatileBits(expected.header, freshHeader));

                    if (untaggedButterfly(freshWord) != untaggedButterfly(expected.taggedButterfly)) {
                        // (c) A §4.4 array-resize CAS won (I16/I17 - element
                        // resizes touch only the butterfly word, lock-free).
                        // Un-nuke and goto step 3: recompute the slices, the
                        // C2 counts and the aliased base/size against the new
                        // flat butterfly (refit escape included). Never
                        // republish an older payload (I27b).
                        structureIDAtomic(object)->store(sourceID.bits(), std::memory_order_seq_cst);
                        reenterStep3 = true;
                        break;
                    }

                    // (b1) SW flip by a lock-free foreign first-writer (F1):
                    // monotone - merge it into the expected word; our desired
                    // payload is a spine and already carries SW=1. The TID
                    // cannot change without a payload change.
                    RELEASE_ASSERT(butterflyTID(freshWord) == butterflyTID(expected.taggedButterfly));
                    expected.taggedButterfly = freshWord;

                    // (a) Volatile header bytes (cellState GC CAS, lock parked
                    // bit) changed: fold the freshest values into expected AND
                    // desired (I26), retry.
                    expected.header = mergeVolatileHeaderBits(expected.header, freshHeader);
                    desired.header = mergeVolatileHeaderBits(desired.header, freshHeader);
                }
                if (!reenterStep3)
                    published = spine;
            } else {
                // I36: PreciseAllocation cells sit at 8-mod-16 addresses - the
                // 16B DCAS would fault. Publication is the M8 fenced nuke
                // order under the cell lock: structureID CAS -> nuke (done
                // above); fence; publish the 64-bit butterfly word (8B-aligned
                // - legal); fence; store the new structureID. The butterfly
                // word still goes in by CAS (I17: lock-free §4.4 element
                // resizes race even under our lock).
                WTF::storeStoreFence();

                while (true) {
                    uint64_t previousWord = butterflyWordAtomic(object)->compareExchangeStrong(expectedWord, spineWord, std::memory_order_seq_cst);
                    if (previousWord == expectedWord) {
                        published = spine;
                        break;
                    }
                    RELEASE_ASSERT(!isSegmentedButterfly(previousWord)); // (d): impossible under the lock (PA SW flips are cell-locked too, I36).
                    if (untaggedButterfly(previousWord) != untaggedButterfly(expectedWord)) {
                        // (c) §4.4 array CAS won: un-nuke, goto step 3.
                        structureIDAtomic(object)->store(sourceID.bits(), std::memory_order_seq_cst);
                        reenterStep3 = true;
                        break;
                    }
                    RELEASE_ASSERT(butterflyTID(previousWord) == butterflyTID(expectedWord));
                    expectedWord = previousWord; // (b1) SW merged; retry.
                }

                if (published) {
                    if (newStructureOrNull) {
                        // Remaining semantic header bytes, as JSCell::setStructure
                        // would write them (we cannot name the protected members
                        // from here; lanes are static_asserted in the header).
                        auto* cellBytes = reinterpret_cast<uint8_t*>(static_cast<JSCell*>(object));
                        auto* typeByte = reinterpret_cast<Atomic<uint8_t>*>(cellBytes + JSCell::typeInfoTypeOffset());
                        auto* flagsByte = reinterpret_cast<Atomic<uint8_t>*>(cellBytes + JSCell::typeInfoFlagsOffset());
                        auto* indexingByte = reinterpret_cast<Atomic<uint8_t>*>(cellBytes + JSCell::indexingTypeAndMiscOffset());

                        // Round 4: CAS-merge - the per-cell bit lane is volatile (lock-free setPerCellBit).
                        while (true) {
                            uint8_t oldFlags = flagsByte->load(std::memory_order_relaxed);
                            uint8_t newFlags = TypeInfo::mergeInlineTypeFlags(newStructureOrNull->typeInfo().inlineTypeFlags(), oldFlags);
                            if (oldFlags == newFlags || flagsByte->compareExchangeWeak(oldFlags, newFlags, std::memory_order_seq_cst))
                                break;
                        }
                        typeByte->store(static_cast<uint8_t>(newStructureOrNull->typeInfo().type()), std::memory_order_relaxed);
                        // The indexing byte's lock bits (0xC0; we HOLD the lock
                        // bit) and any concurrent parked-bit flip are volatile:
                        // CAS-merge exactly like JSCell::setStructure.
                        while (true) {
                            uint8_t oldValue = indexingByte->load(std::memory_order_relaxed);
                            uint8_t newValue = static_cast<uint8_t>((oldValue & ~AllArrayTypesAndHistory) | newStructureOrNull->indexingModeIncludingHistory());
                            if (oldValue == newValue)
                                break;
                            if (indexingByte->compareExchangeWeak(oldValue, newValue, std::memory_order_seq_cst))
                                break;
                        }
                    }
                    WTF::storeStoreFence();
                    structureIDAtomic(object)->store(newStructureID.bits(), std::memory_order_seq_cst); // Un-nuked new ID; readers stop spinning (M5).
                }
            }

            if (reenterStep3)
                continue; // §4.3(c)'s "goto 3" - re-read everything under the still-held lock.
            break;
        }

        // ---- Step 6: release the cell lock.
        unlockCellChecked(cellLock);

        if (published) {
            // Barriers (§4.5): spine/butterfly publication barriers the object
            // like setButterfly; the step-4 value store barriers its value.
            // Emitted after unlock (no poll between publication and here).
            vm.writeBarrier(object);
            if (newStructureOrNull) {
                vm.writeBarrier(object, newStructureOrNull);
                if (offset != invalidOffset)
                    vm.writeBarrier(object, value);
            }
            return published;
        }
        if (restart)
            return nullptr; // RESTART: caller re-enters from §2 dispatch, lock-free.
        ASSERT(refit);
        // Refit (counts grew): the step-1 allocations are discarded
        // unreferenced (GC reclaims them); allocate again, unlocked (O1).
    }
}

// ===== Task 6: §4.3 transition protocol, N2, and the §9.5 *Concurrent accessors =====

namespace {

// The object's settled (un-nuked) structure: spin past a mid-publication nuke
// (M5; the nuke window is bounded, straight-line - O2), then decode.
Structure* settledStructure(JSCell* cell)
{
    while (true) {
        StructureID id = cell->structureID(); // RAW bits (M5)
        if (!id.isNuked()) [[likely]]
            return id.decode();
    }
}

ALWAYS_INLINE bool anyTTLSetStillValid(Structure* source, Structure* target)
{
    if (source->transitionThreadLocalIsStillValid() || source->writeThreadLocalIsStillValid())
        return true;
    return target && (target->transitionThreadLocalIsStillValid() || target->writeThreadLocalIsStillValid());
}

// F2: fire BOTH TTL sets on the source and the transition target, in ONE
// §10.6 stop, BEFORE any cell-lock acquisition (I10b/I13). The closure
// allocates nothing (O4). fireTransitionThreadLocal also fires writeThreadLocal
// and applies the F4 chain-fire (Structure.cpp, Task 3). Callers RESTART after
// the stop returns (§4.2 rule).
void fireTTLSetsForSharedTransition(VM& vm, Structure* source, Structure* target, const char* reason)
{
    // STW dedup (T1-butterfly-stw-growth): the sets are monotone, so when W
    // threads race the same fire only the claim winner pays the stop; losers
    // park-wait until the sets read invalid and return stop-free. Every
    // caller RESTARTs unconditionally after this returns, so "the racer
    // fired" and "we fired" are indistinguishable to them.
    PerEventStopClaim claim(vm, source, [&] { return !anyTTLSetStillValid(source, target); });
    if (!claim.acquired() || !anyTTLSetStillValid(source, target))
        return;
    jsThreadsStopTheWorldAndRun(vm, scopedLambda<void()>([&] {
        // Re-check inside the stop: a racing fire may have got here first.
        if (source->transitionThreadLocalIsStillValid() || source->writeThreadLocalIsStillValid())
            source->fireTransitionThreadLocal(vm, reason);
        if (target && target != source
            && (target->transitionThreadLocalIsStillValid() || target->writeThreadLocalIsStillValid()))
            target->fireTransitionThreadLocal(vm, reason);
    }));
}

enum class TransitionFlavor : uint8_t {
    FirstInstall, // §2.1 N3: butterfly word 0 -> first flat butterfly, tag (currentButterflyTID(), 0)
    StayFlat, // §3: owner, SW=0, E4 failed - locked flat transition (reuse or copy-grow)
    StayFlatShared, // §4.2 noseg-property-only: foreign/SW transition on a property-only flat butterfly with no capacity growth - locked reuse, publish (installerTID, SW=1)
    Segmented, // §4.3 proper: spine reuse or replacement spine
};

} // anonymous namespace

// §4.3 restartable core (see the header comment for the full contract).
// Protocol: (1) allocate everything outside the lock (O1); (2) acquire the
// cell lock (L1; O3); (3) re-read header + tagged word, RESTART on structureID
// divergence, refit (unlock, goto 1) when the allocation no longer fits,
// recompute the target slot; (4) release-store the new property's value
// (M2/I9: value before type); (5) nuke + publish {new header, new tagged
// butterfly} via the 128-bit DCAS (PA cells: I36/M8 fenced order) with the
// DCAS-failure taxonomy (a)-(d); (6) release the cell lock.
bool trySegmentedTransition(VM& vm, JSObjectWithButterfly* object, Structure* expectedSource, Structure* newStructure, PropertyOffset offset, JSValue value)
{
    RELEASE_ASSERT(Options::useJSThreads());
    ASSERT(vm.currentThreadIsHoldingAPILock());
    ASSERT(expectedSource && newStructure);
    RELEASE_ASSERT(offset == invalidOffset || isOutOfLineOffset(offset)); // Inline adds are N2 (tryStructureOnlyTransition).

    StructureID sourceID = object->structureID(); // RAW bits (M5).
    if (sourceID.isNuked() || sourceID != expectedSource->id())
        return false; // RESTART: a racing publication is mid-flight or the source settled away.
    Structure* source = expectedSource;

    // I31: ArrayStorage never segments and its shared transitions are
    // per-event STW that publish FLAT (§4.6; Task 8) - the caller routes AS
    // shapes to tryArrayStoragePropertyTransition (FUZZ r3-001), so reaching
    // here with an AS source is a routing bug. I35: CoW materializes a private
    // flat butterfly (§4.8; Task 7) before any §3/§4 protocol.
    RELEASE_ASSERT(!hasAnyArrayStorage(source->indexingType()));
    RELEASE_ASSERT(!hasAnyArrayStorage(newStructure->indexingType()));
    RELEASE_ASSERT(!isCopyOnWrite(source->indexingMode()));

    // ---- Step 0: F2 firing, before any lock (I10b/I13). Trigger taxonomy
    // (§5 F2, per-object keying): butterfly-bearing instance - foreign tag TID,
    // or owner with SW=1 (segmented words always trigger: TID==notTTLTID);
    // butterfly-less instance - thread != the structure's N1 transition TID.
    uint64_t planningWord = butterflyWordAtomic(object)->load(std::memory_order_seq_cst);
    {
        bool trigger;
        if (!(planningWord & butterflyPointerMask))
            trigger = currentButterflyTID() != source->transitionThreadLocalTID();
        else
            trigger = butterflyTID(planningWord) != currentButterflyTID() || butterflySharedWrite(planningWord);
        if (trigger && anyTTLSetStillValid(source, newStructure)) {
            fireTTLSetsForSharedTransition(vm, source, newStructure, "F2: shared transition (§4.3)");
            return false; // RESTART after the stop (§4.2 rule).
        }
    }

    size_t oldOutOfLineCapacity = source->outOfLineCapacity();
    size_t newOutOfLineCapacity = newStructure->outOfLineCapacity();
    RELEASE_ASSERT(newOutOfLineCapacity >= oldOutOfLineCapacity); // Live storage never shrinks (I18/I30; deletes quarantine).
    bool hasIndexingHeader = source->hasIndexingHeader(object);
    RELEASE_ASSERT(newStructure->hasIndexingHeader(object) == hasIndexingHeader); // Header-ness changes re-fragment (C2): §4.4 / Task 8.

    // §4.2 noseg-property-only gate (SCALEBENCH §44 / MAP-BIMODAL-EVIDENCE.md
    // root cause (g)): a foreign-TID or SW=1 property transition on a Flat
    // butterfly normally converts (§4.2) and the object stays Segmented for
    // life - which makes DFG compileGetButterfly's segmented predicate
    // (DFGSpeculativeJIT.cpp:12086) BadIndexingType-exit forever on every
    // out-of-line GetById against it (handleGetById doesn't consult that exit
    // site). When the butterfly carries NO indexed storage and the transition
    // does not grow outOfLineCapacity, segmentation buys nothing: the slot
    // already exists in the live flat allocation, fragments would alias the
    // exact same bytes, and there is no concurrent indexed growth to enable.
    // So a foreign/SW transitioner can take the SAME locked reuse path the
    // owner StayFlat case takes - release-store the value into the existing
    // slot, then nuke + DCAS {new structure, (installerTID, SW=1)} - and keep
    // the butterfly Flat. The R7 read protocol (structureID re-load + address-
    // dependent butterfly load + slot load) is satisfied by the same M2/M5
    // ordering the owner StayFlat reuse path already relies on (the value
    // release-store happens-before the seq_cst DCAS that publishes the un-
    // nuked new ID; the butterfly payload is unchanged so every reader's slot
    // address is data-dependent on the SAME word it always was). I12 holds
    // because the step-0 F2 fire (above) invalidated writeThreadLocal on the
    // source AND target before any (installerTID, SW=1) publication.
    // GIL-off only (the perf hazard is a multi-mutator reify race; under the
    // GIL the conversion path is reached but the recompile loop never bites at
    // scale, and keeping it unchanged preserves the GIL-on corpus baseline).
    const bool canStayFlatShared = !Options::useThreadGIL()
        && !hasIndexingHeader
        && newOutOfLineCapacity == oldOutOfLineCapacity
        && source->indexingModeIncludingHistory() == newStructure->indexingModeIncludingHistory()
        && !forceSegmentedButterfliesEnabled();

    // §3 dispatch: a FLAT instance transitioned by a foreign thread, or by the
    // owner with SW=1, converts (§4.2) and goes segmented (I10) - UNLESS the
    // noseg-property-only gate above holds, in which case it falls through to
    // the StayFlatShared flavor below. The conversion is its own protocol (it
    // takes the cell lock itself; b2's prohibition is structural - never
    // convert holding the lock).
    {
        ButterflyRegime planningRegime = butterflyRegimeForWord(planningWord);
        // §9.6 (Task 10): forceSegmentedButterflies makes EVERY flat transition
        // take the conversion route - owner stay-flat included - so the
        // segmented machinery runs on single-threaded workloads. The owner case
        // is sound the same way the foreign case is: §4.2's step 0 fires the
        // still-valid TTL sets (monotone) before publishing (notTTLTID, 1).
        if ((planningRegime == ButterflyRegime::Flat || planningRegime == ButterflyRegime::FlatShared)
            && (butterflyTID(planningWord) != currentButterflyTID() || butterflySharedWrite(planningWord)
                || forceSegmentedButterfliesEnabled())
            && !canStayFlatShared) {
            if (source->indexingModeIncludingHistory() == newStructure->indexingModeIncludingHistory())
                return !!convertToSegmentedButterfly(vm, object, source, newStructure, offset, value);
            // T4 indexing-shape transition on a shared flat object: convert in
            // place first (§4.2 nullptr form), then RESTART - the re-dispatch
            // lands back here with the object segmented and runs full §4.3.
            convertToSegmentedButterfly(vm, object, nullptr, nullptr, invalidOffset, JSValue());
            return false;
        }
    }

    const bool isPA = object->isPreciseAllocation(); // I36
    JSCellLock& cellLock = object->cellLock();

    while (true) {
        // Review round 4 (blocker fix): DeferGC across the allocate-to-
        // publication window - same rationale as convertToSegmentedButterfly's
        // round-4 note (heap-spilled freshFragments and unpublished
        // replacementSpine contents are invisible to the conservative scan, so
        // a GC triggered by a later allocation or a cell-lock park would sweep
        // already-allocated fragments).
        DeferGC deferGC(vm);

        // ---- Step 1: plan from an unlocked read; allocate everything (O1).
        if (structureIDAtomic(object)->load(std::memory_order_seq_cst) != sourceID.bits())
            return false; // RESTART: source settled away while planning.
        planningWord = butterflyWordAtomic(object)->load(std::memory_order_seq_cst);
        ButterflyRegime planningRegime = butterflyRegimeForWord(planningWord);

        TransitionFlavor flavor = TransitionFlavor::FirstInstall;
        Butterfly* newButterfly = nullptr;
        ButterflySpine* replacementSpine = nullptr;
        Vector<ButterflyFragment*, 4> freshFragments;
        uint32_t plannedVectorLength = 0;
        uint32_t plannedIndexedFragments = 0;
        uint32_t neededOutOfLineFragments = 0;
        uint32_t planningSpineOutOfLineFragments = 0;

        switch (planningRegime) {
        case ButterflyRegime::None: {
            flavor = TransitionFlavor::FirstInstall;
            RELEASE_ASSERT(!hasIndexingHeader); // The header lives in a butterfly; None has none.
            RELEASE_ASSERT(newOutOfLineCapacity); // Butterfly-untouched reshapes are N2.
            RELEASE_ASSERT(!(newOutOfLineCapacity % butterflyFragmentSlots)); // GT#4 capacity granularity (C1 for any later conversion).
            newButterfly = Butterfly::createUninitialized(vm, object, 0, newOutOfLineCapacity, false, 0);
            for (size_t k = 0; k < newOutOfLineCapacity; ++k)
                (newButterfly->propertyStorage() - (k + 1))->clear();
            break;
        }
        case ButterflyRegime::Flat:
        case ButterflyRegime::FlatShared: {
            // Owner, SW=0 stay-flat path (§3); foreign/SW=1 was dispatched to
            // §4.2 above (or fell through to StayFlatShared when the noseg-
            // property-only gate held) - if the word changed since, RESTART
            // re-dispatches.
            if (butterflyTID(planningWord) != currentButterflyTID() || butterflySharedWrite(planningWord)) {
                if (!canStayFlatShared)
                    return false;
                // §4.2 noseg-property-only: locked reuse of the existing flat
                // butterfly; no allocation (the slot already exists -
                // newOutOfLineCapacity == oldOutOfLineCapacity by the gate).
                flavor = TransitionFlavor::StayFlatShared;
                break;
            }
            if (forceSegmentedButterfliesEnabled()) [[unlikely]]
                return false; // §9.6 (Task 10): stay-flat suppressed - RESTART; the re-dispatch routes through the §3 conversion block above.
            flavor = TransitionFlavor::StayFlat;
            // Flat owner shape changes ride today's array-conversion paths
            // (§4.4 T4 is for shared objects; Task 8) - not this entry point.
            RELEASE_ASSERT(source->indexingModeIncludingHistory() == newStructure->indexingModeIncludingHistory());
            if (hasIndexingHeader)
                plannedVectorLength = untaggedButterfly(planningWord)->vectorLength();
            if (newOutOfLineCapacity > oldOutOfLineCapacity) {
                newButterfly = Butterfly::createUninitialized(vm, object, 0, newOutOfLineCapacity, hasIndexingHeader,
                    static_cast<size_t>(plannedVectorLength) * sizeof(EncodedJSValue));
            }
            break;
        }
        case ButterflyRegime::Segmented: {
            flavor = TransitionFlavor::Segmented;
            // §4.7/I28: shape changes TOUCHING Double on a shared object are
            // per-event STW relabels (Tasks 7/8), never this locked protocol.
            RELEASE_ASSERT(hasDouble(source->indexingType()) == hasDouble(newStructure->indexingType()));
            ButterflySpine* planningSpine = butterflySpine(planningWord);
            planningSpine->tsanConsume(); // V7: published spine.
            planningSpineOutOfLineFragments = planningSpine->outOfLineFragmentCountConcurrent();
            plannedIndexedFragments = planningSpine->indexedFragmentCountConcurrent();
            RELEASE_ASSERT(!(newOutOfLineCapacity % butterflyFragmentSlots)); // C1
            neededOutOfLineFragments = static_cast<uint32_t>(newOutOfLineCapacity / butterflyFragmentSlots);
            if (neededOutOfLineFragments > planningSpineOutOfLineFragments) {
                // §4.3 step 1: new spine = copy + append; fragments are never
                // moved or reused; aliased base/size copied VERBATIM under the
                // lock (I7). Fields/pointers are filled in at step 3 from the
                // spine re-read under the lock.
                replacementSpine = static_cast<ButterflySpine*>(vm.auxiliarySpace().allocate(
                    vm, ButterflySpine::allocationSize(neededOutOfLineFragments + plannedIndexedFragments), nullptr, AllocationFailureMode::Assert));
                uint32_t freshCount = neededOutOfLineFragments - planningSpineOutOfLineFragments;
                freshFragments.reserveInitialCapacity(freshCount);
                for (uint32_t j = 0; j < freshCount; ++j) {
                    auto* fragment = static_cast<ButterflyFragment*>(
                        vm.auxiliarySpace().allocate(vm, sizeof(ButterflyFragment), nullptr, AllocationFailureMode::Assert));
                    for (size_t slotIndex = 0; slotIndex < butterflyFragmentSlots; ++slotIndex)
                        fragment->slots[slotIndex].clear(); // Beyond outOfLineSize: never value-visited (§4.5 step 4).
                    freshFragments.append(fragment);
                }
            }
            break;
        }
        }

        // ---- Step 2: acquire the cell lock (L1; O3: the only cell lock held).
        lockCellChecked(cellLock); // O3/I20 depth witness (Task 10)

        bool restart = false;
        bool refit = false;
        bool published = false;
        bool storedValue = false;

        while (true) {
            // ---- Step 3 (re-entered by taxonomy (c)'s "goto 3").
            uint32_t rawStructureIDBits = structureIDAtomic(object)->load(std::memory_order_seq_cst);
            if (rawStructureIDBits != sourceID.bits()) {
                restart = true; // RESTART (§4.2 rule; I10b re-check under the lock).
                break;
            }
            uint64_t expectedWord = butterflyWordAtomic(object)->load(std::memory_order_seq_cst);
            ButterflyRegime regime = butterflyRegimeForWord(expectedWord);

            uint64_t desiredWord = 0;
            bool desiredPayloadIsFreshFlatCopy = false;
            storedValue = false;

            if (flavor == TransitionFlavor::FirstInstall) {
                if (regime != ButterflyRegime::None) {
                    refit = true; // A racing install won between planning and the lock: replan.
                    break;
                }
                if (offset != invalidOffset) {
                    unsigned outOfLineIndex = outOfLineButterflyIndex(offset);
                    RELEASE_ASSERT(outOfLineIndex < newOutOfLineCapacity);
                    reinterpret_cast<Atomic<uint64_t>*>(newButterfly->propertyStorage() - (outOfLineIndex + 1))->store(JSValue::encode(value), std::memory_order_release); // Step 4 (M2/I9)
                    storedValue = true;
                }
                desiredWord = encodeButterfly(newButterfly, currentButterflyTID(), false); // N3: installer's TID, SW=0.
            } else if (flavor == TransitionFlavor::StayFlat) {
                if (regime != ButterflyRegime::Flat && regime != ButterflyRegime::FlatShared) {
                    refit = true;
                    break;
                }
                if (butterflyTID(expectedWord) != currentButterflyTID()) {
                    restart = true; // Ownership lost: re-dispatch.
                    break;
                }
                if (butterflySharedWrite(expectedWord)) {
                    restart = true; // §3: owner with SW=1 => convert (§4.2); never under this lock.
                    break;
                }
                Butterfly* flat = untaggedButterfly(expectedWord);
                uint32_t flatVectorLength = hasIndexingHeader ? flat->vectorLength() : 0;
                if (newButterfly && flatVectorLength != plannedVectorLength) {
                    refit = true; // Indexing payload was resized between planning and the lock.
                    break;
                }
                Butterfly* storage = flat;
                if (newButterfly) {
                    // Copy the whole live allocation (out-of-line slots +
                    // IndexingHeader + elements; raw memcpy is fine for Double
                    // shapes - 8-byte lanes). SW=0 means no foreign store has
                    // completed; a SW flip AFTER this copy fails the DCAS and
                    // is taxonomy (b2) - the copy is never published then.
                    size_t bytesToCopy = Butterfly::totalSize(0, oldOutOfLineCapacity, hasIndexingHeader,
                        static_cast<size_t>(flatVectorLength) * sizeof(EncodedJSValue));
                    char* sourceBase = static_cast<char*>(flat->base(0, oldOutOfLineCapacity));
                    char* destinationBase = static_cast<char*>(newButterfly->base(0, newOutOfLineCapacity))
                        + sizeof(EncodedJSValue) * (newOutOfLineCapacity - oldOutOfLineCapacity);
                    memcpy(destinationBase, sourceBase, bytesToCopy);
                    for (size_t k = oldOutOfLineCapacity; k < newOutOfLineCapacity; ++k)
                        (newButterfly->propertyStorage() - (k + 1))->clear();
                    storage = newButterfly;
                    desiredPayloadIsFreshFlatCopy = true;
                }
                if (offset != invalidOffset) {
                    unsigned outOfLineIndex = outOfLineButterflyIndex(offset);
                    RELEASE_ASSERT(outOfLineIndex < newOutOfLineCapacity);
                    reinterpret_cast<Atomic<uint64_t>*>(storage->propertyStorage() - (outOfLineIndex + 1))->store(JSValue::encode(value), std::memory_order_release); // Step 4 (M2/I9)
                    storedValue = true;
                }
                desiredWord = encodeButterfly(storage, currentButterflyTID(), false); // Stays (t, 0) - I27-compatible.
            } else if (flavor == TransitionFlavor::StayFlatShared) {
                if (regime != ButterflyRegime::Flat && regime != ButterflyRegime::FlatShared) {
                    restart = true; // Someone converted (or removed) it: re-dispatch onto §4.3/None.
                    break;
                }
                ASSERT(canStayFlatShared);
                ASSERT(!hasIndexingHeader && newOutOfLineCapacity == oldOutOfLineCapacity);
                Butterfly* flat = untaggedButterfly(expectedWord);
                if (offset != invalidOffset) {
                    unsigned outOfLineIndex = outOfLineButterflyIndex(offset);
                    RELEASE_ASSERT(outOfLineIndex < newOutOfLineCapacity);
                    reinterpret_cast<Atomic<uint64_t>*>(flat->propertyStorage() - (outOfLineIndex + 1))->store(JSValue::encode(value), std::memory_order_release); // Step 4 (M2/I9)
                    storedValue = true;
                }
                // Preserve the original installer's TID (we are NOT taking
                // ownership); set SW=1 - this IS a foreign/shared write. The
                // payload is the SAME flat allocation, so I27 (no payload-
                // preserving tag-only flips outside ensureSharedWriteBit) is
                // not violated: this is the §3.0 SW DCAS plus a structure
                // publish, fused into one nuke+DCAS under the cell lock.
                desiredWord = encodeButterfly(flat, butterflyTID(expectedWord), true);
                // I12/I11: F2 fired at step 0 (foreign/SW => trigger=true), so
                // both writeThreadLocal and transitionThreadLocal are invalid on
                // the source AND target before any (installerTID, SW=1)
                // publication.
                ASSERT(!source->transitionThreadLocalIsStillValid());
                ASSERT(!newStructure->transitionThreadLocalIsStillValid());
                ASSERT(!source->writeThreadLocalIsStillValid());
                ASSERT(!newStructure->writeThreadLocalIsStillValid());
                if (verifyConcurrentButterflyEnabled()) [[unlikely]] {
                    RELEASE_ASSERT(!source->transitionThreadLocalIsStillValid());
                    RELEASE_ASSERT(!newStructure->transitionThreadLocalIsStillValid());
                    RELEASE_ASSERT(!source->writeThreadLocalIsStillValid());
                    RELEASE_ASSERT(!newStructure->writeThreadLocalIsStillValid());
                }
            } else {
                if (regime != ButterflyRegime::Segmented) {
                    refit = true;
                    break;
                }
                ButterflySpine* currentSpine = butterflySpine(expectedWord);
                currentSpine->tsanConsume(); // V7: published spine; import the publisher's clock.
                // The out-of-line fragment count is capacity-derived and the
                // capacity is structure-determined: stable while the
                // structureID check above holds.
                ASSERT(currentSpine->outOfLineFragmentCountConcurrent() == planningSpineOutOfLineFragments);
                uint32_t currentOutOfLineFragments = currentSpine->outOfLineFragmentCountConcurrent();
                uint32_t currentIndexedFragments = currentSpine->indexedFragmentCountConcurrent();
                bool needGrow = neededOutOfLineFragments > currentOutOfLineFragments;
                ButterflySpine* targetSpine = currentSpine;
                if (needGrow) {
                    if (!replacementSpine || currentIndexedFragments > plannedIndexedFragments) {
                        refit = true; // A racing T2 grew the right side: the step-1 spine is too small.
                        break;
                    }
                    butterflyConcurrentStore(&replacementSpine->outOfLineFragmentCount, neededOutOfLineFragments); // V7: paired with reader-side relaxed loads.
                    butterflyConcurrentStore(&replacementSpine->indexedFragmentCount, currentIndexedFragments);
                    butterflyConcurrentStore(&replacementSpine->vectorLength, currentSpine->vectorLengthConcurrent());
                    butterflyConcurrentStore(&replacementSpine->spineEpoch, butterflyConcurrentLoad(&currentSpine->spineEpoch) + 1);
                    butterflyConcurrentStore(&replacementSpine->aliasedAllocationBase, butterflyConcurrentLoad(&currentSpine->aliasedAllocationBase)); // VERBATIM (I7)
                    butterflyConcurrentStore(&replacementSpine->aliasedAllocationSize, butterflyConcurrentLoad(&currentSpine->aliasedAllocationSize)); // VERBATIM (I7)
                    for (uint32_t j = 0; j < currentOutOfLineFragments; ++j)
                        butterflyConcurrentStore(&replacementSpine->fragments()[j], currentSpine->outOfLineFragment(j));
                    for (uint32_t j = 0; j < neededOutOfLineFragments - currentOutOfLineFragments; ++j)
                        butterflyConcurrentStore(&replacementSpine->fragments()[currentOutOfLineFragments + j], freshFragments[j]);
                    for (uint32_t f = 0; f < currentIndexedFragments; ++f)
                        butterflyConcurrentStore(&replacementSpine->fragments()[neededOutOfLineFragments + f], currentSpine->indexedFragment(f));
                    replacementSpine->validateConsistency();
                    targetSpine = replacementSpine;
                }
                if (offset != invalidOffset) {
                    unsigned outOfLineIndex = outOfLineButterflyIndex(offset);
                    RELEASE_ASSERT(static_cast<uint64_t>(outOfLineIndex) < static_cast<uint64_t>(butterflyFragmentSlots) * targetSpine->outOfLineFragmentCountConcurrent()); // I33 by construction.
                    reinterpret_cast<Atomic<uint64_t>*>(targetSpine->outOfLineSlot(outOfLineIndex))->store(JSValue::encode(value), std::memory_order_release); // Step 4 (M2/I9)
                    if (verifyConcurrentButterflyEnabled()) [[unlikely]]
                        RELEASE_ASSERT(reinterpret_cast<Atomic<uint64_t>*>(targetSpine->outOfLineSlot(outOfLineIndex))->load(std::memory_order_relaxed) == JSValue::encode(value)); // I9/M2 witness (Task 10).
                    storedValue = true;
                }
                targetSpine->tsanPublish(); // V7: last pre-publication store done (no-op re-annotation when targetSpine == currentSpine).
                desiredWord = encodeSegmentedButterfly(targetSpine); // (notTTLTID, 1) - I3.
                // Publication debug-assert (I11/E1): never publish
                // (notTTLTID, 1) while transitionThreadLocal is valid.
                ASSERT(!source->transitionThreadLocalIsStillValid());
                ASSERT(!newStructure->transitionThreadLocalIsStillValid());
                if (verifyConcurrentButterflyEnabled()) [[unlikely]] {
                    // I10b/I11 witness (Task 10) - as in §4.2 step 5.
                    RELEASE_ASSERT(!source->transitionThreadLocalIsStillValid());
                    RELEASE_ASSERT(!newStructure->transitionThreadLocalIsStillValid());
                }
            }

            // ---- Step 5: nuke + publish under the §3.0 discipline (M5/M3).
            // The semantic structureID lane is lock-owned here (E4
            // transitioners are excluded: either the sets are fired, or we ARE
            // the single owner thread; the indexed installers — AB18-S3,
            // createInitialIndexedStorageConcurrent / createArrayStorageConcurrent —
            // publish either under this same cell lock, under a §10.6 stop, or
            // inside an owner poll-free sets-valid window, all of which exclude
            // this locked window), so the nuke CAS must succeed.
            uint32_t previousIDBits = structureIDAtomic(object)->compareExchangeStrong(sourceID.bits(), sourceID.nuke().bits());
            RELEASE_ASSERT(previousIDBits == sourceID.bits());

            StructureID newStructureID = newStructure->id();
            bool reenterStep3 = false;

            if (!isPA) {
                uint64_t nukedHeader = cellHeaderAtomic(object)->load(std::memory_order_seq_cst); // Freshest volatile bytes; ID lane nuked by us.
                CellHeaderAndButterfly expected { nukedHeader, expectedWord };
                CellHeaderAndButterfly desired { headerForPublication(nukedHeader, newStructureID, newStructure), desiredWord };

                while (!dcasHeaderAndButterfly(object, expected, desired)) {
                    // §4.3 DCAS-failure taxonomy (exhaustive); re-read both words.
                    uint64_t freshHeader = cellHeaderAtomic(object)->load(std::memory_order_seq_cst);
                    uint64_t freshWord = butterflyWordAtomic(object)->load(std::memory_order_seq_cst);

                    if (untaggedButterfly(freshWord) != untaggedButterfly(expected.taggedButterfly)) {
                        // (c) A §4.4 butterfly-pointer CAS won (lock-free
                        // element resize / replacement spine - I16/I17):
                        // un-nuke and goto step 3 (recompute target slot,
                        // refit escape included). Never republish an older
                        // payload (I27b).
                        structureIDAtomic(object)->store(sourceID.bits(), std::memory_order_seq_cst);
                        reenterStep3 = true;
                        break;
                    }
                    if ((freshWord ^ expected.taggedButterfly) & butterflyTagMask) {
                        // (b) SW flip by a lock-free foreign first-writer (F1).
                        RELEASE_ASSERT(butterflyTID(freshWord) == butterflyTID(expected.taggedButterfly)); // TID never changes without a payload change.
                        RELEASE_ASSERT(butterflySharedWrite(freshWord) && !butterflySharedWrite(expected.taggedButterfly)); // I4: SW monotonic.
                        if (desiredPayloadIsFreshFlatCopy) {
                            // (b2) The foreign plain store hit the OLD flat
                            // butterfly; merge-and-retry would drop it (lost
                            // write, I21): un-nuke, RESTART. Never call §4.2
                            // holding the cell lock (O2/heap L5).
                            structureIDAtomic(object)->store(sourceID.bits(), std::memory_order_seq_cst);
                            restart = true;
                            break;
                        }
                        if (flavor == TransitionFlavor::StayFlat && newStructure->writeThreadLocalIsStillValid()) {
                            // Publishing a flat SW=1 instance under a target
                            // whose writeThreadLocal set is still valid would
                            // break I12 (the F1/F4 chain-fire may predate this
                            // target): un-nuke, RESTART - the re-dispatch
                            // converts via §4.2 after F2 fires on both.
                            structureIDAtomic(object)->store(sourceID.bits(), std::memory_order_seq_cst);
                            restart = true;
                            break;
                        }
                        // (b1) Merge: SW is monotone and the slots are shared,
                        // so the flip composes with our publication (spine
                        // words already carry SW=1; flat reuse gains it).
                        expected.taggedButterfly = freshWord;
                        desired.taggedButterfly |= butterflySWBit;
                    }
                    // (a) Volatile header bytes changed (GC cellState CAS,
                    // lock parked bit): fold the freshest values into expected
                    // AND desired (I26), retry. Anything else is (d): under
                    // the cell lock with the lane nuked, a semantic header
                    // change is a logic error (§3.0 step 4).
                    RELEASE_ASSERT(headerDiffersOnlyInVolatileBits(expected.header, freshHeader));
                    expected.header = mergeVolatileHeaderBits(expected.header, freshHeader);
                    desired.header = mergeVolatileHeaderBits(desired.header, freshHeader);
                }
                if (!reenterStep3 && !restart)
                    published = true;
            } else {
                // I36: PA cells (8-mod-16 bases) cannot take the 16B DCAS.
                // M8 fenced nuke order under the cell lock: nuke (done above),
                // fence, publish the 64-bit butterfly word by CAS (I17 -
                // lock-free §4.4 CASes race even under our lock), write the
                // remaining semantic header bytes, fence, store the new ID.
                WTF::storeStoreFence();
                uint64_t expectedWordPA = expectedWord;
                while (true) {
                    uint64_t previousWord = butterflyWordAtomic(object)->compareExchangeStrong(expectedWordPA, desiredWord, std::memory_order_seq_cst);
                    if (previousWord == expectedWordPA) {
                        published = true;
                        break;
                    }
                    if (untaggedButterfly(previousWord) != untaggedButterfly(expectedWordPA)) {
                        // (c) un-nuke, goto step 3.
                        structureIDAtomic(object)->store(sourceID.bits(), std::memory_order_seq_cst);
                        reenterStep3 = true;
                        break;
                    }
                    // (b)
                    RELEASE_ASSERT(butterflyTID(previousWord) == butterflyTID(expectedWordPA));
                    RELEASE_ASSERT(butterflySharedWrite(previousWord) && !butterflySharedWrite(expectedWordPA)); // I4
                    if (desiredPayloadIsFreshFlatCopy
                        || (flavor == TransitionFlavor::StayFlat && newStructure->writeThreadLocalIsStillValid())) {
                        // (b2) / I12 guard, as in the DCAS branch above.
                        structureIDAtomic(object)->store(sourceID.bits(), std::memory_order_seq_cst);
                        restart = true;
                        break;
                    }
                    expectedWordPA = previousWord; // (b1)
                    desiredWord |= butterflySWBit;
                }
                if (published) {
                    // Remaining semantic header bytes, as JSCell::setStructure
                    // writes them (lanes static_asserted in the header; the
                    // indexing byte's lock bits - we HOLD the lock bit - and
                    // parked-bit flips are volatile: CAS-merge).
                    auto* cellBytes = reinterpret_cast<uint8_t*>(static_cast<JSCell*>(object));
                    auto* typeByte = reinterpret_cast<Atomic<uint8_t>*>(cellBytes + JSCell::typeInfoTypeOffset());
                    auto* flagsByte = reinterpret_cast<Atomic<uint8_t>*>(cellBytes + JSCell::typeInfoFlagsOffset());
                    auto* indexingByte = reinterpret_cast<Atomic<uint8_t>*>(cellBytes + JSCell::indexingTypeAndMiscOffset());

                    // Round 4: CAS-merge - the per-cell bit lane is volatile (lock-free setPerCellBit).
                    while (true) {
                        uint8_t oldFlags = flagsByte->load(std::memory_order_relaxed);
                        uint8_t newFlags = TypeInfo::mergeInlineTypeFlags(newStructure->typeInfo().inlineTypeFlags(), oldFlags);
                        if (oldFlags == newFlags || flagsByte->compareExchangeWeak(oldFlags, newFlags, std::memory_order_seq_cst))
                            break;
                    }
                    typeByte->store(static_cast<uint8_t>(newStructure->typeInfo().type()), std::memory_order_relaxed);
                    while (true) {
                        uint8_t oldValue = indexingByte->load(std::memory_order_relaxed);
                        uint8_t newValue = static_cast<uint8_t>((oldValue & ~AllArrayTypesAndHistory) | newStructure->indexingModeIncludingHistory());
                        if (oldValue == newValue)
                            break;
                        if (indexingByte->compareExchangeWeak(oldValue, newValue, std::memory_order_seq_cst))
                            break;
                    }
                    WTF::storeStoreFence();
                    structureIDAtomic(object)->store(newStructureID.bits(), std::memory_order_seq_cst); // Un-nuked new ID (M5).
                }
            }

            if (reenterStep3)
                continue; // Taxonomy (c)'s "goto 3" - re-read everything under the still-held lock.
            break;
        }

        // ---- Step 6: release the cell lock.
        unlockCellChecked(cellLock);

        if (published) {
            // Barriers (§4.5): publication barriers the object like
            // setButterfly; the step-4 value store barriers its value.
            vm.writeBarrier(object);
            vm.writeBarrier(object, newStructure);
            if (storedValue)
                vm.writeBarrier(object, value);
            // §9.6 (Task 10): under forceSegmentedButterflies a FirstInstall
            // publication legitimately installed FLAT (N3; a spine needs a flat
            // butterfly to alias) - convert it now, post-unlock (the helper
            // no-ops unless the word is still flat and the option is on).
            applyForceSegmentedButterfliesStressIfNeeded(vm, object);
            return true;
        }
        if (restart)
            return false; // RESTART: caller re-enters from §2 dispatch, lock-free.
        ASSERT(refit);
        // Refit: the step-1 allocations are discarded unreferenced (GC
        // reclaims them); replan and allocate again, unlocked (O1).
    }
}

// Frozen §9.3 driver (= §4.3); see the header comment for the contract.
void segmentedTransition(VM& vm, JSObjectWithButterfly* object, Structure* newStructure, PropertyOffset offset, JSValue value)
{
    Structure* source = settledStructure(object);
    while (!trySegmentedTransition(vm, object, source, newStructure, offset, value)) {
        Structure* settled = settledStructure(object);
        if (settled == newStructure) {
            // An identical racing transition published the target; the slot
            // exists - store our value (SAB last-writer-wins; no lost ADD, I21).
#if USE(JSVALUE64)
            if (offset != invalidOffset)
                object->putDirectConcurrent(vm, offset, value);
#else
            RELEASE_ASSERT_NOT_REACHED(); // useJSThreads requires 64-bit (I32).
#endif
            return;
        }
        // Racy callers must use trySegmentedTransition from their own §2
        // dispatch loop (RESTART recomputes the target from the fresh source).
        RELEASE_ASSERT(settled == source);
    }
}

// N2 (§2.1) restartable locked core; see the header comment for the contract.
bool tryStructureOnlyTransition(VM& vm, JSObject* object, Structure* expectedSource, Structure* newStructure, PropertyOffset inlineOffset, JSValue value)
{
    RELEASE_ASSERT(Options::useJSThreads());
    ASSERT(vm.currentThreadIsHoldingAPILock());
    ASSERT(expectedSource && newStructure);
    RELEASE_ASSERT(inlineOffset == invalidOffset || isInlineOffset(inlineOffset)); // Out-of-line adds touch the butterfly: §4.3.
    ASSERT(newStructure->outOfLineCapacity() == expectedSource->outOfLineCapacity()); // Butterfly untouched (N2).

    StructureID sourceID = object->structureID(); // RAW bits (M5).
    if (sourceID.isNuked() || sourceID != expectedSource->id())
        return false; // RESTART
    Structure* source = expectedSource;

    // ---- Step 0: F2 firing (I10b/I13), same trigger taxonomy as §4.3.
    {
        uint64_t word = object->taggedButterflyWord();
        bool trigger;
        if (!(word & butterflyPointerMask))
            trigger = currentButterflyTID() != source->transitionThreadLocalTID(); // N1 keying.
        else if (isSegmentedButterfly(word))
            trigger = true;
        else
            trigger = butterflyTID(word) != currentButterflyTID() || butterflySharedWrite(word);
        if (trigger && anyTTLSetStillValid(source, newStructure)) {
            fireTTLSetsForSharedTransition(vm, source, newStructure, "F2: shared structure-only transition (N2)");
            return false; // RESTART after the stop.
        }
    }

    JSCellLock& cellLock = object->cellLock();
    lockCellChecked(cellLock); // O3/I20 depth witness (Task 10)

    uint64_t expectedHeader = cellHeaderAtomic(object)->load(std::memory_order_seq_cst);
    if (static_cast<uint32_t>(expectedHeader) != sourceID.bits()) {
        unlockCellChecked(cellLock);
        return false; // RESTART: a racing (E4 or locked) transition won.
    }

    // Release-store the inline value FIRST (no holes, I9): any reader that
    // sees the new StructureID sees the value. Existing-slot inline access
    // stays lock-free (§2.1 N2) - this slot only becomes live with the new
    // structure, published below.
    if (inlineOffset != invalidOffset)
        reinterpret_cast<Atomic<uint64_t>*>(&object->inlineStorage()[offsetInInlineStorage(inlineOffset)])->store(JSValue::encode(value), std::memory_order_release); // M2

    // ONE 64-bit header CAS under the §3.0 merge discipline. No nuke: the
    // butterfly word is untouched, so there is no {structure, butterfly}
    // pairing to protect (GC's structureID+maxOffset re-check -> didRace
    // covers visitation). The CAS is 8B-aligned at the cell base, so it is
    // legal on PA cells too (I36 forbids only the 16B DCAS).
    uint64_t desiredHeader = headerForPublication(expectedHeader, newStructure->id(), newStructure);
    while (true) {
        uint64_t previousHeader = cellHeaderAtomic(object)->compareExchangeStrong(expectedHeader, desiredHeader, std::memory_order_seq_cst);
        if (previousHeader == expectedHeader)
            break;
        // Under the cell lock the semantic bytes are ours alone (E4
        // transitioners were excluded by step 0 / ownership; the indexed
        // installers publish locked / under a stop / in an owner poll-free
        // sets-valid window — AB18-S3; foreign SW
        // DCASes touch only the butterfly word); only the volatile bytes (GC
        // cellState CAS, lock parked bit - GT#2) may move. Anything else is
        // taxonomy (d): logic error (§3.0 step 4).
        RELEASE_ASSERT(headerDiffersOnlyInVolatileBits(expectedHeader, previousHeader));
        expectedHeader = mergeVolatileHeaderBits(expectedHeader, previousHeader);
        desiredHeader = mergeVolatileHeaderBits(desiredHeader, previousHeader);
    }

    unlockCellChecked(cellLock);

    vm.writeBarrier(object);
    vm.writeBarrier(object, newStructure);
    if (inlineOffset != invalidOffset)
        vm.writeBarrier(object, value);
    return true;
}

// Frozen §9.3 driver (N2 locked path); see the header comment for the contract.
void structureOnlyTransition(VM& vm, JSObject* object, Structure* newStructure, PropertyOffset inlineOffset, JSValue value)
{
    Structure* source = settledStructure(object);
    while (!tryStructureOnlyTransition(vm, object, source, newStructure, inlineOffset, value)) {
        Structure* settled = settledStructure(object);
        if (settled == newStructure) {
            // An identical racing transition published the target: store our
            // value into the now-live inline slot (SAB last-writer-wins).
            if (inlineOffset != invalidOffset)
                object->inlineStorage()[offsetInInlineStorage(inlineOffset)].set(vm, object, value);
            return;
        }
        RELEASE_ASSERT(settled == source); // Racy callers must use the try* form.
    }
}

// ===== Task 7: §3 foreign first write - ensureSharedWriteBit =====
//
// The §3 "Write existing slot, foreign, SW=0" rule: fire writeThreadLocal(S)
// (F1/I12) under a §10.6 stop, then flip the tag (t,0) -> (t,1) via the
// 128-bit DCAS under the §3.0 merge loop (divergence => re-dispatch). Plus the
// three carve-outs the §2/§3 dispatch routes through here:
//
//   - AS shapes (§4.6/I31, overrides everything): AS never segments and never
//     takes the lock-free flip - the SW=1 publication is a per-event STW that
//     fires the still-valid set(s) and publishes (installerTID, 1) FLAT inside
//     the same stop.
//   - CoW shapes (§4.8/I35, precedes F1's SW DCAS): CoW words never reach
//     SW=1 - first materialize a private flat butterfly tagged
//     (currentButterflyTID(), 0); only then does the triggering write
//     re-dispatch (and, since the materializer became the tag owner, lands on
//     the owner-store path unless forceButterflySWBit keeps treating it as
//     foreign).
//   - PA cells (I36): no 16B DCAS at 8-mod-16 bases - the flip is a cell-locked
//     64-bit CAS (the lock freezes the structure: every PA transition is
//     cell-locked and E4 excludes PA, so the fired-set check cannot go stale
//     under it; lock-free §4.4 element-resize CASes still race the word, hence
//     CAS not store).
//
// R-DOUBLE (§4.7, r12): a shared ContiguousDouble object STAYS Double - the
// SW flip below is shape-blind, with NO reboxing pass, NO boxing of slots and
// NO sharing-onset stop beyond the F1 fire. Double payloads are raw 8-byte
// lanes (GT#15): the caller's subsequent foreign element store is a raw
// aligned 8B store (tear-free, SAB semantics; holes stay PNaN), and slot
// interpretation stays shape-keyed (readers M7-order or re-check structureID).
// Only shape changes TOUCHING Double on an SW=1/segmented object are per-event
// STW relabels (I28/I34 - §4.4 T4 / Task 8), never this path.

namespace {

// The PA (I36/M8) tail of a publication: after the nuke and the 64-bit
// butterfly-word publication, write the remaining semantic header bytes as
// JSCell::setStructure would (we cannot name the protected members from here;
// the byte lanes are static_asserted in ConcurrentButterfly.h), fence, store
// the new un-nuked structureID. Caller holds the cell lock and has nuked the
// ID lane. Mirrors the inline blocks in convertToSegmentedButterfly /
// trySegmentedTransition.
void storeRemainingSemanticHeaderBytesAndStructureID(JSObjectWithButterfly* object, Structure* newStructure)
{
    auto* cellBytes = reinterpret_cast<uint8_t*>(static_cast<JSCell*>(object));
    auto* typeByte = reinterpret_cast<Atomic<uint8_t>*>(cellBytes + JSCell::typeInfoTypeOffset());
    auto* flagsByte = reinterpret_cast<Atomic<uint8_t>*>(cellBytes + JSCell::typeInfoFlagsOffset());
    auto* indexingByte = reinterpret_cast<Atomic<uint8_t>*>(cellBytes + JSCell::indexingTypeAndMiscOffset());

    // Review round 4: the per-cell bit lane of m_flags is volatile (a lock-free
    // JSCell::setPerCellBit byte CAS may land even under the cell lock) -
    // CAS-merge so a racing flip is never overwritten by a stale RMW.
    while (true) {
        uint8_t oldFlags = flagsByte->load(std::memory_order_relaxed);
        uint8_t newFlags = TypeInfo::mergeInlineTypeFlags(newStructure->typeInfo().inlineTypeFlags(), oldFlags);
        if (oldFlags == newFlags || flagsByte->compareExchangeWeak(oldFlags, newFlags, std::memory_order_seq_cst))
            break;
    }
    typeByte->store(static_cast<uint8_t>(newStructure->typeInfo().type()), std::memory_order_relaxed);
    // The indexing byte's lock bits (0xC0; the caller HOLDS the lock bit) and
    // concurrent parked-bit flips are volatile (GT#2): CAS-merge exactly like
    // JSCell::setStructure.
    while (true) {
        uint8_t oldValue = indexingByte->load(std::memory_order_relaxed);
        uint8_t newValue = static_cast<uint8_t>((oldValue & ~AllArrayTypesAndHistory) | newStructure->indexingModeIncludingHistory());
        if (oldValue == newValue)
            break;
        if (indexingByte->compareExchangeWeak(oldValue, newValue, std::memory_order_seq_cst))
            break;
    }
    WTF::storeStoreFence();
    structureIDAtomic(object)->store(newStructure->id().bits(), std::memory_order_seq_cst); // Un-nuked new ID; readers stop spinning (M5).
}

// §4.8 (I35): CoW materialize-first. Replicates today's
// JSObject::convertFromCopyOnWrite (JSObject.cpp) - private writable copy of
// the shared JSImmutableButterfly payload + the AllocateInt32/Double/Contiguous
// nonPropertyTransition - but publishes concurrency-safely: F2 firing first
// (a foreign butterfly-bearing transition - §5 F2 / I10b), then nuke +
// {header, butterfly} DCAS under the cell lock (PA: I36/M8 fenced order), the
// butterfly side expecting EXACTLY the loaded CoW-tagged word and installing
// (currentButterflyTID(), 0, freshFlat). Returns false for RESTART (racing
// materializer / racing transition / F2 fired): the caller re-dispatches on
// the fresh tag + structureID.
//
// Why the {structure, butterfly} pair is nuke-bracketed even though m_offset
// and the indexing SHAPE are unaffected (GT#7 would otherwise let us skip it):
// visitButterflyImpl dispatches on the structure's CoW-ness - a visitor pairing
// {CoW structure, fresh auxiliary butterfly} would visit our auxiliary
// allocation as a JSImmutableButterfly CELL (heap-accounting corruption,
// history §15.8), and {writable structure, CoW butterfly} would markAuxiliary
// a GC cell. The nuke makes either skew a didRace revisit instead.
//
// Serialization: racing materializations serialize on the butterfly-word
// publication (§4.8); we additionally hold the cell lock across nuke ->
// publish -> un-nuke so the nuke CAS cannot collide with a locked
// transitioner's (their step 3 re-reads under the lock first). Lock-free
// word-CASers cannot hit a CoW word: every §3/§4.2/§4.4 protocol (including
// the flag-on butterfly-replacing JSObject.cpp paths, routed through
// casButterfly/the locked protocols by Task 8) materializes first per §4.8,
// and F1 SW flips check CoW before their DCAS - so under the lock the word is
// stable (RELEASE_ASSERTed; an SW bit appearing on a CoW word is an I35
// violation). Review round 3 closed the last violator: the OWNER's
// JSObject::convertFromCopyOnWrite no longer plain-nukes flag-on - it routes
// through materializeCopyOnWriteButterflyConcurrent into this same
// cell-locked publication, which is what makes the stability RELEASE_ASSERTs
// below true statements rather than assumptions.
bool tryMaterializeCopyOnWriteButterflyForSharedWrite(VM& vm, JSObjectWithButterfly* object, uint64_t expectedWord, Structure* sourceStructure)
{
    ASSERT(vm.currentThreadIsHoldingAPILock());
    ASSERT(isCopyOnWrite(sourceStructure->indexingMode()));
    RELEASE_ASSERT(!isSegmentedButterfly(expectedWord) && !butterflySharedWrite(expectedWord)); // I35: CoW words never SW=1/segmented.
    RELEASE_ASSERT(expectedWord & butterflyPointerMask); // CoW shapes always carry a butterfly.

    StructureID sourceID = sourceStructure->id();

    TransitionKind transitionKind;
    switch (sourceStructure->indexingType()) { // indexingType() masks the CopyOnWrite bit, as in convertFromCopyOnWrite.
    case ArrayWithInt32:
        transitionKind = TransitionKind::AllocateInt32;
        break;
    case ArrayWithDouble:
        transitionKind = TransitionKind::AllocateDouble;
        break;
    case ArrayWithContiguous:
        transitionKind = TransitionKind::AllocateContiguous;
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return false;
    }

    DeferredStructureTransitionWatchpointFire deferred(vm, sourceStructure);
    Structure* newStructure = Structure::nonPropertyTransition(vm, sourceStructure, transitionKind, &deferred);

    // F2 (I10b/I13): a butterfly-bearing transition by a thread != the tag TID
    // fires BOTH sets on source and target before any lock or publication.
    // Without this, an E4 owner transition's plain-store publication could
    // race our CAS (lost update, I21). RESTART after the stop returns (§4.2
    // rule).
    //
    // Review round 3 (owner route): this function is now ALSO the OWNER's
    // materialization (JSObject::convertFromCopyOnWrite flag-on routes through
    // materializeCopyOnWriteButterflyConcurrent). An owner materialization is
    // an owner-SW=0 action (CoW words never reach SW=1, I35) and per §5 F2
    // fires NOTHING - so the fire is keyed on the word's writer being foreign
    // (incl. §9.6 forceButterflySWBit, matching ensureSharedWriteBit's calling
    // context). Owner-with-valid-sets is still race-free under the cell lock:
    // the only lock-free actors that could move an owner-tagged CoW word are
    // the owner itself (us) and foreign materializers, which take this same
    // cell lock (F1 flippers check CoW first and route here; E4 transitions
    // key on the instance tag = us).
    if (butterflyWriterIsForeign(expectedWord) && anyTTLSetStillValid(sourceStructure, newStructure)) {
        fireTTLSetsForSharedTransition(vm, sourceStructure, newStructure, "F2: foreign CopyOnWrite materialization (§4.8)");
        return false; // RESTART
    }

    // ---- Allocate + copy, outside the lock (O1), exactly as today's
    // convertFromCopyOnWrite: the CoW payload is an immutable
    // JSImmutableButterfly butterfly (GT#16), so unlocked reads of its
    // vectorLength/contents are safe; raw memcpy is sound for Double shapes
    // too (8-byte lanes, §4.7); the copied IndexingHeader carries the CoW
    // publicLength/vectorLength verbatim.
    Butterfly* oldButterfly = untaggedButterfly(expectedWord);
    constexpr size_t propertyCapacity = 0;
    unsigned newVectorLength = Butterfly::optimalContiguousVectorLength(
        propertyCapacity, std::min<size_t>(nextLength(oldButterfly->vectorLength()), MAX_STORAGE_VECTOR_LENGTH));
    Butterfly* newButterfly = Butterfly::createUninitialized(vm, object, 0, propertyCapacity, true, newVectorLength * sizeof(JSValue));
    memcpy(newButterfly->propertyStorage(), oldButterfly->propertyStorage(), oldButterfly->vectorLength() * sizeof(JSValue) + sizeof(IndexingHeader));

    WTF::storeStoreFence(); // Contents before publication (M2-equivalent; today's convertFromCopyOnWrite fences here too).

    const bool isPA = object->isPreciseAllocation(); // I36
    uint64_t desiredWord = encodeButterfly(newButterfly, currentButterflyTID(), false); // §4.8: private flat, (currentTID, 0).
    StructureID newStructureID = newStructure->id();

    JSCellLock& cellLock = object->cellLock();
    lockCellChecked(cellLock); // O3/I20 depth witness (Task 10)

    bool published = false;
    while (true) {
        // Re-verify under the lock (the §4.2-3 analogue).
        if (structureIDAtomic(object)->load(std::memory_order_seq_cst) != sourceID.bits())
            break; // RESTART: a racing transition (or materializer) won.
        if (butterflyWordAtomic(object)->load(std::memory_order_seq_cst) != expectedWord)
            break; // RESTART: racing materializer won the word (serialization point, §4.8).

        // Nuke (M5): see the function comment - GC visitors must never pair a
        // CoW structure with the writable copy or vice versa. Under the lock
        // the semantic ID lane is ours - foreign triggers fired both sets
        // above (E4 excluded), and for the round-3 owner route the only E4
        // key owner is the calling thread itself, while every other mutator
        // of this lane is cell-locked and serialized with us - so the CAS
        // must succeed.
        uint32_t previousIDBits = structureIDAtomic(object)->compareExchangeStrong(sourceID.bits(), sourceID.nuke().bits());
        RELEASE_ASSERT(previousIDBits == sourceID.bits());

        if (!isPA) {
            // 128-bit DCAS {nuked header, CoW word} -> {writable header,
            // (currentTID, 0, fresh)}; seq_cst (M3), §3.0 merge loop. Under
            // the cell lock with the sets fired, the word cannot move (see
            // function comment): the only legal failure is (a) volatile
            // header bytes (GC cellState CAS, lock parked bit).
            uint64_t nukedHeader = cellHeaderAtomic(object)->load(std::memory_order_seq_cst);
            CellHeaderAndButterfly expected { nukedHeader, expectedWord };
            CellHeaderAndButterfly desired { headerForPublication(nukedHeader, newStructureID, newStructure), desiredWord };
            while (!dcasHeaderAndButterfly(object, expected, desired)) {
                uint64_t freshHeader = cellHeaderAtomic(object)->load(std::memory_order_seq_cst);
                uint64_t freshWord = butterflyWordAtomic(object)->load(std::memory_order_seq_cst);
                RELEASE_ASSERT(freshWord == expected.taggedButterfly); // I35/§4.8: no lock-free CAS ever targets a CoW word.
                RELEASE_ASSERT(headerDiffersOnlyInVolatileBits(expected.header, freshHeader)); // (d) is a logic error under the lock.
                expected.header = mergeVolatileHeaderBits(expected.header, freshHeader); // (a): I26.
                desired.header = mergeVolatileHeaderBits(desired.header, freshHeader);
            }
        } else {
            // I36: PA cells take the M8 fenced order - nuke (done), fence,
            // 64-bit CAS of the butterfly word (8B-aligned, legal), remaining
            // semantic bytes, fence, new ID.
            WTF::storeStoreFence();
            uint64_t previousWord = butterflyWordAtomic(object)->compareExchangeStrong(expectedWord, desiredWord, std::memory_order_seq_cst);
            RELEASE_ASSERT(previousWord == expectedWord); // I35/§4.8: stable under the lock (PA flips/transitions all cell-locked, I36).
            storeRemainingSemanticHeaderBytesAndStructureID(object, newStructure);
        }
        published = true;
        break;
    }

    unlockCellChecked(cellLock);

    if (!published)
        return false; // RESTART: caller re-enters from §2 dispatch, lock-free.

    // Barriers (§4.5/I25): publication barriers the object like setButterfly -
    // vm.writeBarrier(object) re-greys it so the fresh butterfly's elements
    // (raw-copied above) are scanned; the structure edge is barriered like
    // setStructure.
    vm.writeBarrier(object);
    vm.writeBarrier(object, newStructure);
    return true;
}

} // anonymous namespace

// §3 foreign first write (frozen §9.3 signature). Ensures the object's flat
// butterfly word carries SW=1 (or has left the flat regime), with F1 fired
// first (I12) and the carve-outs described above. On return the caller
// re-dispatches on the fresh tag (§2): the word is then segmented, flat SW=1,
// flat owned by the calling thread (post-CoW materialization), or None.
void ensureSharedWriteBit(VM& vm, JSObjectWithButterfly* object)
{
    RELEASE_ASSERT(Options::useJSThreads());
    ASSERT(vm.currentThreadIsHoldingAPILock());

    while (true) {
        uint64_t word = butterflyWordAtomic(object)->load(std::memory_order_seq_cst);
        ButterflyRegime regime = butterflyRegimeForWord(word);
        if (regime == ButterflyRegime::None)
            return; // No out-of-line storage to share; a racing install re-dispatches at the caller.
        if (regime == ButterflyRegime::Segmented)
            return; // SW=1 by construction (I3).

        // Settled structure (M5): never decode a nuked ID; the nuke window is
        // bounded straight-line (O2), so spinning via re-dispatch terminates.
        StructureID structureIDValue = object->structureID(); // RAW bits.
        if (structureIDValue.isNuked())
            continue;
        Structure* structure = structureIDValue.decode();

        // ---- §4.6 carve-out (I31, overrides all below): AS-shape objects.
        // The first foreign WRITE is a per-event STW: fire the still-valid
        // set(s) and publish (installerTID, 1) FLAT inside the SAME stop. AS
        // never segments; AS storage relayouts are AS-COPY under the cell
        // lock (Task 8) - the flip itself moves no storage, so the stop
        // closure allocates nothing (O4). Caller contract for the veneer
        // (GT11) holds: we are an entered mutator holding no §6-ranked lock.
        if (hasAnyArrayStorage(structure->indexingType())) [[unlikely]] {
            if (butterflySharedWrite(word))
                return; // Already (t, 1); every AS access is cell-locked anyway (I31/L5).
            bool flipped = false;
            // STW dedup (T1-butterfly-stw-growth): racers flipping the same
            // object's SW bit collapse into the claim winner's single stop;
            // losers park-wait until the word moves and re-dispatch (the next
            // pass sees SW=1 and returns).
            PerEventStopClaim claim(vm, object, [&] {
                return butterflyWordAtomic(object)->load(std::memory_order_seq_cst) != word;
            });
            if (!claim.acquired())
                continue; // The word moved while a competing stop was in flight: re-dispatch.
            if (butterflyWordAtomic(object)->load(std::memory_order_seq_cst) != word)
                continue; // Cheap pre-stop re-validation: never pay a rendezvous for a stale plan.
            jsThreadsStopTheWorldAndRun(vm, scopedLambda<void()>([&] {
                // Re-verify the settled pair inside the stop: mutators are
                // stopped at safepoints, and nuke windows are poll-free
                // (O2/M5), so the ID cannot be caught mid-nuke here.
                StructureID stoppedID = object->structureID();
                RELEASE_ASSERT(!stoppedID.isNuked());
                Structure* stoppedStructure = stoppedID.decode();
                if (!hasAnyArrayStorage(stoppedStructure->indexingType()))
                    return; // Shape moved before the stop landed: re-dispatch.
                uint64_t stoppedWord = butterflyWordAtomic(object)->load(std::memory_order_seq_cst);
                RELEASE_ASSERT(!isSegmentedButterfly(stoppedWord)); // I31: AS never segments.
                if (!(stoppedWord & butterflyPointerMask))
                    return; // Defensive: nothing to flip; re-dispatch.
                if (butterflySharedWrite(stoppedWord)) {
                    flipped = true; // A racing per-event stop already published (t, 1).
                    return;
                }
                // F1 under the stop (I12/I13): a WRITE invalidates exactly
                // write-thread-locality; fireWriteThreadLocal chain-fires per
                // F4. transitionThreadLocal stays valid - a write is not a
                // transition (I11 untouched; shared transitions INTO/ON AS are
                // F2 sites, §4.6/Task 8).
                if (stoppedStructure->writeThreadLocalIsStillValid())
                    stoppedStructure->fireWriteThreadLocal(vm, "F1: first foreign write to an ArrayStorage object (§4.6 per-event stop)");
                // Publish (installerTID, 1) flat. World-stopped => no mutator
                // races; CAS keeps the publication I17-shaped regardless
                // (concurrent markers CAS only header bytes, never the word).
                uint64_t previousWord = butterflyWordAtomic(object)->compareExchangeStrong(stoppedWord, stoppedWord | butterflySWBit, std::memory_order_seq_cst);
                RELEASE_ASSERT(previousWord == stoppedWord);
                flipped = true;
            }));
            if (flipped)
                return;
            continue; // Re-dispatch on the post-stop state.
        }

        // ---- §4.8 carve-out (I35, precedes F1's SW DCAS): CoW shapes
        // materialize a private flat butterfly first; the triggering write
        // then re-dispatches (post-materialization the calling thread IS the
        // tag owner, so §2/§3 routes it to the owner plain-store path - or
        // back here under forceButterflySWBit, now on a non-CoW word).
        if (isCopyOnWrite(structure->indexingMode())) [[unlikely]] {
            if (object->taggedButterflyWord() != word)
                continue; // The word moved since classification: re-dispatch.
            RELEASE_ASSERT(!butterflySharedWrite(word)); // I35: CoW words never reach SW=1.
            if (tryMaterializeCopyOnWriteButterflyForSharedWrite(vm, object, word, structure))
                return;
            continue; // RESTART (racing materializer / racing transition / F2 fired).
        }

        if (butterflySharedWrite(word))
            return; // Already shared-written (I4: monotonic).

        if (!butterflyWriterIsForeign(word)) // §9.6: forceButterflySWBit keeps even the owner on the F1 route.
            return; // Owner write (e.g. after OUR CoW materialization re-tagged the word): no SW needed.

        // ---- F1 (I12): fire writeThreadLocal(S) under a §10.6 stop BEFORE
        // the SW DCAS completes; firing precedes any cell-lock acquisition
        // (I10b). Sets are monotone, so once this reads invalid it stays
        // invalid; the publication below pins the structure whose set we
        // checked (DCAS carries the ID lane; PA holds the lock that all PA
        // transitions take - I36).
        if (structure->writeThreadLocalIsStillValid()) {
            // STW dedup (T1-butterfly-stw-growth): the set is monotone; the
            // claim winner's stop fires it for every racer. Losers park-wait
            // until it reads invalid and re-dispatch stop-free.
            PerEventStopClaim claim(vm, structure, [&] { return !structure->writeThreadLocalIsStillValid(); });
            if (claim.acquired() && structure->writeThreadLocalIsStillValid()) {
                jsThreadsStopTheWorldAndRun(vm, scopedLambda<void()>([&] {
                    if (structure->writeThreadLocalIsStillValid()) // A racing fire may have got here first.
                        structure->fireWriteThreadLocal(vm, "F1: first foreign write to a flat thread-local-write instance");
                }));
            }
            continue; // Re-dispatch on the fresh word + structure after the stop (ours or the racing winner's).
        }

        // ---- I36 carve-out: PA cells (8-mod-16 bases) cannot take the 16B
        // DCAS - the flip is a cell-locked 64-bit CAS. The lock freezes the
        // structure (every PA transition/conversion is cell-locked, E4
        // excludes PA), so the fired-set check above cannot be invalidated by
        // a transition between here and the CAS; only lock-free §4.4
        // element-resize CASes still race the word.
        if (object->isPreciseAllocation()) [[unlikely]] {
            JSCellLock& cellLock = object->cellLock();
            lockCellChecked(cellLock); // O3/I20 depth witness (Task 10)
            StructureID lockedID = object->structureID();
            if (lockedID.isNuked() || lockedID != structureIDValue) {
                unlockCellChecked(cellLock);
                continue; // A transition won before we locked: re-dispatch (its target's set may still be valid - I12).
            }
            uint64_t freshWord = butterflyWordAtomic(object)->load(std::memory_order_seq_cst);
            if (isSegmentedButterfly(freshWord) || !(freshWord & butterflyPointerMask)) {
                unlockCellChecked(cellLock);
                continue; // Regime moved: re-dispatch.
            }
            if (butterflySharedWrite(freshWord)) {
                unlockCellChecked(cellLock);
                return; // PA flips are cell-locked (I36), but a pre-lock flip may have won.
            }
            uint64_t previousWord = butterflyWordAtomic(object)->compareExchangeStrong(freshWord, freshWord | butterflySWBit, std::memory_order_seq_cst); // M4
            unlockCellChecked(cellLock);
            if (previousWord == freshWord) {
                if (verifyConcurrentButterflyEnabled()) [[unlikely]]
                    RELEASE_ASSERT(!structure->writeThreadLocalIsStillValid()); // I12 witness (Task 10), PA flavor.
                return;
            }
            continue; // A lock-free T1/§4.4 resize CAS won the word (legal on PA): re-dispatch.
        }

        // ---- The §3 flip proper: 128-bit DCAS (t, 0) -> (t, 1) under the
        // §3.0 merge loop; seq_cst (M4; the caller's subsequent property
        // store may be plain). Pairing the header pins the structureID lane
        // to the structure whose writeThreadLocal we just verified fired
        // (I12): any transition in between changes the semantic header and
        // fails the DCAS.
        uint64_t header = cellHeaderAtomic(object)->load(std::memory_order_seq_cst);
        if (static_cast<uint32_t>(header) != structureIDValue.bits())
            continue; // Structure moved (or mid-nuke) between reads: re-dispatch.
        if (butterflyWordAtomic(object)->load(std::memory_order_seq_cst) != word)
            continue; // Word moved since classification: re-dispatch (cheap pre-check).

        CellHeaderAndButterfly expected { header, word };
        CellHeaderAndButterfly desired { header, word | butterflySWBit }; // Tag (t, 0) -> (t, 1); payload + semantic header unchanged.
        bool flipped = false;
        bool redispatch = false;
        while (!dcasHeaderAndButterfly(object, expected, desired)) {
            // §3.0 loop, lock-free flavor: fold volatile divergence, abandon
            // on semantic divergence (step 4: re-dispatch, I5/I26).
            uint64_t freshHeader = cellHeaderAtomic(object)->load(std::memory_order_seq_cst);
            uint64_t freshWord = butterflyWordAtomic(object)->load(std::memory_order_seq_cst);
            if (freshWord != expected.taggedButterfly) {
                if (untaggedButterfly(freshWord) == untaggedButterfly(expected.taggedButterfly) && butterflySharedWrite(freshWord)) {
                    // A racing F1 flipper won with the same payload: SW is
                    // monotone (I4) - goal achieved.
                    flipped = true;
                    break;
                }
                // Payload replaced (owner T1 resize / E4 transition /
                // conversion): abandon, re-dispatch on the winner's tag (§3).
                redispatch = true;
                break;
            }
            if (!headerDiffersOnlyInVolatileBits(expected.header, freshHeader)) {
                // Semantic header change with the word unchanged: an E4 owner
                // publication (nuke or new ID) is mid-flight - abandon,
                // re-dispatch (§3.0 step 4, lock-free).
                redispatch = true;
                break;
            }
            expected.header = mergeVolatileHeaderBits(expected.header, freshHeader); // (a): I26.
            desired.header = mergeVolatileHeaderBits(desired.header, freshHeader);
        }
        if (flipped || !redispatch) {
            if (verifyConcurrentButterflyEnabled()) [[unlikely]]
                RELEASE_ASSERT(!structure->writeThreadLocalIsStillValid()); // I12 witness (Task 10): the F1 fire completed before the SW DCAS (sets are monotone).
            return; // DCAS succeeded, or a racing flipper finished the job.
        }
        // Divergence: re-dispatch on the fresh tag + structureID.
    }
}

#if USE(JSVALUE64)
// §4.8 driver (review round 3): the flag-on replacement for the OWNER's
// JSObject::convertFromCopyOnWrite plain nuke + publish (which raced the
// cell-locked foreign materializer - see the round-3 comment at that site).
// Loops the cell-locked materializer until the object has left the CoW
// regime. The WINNER may be a foreign thread, so on return the word may be a
// foreign-tagged flat (winnerTID, 0): callers must re-dispatch on the fresh
// tag (the §3 probes at every flat fast path do) before treating the storage
// as their own.
void materializeCopyOnWriteButterflyConcurrent(VM& vm, JSObjectWithButterfly* object)
{
    RELEASE_ASSERT(Options::useJSThreads());
    ASSERT(vm.currentThreadIsHoldingAPILock());
    while (true) {
        StructureID id = object->structureID(); // RAW bits (M5): never decode a nuked ID.
        if (id.isNuked())
            continue; // Bounded nuke window (O2); spin to the settled ID.
        Structure* structure = id.decode();
        if (!isCopyOnWrite(structure->indexingMode()))
            return; // Done - we won earlier, or a racing materializer won.
        uint64_t word = butterflyWordAtomic(object)->load(std::memory_order_seq_cst);
        if (!(word & butterflyPointerMask) || isSegmentedButterfly(word))
            continue; // Skew with the structure read (a publication landed in between): re-settle.
        if (tryMaterializeCopyOnWriteButterflyForSharedWrite(vm, object, word, structure))
            return;
        // RESTART: a racing materializer/transition won, or the F2 stop fired
        // (foreign trigger) - re-classify on the fresh state.
    }
}

// §6 dictionary growth (review round 3): raise a segmented word's out-of-line
// fragment COVERAGE to >= neededCapacitySlots without any structure
// transition: replacement spine per §4.3-1 (copy the header + fragment-pointer
// prefix verbatim, append fresh cleared out-of-line fragments before the
// indexed pointers; aliased base/size VERBATIM - I7; spineEpoch + 1), one
// casButterfly per attempt (I16/I17; segmented -> segmented). Coverage is
// MONOTONE across replacement spines (§4.3-1 and T2 both copy the out-of-line
// prefix), so a true return stays sufficient for the caller's subsequent
// cell-locked maxOffset bump even if a racing element resize republishes a
// newer spine. Returns false when the word is no longer segmented (the caller
// re-dispatches). Allocates: holds its own DeferGC across each
// allocate-to-publication attempt (round 4 - see below); callers hold no
// §6-ranked lock.
bool ensureSegmentedOutOfLineCapacity(VM& vm, JSObjectWithButterfly* object, size_t neededCapacitySlots)
{
    RELEASE_ASSERT(Options::useJSThreads());
    ASSERT(vm.currentThreadIsHoldingAPILock());
    while (true) {
        // Review round 4 (blocker fix): the fresh fragments below are stored
        // ONLY into the not-yet-published newSpine - an auxiliary cell whose
        // contents the conservative scan never traces - so a collection
        // triggered by fragment j+1's allocation would sweep fragment j.
        // DeferGC pins the whole attempt; its destructor runs at each retry
        // boundary so deferred collections make progress.
        DeferGC deferGC(vm);
        uint64_t word = butterflyWordAtomic(object)->load(std::memory_order_seq_cst);
        if (!isSegmentedButterfly(word))
            return false; // Regime moved: re-dispatch at the caller.
        ButterflySpine* spine = butterflySpine(word);
        spine->tsanConsume(); // V7: published spine; import the publisher's clock.
        uint32_t haveFragments = spine->outOfLineFragmentCountConcurrent();
        uint32_t needFragments = static_cast<uint32_t>((neededCapacitySlots + butterflyFragmentSlots - 1) / butterflyFragmentSlots);
        if (haveFragments >= needFragments)
            return true;
        uint32_t indexedFragments = spine->indexedFragmentCountConcurrent();
        auto* newSpine = static_cast<ButterflySpine*>(vm.auxiliarySpace().allocate(
            vm, ButterflySpine::allocationSize(needFragments + indexedFragments), nullptr, AllocationFailureMode::Assert));
        butterflyConcurrentStore(&newSpine->outOfLineFragmentCount, needFragments); // V7: paired with reader-side relaxed loads.
        butterflyConcurrentStore(&newSpine->indexedFragmentCount, indexedFragments);
        butterflyConcurrentStore(&newSpine->vectorLength, spine->vectorLengthConcurrent());
        butterflyConcurrentStore(&newSpine->spineEpoch, butterflyConcurrentLoad(&spine->spineEpoch) + 1);
        butterflyConcurrentStore(&newSpine->aliasedAllocationBase, butterflyConcurrentLoad(&spine->aliasedAllocationBase)); // VERBATIM (I7)
        butterflyConcurrentStore(&newSpine->aliasedAllocationSize, butterflyConcurrentLoad(&spine->aliasedAllocationSize)); // VERBATIM (I7)
        for (uint32_t j = 0; j < haveFragments; ++j)
            butterflyConcurrentStore(&newSpine->fragments()[j], spine->outOfLineFragment(j)); // Shared fragments aliased verbatim (I6).
        for (uint32_t j = haveFragments; j < needFragments; ++j) {
            auto* fragment = static_cast<ButterflyFragment*>(
                vm.auxiliarySpace().allocate(vm, sizeof(ButterflyFragment), nullptr, AllocationFailureMode::Assert));
            for (size_t slotIndex = 0; slotIndex < butterflyFragmentSlots; ++slotIndex)
                fragment->slots[slotIndex].clear(); // Beyond outOfLineSize: never value-visited (§4.5 step 4).
            butterflyConcurrentStore(&newSpine->fragments()[j], fragment);
        }
        for (uint32_t f = 0; f < indexedFragments; ++f)
            butterflyConcurrentStore(&newSpine->fragments()[needFragments + f], spine->indexedFragment(f));
        newSpine->validateConsistency();
        newSpine->tsanPublish(); // V7: last pre-publication store done; pairs with tsanConsume() in readers.
        WTF::storeStoreFence(); // Spine contents before publication.
        if (casButterfly(object, word, encodeSegmentedButterfly(newSpine))) {
            vm.writeBarrier(object);
            return true;
        }
        // §4.3(c): a racing butterfly-pointer CAS won - re-plan from the
        // fresh spine; the allocations drop unreferenced.
    }
}
#endif // USE(JSVALUE64)

#if USE(JSVALUE64)

// ===== §9.5 full-§2-dispatch slow paths (Task 6) =====
//
// E5: interpreter/runtime slow paths never rely on elision - they dispatch on
// the loaded tagged word (None first), conform to M7 (the (d) loadLoadFence
// orders the caller's structureID/offset provenance before the tagged-word
// load - I24), are M5 nuke-tolerant (they never decode a possibly-nuked
// StructureID: dispatch keys on the tagged word and the indexing byte only),
// and are poll-free between slot resolution and access (I34).

JSValue JSObjectWithButterfly::getDirectConcurrent(PropertyOffset offset) const
{
    ASSERT(Options::useJSThreads());
    if (isInlineOffset(offset)) {
        // Inline slots: the cell never resizes - one atomic EncodedJSValue
        // load (§3).
        return inlineStorage()[offsetInInlineStorage(offset)].get();
    }
    if (hasAnyArrayStorage(indexingType())) [[unlikely]] {
        // I31/L5: flag-on, EVERY runtime access to an AS-shape object (reads
        // included, any SW) is cell-locked; re-load the word under the lock
        // (AS-COPY may have republished a fresh AS butterfly, §4.6).
        Locker locker { cellLock() };
        CellLockDepthScope cellLockDepthScope; // O3/I20 (Task 10)
        uint64_t word = taggedButterflyWord();
        RELEASE_ASSERT(!isSegmentedButterfly(word)); // I31: AS never segments.
        return untaggedButterfly(word)->propertyStorage()[offsetInOutOfLineStorage(offset)].get();
    }
    WTF::loadLoadFence(); // M7(d)
    uint64_t word = taggedButterflyWord();
    while (true) {
        if (isSegmentedButterfly(word)) [[unlikely]] {
            if (const WriteBarrierBase<Unknown>* slot = segmentedOutOfLineSlotIfWithinBounds(butterflySpine(word), offset))
                return slot->get();
            // I33: out of the loaded spine's bounds = stale spine =>
            // acquire-re-load the tagged word, re-dispatch.
            WTF::loadLoadFence();
            word = taggedButterflyWord();
            continue;
        }
        RELEASE_ASSERT(word & butterflyPointerMask); // A valid out-of-line offset implies storage (live storage never shrinks - I18).
        // Flat (any TID, any SW): mask and read as today (§3; flat-branch
        // soundness proof: history §15.4 via M7(d) above).
        return untaggedButterfly(word)->propertyStorage()[offsetInOutOfLineStorage(offset)].get();
    }
}

void JSObjectWithButterfly::putDirectConcurrent(VM& vm, PropertyOffset offset, JSValue value)
{
    ASSERT(Options::useJSThreads());
    ASSERT(value);
    if (isInlineOffset(offset)) {
        // Existing inline slot: one atomic store + barrier (§3).
        inlineStorage()[offsetInInlineStorage(offset)].set(vm, this, value);
        return;
    }
    if (hasAnyArrayStorage(indexingType())) [[unlikely]] {
        // §4.6 (Task 8): a foreign write to an SW=0 AS instance first runs the
        // per-event stop (F1 + (installerTID, 1) flat publication) inside
        // ensureSharedWriteBit - called with NO lock held (veneer caller
        // contract, GT11) - before the I31 locked access below.
        uint64_t probeWord = taggedButterflyWord();
        if ((probeWord & butterflyPointerMask) && !butterflySharedWrite(probeWord)
            && butterflyWriterIsForeign(probeWord)) // incl. §9.6 forceButterflySWBit
            ensureSharedWriteBit(vm, this);
        Locker locker { cellLock() }; // I31/L5
        CellLockDepthScope cellLockDepthScope; // O3/I20 (Task 10)
        uint64_t word = taggedButterflyWord();
        RELEASE_ASSERT(!isSegmentedButterfly(word)); // I31
        untaggedButterfly(word)->propertyStorage()[offsetInOutOfLineStorage(offset)].set(vm, this, value);
        return;
    }
    WTF::loadLoadFence(); // M7(d)
    uint64_t word = taggedButterflyWord();
    while (true) {
        if (isSegmentedButterfly(word)) [[unlikely]] {
            if (WriteBarrierBase<Unknown>* slot = segmentedOutOfLineSlotIfWithinBounds(butterflySpine(word), offset)) {
                slot->set(vm, this, value); // §4.5: fragment-slot stores = WriteBarrierBase::set on the owner.
                return;
            }
            WTF::loadLoadFence();
            word = taggedButterflyWord();
            continue;
        }
        RELEASE_ASSERT(word & butterflyPointerMask);
        bool foreign = butterflyWriterIsForeign(word); // incl. §9.6 forceButterflySWBit
        if (foreign && !butterflySharedWrite(word)) [[unlikely]] {
            // §3: first foreign write to a flat instance with SW=0 - fire
            // writeThreadLocal (F1/I12) and flip SW via the §3.0 DCAS, with
            // the R-DOUBLE/CoW/PA carve-outs, all inside ensureSharedWriteBit
            // (defined by Task 7). Then re-dispatch on the fresh tag (the word
            // may have gone segmented or been replaced meanwhile).
            ensureSharedWriteBit(vm, this);
            WTF::loadLoadFence();
            word = taggedButterflyWord();
            continue;
        }
        // Owner, or SW already set: mask, store, as today (§3).
        untaggedButterfly(word)->propertyStorage()[offsetInOutOfLineStorage(offset)].set(vm, this, value);
        return;
    }
}

// ===== Task 8: §4.4 array transitions - casButterfly and the resize drivers =====

namespace {

// I27/I17/I4/I2/I3 shape validation for casButterfly (debug; promoted to
// RELEASE_ASSERT under verifyConcurrentButterfly via the validate calls).
// Exhaustive over the sanctioned forms - see the header comment.
void assertCasButterflyShape(JSObjectWithButterfly* object, uint64_t expectedTagged, uint64_t newTagged)
{
    if (verifyConcurrentButterflyEnabled()) [[unlikely]] {
        validateTaggedButterflyWord(expectedTagged); // I2/I3
        validateTaggedButterflyWord(newTagged);
    }
    ASSERT(newTagged & butterflyPointerMask); // Resizes/installs never publish None.
    ASSERT(untaggedButterfly(newTagged) != untaggedButterfly(expectedTagged)); // Payload-replacing only; SW flips use the §3.0 DCAS / I36 locked CAS.
    ASSERT(!butterflySharedWrite(expectedTagged) || butterflySharedWrite(newTagged)); // I4: SW monotonic.

    if (!(expectedTagged & butterflyPointerMask)) {
        // N3 first install: 0 -> (currentButterflyTID(), 0) flat.
        ASSERT(!isSegmentedButterfly(newTagged));
        ASSERT(butterflyTID(newTagged) == currentButterflyTID());
        ASSERT(!butterflySharedWrite(newTagged));
        return;
    }
    if (isSegmentedButterfly(newTagged)) {
        // T2 replacement-spine publication: segmented stays segmented (I27:
        // T2 publishes (notTTLTID, 1)); flat->segmented is the §4.2
        // nuke + DCAS, never this CAS.
        ASSERT(isSegmentedButterfly(expectedTagged));
        return;
    }
    ASSERT(!isSegmentedButterfly(expectedTagged)); // Spines never demote outside STW.
    // Flat payload replacement preserves the tag verbatim (I27: T1's new word
    // is identical (t, 0); AS-COPY republishes under the same (t, sw)).
    ASSERT(butterflyTID(newTagged) == butterflyTID(expectedTagged));
    ASSERT(butterflySharedWrite(newTagged) == butterflySharedWrite(expectedTagged));
    if (butterflySharedWrite(expectedTagged) || butterflyTID(expectedTagged) != currentButterflyTID()) {
        // I27: the sole copying resize of a non-(currentTID, 0) butterfly
        // outside STW is the cell-locked §4.6 AS-COPY (I31).
        ASSERT(object->cellLock().isLocked());
        ASSERT(hasAnyArrayStorage(object->indexingType()));
    }
#if !ASSERT_ENABLED
    UNUSED_PARAM(object);
#endif
}

} // anonymous namespace

// §4.4 (frozen §9.3 signature): ONE 64-bit seq_cst CAS on the tagged butterfly
// word (M3/I16/I17). false => the caller RE-DISPATCHES on the fresh tag,
// never blind-retries (an SW flip mid-resize means a foreign store landed in
// the old payload after the copy - re-CASing the copy would drop it, I21).
bool casButterfly(JSObjectWithButterfly* object, uint64_t expectedTagged, uint64_t newTagged)
{
    RELEASE_ASSERT(Options::useJSThreads());
    assertCasButterflyShape(object, expectedTagged, newTagged);
    uint64_t previousWord = butterflyWordAtomic(object)->compareExchangeStrong(expectedTagged, newTagged, std::memory_order_seq_cst);
    return previousWord == expectedTagged;
}

// §4.6 AS-COPY publication form (T3/I17): see the header comment.
void publishArrayStorageButterflyLocked(VM& vm, JSObjectWithButterfly* object, Butterfly* newButterfly)
{
    RELEASE_ASSERT(Options::useJSThreads());
    ASSERT(object->cellLock().isLocked()); // I31/L5: every AS butterfly mutation is cell-locked.
    ASSERT(hasAnyArrayStorage(object->indexingType()));
    ASSERT(newButterfly);
    WTF::storeStoreFence(); // Contents before publication (M2-equivalent; M8 forces fenced publication flag-on).
    uint64_t expectedWord = butterflyWordAtomic(object)->load(std::memory_order_seq_cst);
    RELEASE_ASSERT(!isSegmentedButterfly(expectedWord)); // I31: AS never segments.
    RELEASE_ASSERT(expectedWord & butterflyPointerMask); // AS shapes always carry a butterfly.
    bool ok = casButterfly(object, expectedWord, encodeButterfly(newButterfly, butterflyTID(expectedWord), butterflySharedWrite(expectedWord)));
    // Under the cell lock the word cannot move: AS SW flips are per-event
    // stops (§4.6), other AS relayouts hold this lock (I31), and lock-free
    // §4.4 CASes never target AS words. Failure = logic error.
    RELEASE_ASSERT(ok);
    vm.writeBarrier(object); // Superseded storage is never written again (AS-COPY); stale readers see a frozen snapshot (I7: conservative scan keeps it alive).
}

// §4.6 (I31) AS out-of-line property transition; see the header comment.
// FUZZ r3-001: non-dictionary AS objects (object literal with a sparse-index
// integer key + out-of-line named properties) had no route between the E4 AS
// exclusion (StructureInlines.h I31 review-round-3 carve-out) and the
// trySegmentedTransition I31 entry assert. This is the §4.6 analogue of the
// dictionary AS growth leg in JSObjectInlines.h
// growOutOfLineStorageForConcurrentLockedAdd, with a structure publish.
bool tryArrayStoragePropertyTransition(VM& vm, JSObjectWithButterfly* object, Structure* expectedSource, Structure* newStructure, PropertyOffset offset, JSValue value)
{
    RELEASE_ASSERT(Options::useJSThreads());
    ASSERT(vm.currentThreadIsHoldingAPILock());
    ASSERT(expectedSource && newStructure);
    RELEASE_ASSERT(isOutOfLineOffset(offset)); // Inline AS adds are N2 (tryStructureOnlyTransition); attribute-only reshapes are N2 too.
    RELEASE_ASSERT(hasAnyArrayStorage(expectedSource->indexingType()));
    RELEASE_ASSERT(hasAnyArrayStorage(newStructure->indexingType())); // Property adds preserve indexing shape.
    RELEASE_ASSERT(expectedSource->indexingModeIncludingHistory() == newStructure->indexingModeIncludingHistory()); // §4.6: AS shape changes are Task 8 STW relayouts, never this entry.

    StructureID sourceID = object->structureID(); // RAW bits (M5).
    if (sourceID.isNuked() || sourceID != expectedSource->id())
        return false; // RESTART: a racing publication is mid-flight or the source settled away.
    Structure* source = expectedSource;

    // ---- Step 0: F2 firing (I10b/I13). AS shapes always carry a butterfly
    // (the ArrayStorage payload), so the §5 F2 trigger is the instance tag.
    uint64_t planningWord = butterflyWordAtomic(object)->load(std::memory_order_seq_cst);
    RELEASE_ASSERT(planningWord & butterflyPointerMask);
    RELEASE_ASSERT(!isSegmentedButterfly(planningWord)); // I31: AS never segments.
    {
        bool trigger = butterflyTID(planningWord) != currentButterflyTID() || butterflySharedWrite(planningWord);
        if (trigger && anyTTLSetStillValid(source, newStructure)) {
            fireTTLSetsForSharedTransition(vm, source, newStructure, "F2: shared ArrayStorage property transition (§4.6)");
            return false; // RESTART after the stop (§4.2 rule).
        }
    }

    // §4.6 (I12): a foreign first WRITE to an SW=0 AS instance runs the
    // per-event SW stop FIRST - F1 fire + (installerTID, 1) flat publication
    // inside ensureSharedWriteBit's AS carve-out. Called with NO lock held
    // (GT11 veneer contract). RESTART so the re-dispatch sees SW=1 and the
    // AS-COPY below preserves the (installerTID, 1) tag verbatim (I27/T3).
    if (!butterflySharedWrite(planningWord) && butterflyWriterIsForeign(planningWord)) {
        ensureSharedWriteBit(vm, object);
        return false;
    }

    size_t oldOutOfLineCapacity = source->outOfLineCapacity();
    size_t newOutOfLineCapacity = newStructure->outOfLineCapacity();
    RELEASE_ASSERT(newOutOfLineCapacity >= oldOutOfLineCapacity); // I18/I30: live storage never shrinks.

    // I31/L5: every AS access AND transition is cell-locked flag-on (E4
    // excludes AS), so the under-lock allocate + copy cannot race lock-free
    // stores; SW flips are per-event stops, other AS relayouts hold this lock,
    // and lock-free §4.4 CASes never target AS words - the locked window owns
    // the {structureID lane, butterfly word, AS payload} outright. DeferGC so
    // allocateMoreOutOfLineStorage cannot poll/park while we hold the cell
    // lock (O2; matches growOutOfLineStorageForConcurrentLockedAdd's caller
    // contract).
    DeferGC deferGC(vm);
    JSCellLock& cellLock = object->cellLock();
    lockCellChecked(cellLock); // O3/I20 depth witness (Task 10)

    // Re-validate under the lock: a racing locked AS transition / per-event
    // stop may have moved the structure between step 0 and the lock.
    if (structureIDAtomic(object)->load(std::memory_order_seq_cst) != sourceID.bits()) {
        unlockCellChecked(cellLock);
        return false; // RESTART
    }
    uint64_t lockedWord = butterflyWordAtomic(object)->load(std::memory_order_seq_cst);
    RELEASE_ASSERT(lockedWord & butterflyPointerMask);
    RELEASE_ASSERT(!isSegmentedButterfly(lockedWord)); // I31
    // The §4.6 SW stop above + I4 (SW monotonic) + every AS word mutation
    // being cell-locked / per-event-stopped means the tag cannot have moved
    // foreign+SW=0 here (the only lock-free AS word actor is the SW stop's
    // own publication, which sets SW=1).
    ASSERT(butterflySharedWrite(lockedWord) || !butterflyWriterIsForeign(lockedWord));

    Butterfly* targetButterfly = untaggedButterfly(lockedWord);
    if (newOutOfLineCapacity > oldOutOfLineCapacity) {
        // AS-COPY (T3): allocateMoreOutOfLineStorage copies the AS payload
        // (out-of-line slots + IndexingHeader + ArrayStorage) verbatim; the
        // tag is preserved by publishArrayStorageButterflyLocked below.
        targetButterfly = object->allocateMoreOutOfLineStorage(vm, oldOutOfLineCapacity, newOutOfLineCapacity);
    }

    // Step 4 (M2/I9): release-store the value BEFORE the new StructureID is
    // visible (no holes). Under the cell lock no AS reader can dereference the
    // slot meanwhile (every AS out-of-line read is cell-locked,
    // getDirectConcurrent I31/L5).
    unsigned outOfLineIndex = outOfLineButterflyIndex(offset);
    RELEASE_ASSERT(outOfLineIndex < newOutOfLineCapacity);
    reinterpret_cast<Atomic<uint64_t>*>(targetButterfly->propertyStorage() - (outOfLineIndex + 1))->store(JSValue::encode(value), std::memory_order_release);

    // Step 5 (M5/M8): nuke-bracketed publication. Under the AS cell lock the
    // structureID lane is ours (E4 excludes AS, every AS transition is locked
    // or STW), so the nuke CAS must succeed.
    uint32_t previousIDBits = structureIDAtomic(object)->compareExchangeStrong(sourceID.bits(), sourceID.nuke().bits());
    RELEASE_ASSERT(previousIDBits == sourceID.bits());
    WTF::storeStoreFence();
    if (targetButterfly != untaggedButterfly(lockedWord)) {
        // T3/I17 publication form: tag preserved verbatim; CAS cannot lose
        // (RELEASE_ASSERTed inside).
        publishArrayStorageButterflyLocked(vm, object, targetButterfly);
    }
    // Remaining semantic header bytes + the un-nuked new ID. Property adds
    // change neither type, indexing mode, nor (here) the inline flags - the
    // helper is the established M8 byte-lane sequence (CAS-merges the volatile
    // perCellBit / lock-bit lanes), so it is safe even when every byte is
    // unchanged.
    storeRemainingSemanticHeaderBytesAndStructureID(object, newStructure);

    unlockCellChecked(cellLock);

    vm.writeBarrier(object);
    vm.writeBarrier(object, newStructure);
    vm.writeBarrier(object, value);
    return true;
}

namespace {

ALWAYS_INLINE void fillFragmentSlotWithHole(WriteBarrierBase<Unknown>* slot, bool fillDouble)
{
    if (fillDouble)
        *std::bit_cast<double*>(slot) = PNaN; // §4.7 raw hole.
    else
        slot->clear();
}

} // anonymous namespace

// §4.4 T2: replacement-spine vectorLength growth. See the header comment.
//
// Two modes, keyed on whether the loaded spine is FULL-COVERAGE
// (vectorLength == 4 * indexedFragmentCount - 1):
//
//   (a) Full-coverage spine: pure copy + append of fresh, fully hole-filled
//       fragments, published by ONE lock-free casButterfly. No write ever
//       touches a shared old fragment, so there is no clobber race and no
//       aliased-tail exposure.
//
//   (b) The spine carries a conversion-era C2 tail (vectorLength <
//       coverage). T3-segmented-born-fullcoverage made this the RARE
//       residual: flag-on optimalContiguousVectorLength rounds every
//       freshly-sized contiguous flat VL so (1+VL)%4 == 0 (no tail at
//       conversion), and §4.2 step 3 additionally hole-fills any
//       slack-backed tail and publishes full coverage. Mode (b) now fires
//       only for a flat butterfly that was sized OUTSIDE
//       optimalContiguousVectorLength AND whose heap-cell size class left
//       no usable slack past flatVectorLength, or after an explicit
//       shrink/reshape. When it does fire: those tail slots alias memory
//       PAST the flat allocation's precise end (Butterfly sizing makes
//       flat vectorLengths odd, so a VL % 4 == 1 conversion leaves a
//       2-slot tail outside the recorded aliasedAllocationSize) - they may
//       not exist and are never dereferenced (C2/C4). Raising vectorLength
//       across them is therefore ILLEGAL, and hole-filling them
//       pre-publication could also clobber a racing grow+store (I21).
//       Resolution: rebuild ALL indexed fragments
//       FRESH under a §10.6 per-event stop - I34 guarantees no access is
//       mid-flight across a stop (no path holds a slot pointer across a
//       poll), so indexed-fragment identity (including fragment 0's
//       publicLength slot) may change there; stale spines frozen on reader
//       stacks stay readable (conservative scan, I7) and self-consistent.
//       Everything is pre-allocated outside the stop (O4); the spine
//       publishes with vectorLength == its full coverage, so every later
//       grow takes mode (a).
//
// Out-of-line fragments are aliased verbatim in both modes;
// aliasedAllocationBase/Size are copied VERBATIM regardless (the out-of-line
// side may still alias the flat allocation - I7).
bool tryGrowSegmentedVectorLength(VM& vm, JSObjectWithButterfly* object, unsigned newVectorLength)
{
    RELEASE_ASSERT(Options::useJSThreads());
    ASSERT(vm.currentThreadIsHoldingAPILock());
    ASSERT(newVectorLength <= MAX_STORAGE_VECTOR_LENGTH);

    uint64_t word = butterflyWordAtomic(object)->load(std::memory_order_seq_cst);
    if (!isSegmentedButterfly(word))
        return false; // Re-dispatch: the word left the segmented regime (cannot happen outside STW today; defensive).
    ButterflySpine* spine = butterflySpine(word);
    spine->tsanConsume(); // V7: published spine; import the publisher's clock.
    if (spine->vectorLengthConcurrent() >= newVectorLength)
        return true; // A racing T2 grow already covers us.

    // C2: gaining elements on a header-less spine is a §4.3 SHAPE transition
    // (new spine + header fragment), not a T2 resize.
    RELEASE_ASSERT(spine->indexedFragmentCountConcurrent());

    uint32_t outOfLineFragments = spine->outOfLineFragmentCountConcurrent();
    uint32_t oldIndexedFragments = spine->indexedFragmentCountConcurrent();
    uint32_t oldCoveredVectorLength = oldIndexedFragments * butterflyFragmentSlots - 1;
    bool fullCoverage = spine->vectorLengthConcurrent() == oldCoveredVectorLength;

    // +1 = the header slot (C2).
    uint32_t neededIndexedFragments = std::max<uint32_t>(oldIndexedFragments,
        static_cast<uint32_t>((static_cast<uint64_t>(newVectorLength) + 1 + (butterflyFragmentSlots - 1)) / butterflyFragmentSlots));
    if (fullCoverage)
        ASSERT(neededIndexedFragments > oldIndexedFragments); // vectorLength < newVectorLength and old coverage exhausted.

    // ---- Geometric over-allocation (T1-butterfly-stw-growth): exact-fit
    // coverage made a steady append stream replace the spine every
    // butterflyFragmentSlots elements (the locksites profile counted 2.25M
    // grow calls on the W=16 scale bench). Over-allocate coverage to the same
    // 1.5x policy the flat T1 path (and flag-off ensureLengthSlow) already
    // uses via nextLength(), so a growing array takes O(log n) replacement
    // spines instead of O(n) - and a mode-(b) tail migration's ONE stop buys
    // the whole next 1.5x window instead of four slots. Pure amortization:
    // the published spine still covers exactly >= newVectorLength (asserted
    // below), every protocol step is unchanged, and the memory policy matches
    // what the same array would have held flag-off.
    uint32_t maxIndexedFragments = static_cast<uint32_t>((static_cast<uint64_t>(MAX_STORAGE_VECTOR_LENGTH) + 1 + (butterflyFragmentSlots - 1)) / butterflyFragmentSlots);
    uint64_t amortizedVectorLength = std::min<uint64_t>(nextLength(newVectorLength), MAX_STORAGE_VECTOR_LENGTH);
    uint32_t amortizedIndexedFragments = static_cast<uint32_t>((amortizedVectorLength + 1 + (butterflyFragmentSlots - 1)) / butterflyFragmentSlots);
    neededIndexedFragments = std::min(std::max(neededIndexedFragments, amortizedIndexedFragments), maxIndexedFragments);
    ASSERT(static_cast<uint64_t>(neededIndexedFragments) * butterflyFragmentSlots - 1 >= newVectorLength); // The exact-fit need survives the cap (newVectorLength <= MAX_STORAGE_VECTOR_LENGTH).
    uint32_t coveredVectorLength = neededIndexedFragments * butterflyFragmentSlots - 1; // Use every allocated slot (C4 bounds derefs).
    uint32_t publishedVectorLength = std::min<uint32_t>(coveredVectorLength, MAX_STORAGE_VECTOR_LENGTH);
    ASSERT(publishedVectorLength >= newVectorLength);

    // Hole-fill is shape-keyed (§4.7: Double fragments are raw 8B lanes,
    // holes = PNaN; everything else clears). The shape is re-checked
    // immediately before publication: a per-event Double relabel stop (I28)
    // can only intervene at a poll/allocation, and there is none between the
    // re-check and the publication (straight-line, I34-style); in mode (b)
    // the publication itself is inside a stop, which re-checks too.
    bool fillDouble = hasDouble(object->indexingType());

    // ---- Allocate everything outside any lock/stop (mode (a): §4.4 resizes
    // are lock-free, O1 does not apply; mode (b): O4 - the stop closure
    // allocates nothing). Fresh fragments are fully hole-filled.
    //
    // Review round 4 (blocker fix): DeferGC across the allocate-to-publication
    // window. The fresh fragments are stored ONLY into the not-yet-published
    // newSpine, whose contents the conservative scan never traces (auxiliary
    // cells are visited only via the owning object after publication) - so a
    // collection triggered by a later fragment allocation would sweep the
    // earlier ones. Held across the mode-(b) stop too: the stop closure
    // allocates nothing (O4) and never collects, and the publication CAS that
    // makes the fragments object-reachable runs inside it.
    DeferGC deferGC(vm);
    uint32_t aliasedIndexedFragments = fullCoverage ? oldIndexedFragments : 0; // Mode (b) rebuilds ALL indexed fragments.
    ButterflySpine* newSpine = static_cast<ButterflySpine*>(vm.auxiliarySpace().allocate(
        vm, ButterflySpine::allocationSize(outOfLineFragments + neededIndexedFragments), nullptr, AllocationFailureMode::Assert));
    butterflyConcurrentStore(&newSpine->outOfLineFragmentCount, outOfLineFragments); // V7: paired with reader-side relaxed loads.
    butterflyConcurrentStore(&newSpine->indexedFragmentCount, neededIndexedFragments);
    butterflyConcurrentStore(&newSpine->vectorLength, publishedVectorLength);
    butterflyConcurrentStore(&newSpine->spineEpoch, butterflyConcurrentLoad(&spine->spineEpoch) + 1);
    butterflyConcurrentStore(&newSpine->aliasedAllocationBase, butterflyConcurrentLoad(&spine->aliasedAllocationBase)); // VERBATIM (I7)
    butterflyConcurrentStore(&newSpine->aliasedAllocationSize, butterflyConcurrentLoad(&spine->aliasedAllocationSize));
    for (uint32_t j = 0; j < outOfLineFragments + aliasedIndexedFragments; ++j)
        butterflyConcurrentStore(&newSpine->fragments()[j], butterflyConcurrentLoad(&spine->fragments()[j])); // Shared fragments aliased verbatim (mode (a) incl. fragment 0 = the shared publicLength slot, C4).
    for (uint32_t f = aliasedIndexedFragments; f < neededIndexedFragments; ++f) {
        auto* fragment = static_cast<ButterflyFragment*>(
            vm.auxiliarySpace().allocate(vm, sizeof(ButterflyFragment), nullptr, AllocationFailureMode::Assert));
        for (size_t slotIndex = 0; slotIndex < butterflyFragmentSlots; ++slotIndex)
            fillFragmentSlotWithHole(&fragment->slots[slotIndex], fillDouble);
        butterflyConcurrentStore(&newSpine->fragments()[outOfLineFragments + f], fragment);
    }
    newSpine->validateConsistency();

    if (fullCoverage) {
        // ---- Mode (a): ONE lock-free 64-bit CAS, tag (notTTLTID, 1)
        // (I27/T2). Failure => re-dispatch on the fresh tag, never
        // blind-retry (§4.4).
        newSpine->tsanPublish(); // V7: last pre-publication store done.
        WTF::storeStoreFence(); // Spine contents before publication (M2-equivalent).
        if (hasDouble(object->indexingType()) != fillDouble)
            return false; // I28: a relabel stop intervened during allocation; re-dispatch with the right fill.
        if (!casButterfly(object, word, encodeSegmentedButterfly(newSpine)))
            return false;
        vm.writeBarrier(object); // Publication barrier, like setButterfly (§4.5/I25).
        return true;
    }

    // ---- Mode (b): per-event stop migration (no §6-ranked lock held - GT11).
    //
    // STW dedup (T1-butterfly-stw-growth): a tail-carrying spine's FIRST grow
    // is raced by every appender of a freshly shared array, and each racer
    // used to pay a full rendezvous only for its closure's word re-check to
    // no-op (the locksites profile measured this line as the single largest
    // STW source at W=16). The claim winner migrates; losers park-wait until
    // the word moves and re-dispatch stop-free (the winner published full
    // coverage, so they land on the lock-free mode (a) - or return covered).
    // The pre-stop word re-validation below additionally catches plans gone
    // stale during the allocation window without paying a rendezvous.
    bool published = false;
    {
        PerEventStopClaim claim(vm, object, [&] {
            return butterflyWordAtomic(object)->load(std::memory_order_seq_cst) != word;
        });
        if (!claim.acquired())
            return false; // The word moved while a competing migration was in flight: re-dispatch on the fresh state.
        if (butterflyWordAtomic(object)->load(std::memory_order_seq_cst) != word)
            return false; // Cheap pre-stop re-validation: never pay a rendezvous for a stale plan.
        jsThreadsStopTheWorldAndRun(vm, scopedLambda<void()>([&] {
        // Re-verify inside the stop; allocate nothing (O4).
        if (butterflyWordAtomic(object)->load(std::memory_order_seq_cst) != word)
            return; // The world moved before the stop landed: re-dispatch.
        if (hasDouble(object->indexingType()) != fillDouble)
            return; // I28 relabel raced the planning: re-dispatch with the right fill.
        // Copy the live indexed contents - the header slot (fragment 0 slot 0:
        // shared publicLength + frozen flat-era vectorLength, I9b) verbatim,
        // and every element below the OLD spine's vectorLength as a raw
        // 64-bit lane (same shape on both sides: JSValues and raw doubles
        // copy bit-exactly; world stopped, so nothing races the copy).
        ButterflyFragment* oldHeaderFragment = spine->indexedFragment(0);
        ButterflyFragment* newHeaderFragment = newSpine->indexedFragment(0);
        // TSAN (r11 report 12): the source lanes were written with relaxed
        // atomic lane stores (trySetIndexQuicklyConcurrent) BEFORE the stop
        // landed; the stop handshake is the real happens-before edge, but
        // TSAN cannot see it through the safepoint, so read the lanes with
        // relaxed atomic loads (same instruction; world is stopped, the
        // values cannot change under us). Destination writes stay plain:
        // newSpine is pre-publication (tsanPublish below).
        *std::bit_cast<uint64_t*>(&newHeaderFragment->slots[0]) = WTF::atomicLoad(std::bit_cast<uint64_t*>(&oldHeaderFragment->slots[0]), std::memory_order_relaxed);
        for (uint32_t i = 0; i < spine->vectorLengthConcurrent(); ++i)
            *std::bit_cast<uint64_t*>(newSpine->indexedSlot(i)) = WTF::atomicLoad(std::bit_cast<uint64_t*>(spine->indexedSlot(i)), std::memory_order_relaxed);
        newSpine->tsanPublish(); // V7: last pre-publication store done (mode (b): inside the stop).
        WTF::storeStoreFence(); // Contents before publication.
        // World-stopped => unraced; CAS anyway so the publication stays
        // I17-shaped (cf. the Task 7 AS per-event stop).
        bool ok = casButterfly(object, word, encodeSegmentedButterfly(newSpine));
        RELEASE_ASSERT(ok);
        published = true;
        }));
    } // Release the PerEventStopClaim before returning either way.
    if (!published)
        return false; // Re-dispatch on the fresh state.
    vm.writeBarrier(object); // Publication barrier (§4.5/I25); the old spine's
    // tailed fragments are never written again; stale readers see a frozen
    // snapshot (I7: conservative scan keeps them alive).
    return true;
}

// Flag-on JSObject::ensureLengthSlow (GT10). See the header comment for the
// dispatch table (T1/T2/§4.8; T5 removed in review round 1).
bool ensureLengthSlowConcurrent(VM& vm, JSObjectWithButterfly* object, unsigned length)
{
    RELEASE_ASSERT(Options::useJSThreads());
    ASSERT(vm.currentThreadIsHoldingAPILock());
    ASSERT(length <= MAX_STORAGE_VECTOR_LENGTH);

    while (true) {
        // §4.8 first (I35): CoW words never reach SW=1 or the §4.4 CASes.
        if (isCopyOnWrite(object->indexingMode())) [[unlikely]] {
            uint64_t cowWord = butterflyWordAtomic(object)->load(std::memory_order_seq_cst);
            if (!butterflyWriterIsForeign(cowWord)) // incl. §9.6 forceButterflySWBit
                object->ensureWritable(vm); // Owner materialization: flag-on this routes through materializeCopyOnWriteButterflyConcurrent (round 3); the winner stamps its own (TID, 0).
            else
                ensureSharedWriteBit(vm, object); // Foreign: §4.8 materialize-first, then re-dispatch.
            continue;
        }

        ASSERT(hasContiguous(object->indexingType()) || hasInt32(object->indexingType())
            || hasDouble(object->indexingType()) || hasUndecided(object->indexingType())); // AS never reaches ensureLength (GT10).

        uint64_t word = butterflyWordAtomic(object)->load(std::memory_order_seq_cst);
        RELEASE_ASSERT(word & butterflyPointerMask); // ensureLength's precondition: indexed storage exists.

        // ---- Segmented: T2.
        if (isSegmentedButterfly(word)) [[unlikely]] {
            if (butterflySpine(word)->vectorLengthConcurrent() >= length) // V7 (TSAN): relaxed read of the published spine.
                return true;
            tryGrowSegmentedVectorLength(vm, object, length);
            continue; // Success and failure both re-dispatch; the next pass re-reads the fresh spine.
        }

        // ---- Flat, foreign or SW=1: convert in place (§4.2 nullptr form),
        // re-dispatch lands on T2. SW=1 flat resizes never copy element
        // storage outside STW (I27); T1 is owner-only (I21). §9.6 (Task 10):
        // forceSegmentedButterflies routes the owner T1 form here too, so
        // every resize allocation lands on the segmented T2 path.
        if (butterflySharedWrite(word) || butterflyTID(word) != currentButterflyTID()
            || forceSegmentedButterfliesEnabled()) {
            convertToSegmentedButterfly(vm, object, nullptr, nullptr, invalidOffset, JSValue());
            continue;
        }

        Butterfly* butterfly = untaggedButterfly(word);
        unsigned oldVectorLength = butterfly->vectorLength();
        if (oldVectorLength >= length)
            return true; // Defensive (the owner cannot race itself).
        IndexingType type = object->indexingType();
        Structure* structure = object->structure(); // Flag-on structure() is nuke-masked (M5); flat owner words keep capacities stable (foreign out-of-line adds go segmented).
        unsigned propertyCapacity = structure->outOfLineCapacity();
        unsigned availableOldLength = Butterfly::availableContiguousVectorLength(propertyCapacity, oldVectorLength);

        // ---- T5 REMOVED (adversarial-review round 1): the in-place
        // vectorLength growth (hole-fill [oldVL,newVL), fence, setVectorLength
        // on the CURRENT payload) was the only growth form that raised a
        // lock-free foreign reader's flat bound without publishing fresh
        // storage behind the butterfly-word load. The SW bit gates foreign
        // WRITES only - foreign READS of a flat word are always legal (§3) -
        // and a reader's vectorLength load and slot load have only a CONTROL
        // dependency between them (no address dependency: same base pointer),
        // which does not order load->load on arm64. Such a reader could pair
        // the post-growth vectorLength with a pre-hole-fill slot load and
        // materialize uninitialized tryCreateUninitialized slack as a JSValue.
        // The writer-side cell lock cannot help: readers are lock-free by
        // design. So flag-on, EVERY flat owner growth takes the T1 fresh-copy
        // route below - the new payload is only reachable through the freshly
        // CASed butterfly word, so slot loads are dependency-ordered behind a
        // word load that already sees the new vectorLength (same argument as
        // T2/conversion publications).
        UNUSED_VARIABLE(availableOldLength);

        // ---- T1: owner-only lock-free copying resize, expected tag exactly
        // (currentButterflyTID(), 0) (I27). Always a FRESH allocation flag-on
        // (M8 disables in-place butterfly reallocs; we do not rely on the
        // manifest-4b flag and copy explicitly).
        unsigned newVectorLength = Butterfly::optimalContiguousVectorLength(
            propertyCapacity, std::min<size_t>(nextLength(length), MAX_STORAGE_VECTOR_LENGTH));
        GCDeferralContext deferralContext(vm);
        Butterfly* newButterfly = Butterfly::tryCreateUninitialized(
            vm, object, 0, propertyCapacity, true, static_cast<size_t>(newVectorLength) * sizeof(EncodedJSValue), &deferralContext);
        if (!newButterfly) [[unlikely]]
            return false;
        {
            AssertNoGC assertNoGC; // Straight-line copy -> CAS window; no polls (I34-style).
            // Prefix (out-of-line properties + IndexingHeader) and live
            // elements. The only writers of a (currentTID, 0) payload are this
            // thread and foreign first-writers - who must flip SW first,
            // changing the word and failing the CAS below; a torn copy is
            // therefore discarded, never published (I21).
            // V7 (TSAN): the source payload's words may be concurrently stored
            // ATOMICALLY by a foreign first-writer whose SW flip lands after
            // our `word` snapshot (its flip changes the word and fails our CAS,
            // so a torn copy is discarded, never published — the protocol
            // argument above is unchanged); the destination is fresh aux memory
            // whose recycled address can pair with stale readers. Both make the
            // copy a benign-but-TSAN-visible race only.
            // butterflyConcurrentCopyWords is the established treatment
            // (Butterfly.h): the relaxed word loop UNDER TSAN ONLY, plain
            // vectorized memcpy in production. seg-t1-atomic-copy-loop:
            // restores parity with flag-off ensureLengthSlow's memmove via
            // Butterfly::resizeArray and reuses the established helper; no
            // measured wall-time delta on x86-64 (relaxed atomics compile to
            // plain MOV, so the prior scalar loop already paid no
            // atomic-instruction cost — SHAREDHEAP-ALLOC-EVIDENCE.md §41).
            {
                size_t copyBytes = propertyCapacity * sizeof(EncodedJSValue) + sizeof(IndexingHeader) + static_cast<size_t>(oldVectorLength) * sizeof(EncodedJSValue);
                butterflyConcurrentCopyWords(newButterfly->base(0, propertyCapacity), butterfly->base(0, propertyCapacity), copyBytes);
            }
            if (hasDouble(type)) {
                for (unsigned i = oldVectorLength; i < newVectorLength; ++i)
                    newButterfly->indexingPayload<double>()[i] = PNaN;
            } else {
                for (unsigned i = oldVectorLength; i < newVectorLength; ++i)
                    newButterfly->indexingPayload<WriteBarrier<Unknown>>()[i].clear();
            }
            newButterfly->setVectorLength(newVectorLength);
            WTF::storeStoreFence(); // Contents before publication.
            if (casButterfly(object, word, encodeButterfly(newButterfly, currentButterflyTID(), false))) {
                vm.writeBarrier(object);
                return true;
            }
        }
        // CAS failed: an SW flip (or conversion) won mid-resize. NEVER
        // re-copy (I27/I21): re-dispatch - the next pass lands on the
        // shared/segmented branch (T2).
    }
}

// Flag-on JSObject::reallocateAndShrinkButterfly (GT10). See the header
// comment for the dispatch table.
void shrinkButterflyForSetLengthConcurrent(VM& vm, JSObjectWithButterfly* object, unsigned length)
{
    RELEASE_ASSERT(Options::useJSThreads());
    ASSERT(vm.currentThreadIsHoldingAPILock());
    ASSERT(length <= MAX_STORAGE_VECTOR_LENGTH);
    ASSERT(hasContiguous(object->indexingType()) || hasInt32(object->indexingType())
        || hasDouble(object->indexingType()) || hasUndecided(object->indexingType()));
    ASSERT(!isCopyOnWrite(object->indexingMode())); // Callers ensureWritable first (as today).

    while (true) {
        uint64_t word = butterflyWordAtomic(object)->load(std::memory_order_seq_cst);
        RELEASE_ASSERT(word & butterflyPointerMask);
        bool fillDouble = hasDouble(object->indexingType());

        // ---- Segmented: truncate through the loaded spine (C4/I33);
        // publicLength is the SHARED fragment-0 slot, so no republication.
        // Review round 3: a §3 dense store may legally land in
        // [length, vectorLength) between the clear loop and the plain
        // setSegmentedPublicLength below; that is a program-level shrink-vs-
        // grow race (SAB semantics), and it can no longer hide a live value
        // from the marker - the §4.5 visit value-bounds segmented elements by
        // the spine's vectorLength (storage bound), not publicLength.
        if (isSegmentedButterfly(word)) [[unlikely]] {
            ButterflySpine* spine = butterflySpine(word);
            spine->tsanConsume(); // V7: published spine.
            uint32_t bound = std::min(segmentedPublicLength(spine), spine->vectorLengthConcurrent());
            for (uint32_t i = length; i < bound; ++i) {
                WriteBarrierBase<Unknown>* slot = spine->indexedSlot(i);
                if (fillDouble)
                    *std::bit_cast<double*>(slot) = PNaN; // §4.7 raw hole.
                else
                    slot->clear();
            }
            WTF::storeStoreFence();
            setSegmentedPublicLength(spine, length);
            return;
        }

        // ---- Flat foreign, SW=0: a shrink is a foreign WRITE - F1 first (§3).
        if (!butterflySharedWrite(word)
            && butterflyWriterIsForeign(word)) { // incl. §9.6 forceButterflySWBit
            ensureSharedWriteBit(vm, object);
            continue;
        }

        Butterfly* butterfly = untaggedButterfly(word);

        // ---- In-place truncation: flat SW=1 (no copy outside STW, I27), or
        // the capacity no longer exceeds `length` (a racing republication
        // landed; the copy-shrink is an optimization only). The stores are
        // the §3 "owner, or SW=1" rule either way. Review round 3: a racing
        // dense store into [length, vectorLength) is a program-level
        // shrink-vs-grow race and stays GC-visible - the §4.5 flat visit
        // value-bounds SW=1 words by vectorLength, not publicLength.
        if (butterflySharedWrite(word) || butterfly->vectorLength() <= length) {
            uint32_t bound = std::min(butterfly->vectorLength(), butterfly->publicLength());
            if (fillDouble) {
                for (uint32_t i = length; i < bound; ++i)
                    butterfly->indexingPayload<double>()[i] = PNaN;
            } else {
                for (uint32_t i = length; i < bound; ++i)
                    butterfly->indexingPayload<WriteBarrier<Unknown>>()[i].clear();
            }
            WTF::storeStoreFence();
            butterfly->setPublicLength(length);
            return;
        }

        // §9.6 (Task 10): forceSegmentedButterflies - the owner copy-shrink
        // below allocates a fresh FLAT butterfly; under stress convert instead
        // (in-place §4.2 form) and re-dispatch onto the segmented truncation.
        if (forceSegmentedButterfliesEnabled()) [[unlikely]] {
            convertToSegmentedButterfly(vm, object, nullptr, nullptr, invalidOffset, JSValue());
            continue; // Success and RESTART both re-dispatch on the fresh tag.
        }

        // ---- Flat owner (currentTID, 0): today's copy-shrink, published by
        // casButterfly (I17/I27 owner form).
        ASSERT(butterfly->vectorLength() > length && butterfly->publicLength() >= length); // As today's asserts.
        ASSERT(!butterfly->indexingHeader()->preCapacity(object->structure()));
        DeferGC deferGC(vm);
        Butterfly* newButterfly = butterfly->resizeArray(vm, object, object->structure(), 0, ArrayStorage::sizeFor(length));
        newButterfly->setVectorLength(length);
        newButterfly->setPublicLength(length);
        WTF::storeStoreFence();
        if (casButterfly(object, word, encodeButterfly(newButterfly, currentButterflyTID(), false))) {
            vm.writeBarrier(object);
            return;
        }
        // CAS failed (SW flip/conversion won): re-dispatch; the next pass
        // takes the in-place/segmented truncation path. Never re-copy (I27).
    }
}

// ===== Task 8: §9.5 indexed slow paths =====
//
// E5 drivers for indexed element access: full §2 dispatch (None first),
// AS-shape accesses cell-locked (I31/L5), foreign first writes through
// ensureSharedWriteBit (F1, §3), in-shape dense growth through the §4.4
// resize drivers above. A false/empty return sends the caller to its generic
// path (sparse maps, shape transitions, length semantics - locked at their
// own §4.6/L3 sites).

JSValue JSObjectWithButterfly::getIndexConcurrent(unsigned i) const
{
    ASSERT(Options::useJSThreads());
    if (hasAnyArrayStorage(indexingType())) [[unlikely]] {
        // I31/L5: EVERY runtime AS access - reads included, any SW - holds the
        // cell lock; re-load the word under it (AS-COPY republishes).
        Locker locker { cellLock() };
        CellLockDepthScope cellLockDepthScope; // O3/I20 (Task 10)
        uint64_t word = taggedButterflyWord();
        RELEASE_ASSERT(!isSegmentedButterfly(word)); // I31
        const ArrayStorage* storage = untaggedButterfly(word)->arrayStorage();
        if (i >= storage->length())
            return JSValue();
        if (i < storage->vectorLength())
            return storage->m_vector[i].get(); // Empty (hole/sparse) => caller's generic path.
        return JSValue();
    }
    return tryGetIndexQuicklyConcurrent(i, nullptr); // Already full §2 dispatch (Task 2); empty => generic path.
}

bool JSObjectWithButterfly::putIndexConcurrent(VM& vm, unsigned i, JSValue value)
{
    ASSERT(Options::useJSThreads());
    ASSERT(value);

    while (true) {
        if (isCopyOnWrite(indexingMode())) [[unlikely]]
            return false; // §4.8 materialization belongs to the caller's generic path (putByIndex/convertFromCopyOnWrite).

        if (hasAnyArrayStorage(indexingType())) [[unlikely]] {
            if (shouldUseSlowPut(indexingType()))
                return false; // Prototype intercepts: generic path.
            // §4.6: foreign write to an SW=0 AS instance runs the per-event
            // stop first, with no lock held (GT11).
            uint64_t probeWord = taggedButterflyWord();
            if ((probeWord & butterflyPointerMask) && !butterflySharedWrite(probeWord)
                && butterflyWriterIsForeign(probeWord)) // incl. §9.6 forceButterflySWBit
                ensureSharedWriteBit(vm, this);
            Locker locker { cellLock() }; // I31/L5
            CellLockDepthScope cellLockDepthScope; // O3/I20 (Task 10)
            uint64_t word = taggedButterflyWord();
            RELEASE_ASSERT(!isSegmentedButterfly(word)); // I31
            ArrayStorage* storage = untaggedButterfly(word)->arrayStorage();
            if (storage->m_sparseMap)
                return false; // Sparse map semantics: generic (L3-locked) path.
            if (i >= storage->length() || i >= storage->vectorLength())
                return false; // Length bump / vector growth: generic path (its §4.6 mutation sites lock).
            WriteBarrier<Unknown>& slot = storage->m_vector[i];
            if (!slot)
                ++storage->m_numValuesInVector;
            slot.set(vm, this, value);
            return true;
        }

        uint64_t word = taggedButterflyWord();
        if (!(word & butterflyPointerMask))
            return false; // No indexed storage (None): generic path creates it.

        // §3: first foreign write to an SW=0 flat instance - F1 + SW flip
        // (with the R-DOUBLE/CoW/PA carve-outs) inside ensureSharedWriteBit,
        // then re-dispatch on the fresh tag.
        if (!isSegmentedButterfly(word) && !butterflySharedWrite(word)
            && butterflyWriterIsForeign(word)) { // incl. §9.6 forceButterflySWBit
            ensureSharedWriteBit(vm, this);
            continue;
        }

        if (trySetIndexQuicklyConcurrent(vm, i, value, nullptr))
            return true;

        // In-shape dense growth (§4.4): anything else - shape mismatch,
        // sparse territory - is the caller's generic path.
        IndexingType type = indexingType();
        if (hasUndecided(type) || (!hasInt32(type) && !hasDouble(type) && !hasContiguous(type)))
            return false;
        if (hasInt32(type) && !value.isInt32())
            return false; // Shape transition (generic; §4.3/§4.7 on shared objects).
        if (hasDouble(type) && (!value.isNumber() || value.asNumber() != value.asNumber()))
            return false;
        if (i >= MIN_SPARSE_ARRAY_INDEX || i + 1 > MAX_STORAGE_VECTOR_LENGTH)
            return false; // Sparse/overlong: generic path.
        if (!ensureLengthSlowConcurrent(vm, this, i + 1))
            return false; // OOM.
        // Re-dispatch: the next trySet lands within the grown storage (or the
        // world moved again - loop).
    }
}

// ===== U-T10: §9.5 atomic slot accessors (SPEC-ungil ANNEX C1) =====
//
// GIL-off, SPEC-api 4.5 property Atomics can no longer lean on the VM lock
// for read-modify-write atomicity: the ThreadAtomics.cpp bodies re-home onto
// the accessors below (§C.2), and §C.3(a) routes the property-waiter
// pre-enqueue validation through the Load form. Receivers are restricted by
// the caller's probe to plain structure/butterfly-backed own data slots (the
// D3 exotic-receiver TypeErrors stay in ThreadAtomics.cpp), so the dispatch
// here is exactly the §2 regime split of get/putDirectConcurrent above plus
// the ANNEX C1 write-side protocols:
//
//   - LOCK-FREE ARMS (inline, flat out-of-line, segmented fragment): one
//     seq_cst 64-bit CAS/RMW loop on the EncodedJSValue slot word. NO cell
//     lock on the segmented arm: a lock-held RMW would not serialize against
//     lock-free fragment stores (U5).
//   - FLAT-PATH SW DISCIPLINE: a foreign writer on an SW=0 flat word FIRST
//     runs the §2/§3 foreign-write SW-set DCAS (ensureSharedWriteBit: F1
//     fire-then-flip with the R-DOUBLE/CoW/PA carve-outs), re-validates
//     structureID + butterfly per I34, THEN CASes the slot. Validation
//     failure restarts the WHOLE probe (I33-bounded by the forward-only
//     shape order); a completed RMW/CAS is NEVER re-applied. Flat GROW is
//     butterfly-CAS + copy with NO nuke, so an old-butterfly CAS would be
//     silently lost - hence the post-resolution word re-validation, after
//     which the payload is pinned (T1 copies are owner-only against a
//     (currentTID, 0) word: we are that owner, or SW=1 - I27).
//   - THIRD ARM (OM-locked regimes): dictionary (I19/L3) and AS shape
//     (§4.6; Thread.restrict FORCES AS) probe + CAS/RMW UNDER the JSCellLock
//     the OM already requires there. AS PRE-LOCK (r8 item 6): the cell lock
//     suffices only AFTER SW=1 - jit §5.5 owner AS fast paths store UNLOCKED
//     while SW=0 - so SW==0 && foreign writer => FIRST the OM §4.6
//     first-foreign-write protocol (per-event STW, fire-then-publish
//     (installerTID, 1); I10b - it lives inside ensureSharedWriteBit's AS
//     carve-out), then RESTART the probe; only SW=1 (or the owner) enters
//     the locked CAS/RMW. The lock is REQUIRED: dictionary delete is
//     I34-blind, so a lock-free CAS could "succeed" on an absent property
//     (U5) - dictionary-ness and the offset are re-checked under the lock.
//   - INDEXED ARM, BY SHAPE (8g re-freeze): CoW materializes per §4.8/I35
//     first; Int32/Double raw-word CAS is REJECTED - the first atomic access
//     CONVERTS to Contiguous (owner direct, foreign SW-set DCAS first);
//     Contiguous is the flat arm verbatim; ArrayStorage/dictionary-indexed
//     is the third arm. ThreadAtomics' §C.2 probe routes parseIndex hits
//     here; one arm per shape.
//   - Write barrier after success, as §9.5 orders.
//
// The locked third arm never allocates under the cell lock (I20): SVZ
// comparison is allocation-free - rope strings bounce out as
// NeedsStringResolution and the caller resolves them OUTSIDE any lock via
// the §N.2 single-flight protocol, then re-probes.

#if USE(JSVALUE64)

namespace {

// SameValueZero, allocation-free. Mirrors ThreadAtomics.cpp's
// sameValueZeroForAtomics except that rope strings are NOT resolved here;
// for non-string, non-number values sameValue() is pure.
bool atomicSlotSameValueZero(JSGlobalObject* globalObject, JSValue a, JSValue b, bool& needsResolution)
{
    needsResolution = false;
    if (a.isNumber() && b.isNumber()) {
        double x = a.asNumber();
        double y = b.asNumber();
        if (std::isnan(x) && std::isnan(y))
            return true;
        return x == y; // +0 == -0 under SVZ.
    }
    bool aIsString = a.isString();
    bool bIsString = b.isString();
    if (aIsString != bIsString)
        return false;
    if (aIsString) {
        StringImpl* aImpl = asString(a)->tryGetValueImpl();
        StringImpl* bImpl = asString(b)->tryGetValueImpl();
        if (!aImpl || !bImpl) {
            // Rope: resolving may allocate - illegal under the cell lock
            // (I20) and unwanted inside the CAS loop. Bounce to the caller.
            needsResolution = true;
            return false;
        }
        return WTF::equal(aImpl, bImpl);
    }
    return sameValue(globalObject, a, b); // Non-string residue: allocation-free.
}

ALWAYS_INLINE bool atomicSlotOperationWrites(AtomicSlotOperation operation)
{
    return operation != AtomicSlotOperation::Load;
}

// Evaluates the request against the value read. true => the caller stores
// newValue (and reports Applied after the store lands); false => no write,
// status already set (Applied for Load, NotEqual, NotNumber,
// NeedsStringResolution). Allocation-free (jsNumber() is NaN-boxed bits).
bool atomicSlotEvaluate(JSGlobalObject* globalObject, const AtomicSlotRequest& request, JSValue current, JSValue& newValue, AtomicSlotStatus& status)
{
    switch (request.operation) {
    case AtomicSlotOperation::Load:
        status = AtomicSlotStatus::Applied;
        return false;
    case AtomicSlotOperation::Exchange:
        newValue = request.replacement;
        return true;
    case AtomicSlotOperation::CompareExchangeSVZ: {
        bool needsResolution = false;
        bool equal = atomicSlotSameValueZero(globalObject, current, request.expected, needsResolution);
        if (needsResolution) {
            status = AtomicSlotStatus::NeedsStringResolution;
            return false;
        }
        if (!equal) {
            status = AtomicSlotStatus::NotEqual;
            return false;
        }
        newValue = request.replacement;
        return true;
    }
    case AtomicSlotOperation::Add:
    case AtomicSlotOperation::Sub:
    case AtomicSlotOperation::And:
    case AtomicSlotOperation::Or:
    case AtomicSlotOperation::Xor:
        break;
    }
    if (!current.isNumber()) {
        status = AtomicSlotStatus::NotNumber;
        return false;
    }
    switch (request.operation) {
    case AtomicSlotOperation::Add:
        newValue = jsNumber(current.asNumber() + request.operandNumber);
        break;
    case AtomicSlotOperation::Sub:
        newValue = jsNumber(current.asNumber() - request.operandNumber);
        break;
    case AtomicSlotOperation::And:
        newValue = jsNumber(JSC::toInt32(current.asNumber()) & request.operandInt);
        break;
    case AtomicSlotOperation::Or:
        newValue = jsNumber(JSC::toInt32(current.asNumber()) | request.operandInt);
        break;
    case AtomicSlotOperation::Xor:
        newValue = jsNumber(JSC::toInt32(current.asNumber()) ^ request.operandInt);
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }
    return true;
}

// The ANNEX C1 lock-free arm core: one seq_cst 64-bit CAS/RMW loop on the
// EncodedJSValue slot word. The slot's storage identity must already be
// pinned by the caller's I34 validation; an empty word (hole / mid-delete)
// restarts the whole probe. The loop re-reads and re-EVALUATES on CAS
// failure - the never-re-apply rule governs probe restarts, and no write has
// landed when the CAS fails.
//
// U-T10 amend (U5 hardening): the entry/I34 checks validate the
// {offset, structureID} provenance ONCE, but the loop can spin across owner
// transitions, so every iteration re-validates structureID between the
// seq_cst slot load and the CAS. Without it, a flat->dictionary conversion
// (the butterfly word is untouched) followed by a dictionary delete - or a
// plain non-dictionary delete (structure-only transition, word untouched) -
// could land its D1 jsUndefined quarantine store (I30) under the loop's
// nose: a CompareExchangeSVZ with expected===undefined would CAS-"succeed"
// on an ABSENT property (the exact U5 outcome the third arm's lock exists to
// forbid), and Load / a failed CAS would surface undefined as a read of a
// property that never held it. The ID check alone is NOT sufficient for
// named slots: non-dictionary deletes D1-store the sentinel BEFORE the
// structure publication (storeUndefinedIntoDoomedSlotConcurrent, "value
// ordered before the edit that publishes its death"), so a named-slot
// jsUndefined read is AMBIGUOUS while a delete is mid-window - those bounce
// out with LockedRevalidate, and the named caller disambiguates under the
// cell lock (both delete flavors run their sentinel-store -> publication
// window entirely under it, §6 L4). Indexed callers pass
// revalidateUndefined=false: indexed deletes/holes are EMPTY JSValue()s
// (never the jsUndefined sentinel), which the !current check restarts.
JSValue atomicSlotLockFreeLoop(JSGlobalObject* globalObject, VM& vm, JSObject* owner, WriteBarrierBase<Unknown>* slot, StructureID expectedStructureID, bool revalidateUndefined, const AtomicSlotRequest& request, AtomicSlotStatus& status)
{
    Atomic<uint64_t>* atomicSlot = std::bit_cast<Atomic<uint64_t>*>(slot);
    while (true) {
        uint64_t currentBits = atomicSlot->load(std::memory_order_seq_cst);
        JSValue current = JSValue::decode(currentBits);
        if (!current) [[unlikely]] {
            status = AtomicSlotStatus::Restart;
            return { };
        }
        // Provenance re-validation, ordered AFTER the slot load (the seq_cst
        // load's acquire half keeps this read from hoisting above it): any
        // published transition since the probe restarts the whole probe.
        if (owner->structureID() != expectedStructureID) [[unlikely]] {
            status = AtomicSlotStatus::Restart;
            return { };
        }
        if (revalidateUndefined && current.isUndefined()) [[unlikely]] {
            // Possible D1 quarantine sentinel (named slots only, see above):
            // the caller re-validates under the cell lock. Nothing applied.
            status = AtomicSlotStatus::LockedRevalidate;
            return { };
        }
        // §C.2 accessor-clobber fix (I9 in-flight define): putDirectInternal's
        // define-own arm publishes the GetterSetter/CustomGetterSetter VALUE
        // before the attribute change lands (putDirectOffset under the cell
        // lock; attributeChangeTransition after it drops), so the probe's
        // attributes and this provenance can both still read "plain data"
        // while the slot already holds the accessor cell. CASing a primitive
        // over it converts a racing accessor into a data property — a heap
        // state no sequential interleaving of Atomics ops can produce (D3/D7).
        // Restart: the re-probe spins until the writer's publication lands,
        // then classifies Accessor and throws the precise TypeError. Plain
        // data values can never legitimately BE these internal cell types
        // (GetterSetter is never JS-exposed; CustomValue slots never reach
        // the lock-free arms — the probe rejects CustomAccessorOrValue).
        if (current.isCell()) [[likely]] {
            JSType currentType = current.asCell()->type();
            if (currentType == GetterSetterType || currentType == CustomGetterSetterType) [[unlikely]] {
                status = AtomicSlotStatus::Restart;
                return { };
            }
        }
        JSValue newValue;
        if (!atomicSlotEvaluate(globalObject, request, current, newValue, status))
            return current;
        if (atomicSlot->compareExchangeStrong(currentBits, JSValue::encode(newValue), std::memory_order_seq_cst) == currentBits) {
            vm.writeBarrier(owner, newValue); // Write barrier after success, as §9.5 orders.
            status = AtomicSlotStatus::Applied;
            return current;
        }
    }
}

} // anonymous namespace

JSValue JSObject::atomicSlotReadModifyWrite(JSGlobalObject* globalObject, UniquedStringImpl* uid, PropertyOffset offset, StructureID expectedStructureID, const AtomicSlotRequest& request, AtomicSlotStatus& status)
{
    ASSERT(Options::useJSThreads());
    ASSERT(isValidOffset(offset));
    VM& vm = globalObject->vm();
    status = AtomicSlotStatus::Restart;
    bool isWrite = atomicSlotOperationWrites(request.operation);

    // I34 provenance: the offset is only meaningful against the structure the
    // caller probed. structureID() is read RAW (M5): a nuked ID never equals
    // a live probed ID, so nuke windows Restart instead of decoding.
    if (structureID() != expectedStructureID)
        return { };
    Structure* structure = this->structure(); // M5 nuke-masked.
    bool arrayStorageShape = hasAnyArrayStorage(indexingType());
    if (structure->isDictionary() || arrayStorageShape) [[unlikely]] {
        // ---- Third arm: OM-locked regimes. AS PRE-LOCK SW protocol (r8
        // item 6) - and the I12 first-foreign-write fire for the dictionary
        // regime - BEFORE the lock, with no lock held (GT11), then RESTART.
        uint64_t probeWord = taggedButterflyWord();
        if (isWrite && (probeWord & butterflyPointerMask) && !butterflySharedWrite(probeWord)
            && butterflyWriterIsForeign(probeWord)) { // incl. §9.6 forceButterflySWBit
            ensureSharedWriteBit(vm, static_cast<JSObjectWithButterfly*>(this));
            return { }; // Restart the whole probe on the fresh tag.
        }
        Locker locker { cellLock() };
        CellLockDepthScope cellLockDepthScope; // O3/I20
        if (structureID() != expectedStructureID)
            return { };
        // Re-resolve under the lock: dictionary deletes are I34-blind - a
        // lock-free CAS could "succeed" on an absent property (U5). No
        // allocation under the cell lock (I20): getConcurrently only walks.
        unsigned attributes = 0;
        PropertyOffset lockedOffset = this->structure()->getConcurrently(uid, attributes);
        if (lockedOffset != offset || !isValidOffset(lockedOffset))
            return { };
        if (attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue))
            return { }; // Reified into a non-plain slot since the probe: re-probe rejects.
        if (isWrite && (attributes & PropertyAttribute::ReadOnly))
            return { }; // D7 re-validated inside the atomic body; the caller's re-probe throws.
        WriteBarrierBase<Unknown>* slot = nullptr;
        if (isInlineOffset(offset))
            slot = &inlineStorage()[offsetInInlineStorage(offset)];
        else {
            uint64_t word = taggedButterflyWord();
            if (arrayStorageShape)
                RELEASE_ASSERT(!isSegmentedButterfly(word)); // I31: AS never segments.
            if (isSegmentedButterfly(word)) {
                slot = segmentedOutOfLineSlotIfWithinBounds(butterflySpine(word), offset);
                if (!slot)
                    return { }; // Defensive: stale coverage => re-probe.
            } else {
                RELEASE_ASSERT(word & butterflyPointerMask); // Live storage never shrinks (I18).
                slot = &untaggedButterfly(word)->propertyStorage()[offsetInOutOfLineStorage(offset)];
            }
        }
        JSValue current = slot->get();
        if (!current)
            return { }; // Quarantined/cleared slot (D1/I30 family): re-probe.
        // §C.2 accessor-clobber fix (see atomicSlotLockFreeLoop): a dictionary
        // define-own stores the accessor cell under the cell lock but runs
        // attributeChangeTransition AFTER dropping it, so the locked
        // attribute re-resolve above can still read "plain data" while the
        // slot already holds the GetterSetter. Never write over an accessor
        // cell: restart, and the re-probe classifies once the attribute
        // publication lands.
        if (current.isCell()) [[likely]] {
            JSType currentType = current.asCell()->type();
            if (currentType == GetterSetterType || currentType == CustomGetterSetterType) [[unlikely]]
                return { }; // In-flight define (value published first): re-probe.
        }
        JSValue newValue;
        if (!atomicSlotEvaluate(globalObject, request, current, newValue, status))
            return current;
        // Lock-serialized store: every access to dictionary/AS slots holds
        // this lock (I19/L3, I31/L5); the pre-lock protocol above retired the
        // only unlocked writers (owner AS fast paths gate on SW=0).
        slot->set(vm, this, newValue);
        status = AtomicSlotStatus::Applied;
        return current;
    }

    // ---- Lock-free arms: inline, flat out-of-line, segmented fragment.
    //
    // U-T10 amend (U5): named lock-free loops bounce jsUndefined reads out as
    // LockedRevalidate - a named jsUndefined can be the D1 quarantine
    // sentinel of a delete whose structure publication has not landed yet
    // (storeUndefinedIntoDoomedSlotConcurrent stores the sentinel BEFORE the
    // header CAS / table edit, I30). Disambiguation runs under the cell
    // lock: both delete flavors hold it across their whole sentinel-store ->
    // publication window (§6 L4), so with the lock held and structureID()
    // still == expectedStructureID (non-dictionary deletes always publish a
    // NEW structureID; dictionary regimes never reach the lock-free arms,
    // and a dictionary CONVERSION also changes the ID), no delete has
    // published since the probe and none is mid-window - a re-read
    // jsUndefined is the property's GENUINE value. The lock excludes
    // deleters, NOT other lock-free CASers, so the write stays a seq_cst
    // slot CAS (failure => Restart; nothing applied, never-re-apply holds).
    // No allocation under the lock (I20): atomicSlotEvaluate is
    // allocation-free and the slot is re-resolved by walking only.
    auto lockedUndefinedArm = [&]() -> JSValue {
        status = AtomicSlotStatus::Restart;
        Locker locker { cellLock() };
        CellLockDepthScope cellLockDepthScope; // O3/I20
        if (structureID() != expectedStructureID)
            return { }; // A delete (or any transition) published: re-probe classifies (a vanished property throws).
        WriteBarrierBase<Unknown>* lockedSlot = nullptr;
        if (isInlineOffset(offset))
            lockedSlot = &inlineStorage()[offsetInInlineStorage(offset)];
        else {
            uint64_t lockedWord = taggedButterflyWord();
            if (isSegmentedButterfly(lockedWord)) {
                lockedSlot = segmentedOutOfLineSlotIfWithinBounds(butterflySpine(lockedWord), offset);
                if (!lockedSlot)
                    return { }; // Defensive: stale coverage => re-probe.
            } else {
                RELEASE_ASSERT(lockedWord & butterflyPointerMask); // I18
                lockedSlot = &untaggedButterfly(lockedWord)->propertyStorage()[offsetInOutOfLineStorage(offset)];
            }
        }
        Atomic<uint64_t>* atomicSlot = std::bit_cast<Atomic<uint64_t>*>(lockedSlot);
        uint64_t currentBits = atomicSlot->load(std::memory_order_seq_cst);
        JSValue current = JSValue::decode(currentBits);
        if (!current || !current.isUndefined())
            return { }; // The slot moved on while we acquired the lock: re-probe (nothing applied).
        JSValue newValue;
        if (!atomicSlotEvaluate(globalObject, request, current, newValue, status))
            return current; // Load / NotEqual / NotNumber against a GENUINE undefined.
        if (atomicSlot->compareExchangeStrong(currentBits, JSValue::encode(newValue), std::memory_order_seq_cst) == currentBits) {
            vm.writeBarrier(this, newValue); // After success, as §9.5 orders.
            status = AtomicSlotStatus::Applied;
            return current;
        }
        status = AtomicSlotStatus::Restart; // A racing lock-free writer won: re-probe.
        return { };
    };
    if (isInlineOffset(offset)) {
        // Inline slots: the cell never resizes; there is no butterfly word,
        // hence no SW bit and no SW protocol (§3 - matching
        // putDirectConcurrent's inline arm). Attributes are pinned by
        // expectedStructureID (non-dictionary structures are immutable).
        JSValue result = atomicSlotLockFreeLoop(globalObject, vm, this, &inlineStorage()[offsetInInlineStorage(offset)], expectedStructureID, true, request, status);
        if (status == AtomicSlotStatus::LockedRevalidate) [[unlikely]]
            return lockedUndefinedArm();
        return result;
    }
    WTF::loadLoadFence(); // M7(d): order the probe's structureID/offset provenance before the tagged-word load.
    uint64_t word = taggedButterflyWord();
    WriteBarrierBase<Unknown>* slot = nullptr;
    while (true) {
        if (isSegmentedButterfly(word)) [[unlikely]] {
            slot = segmentedOutOfLineSlotIfWithinBounds(butterflySpine(word), offset);
            if (!slot) {
                // I33: out of the loaded spine's bounds = stale spine =>
                // acquire-re-load the tagged word, re-dispatch.
                WTF::loadLoadFence();
                word = taggedButterflyWord();
                continue;
            }
            // Segmented fragment slot: NO cell lock (U5) and no SW protocol
            // (segmented words are shared by construction, I3/I4).
            // Out-of-line fragment identity is stable across T2 replacement
            // spines (out-of-line fragments are aliased verbatim) and the
            // mode-(b) rebuild runs inside a stop we cannot be racing (I34:
            // no poll between here and the CAS).
            break;
        }
        RELEASE_ASSERT(word & butterflyPointerMask); // A valid out-of-line offset implies storage (I18).
        if (isWrite && !butterflySharedWrite(word) && butterflyWriterIsForeign(word)) [[unlikely]] {
            // Flat-path SW discipline (ANNEX C1): FIRST the §2/§3 foreign
            // first-write SW-set DCAS, THEN re-validate and CAS. Re-dispatch:
            // the word may have gone segmented or been republished meanwhile.
            ensureSharedWriteBit(vm, static_cast<JSObjectWithButterfly*>(this));
            WTF::loadLoadFence();
            word = taggedButterflyWord();
            continue;
        }
        slot = &untaggedButterfly(word)->propertyStorage()[offsetInOutOfLineStorage(offset)];
        break;
    }
    // I34 re-validation, THEN the slot CAS. After this check the flat payload
    // is pinned for the loop's lifetime: flat GROW is butterfly-CAS + copy
    // (T1), owner-only against a (currentTID, 0) word - we are either that
    // owner (cannot race ourselves) or the word carries SW=1 (T1 impossible,
    // I27); a §4.2 conversion aliases the flat allocation's slices, so even a
    // racing conversion leaves this slot address live (I7).
    if (structureID() != expectedStructureID || (!isSegmentedButterfly(word) && taggedButterflyWord() != word)) {
        status = AtomicSlotStatus::Restart;
        return { };
    }
    JSValue result = atomicSlotLockFreeLoop(globalObject, vm, this, slot, expectedStructureID, true, request, status);
    if (status == AtomicSlotStatus::LockedRevalidate) [[unlikely]]
        return lockedUndefinedArm();
    return result;
}

JSValue JSObject::atomicSlotReadModifyWriteAtIndex(JSGlobalObject* globalObject, unsigned index, const AtomicSlotRequest& request, AtomicSlotStatus& status)
{
    ASSERT(Options::useJSThreads());
    VM& vm = globalObject->vm();
    status = AtomicSlotStatus::Restart;
    bool isWrite = atomicSlotOperationWrites(request.operation);

    while (true) {
        // ---- CoW: materialize per §4.8/I35 FIRST. The first atomic ACCESS
        // converts (reads included) - this is what §C.3(a)'s monotonicity
        // lemma relies on: a §9.5-touched slot never returns to a converting
        // arm.
        if (isCopyOnWrite(indexingMode())) [[unlikely]] {
            uint64_t cowWord = taggedButterflyWord();
            if (!butterflyWriterIsForeign(cowWord)) // incl. §9.6 forceButterflySWBit
                ensureWritable(vm); // Owner: routes through materializeCopyOnWriteButterflyConcurrent flag-on.
            else
                ensureSharedWriteBit(vm, static_cast<JSObjectWithButterfly*>(this)); // Foreign: §4.8 materialize-first carve-out.
            continue;
        }
        IndexingType type = indexingType();

        // ---- ArrayStorage / dictionary-indexed: third arm (cell-locked).
        // Existing-element stores are LikePutDirect (matching the GIL bodies'
        // putDirectIndex semantics), so SlowPut prototype intercepts do not
        // apply to an in-vector hit.
        if (hasAnyArrayStorage(type)) [[unlikely]] {
            uint64_t probeWord = taggedButterflyWord();
            if (isWrite && (probeWord & butterflyPointerMask) && !butterflySharedWrite(probeWord)
                && butterflyWriterIsForeign(probeWord)) { // AS PRE-LOCK SW protocol (r8 item 6), no lock held (GT11).
                ensureSharedWriteBit(vm, static_cast<JSObjectWithButterfly*>(this));
                return { }; // Restart the whole probe.
            }
            Locker locker { cellLock() }; // I31/L5
            CellLockDepthScope cellLockDepthScope; // O3/I20
            if (!hasAnyArrayStorage(indexingType()))
                return { }; // Shape moved before the lock landed: re-probe.
            uint64_t word = taggedButterflyWord();
            RELEASE_ASSERT(!isSegmentedButterfly(word)); // I31
            ArrayStorage* storage = untaggedButterfly(word)->arrayStorage();
            if (storage->m_sparseMap || index >= storage->vectorLength() || index >= storage->length())
                return { }; // Sparse/out-of-bounds: re-probe (Atomics never create elements).
            WriteBarrier<Unknown>& slot = storage->m_vector[index];
            JSValue current = slot.get();
            if (!current)
                return { }; // Hole: re-probe rejects.
            JSValue newValue;
            if (!atomicSlotEvaluate(globalObject, request, current, newValue, status))
                return current;
            slot.set(vm, this, newValue); // Lock-serialized (I31; pre-lock flip retired the unlocked owner fast paths).
            status = AtomicSlotStatus::Applied;
            return current;
        }

        // ---- Int32/Double: raw-word CAS REJECTED (8g) - the FIRST atomic
        // access converts to Contiguous (owner direct; foreign SW-set DCAS
        // first), so every later atomic access takes the flat arm verbatim.
        if (hasInt32(type) || hasDouble(type)) [[unlikely]] {
            uint64_t word = taggedButterflyWord();
            if ((word & butterflyPointerMask) && !butterflySharedWrite(word)
                && butterflyWriterIsForeign(word)) { // incl. §9.6 forceButterflySWBit
                ensureSharedWriteBit(vm, static_cast<JSObjectWithButterfly*>(this));
                continue; // Re-dispatch on the fresh tag, then convert.
            }
            if (hasInt32(indexingType()))
                convertInt32ToContiguous(vm); // Flag-on: per-event-stop relabel (§4.7/I28).
            else if (hasDouble(indexingType()))
                convertDoubleToContiguous(vm);
            continue;
        }
        if (!hasContiguous(type))
            return { }; // Undecided/None/blank territory: the caller's re-probe classifies (and throws).

        // ---- Contiguous: flat arm verbatim (+ the segmented fragment arm).
        WTF::loadLoadFence(); // M7(d)
        uint64_t word = taggedButterflyWord();
        if (!(word & butterflyPointerMask))
            return { }; // Racing N3 first install: re-probe.
        WriteBarrierBase<Unknown>* slot = nullptr;
        if (isSegmentedButterfly(word)) [[unlikely]] {
            slot = segmentedIndexedSlotIfReadable(butterflySpine(word), index);
            if (!slot) {
                // Stale spine (I33) or genuinely out of bounds: re-load once;
                // an unchanged word means real OOB (re-probe rejects).
                WTF::loadLoadFence();
                if (taggedButterflyWord() == word)
                    return { };
                continue;
            }
        } else {
            if (isWrite && !butterflySharedWrite(word) && butterflyWriterIsForeign(word)) [[unlikely]] {
                ensureSharedWriteBit(vm, static_cast<JSObjectWithButterfly*>(this)); // Flat-path SW discipline.
                continue;
            }
            Butterfly* butterfly = untaggedButterfly(word);
            if (index >= butterfly->vectorLength() || index >= butterfly->publicLength())
                return { }; // Out of bounds: re-probe rejects.
            slot = &butterfly->indexingPayload<WriteBarrier<Unknown>>()[index];
            // I34 re-validation: a racing owner T1 grow republishes a COPY -
            // a CAS into the old payload would be silently lost (ANNEX C1
            // "flat GROW = butterfly-CAS + copy, NO nuke"). After this check
            // the payload is pinned: T1 is owner-only on (currentTID, 0)
            // words - we are that owner, or SW=1 (I27).
            if (taggedButterflyWord() != word)
                continue;
        }
        // U-T10 amend: pin the structure observed AFTER the payload was
        // validated - the in-loop re-validation then restarts on any
        // published transition (e.g. Contiguous -> ArrayStorage migration)
        // instead of CASing through it. revalidateUndefined=false: indexed
        // deletes/holes are EMPTY JSValue()s, never the D1 jsUndefined
        // sentinel - the loop's !current check already restarts them.
        StructureID pinnedStructureID = structureID(); // RAW read (M5): a nuked ID never matches in-loop, but never decode one.
        if (pinnedStructureID.isNuked()) [[unlikely]]
            return { }; // A racing publication is mid-flight: re-probe.
        if (!hasContiguous(pinnedStructureID.decode()->indexingType())) [[unlikely]]
            return { }; // Shape moved between the word validation and the pin: re-probe.
        JSValue result = atomicSlotLockFreeLoop(globalObject, vm, this, slot, pinnedStructureID, false, request, status);
        ASSERT(status != AtomicSlotStatus::LockedRevalidate);
        return result;
    }
}

// U-T10 amend (§C.2 Missing arm conditional add): see the JSObject.h comment.
// PutModePut (NOT PutModeDefineOwnProperty): the GIL-off probe->put window
// means the key can materialize between the Missing probe and the put, and
// define-own semantics would replace a racing accessor / strip ReadOnly via
// an attribute-change transition to attributes 0 - a heap state no
// sequential Atomics.store interleaving can produce. PutModePut's flag-on
// body re-derives existence/extensibility INSIDE its §2 re-dispatch loop and
// publishes adds through the E4 structureID CAS (a racing transition -
// including preventExtensions, which always publishes a new structure -
// fails the CAS and re-runs the iteration's checks), so the returned error /
// success is linearizable against racing defines.
ASCIILiteral JSObject::putDirectForAtomicsMissingAdd(VM& vm, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    ASSERT(Options::useJSThreads());
    ASSERT(!parseIndex(propertyName));
    return putDirectInternal<PutModePut>(vm, propertyName, value, 0, slot);
}

#endif // USE(JSVALUE64)

// ===== Task 6b: §4.5 GC visit of a segmented butterfly =====
//
// Called from the owned JSObject.cpp visitButterflyImpl (both the
// mutatorIsStopped() branch and the concurrent double-collect branch) once its
// dispatch has loaded a Segmented tagged word - §4.5 step 2 (Dependency-ordered
// tagged-word load; non-segmented words take the flat path) lives at the call
// site. Steps here:
//
//   1. (review round 2) The {structureID, structure, maxOffset, indexingMode}
//      snapshot is supplied by the CALLER, bracketed around its spine load
//      (early read -> Dependency-ordered spine load -> late re-check). No
//      fresh structureID load decides the element shape here - see the
//      in-function comment for the relabel/mode-(b) pairing hazards a fresh
//      load would reintroduce.
//   3. markAuxiliary the spine and, if set, aliasedAllocationBase - after an
//      aliasing conversion the old flat allocation's ONLY references are the
//      spine's interior fragment pointers, invisible to markAuxiliary of the
//      fragments themselves (I7; cf. the flat path's base marking,
//      markAuxiliaryAndVisitOutOfLineProperties).
//   4. markAuxiliary ALL out-of-line fragments outside [aliasedBase,
//      aliasedBase + aliasedSize); value-visit ONLY j < (outOfLineSize+3)/4,
//      HIGH-end slots (out-of-line indices DESCEND within a fragment, §4.1/I8;
//      slots beyond outOfLineSize are uninitialized and never visited; the
//      bound never shrinks while quarantined - D1/I30, JSObject.cpp's
//      setStructureAndReallocateStorageIfNecessary family never shrinks live
//      storage either, I18).
//   5. indexed fragments (none when header-less, C2): mark as in step 4;
//      value-visit per C4/I33 - bound = min(publicLength, the SAME loaded
//      spine's vectorLength) - skipping fragment 0 slot 0 (the frozen flat
//      IndexingHeader, I9b) and leaving the C2 tail unvisited. Shape-keyed off
//      the step-1 structure (re-checked at step 6): only writable Contiguous
//      value-visits, exactly like the flat visitElements - Int32/empty slots
//      hold no cells, and Double fragments hold RAW doubles (GT#15/§4.7):
//      value-visiting them would type-confuse the marker. AS never appears
//      (I31); CoW never segments (I35).
//   6. re-load structureID, compare against the caller's EARLY id (+ re-check
//      maxOffset); mismatch => nullptr (didRace). Spine immutability (I6)
//      means there is no torn-spine case: a superseded spine's fragments are
//      still the live fragments for every slot it covers (replacement spines
//      alias them verbatim), so the only consequence of staleness is a
//      conservative revisit.
//
// Race-not-assert note on the step-4 bound (review round 2): with the
// caller-supplied bracket the structure can no longer be NEWER than the spine
// (the spine was loaded after the early structureID, and storage never
// shrinks), so the outOfLineSize-vs-capacity overrun should be unreachable;
// it is kept as a defensive didRace (NOT an assert) because a stale spine on
// a racing growth path costs only a conservative revisit (the publication's
// vm.writeBarrier(object) re-greys the object). The reverse skew (structure
// older than spine) is benign: storage requirements only grow, and the visit
// bound from the older structure is a subset of the spine's capacity.
//
// I25 barrier audit (Task 6b; every owned segmented store/publication site):
//   - fragment-slot stores on the mutator: putDirectConcurrent's segmented
//     branch uses WriteBarrierBase::set(vm, owner, value) (above);
//     structureOnlyTransition's already-published fallback uses
//     WriteBarrierBase::set on the inline slot.
//   - §4.2-4/§4.3-4 pre-publication value stores are raw release stores into a
//     PRIVATE spine/butterfly (unreachable by the collector except via
//     conservative scan, I7); the post-unlock vm.writeBarrier(object, value)
//     in convertToSegmentedButterfly / trySegmentedTransition covers them, and
//     vm.writeBarrier(object) re-greys the object so the new spine is scanned.
//   - spine/butterfly publications (the DCAS / PA fenced order, N3 installs,
//     and tryStructureOnlyTransition's header CAS) each emit
//     vm.writeBarrier(object) [+ (object, newStructure)] after unlock, exactly
//     like setButterfly's emitted barrier.
//   - spines and fragments are GC-auxiliary cells from vm.auxiliarySpace() -
//     the SAME CompleteSubspace Butterfly::createUninitialized uses
//     (ButterflyInlines.h), so markAuxiliary/markAuxiliaryAndVisitOutOfLine-
//     Properties treat flat butterflies, spines and fragments uniformly (I25).
//   No unbarriered segmented store site remains in this TU or in the owned
//   JSObject.cpp paths.

template<typename Visitor>
Structure* visitSegmentedButterfly(Visitor& visitor, JSObjectWithButterfly* object, ButterflySpine* spine, StructureID expectedStructureID, Structure* structure, PropertyOffset maxOffset, IndexingType indexingMode)
{
    ASSERT(Options::useJSThreads()); // Segmented words cannot exist flag-off (I22).
    ASSERT(spine);
    ASSERT(!expectedStructureID.isNuked());
    ASSERT(structure == expectedStructureID.decode());

    // ---- Step 1 (review round 2): the {structureID, structure, maxOffset,
    // indexingMode} snapshot comes from the CALLER, which bracketed it around
    // the spine load (visitButterflyImpl: ReadStructureEarly ->
    // Dependency-ordered ReadButterfly -> ReadStructureLate, all BEFORE this
    // call). We must NOT re-load the structureID here to decide the element
    // shape: a fresh load can return a structure NEWER than the caller's
    // spine - e.g. a §4.7 Double->Contiguous relabel stop (which rewrites
    // lanes in place, leaves the butterfly word untouched, and stops mutators
    // but NOT concurrent markers), or a structure published after a mode-(b)
    // T2 migration abandoned this spine's never-rewritten fragments. Pairing
    // that newer Contiguous structure with this older spine would value-visit
    // RAW double (or uninitialized Undecided) lanes as JSValues - a denormal
    // double decodes as a cell pointer under JSVALUE64 and the marker would
    // mark arbitrary memory. With the caller's bracket, any relabel /
    // migration / AS conversion completing after the caller's early read
    // changes the structureID and is caught by the step-6 re-check against
    // the EARLY id (didRace) - exactly the flat path's discipline.
    //
    // M7(d) belt-and-braces (review round 2, finding 6): the fragment-slot
    // loads below are address-dependent on the spine pointer, which the
    // caller Dependency-ordered behind the early structureID load - that
    // chain is the load-load ordering the flat path gets from
    // fencedIndexingMode/fencedButterfly. The explicit fence below documents
    // and enforces the ordering independently of the consume chain (no-op on
    // x86-64; one dmb ishld per segmented visit on arm64).
    WTF::loadLoadFence();

    // I31 (AS never segments) and I35 (no segmented word points at a
    // JSImmutableButterfly) hold for the PAIRED {structure, butterfly}
    // snapshot the caller supplied. A §4.6 transition INTO AS publishes
    // {flat AS word, AS structure} together; if the caller's early structure
    // is AS-shaped while its loaded word was segmented, the pair is torn
    // (stale-spine race): didRace. A GENUINE violation cannot hide behind
    // this - the mutator-stopped revisit re-dispatches on the settled pair,
    // and the stopped call site RELEASE_ASSERTs a non-null result.
    if (hasAnyArrayStorage(indexingMode) || isCopyOnWrite(indexingMode)) [[unlikely]]
        return nullptr;

    spine->tsanConsume(); // V7: published spine; import the publisher's clock (GC visit races the publish benignly — I6).
    spine->validateConsistency(); // Debug/verify-only inside.

    // I6/I7 witnesses (Task 10): published spines are immutable - snapshot the
    // header fields here and re-compare after the visit (any divergence means
    // someone mutated a PUBLISHED spine: growth must allocate a NEW spine);
    // and aliasedAllocationBase/Size are null/0 together (§4.1; a base without
    // a size - or vice versa - would let GC sweep the aliased flat allocation,
    // the exact UAF I7 exists to prevent).
    const uint32_t entryOutOfLineFragmentCount = spine->outOfLineFragmentCountConcurrent();
    const uint32_t entryIndexedFragmentCount = spine->indexedFragmentCountConcurrent();
    const uint32_t entryVectorLength = spine->vectorLengthConcurrent();
    const uint32_t entrySpineEpoch = butterflyConcurrentLoad(&spine->spineEpoch);
    const void* entryAliasedBase = butterflyConcurrentLoad(&spine->aliasedAllocationBase);
    const uint64_t entryAliasedSize = butterflyConcurrentLoad(&spine->aliasedAllocationSize);
    if (verifyConcurrentButterflyEnabled()) [[unlikely]]
        RELEASE_ASSERT(!!entryAliasedBase == !!entryAliasedSize); // §4.1/I7

    // ---- Step 3: mark the spine and the aliased flat allocation's base.
    ASSERT(visitor.heap() == Heap::heap(object));
    visitor.markAuxiliary(spine);
    char* aliasedBegin = static_cast<char*>(const_cast<void*>(entryAliasedBase));
    char* aliasedEnd = aliasedBegin + entryAliasedSize; // aliasedSize is 0 when base is null (§4.1).
    if (aliasedBegin)
        visitor.markAuxiliary(aliasedBegin);

    auto fragmentIsInAliasedAllocation = [&](ButterflyFragment* fragment) -> bool {
        char* raw = reinterpret_cast<char*>(fragment);
        return aliasedBegin && raw >= aliasedBegin && raw < aliasedEnd;
    };

    // ---- Step 4: out-of-line fragments. Mark ALL of them (quarantined slots
    // stay visited until released - D1/I18/I30 keep the bound from shrinking);
    // value-visit only up to outOfLineSize.
    uint32_t outOfLineFragmentCount = entryOutOfLineFragmentCount;
    for (uint32_t j = 0; j < outOfLineFragmentCount; ++j) {
        ButterflyFragment* fragment = spine->outOfLineFragment(j);
        if (!fragmentIsInAliasedAllocation(fragment))
            visitor.markAuxiliary(fragment);
    }

    size_t outOfLineSize = Structure::outOfLineSize(maxOffset);
    if (outOfLineSize > static_cast<size_t>(butterflyFragmentSlots) * outOfLineFragmentCount) {
        // Stale spine vs a newer structure (see the race-not-assert note
        // above): didRace; the publication barrier re-greys the object.
        return nullptr;
    }
    uint32_t liveOutOfLineFragments = static_cast<uint32_t>((outOfLineSize + butterflyFragmentSlots - 1) / butterflyFragmentSlots);
    for (uint32_t j = 0; j < liveOutOfLineFragments; ++j) {
        ButterflyFragment* fragment = spine->outOfLineFragment(j);
        // Out-of-line index k lives at slot 3-(k%4) (§4.1): the live slots of
        // fragment j are the HIGH end.
        size_t liveCount = std::min<size_t>(butterflyFragmentSlots, outOfLineSize - static_cast<size_t>(butterflyFragmentSlots) * j);
        visitor.appendValuesHidden(fragment->slots + (butterflyFragmentSlots - liveCount), liveCount);
    }

    // ---- Step 5: indexed fragments (indexedFragmentCount == 0 iff the flat
    // butterfly was header-less at conversion, C2).
    uint32_t indexedFragmentCount = entryIndexedFragmentCount;
    if (indexedFragmentCount) {
        for (uint32_t f = 0; f < indexedFragmentCount; ++f) {
            ButterflyFragment* fragment = spine->indexedFragment(f);
            if (!fragmentIsInAliasedAllocation(fragment))
                visitor.markAuxiliary(fragment);
        }

        bool valueVisitElements = false;
        switch (indexingMode) {
        case ALL_WRITABLE_CONTIGUOUS_INDEXING_TYPES:
            // The only segmented shape whose elements are barriered JSValue
            // cells - mirrors the flat visitElements exactly.
            valueVisitElements = true;
            break;
        default:
            // Int32/empty: no cells. Double: RAW doubles (GT#15/§4.7) - the
            // fragments are marked above but the slots are NEVER value-visited.
            break;
        }

        if (valueVisitElements) {
            // Review round 3 (I21/I25 - GC bound vs lock-free truncation): the
            // value-visit bound is the loaded spine's STORAGE bound
            // (vectorLength), NOT min(publicLength, vectorLength). A §3 dense
            // store may legally land in [length, vectorLength) while a
            // truncating plain setSegmentedPublicLength (pop's segmented leg /
            // shrinkButterflyForSetLengthConcurrent) is mid-flight; bounding
            // by publicLength would leave that store's cell unmarked forever
            // (the store's barrier re-greys the object, but the revisit would
            // reuse the same too-small bound) - a sweep of a reachable value.
            // Slots in [publicLength, vectorLength) are always initialized:
            // aliased fragments alias a hole-initialized flat allocation,
            // fresh fragments are cleared before publication (I6), so they
            // hold holes or stale-but-valid JSValues. publicLength may also
            // EXCEED this spine's vectorLength after a racing T2 grow (C4) -
            // [vectorLength, publicLength) belongs to a newer spine, which the
            // re-greyed revisit scans. The C2 tail past vectorLength is never
            // visited.
            uint32_t bound = entryVectorLength; // V7: the (annotated) entry snapshot — same immutable word.
            for (uint32_t f = 0; f < indexedFragmentCount; ++f) {
                // Element i lives at fragment (i+1)/4, slot (i+1)%4 (§4.1):
                // fragment 0 carries elements 0..2 in slots 1..3 (slot 0 is the
                // frozen flat IndexingHeader - skipped); fragment f >= 1
                // carries elements 4f-1..4f+2 in slots 0..3.
                uint32_t firstElement = f ? butterflyFragmentSlots * f - 1 : 0;
                if (firstElement >= bound)
                    break;
                unsigned firstSlot = f ? 0 : 1;
                size_t slotsAvailable = butterflyFragmentSlots - firstSlot;
                size_t liveCount = std::min<size_t>(slotsAvailable, bound - firstElement);
                visitor.appendValuesHidden(spine->indexedFragment(f)->slots + firstSlot, liveCount);
            }
        }
    }

    // ---- Step 6: re-load the structureID and compare against the CALLER'S
    // EARLY id (review round 2; the segmented analogue of the flat
    // double-collect's late reads, JSObject.cpp). Comparing against the early
    // id - not a fresh step-1 load - is what makes a relabel/migration that
    // completed mid-visit force didRace before the visited values can be
    // trusted. The fence orders the slot/fragment loads above before the late
    // re-loads (no-op on x86-64; cf. M7). Spine immutability (I6) leaves
    // staleness, not tearing, as the only failure mode - didRace covers it.
    WTF::loadLoadFence();
    if (verifyConcurrentButterflyEnabled()) [[unlikely]] {
        // I6 witness (Task 10): the spine header is bit-identical to the entry
        // snapshot - published spines are never mutated (growth = new spine;
        // the SOLE mutable spine-reachable word is fragment 0 slot 0, the
        // shared publicLength, which lives in a FRAGMENT, not the header).
        RELEASE_ASSERT(spine->outOfLineFragmentCountConcurrent() == entryOutOfLineFragmentCount);
        RELEASE_ASSERT(spine->indexedFragmentCountConcurrent() == entryIndexedFragmentCount);
        RELEASE_ASSERT(spine->vectorLengthConcurrent() == entryVectorLength);
        RELEASE_ASSERT(butterflyConcurrentLoad(&spine->spineEpoch) == entrySpineEpoch);
        RELEASE_ASSERT(butterflyConcurrentLoad(&spine->aliasedAllocationBase) == entryAliasedBase);
        RELEASE_ASSERT(butterflyConcurrentLoad(&spine->aliasedAllocationSize) == entryAliasedSize);
    }
    if (object->structureID() != expectedStructureID)
        return nullptr;
    if (structure->maxOffset() != maxOffset)
        return nullptr;
    return structure;
}

// The two instantiations visitButterflyImpl needs (DEFINE_VISIT_CHILDREN
// instantiates its visitor templates for exactly these).
template Structure* visitSegmentedButterfly(AbstractSlotVisitor&, JSObjectWithButterfly*, ButterflySpine*, StructureID, Structure*, PropertyOffset, IndexingType);
template Structure* visitSegmentedButterfly(SlotVisitor&, JSObjectWithButterfly*, ButterflySpine*, StructureID, Structure*, PropertyOffset, IndexingType);

#endif // USE(JSVALUE64)

// ===== Task 10: §9.6 stress modes + §8 invariant assertion ledger =====
//
// The four §9.6 options (OptionsList.h text = integration manifest entry 1,
// recorded in docs/threads/INTEGRATE-objectmodel.md; probed via the SFINAE
// accessors in ConcurrentButterfly.h so this TU compiles before the entry
// lands):
//
//   - useJSThreads: master switch. Every protocol entry point here
//     RELEASE_ASSERTs it (the I22 witness that nothing reaches these paths
//     flag-off).
//   - forceButterflySWBit: folded into butterflyWriterIsForeign()
//     (ConcurrentButterfly.h) - every write classifies as foreign, driving
//     F1/SW-DCAS (ensureSharedWriteBit), the §4.6 AS per-event stop, the §4.8
//     CoW materialization and the FlatShared rows on single-threaded
//     workloads. Consumers: putDirectConcurrent, putIndexConcurrent,
//     ensureLengthSlowConcurrent (CoW leg), shrinkButterflyForSetLength-
//     Concurrent, ensureSharedWriteBit's owner check.
//   - forceSegmentedButterflies: every butterfly allocation/transition ends
//     segmented. Consumers: trySegmentedTransition (§3 dispatch routes owner
//     stay-flat to §4.2 with the trigger; StayFlat flavor suppressed; post-
//     publication helper call covers N3 first installs),
//     ensureLengthSlowConcurrent (owner T1 -> conversion + T2),
//     shrinkButterflyForSetLengthConcurrent (owner copy-shrink -> conversion),
//     and applyForceSegmentedButterfliesStressIfNeeded below (also exported
//     for the owned JSObject.cpp/JSObjectInlines.h flat-install sites).
//     Exemptions: AS shapes (I31: AS never segments), CoW words (I35: they
//     materialize flat first; the materialized word re-enters), butterfly-less
//     objects (nothing allocated). PA cells are NOT exempt (§4.2 has the
//     I36/M8 fenced publication branch).
//   - verifyConcurrentButterfly: validateTaggedButterflyWord on every
//     encode/decode (I2/I3), the butterfly() flatness contract (JSObject.h),
//     the self-tests (concurrentButterflySelfTest +
//     concurrentButterflyStressSelfTest, run from VM startup via manifest
//     entry 4a and from JSTests i03-selftest.js), promotion of the I9/I10b/
//     I11/I12 publication witnesses to RELEASE_ASSERT, the I6/I7 spine
//     snapshot in visitSegmentedButterfly, and the casButterfly I27 shape
//     checks.
//
// ---- §8 invariant -> targeted assertion ledger (spec Task 10: ">= 1 targeted
// assertion in owned files" per invariant; CB = this TU, CB.h = the header):
//
//   I1   dcasHeaderAndButterfly RELEASE_ASSERT(!(cell & 15)) (CB.h).
//   I2   validateTaggedButterflyWord: payload-0 => all-zero word (CB.h; every
//        decode under verify; liveness leg = I7's conservative-scan design).
//   I3   validateTaggedButterflyWord notTTLTID => SW; butterflyRegimeForWord /
//        encodeSegmentedButterfly ASSERTs (CB.h).
//   I4   assertCasButterflyShape SW-monotone check (CB); taxonomy-(b)
//        RELEASE_ASSERT(fresh SW && !expected SW) in trySegmentedTransition
//        (DCAS + PA loops, CB); stress self-test flip leg (CB).
//   I5   every locked CAS loop RELEASE_ASSERTs
//        headerDiffersOnlyInVolatileBits on divergence (= volatile bytes
//        preserved; convert / trySegmentedTransition / tryStructureOnly-
//        Transition / CoW materialize, CB); the one-DCAS publication is the
//        protocol structure itself.
//   I6   visitSegmentedButterfly entry/exit spine-header snapshot (verify, CB).
//   I7   aliasedAllocationBase/Size copied VERBATIM (assignment sites, CB) +
//        visit's base<=>size coherence RELEASE_ASSERT + markAuxiliary of the
//        base every visit (CB).
//   I8   butterflyAliasEquationsHold static_assert (Butterfly.h);
//        validateSpineAliasesFlatButterfly (ButterflyInlines.h);
//        validatePartiallyAliasedSpine (CB).
//   I9   step-4 read-back witnesses after the release-stores (verify; §4.2
//        step 4 and §4.3 segmented case, CB); tryStructureOnlyTransition
//        stores the inline value before its header CAS (M2 site, CB).
//   I9b  validatePartiallyAliasedSpine RELEASE_ASSERT(frozenFlatVectorLength
//        == flat->vectorLength()) (CB). C4 at the same site is an
//        address-aliasing witness (slot-0 address == B - 8 + the Butterfly.h
//        half-layout static_asserts), NOT a value compare: the live
//        publicLength may be bumped lock-free between two reads
//        (Butterfly::bumpPublicLengthToAtLeast), so equality of double reads
//        is not an invariant.
//   I10  §4.2/§4.3 step-0 firing precedes every segmented publication; the
//        step-5 verify RELEASE_ASSERTs (no segmented publish under a valid
//        set) are its witness (CB).
//   I10b step-0 fire -> RESTART -> step-3 re-check under the lock; same
//        step-5 witnesses (CB).
//   I11  step-5 RELEASE_ASSERT(!transitionThreadLocalIsStillValid()) at both
//        segmented publication sites (verify-promoted, CB).
//   I12  ensureSharedWriteBit post-flip RELEASE_ASSERT(!writeThreadLocal-
//        IsStillValid()) (DCAS + PA legs, verify, CB); the b-taxonomy I12
//        guard in trySegmentedTransition (StayFlat + still-valid target =>
//        RESTART, CB).
//   I13  fireTransitionThreadLocal/fireWriteThreadLocal RELEASE_ASSERT(
//        butterflyWorldIsStopped(vm)) (Structure.cpp, Task 3).
//   I14  E1-E4 predicates test isStillValid AND isWatched (Structure.h/
//        StructureInlines.h, Task 3).
//   I15  E4 predicate asserts both source sets + key-owner identity +
//        !isPreciseAllocation (StructureInlines.h, Task 3).
//   I16  casButterfly is one 64-bit CAS on the word - it cannot name the
//        header; stress self-test header-lane leg proves the lane is
//        untouched (CB).
//   I17  assertCasButterflyShape on every casButterfly;
//        publishArrayStorageButterflyLocked RELEASE_ASSERTs lock + AS shape +
//        CAS success; PA publications CAS the word even under the lock (CB).
//   I18  takeDeletedOffset draws only from Reusable post-promotion
//        (PropertyTable.h, Task 9); releaseQuarantinedSlots epoch compare
//        (PropertyTable.cpp); getDirectConcurrent RELEASE_ASSERT(payload)
//        ("live storage never shrinks", CB).
//   I19  dictionary-mode cell-lock + m_lock asserts at the owned JSObject.cpp
//        L3/L4 sites (Task 9).
//   I20  lock order: lockCellChecked / veneer-entry RELEASE_ASSERT (no cell
//        lock across a stop) (CB); SAL emission sites assert rank (Task 3b);
//        O1 = AssertNoGC / pre-lock GCDeferralContext at the locked allocs.
//   I21  taxonomy (b2) => RESTART (never merge a copied flat payload, CB);
//        segmentedTransition driver RELEASE_ASSERT(settled == source) (no
//        silent lost-racer overwrite, CB); T1 CAS-failure => re-dispatch.
//   I22  RELEASE_ASSERT(Options::useJSThreads()) at every protocol entry
//        (CB); option probes constant-false when the entry is absent (CB.h).
//   I23  spine chain reads are plain loads + Dependency (owned JSObject.cpp
//        dispatch); no fence emitted in segmented*Slot accessors (CB).
//   I24  M7(d) loadLoadFence before every tagged-word load in the
//        *Concurrent accessors + I33 re-load loops (CB).
//   I25  visitSegmentedButterfly marks spine + aliased base + every
//        non-aliased fragment, value-visits every live slot (CB);
//        validateConsistency RELEASE_ASSERTs non-null fragments
//        (ButterflyInlines.h).
//   I26  headerDiffersOnlyInVolatileBits/mergeVolatileHeaderBits + both
//        self-tests' merge legs (CB.h/CB).
//   I27  assertCasButterflyShape exhaustive form check (T1/T2/N3/AS-COPY, CB).
//   I27b taxonomy (c) re-enters step 3 with a FRESH re-read - the stale
//        expected/desired pair is discarded, never re-CASed (CB).
//   I28  trySegmentedTransition RELEASE_ASSERT(hasDouble(source) ==
//        hasDouble(target)) for segmented words; tryGrowSegmentedVectorLength
//        re-checks the Double fill before publication (CB).
//   I29  E4 re-validation helper (StructureInlines.h, Task 3).
//   I30  D1 jsUndefined release-store before the table edit (owned
//        JSObject.cpp delete sites, Task 9); PropertyTable.h quarantine
//        comments + visited-until-released bound (Task 9).
//   I31  RELEASE_ASSERT(!hasAnyArrayStorage(...)) at the §4.2/§4.3 entries;
//        RELEASE_ASSERT(!isSegmentedButterfly(word)) inside every cell-locked
//        AS access (get/put direct/index, publishArrayStorageButterflyLocked,
//        ensureSharedWriteBit's stop closure) (CB).
//   I32  concurrentButterflyAtomicsAreLockFree + self-test RELEASE_ASSERT
//        (CB.h); manifest 4a wires the startup assert.
//   I33  segmentedOutOfLineSlotIfWithinBounds / segmentedIndexedSlotIf* bound
//        checks (CB); step-4 RELEASE_ASSERT(index < 4 * fragmentCount) at
//        both transition store sites (CB).
//   I34  AssertNoGC across the T1 copy->CAS window (CB); the *Concurrent
//        accessors are poll-free between slot resolution and access (CB);
//        unowned windows = manifest 7b audit.
//   I35  tryMaterializeCopyOnWriteButterflyForSharedWrite RELEASE_ASSERTs
//        (never SW=1/segmented CoW word; word stable under the lock) +
//        ensureSharedWriteBit's CoW RELEASE_ASSERT (CB).
//   I36  dcasHeaderAndButterfly alignment RELEASE_ASSERT (CB.h); every
//        protocol branches on isPreciseAllocation() to the M8 fenced order;
//        PA SW flip is cell-locked (CB).
//   I37  L6 Concurrently lookups + m_lock-held steals/walks/mutations
//        (StructureTransitionTable.h / Structure.cpp / StructureInlines.h,
//        Task 3c).
//   O2/O3 lockCellChecked depth witness + veneer-entry assert (CB, above).

void applyForceSegmentedButterfliesStressIfNeeded(VM& vm, JSObjectWithButterfly* object)
{
    if (!forceSegmentedButterfliesEnabled()) [[likely]]
        return;
    if (!Options::useJSThreads())
        return; // The stress option is meaningless flag-off (I22): no conversion machinery may run.
    ASSERT(vm.currentThreadIsHoldingAPILock());
    RELEASE_ASSERT(!t_cellLocksHeldByConcurrentButterfly); // GT11/O2: the conversion below stops and locks.

    while (true) {
        uint64_t word = object->taggedButterflyWord();
        ButterflyRegime regime = butterflyRegimeForWord(word);
        if (regime != ButterflyRegime::Flat && regime != ButterflyRegime::FlatShared)
            return; // None: nothing allocated. Segmented: stress goal met (I3).

        StructureID id = object->structureID(); // RAW bits (M5).
        if (id.isNuked())
            continue; // Mid-publication: spin (bounded, O2), then re-classify.
        Structure* structure = id.decode();

        // §9.6 exemptions (also RELEASE_ASSERTed at the §4.2 boundary): AS
        // never segments (I31); CoW never reaches §4.2 (I35) - its
        // materialized flat replacement re-enters this loop via the caller's
        // next stress hook.
        if (hasAnyArrayStorage(structure->indexingType()) || isCopyOnWrite(structure->indexingMode())) [[unlikely]]
            return;

        if (convertToSegmentedButterfly(vm, object, nullptr, nullptr, invalidOffset, JSValue()))
            return;
        // nullptr = RESTART: step 0 fired the TTL sets (the next pass
        // converts), or a racing publication moved the word (the next pass
        // re-classifies). Progress is monotone either way.
    }
}

// Pure-memory stress/self-test legs (Task 10; see the declaration comment in
// ConcurrentButterfly.h). No VM, no heap: the "cell" is a 16B-aligned stack
// buffer standing in for a MarkedBlock cell's first 16 bytes.
void concurrentButterflyStressSelfTest()
{
    alignas(16) uint64_t fakeCell[2] = { 0x0123456789abcdefULL, 0 };
    Butterfly* payload = reinterpret_cast<Butterfly*>(&fakeCell[0]);
    auto* wordAtomic = reinterpret_cast<Atomic<uint64_t>*>(&fakeCell[1]);

    // --- I16 witness: a casButterfly-shaped 64-bit CAS on the butterfly lane
    // never touches the header lane.
    const uint64_t headerBefore = fakeCell[0];
    uint64_t installed = encodeButterfly(payload, 7, false); // N3-shaped install (t, 0).
    uint64_t expected = 0;
    RELEASE_ASSERT(wordAtomic->compareExchangeStrong(expected, installed, std::memory_order_seq_cst) == expected);
    RELEASE_ASSERT(fakeCell[0] == headerBefore); // I16: header lane bit-identical.
    RELEASE_ASSERT(fakeCell[1] == installed);

    // --- I4 witness: the SW flip is (t, 0) -> (t, 1), payload preserved,
    // monotone; a stale-expected re-flip fails.
    uint64_t flipped = installed | butterflySWBit;
    RELEASE_ASSERT(wordAtomic->compareExchangeStrong(installed, flipped, std::memory_order_seq_cst) == installed);
    RELEASE_ASSERT(butterflySharedWrite(fakeCell[1]) && untaggedButterfly(fakeCell[1]) == payload && butterflyTID(fakeCell[1]) == 7);
    RELEASE_ASSERT(wordAtomic->compareExchangeStrong(installed, flipped, std::memory_order_seq_cst) != installed); // Re-dispatch shape: stale expected loses.
    RELEASE_ASSERT(fakeCell[0] == headerBefore); // I16 again across both CASes.

#if JSC_CONCURRENT_BUTTERFLY_HAS_HARDWARE_DCAS
    // --- I26/§3.0 witness: a simulated racing volatile-byte flip (GC
    // cellState CAS / parked bit) fails the DCAS once; ONE merge pass
    // converges, and the published header carries the racer's volatile bytes
    // with our semantic bytes - exactly the runtime loops' discipline.
    JSCell* cellPointer = reinterpret_cast<JSCell*>(&fakeCell[0]);
    constexpr uint64_t cellStateLane = 0xffULL << (8 * JSCell::cellStateOffset());
    uint64_t semanticHeader = headerBefore & ~cellHeaderVolatileMask;
    fakeCell[0] = semanticHeader;
    uint64_t currentWord = fakeCell[1];

    CellHeaderAndButterfly raced { semanticHeader, currentWord };
    CellHeaderAndButterfly desired { (~semanticHeader) & ~cellHeaderVolatileMask, encodeSegmentedButterfly(reinterpret_cast<ButterflySpine*>(payload)) };
    fakeCell[0] = semanticHeader ^ cellStateLane; // The "racer's" volatile flip lands first.
    RELEASE_ASSERT(!dcasHeaderAndButterfly(cellPointer, raced, desired)); // Stale volatile bytes: fails.
    uint64_t freshHeader = fakeCell[0];
    RELEASE_ASSERT(headerDiffersOnlyInVolatileBits(raced.header, freshHeader)); // Taxonomy (a), not (d).
    raced.header = mergeVolatileHeaderBits(raced.header, freshHeader);
    desired.header = mergeVolatileHeaderBits(desired.header, freshHeader);
    RELEASE_ASSERT(dcasHeaderAndButterfly(cellPointer, raced, desired)); // One merge pass converges.
    RELEASE_ASSERT((fakeCell[0] & cellHeaderVolatileMask) == (freshHeader & cellHeaderVolatileMask)); // I26: racer's volatile bytes survive.
    RELEASE_ASSERT((fakeCell[0] & ~cellHeaderVolatileMask) == (desired.header & ~cellHeaderVolatileMask)); // Our semantic bytes land.
    RELEASE_ASSERT(isSegmentedButterfly(fakeCell[1]) && butterflyRegimeForWord(fakeCell[1]) == ButterflyRegime::Segmented); // I3.
#endif // JSC_CONCURRENT_BUTTERFLY_HAS_HARDWARE_DCAS

    // --- §4.1 companion: outOfLineButterflyIndex is the 0-based spine index,
    // NOT offsetInOutOfLineStorage's negative PropertyStorage index.
    RELEASE_ASSERT(!outOfLineButterflyIndex(firstOutOfLineOffset));
    RELEASE_ASSERT(outOfLineButterflyIndex(firstOutOfLineOffset + 5) == 5);

    // --- §9.6 predicate coherence: with forceButterflySWBit on, even the
    // calling thread's own word classifies write-foreign; a genuinely foreign
    // TID always does.
    uint64_t ownerWord = encodeButterfly(payload, currentButterflyTID(), false);
    RELEASE_ASSERT(butterflyWriterIsForeign(ownerWord) == forceButterflySWBitEnabled());
    ButterflyTID foreignTID = currentButterflyTID() == 5 ? 6 : 5;
    RELEASE_ASSERT(butterflyWriterIsForeign(encodeButterfly(payload, foreignTID, false)));

    // --- Task 12 PA-lane witness (I32/I36). A PreciseAllocation cell base
    // sits at an 8-mod-16 address (PreciseAllocation.h halfAlignment), so the
    // dcasHeaderAndButterfly alignment gate (RELEASE_ASSERT(!(cell & 15)),
    // I1/I36) REJECTS it - checked here against the gate's own predicate, not
    // by calling the (crashing) gate. The 64-bit butterfly-word CAS that I36
    // keeps legal on PA cells is exercised at exactly such an address:
    // &fakeCell[1] is 8-mod-16 by construction (fakeCell is alignas(16)).
    uint64_t* paShapedWord = &fakeCell[1];
    RELEASE_ASSERT((reinterpret_cast<uintptr_t>(paShapedWord) & 15) == 8); // PA-shaped: 8-mod-16
    RELEASE_ASSERT(reinterpret_cast<uintptr_t>(paShapedWord) & 15); // the DCAS gate predicate fires for it (I36)
    auto* paAtomic = reinterpret_cast<Atomic<uint64_t>*>(paShapedWord);
    uint64_t paExpected = paAtomic->load(std::memory_order_relaxed);
    uint64_t paDesired = encodeButterfly(payload, currentButterflyTID(), true); // the I36 cell-locked SW-flip shape: 64-bit CAS, header untouched
    const uint64_t paHeaderBefore = fakeCell[0];
    RELEASE_ASSERT(paAtomic->compareExchangeStrong(paExpected, paDesired, std::memory_order_seq_cst) == paExpected);
    RELEASE_ASSERT(fakeCell[1] == paDesired);
    RELEASE_ASSERT(fakeCell[0] == paHeaderBefore); // I16/I36: the legal PA flip never touches the header lane
    // A stale-expected PA flip fails and leaves memory untouched - the
    // re-dispatch shape the cell-locked PA paths rely on.
    RELEASE_ASSERT(paAtomic->compareExchangeStrong(paExpected, paDesired | butterflyTIDMask, std::memory_order_seq_cst) != paExpected);
    RELEASE_ASSERT(fakeCell[1] == paDesired);
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
