/*
 * Copyright (C) 2005-2026 Apple Inc. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the NU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA
 *
 */

#include "config.h"
#include "JSLock.h"

#include "HeapInlines.h"
#include "JSGlobalObject.h"
#include "MachineStackMarker.h"
#include "Options.h"
#include "RaceAmplifier.h" // UNGIL EXIT1.8 (U-T6): carrier-TLS-death stall points.
#include "SamplingProfiler.h"
#include "ThreadManager.h" // UNGIL §F.1 (U-T8): isJSThreadCurrent keys the spawned token arm; (U-T6) carrier TIDs + the EXIT1.9 condition/wrapper.
#include "TypedArrayController.h" // UNGIL §G (U-T11): the embedder-policy half of mayBlockSynchronously.
#include "VMLite.h"
#include "VMLiteInlines.h" // UNGIL §F.1 (U-T8): per-lite drain-on-release (I11).
#include "VMLiteShared.h"
#include "VMTrapsInlines.h"
#include "Watchdog.h" // UNGIL annex W W1 (U-T11): the parked-carrier service episode.
#include <wtf/Atomics.h>
#include <wtf/Vector.h>
#include <wtf/HashMap.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/StackPointer.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/ThreadSpecific.h>
#include <wtf/Threading.h>
#include <wtf/threads/Signals.h>

#if USE(WEB_THREAD)
#include <wtf/ios/WebCoreThread.h>
#endif

#if PLATFORM(COCOA)
#include <wtf/cocoa/RuntimeApplicationChecksCocoa.h>
#endif

namespace JSC {

// ============================================================================
// UNGIL §A.3.6 — lazy main/embedder carriers (ANNEXES A36 + A36C, BINDING;
// U-T1, DARK: reachable only when the entered VM has m_gilOff == 1, which no
// shipping configuration produces until the activation tasks land).
//
// GIL-off EVERY thread uses a real carrier lite with a TM-allocated unique
// nonzero TID, lazily installed at first entry; m_mainVMLite (tid 0) is
// GIL-on-only. Carriers are per-(thread, VM) in a TLS VM->carrier map with
// TWO slots, chosen ONCE at first-entry registration (A36 r32):
//   - every NON-MAIN thread's carriers live in the destructor-BEARING
//     WTF::ThreadSpecific map, whose TLS destructor IS the carrier-TLS-death
//     path;
//   - the process MAIN thread's carriers live in a destructor-FREE plain
//     thread_local map (pthread TLS destructors never run for a thread
//     exiting via exit()/return-from-main, and a late Windows FLS callback
//     over the same storage would re-read a walk-freed lite), so NO cleanup
//     is ever installed over the main-thread slot on ANY platform — entries
//     leak at process exit unless a ~VM walk frees them (accepted).
// The choice is recorded per-lite as ownerHasNoTlsDtor, fixed at
// registration time under the registry lock.
//
// §10A.1 STAMPING AUTHORITY (U-T8 row; ANNEX A36C): GIL-off, the JSLock
// install/restore tuple swap in THIS file is the stamping authority for the
// heap §10A.1 currentThreadClient TLS slot on every entered thread —
// consumers of GCClient::Heap::currentThreadClient() (TLC allocation fast
// paths, DeferGC dispatch, validateIsNotSweeping's per-client mutator state)
// must treat the A36C swap here (plus GCClient::Heap::attachCurrentThread for
// spawned threads, which the swap leaves untouched because a spawned lite is
// never installed by JSLock) as the slot's GIL-off source of truth; nothing
// else may write the slot while a token is live on the thread (the U1 client
// clause RELEASE_ASSERTs the agreement at every GIL-off acquisition, §J.7).
//
// The map stores {VM*, epoch, carrier}; lock() compares epochs BEFORE
// trusting a cached carrier (A36 staleness rule — a dead VM's address can
// be reused).
//
// §10A.1 {client, epoch} CLAUSE — RECORDED REFINEMENT (ANNEX A36 BINDING
// clause "heap §10A.1's TLS slot becomes {client, epoch}; stale epoch =>
// null"; INTEGRATE-ungil ledger row 8 assigns the upgrade to U-T6). The slot
// (GCThreadLocalCache.cpp, OUTSIDE U-T6's owned-file set) deliberately stays
// a raw client pointer: the epoch was specified to exclude a reader
// dereferencing a stale client after its VM died, and the restore discipline
// implemented here makes that window unreachable —
//   (a) the A36C install/restore swap is strictly LIFO and re-stamps the
//       SAVED slot value at every depth-0 unlock / ~VM uninstall, so a
//       thread that has left a VM holds no slot reference into it;
//   (b) every live teardown path that deletes a client (carrier TLS-death
//       LIVE arm above; spawned T5, ThreadManager.cpp) runs
//       detachCurrentThread — which CLEARS the slot — strictly before the
//       delete;
//   (c) the ~VM A36 walk deletes only COLLECTED clients of NON-entered
//       threads (an entered-at-~VM carrier fail-stops on the token-free
//       RELEASE_ASSERT), and a non-entered owner's slot was already
//       restored per (a) at its depth-0 unlock — the walk never deletes a
//       client any thread's slot still names.
// So the slot can never hold a stale client pointer and an epoch field
// would be checked by no reachable reader. CONSTRAINT ON FUTURE WRITERS:
// any new writer of the §10A.1 slot must preserve (a)-(c) or implement the
// {client, epoch} pair as specified. The matching banner at the slot
// definition + the ledger-row annotation are an orchestrator follow-up
// (GCThreadLocalCache.cpp and INTEGRATE-ungil.md are outside this task's
// owned-file set).
//
// U-T1 landed the install/restore tuple swap ({lite, TID-tag, §10A.1 client
// slot} — A36C). U-T6 lands the full r31/r32 teardown protocol here:
// per-thread clients (perThreadClientForCarrierEntry), the state-keyed
// carrier-TLS-death path (tearDownCarriersAtThreadDeath: LIVE => live
// EXIT1.3 path, COLLECTED => vmTeardownCondition wait, DETACHED =>
// degenerate free), the post-walk stale-epoch eviction (evictStaleCarrier),
// and the §F.5 nested-entry release/restore hooks in
// didAcquireLock/willReleaseLock. Every physical removal goes through the
// notifying unregisterVMLiteAndNotifyTeardown wrapper (U20 r31) and every
// free is state-keyed; a map entry NEVER bare-deletes its lite. The ~VM-side
// A36 collection walk + EXIT1.9 fence live in VM.cpp.
// ============================================================================

// §J.7/U1 TLS-tag clause (U-T8; INTEGRATE-ungil ledger row 3): the I19
// coherence check — RELEASE_ASSERTs that g_jscButterflyTIDTag (and the
// JIT-visible copy, where one exists) equals currentButterflyTID() << 48,
// i.e. the CURRENT lite's tid. Defined unconditionally in
// jit/ConcurrentButterflyOperations.cpp; redeclared here (namespace-scope,
// same library — the recorded U-T8 seam pattern) because jit/ headers are
// outside this task's include discipline. Reading the REAL TLS word(s) is
// the point: a null/unregistered P5 tag hook or a desynchronized tag is
// exactly the failure mode the §J.7 backstop must fail-stop — comparing the
// lite-derived currentButterflyTID() against lite->tid would be a tautology.
JS_EXPORT_PRIVATE void assertButterflyTIDTagCoherent();

struct CarrierMapEntry {
    uint64_t vmEpoch { 0 };
    std::unique_ptr<VMLite> carrier;
};

using CarrierMap = UncheckedKeyHashMap<VM*, CarrierMapEntry>;

// EXIT1.3 carrier-TLS-death LIVE path + the r31/r32 carrier-state handshake
// (U-T6, replacing the U-T1 skeleton). Runs from the destructor-BEARING
// ThreadSpecific map only — NON-MAIN threads by construction (the main
// thread's map is destructor-free, A36 r32), so every lite here is bit-CLEAR
// (ownerHasNoTlsDtor == false) and its client is OWNED (see
// perThreadClientForCarrierEntry).
//
// The TLS destructor takes the registry lock FIRST and keys ONLY on the
// lock-published lite state — NEVER on "is my lite registered" (EXIT1.9
// carrier-TLS-death disposition):
//   LIVE      => mark TEARDOWN in the same hold, then the live EXIT1.3 path
//                (DCT -> destroy client -> unregisterLite LAST -> free lite);
//   COLLECTED => the ~VM A36 walk is mid-detach on another thread: park on
//                vmLiteTeardownCondition until the walk's DETACHED flip
//                (predicate loop; unregisterLite notifies are tolerated),
//                then the degenerate path;
//   DETACHED  => the degenerate path immediately: every m_server touch was
//                done by the walk — destroy ONLY client-local memory (the
//                lite and, inside ~VMLite, its default MicrotaskQueue, whose
//                M12 removal is a no-op after the M11 force-removal). The
//                walk already deleted the owned GCClient::Heap (recorded
//                refinement, see the VM.cpp walk banner): the deferred dtor
//                here never dereferences lite->clientHeap.
static void tearDownCarriersAtThreadDeath(CarrierMap& map) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    for (auto& mapEntry : map) {
        VMLite* lite = mapEntry.value.carrier.get();
        if (!lite)
            continue;
        ASSERT(!lite->ownerHasNoTlsDtor); // destructor-bearing map => bit-CLEAR (A36 r32)
        if (VMLite::currentIfExists() == lite)
            VMLite::setCurrent(nullptr); // I20: TLS never dangles past destruction (also clears the TID tag via the hook).
        uint16_t tid = lite->tid; // read before any free
        auto& registry = VMLiteRegistry::singleton();
        VMLite::State state;
        {
            Locker locker { registry.lock };
            state = lite->state;
            RELEASE_ASSERT(state != VMLite::State::Teardown); // only THIS thread marks its lites TEARDOWN
            if (state == VMLite::State::Live) {
                // The dtor-wins-LIVE arm (EXIT1.8 reverse variant): mark in
                // the SAME hold so a racing ~VM walk skips this lite and the
                // EXIT1.9 step-(3) wait absorbs it.
                lite->state = VMLite::State::Teardown;
            } else if (state == VMLite::State::Collected) {
                // CARRIER-TLS-DEATH-DURING-DETACH (r31): the walk owns the
                // client; wait for its DETACHED flip — Condition::wait drops
                // the registry lock into the parking lot, so the walk's
                // short flip hold always makes progress (EXIT1.6).
                while (lite->state != VMLite::State::Detached)
                    vmLiteTeardownCondition().wait(registry.lock);
                state = VMLite::State::Detached;
            }
        }
        RaceAmplifier::perturb(); // EXIT1.8 carrier-variant stall point: post-mark/post-wait.
        if (state == VMLite::State::Live) {
            // Live EXIT1.3 path. The thread is dying outside any VM entry:
            // its token was retired and access released at the final depth-0
            // unlock (an embedder thread exiting while ENTERED is a §F.6
            // contract violation — fail-stop).
            GCClient::Heap* client = lite->clientHeap;
            if (client) {
                RELEASE_ASSERT(!client->hasHeapAccess());
                if (GCClient::Heap::currentThreadClient() == client)
                    client->detachCurrentThread(); // DCT: parks the epoch at MAX + clears the §10A.1 slot (must not dangle past the delete)
                RaceAmplifier::perturb(); // EXIT1.8 stall point: pre-destroy.
                // Destroy the OWNED client (the live dtor: access bracket +
                // lastChanceToFinalize under MSPL + clientSet().remove
                // against the live server — the lite is still registered, so
                // the EXIT1.9 fence keeps ~VM blocked through this).
                delete client;
            }
            RaceAmplifier::perturb(); // EXIT1.8 stall point: post-destroy, pre-unregister.
            unregisterVMLiteAndNotifyTeardown(*lite); // PHYSICAL removal LAST, notifying (U20 r31)
        }
        // Degenerate or live tail: free the lite (the unique_ptr in the map,
        // destroyed by our caller) + retire the TID. For DETACHED lites this
        // touches NO VM/server memory: ~VMLite frees lite-local buffers and
        // its queue's M12 removal is isOnList()-guarded under the
        // process-lifetime registry lock (EXIT1.9 residual-tail rule).
        releaseCarrierTIDIfHooked(tid);
    }
    // The unique_ptrs free the lites when the map is destroyed by the caller.
}

class ThreadCarrierMaps {
    WTF_MAKE_TZONE_ALLOCATED(ThreadCarrierMaps);
public:
    CarrierMap map;
    ~ThreadCarrierMaps() { tearDownCarriersAtThreadDeath(map); } // Carrier-TLS-death (non-main threads only).
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(ThreadCarrierMaps);

static CarrierMap& mainThreadCarrierMap()
{
    // Destructor-free by construction (A36 r32) — never freed.
    static thread_local CarrierMap* map { nullptr };
    if (!map) [[unlikely]]
        map = new CarrierMap;
    return *map;
}

static CarrierMap& threadLocalCarrierMap()
{
    static LazyNeverDestroyed<ThreadSpecific<ThreadCarrierMaps>> maps;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        maps.construct();
    });
    return maps.get()->map;
}

// A36 stale-epoch eviction (U-T6 full protocol): the VM that owned this map
// slot died and a new VM was constructed at the same address. An epoch
// mismatch PROVES the old VM's ~VM — including its A36 carrier-collection
// walk and EXIT1.9 wait — ran to completion before the address could be
// reused, so this thread's stale carrier was collected by that walk:
//   - bit-SET (main thread): the walk freed the lite right after its
//     DETACHED flip (A36 r32) — FORGET the pointer without touching it
//     (release(); deleting OR dereferencing here would be UAF/double-free);
//   - bit-CLEAR (embedder thread): destruction was deferred to THIS owner —
//     the lite is alive, its state is DETACHED (the walk's flip strictly
//     preceded ~VM's return), its owned client was already deleted by the
//     walk: run the degenerate free (lite + TID only; no VM/server memory).
static void evictStaleCarrier(CarrierMapEntry& entry, bool isMainThread)
{
    if (!entry.carrier)
        return;
    if (isMainThread) {
        // Never dereference: the ~VM walk owns (and already performed) this
        // lite's free. The TID was retired by the walk too.
        (void)entry.carrier.release();
        return;
    }
    VMLite* lite = entry.carrier.get();
    ASSERT(VMLite::currentIfExists() != lite); // restores ran at depth-0 unlock / ~VM uninstall
    {
        // State reads are under-registry-lock-only (EXIT1.7). DETACHED is
        // terminal and the walk's last touch preceded the old ~VM's return,
        // so no waiting is ever needed here.
        Locker locker { VMLiteRegistry::singleton().lock };
        RELEASE_ASSERT(lite->state == VMLite::State::Detached);
    }
    uint16_t tid = lite->tid;
    entry.carrier = nullptr; // the deferred degenerate free (client-local memory only; ~VMLite's M12 removal is a no-op post-M11)
    releaseCarrierTIDIfHooked(tid);
}

// ANNEX A36C / §A.3.6 SUPERSESSION 3 — per-thread clients (U-T6, replacing
// the U-T8 seam). GIL-off, the JSLock pair acquires/releases on the CURRENT
// carrier's OWN client (§F.1/§B.2) — NEVER the main client (heap Dev 8; the
// heap §10A ISS forward-to-main-client wiring stays GIL-ON-ONLY, §B.3):
//   - the process MAIN thread's carrier REUSES the VM's original client
//     (&vm.clientHeap — F1B "main reuses the original client"); the VM owns
//     it, so no teardown path here ever destroys it (the BORROWED-client
//     rule below);
//   - every OTHER thread's carrier creates its OWN GCClient::Heap against
//     the server (F1B "embedder creates one, §B.2"); the carrier protocol
//     OWNS it — destroyed on the live TLS-death path (EXIT1.3) or by the
//     ~VM A36 walk's lock-free server-side detach (VM.cpp), NEVER by the
//     deferred degenerate dtor (which is restricted to non-server memory).
// OWNERSHIP PREDICATE (normative for this file + the VM.cpp walk): a
// carrier's client is OWNED iff !lite->ownerHasNoTlsDtor — exactly the
// non-main threads; spawned lites' clients (ThreadManager.cpp) are always
// owned by the spawned teardown path.
static GCClient::Heap* perThreadClientForCarrierEntry(VM& vm, bool isMainThread)
{
    if (isMainThread)
        return &vm.clientHeap; // borrowed: the VM's original client (F1B)
    return new GCClient::Heap(vm.heap); // owned: ctor registers with the server's HeapClientSet (§5.1; may run the §10B.4 sticky switch)
}

static VMLite& ensureCarrierLiteForCurrentThread(VM& vm)
{
    ASSERT(vm.gilOff());
    bool isMainThread = WTF::isMainThread();
    CarrierMap& map = isMainThread ? mainThreadCarrierMap() : threadLocalCarrierMap();

    auto addResult = map.add(&vm, CarrierMapEntry { });
    CarrierMapEntry& entry = addResult.iterator->value;
    if (!addResult.isNewEntry) {
        if (entry.vmEpoch == vm.vmEpoch()) // Epochs compared BEFORE the cached carrier (A36).
            return *entry.carrier;
        evictStaleCarrier(entry, isMainThread);
    }

    ThreadManager::installCarrierTIDHooksIfNeeded(); // U-T6: the A36 provider pair (lazy; before the first allocateCarrierTID).

    auto carrier = makeUnique<VMLite>();
    // All fields stamped BEFORE registerLite publishes the lite to registry
    // walkers; the registration lock hold is what fixes them (A36), and the
    // fence below gives clientHeap its EXIT1.4(b) release-publish ordering
    // (constructed-before-published; samplers read it only under the
    // registry lock, EXIT1.2). The store is write-once per registration
    // epoch and strictly precedes this thread's first access acquisition
    // (didAcquireLock's gated acquire runs after the install).
    carrier->tid = allocateCarrierTID(); // unique nonzero TM allocation from the carrier range; main/embedder ThreadState.tid stays 0 (r9 F4).
    ASSERT(ThreadManager::isCarrierTID(carrier->tid)); // the ~VM walk's carrier-vs-spawned discriminator (U-T6 range split)
    carrier->gilOff = vm.gilOff() ? 1 : 0; // §A.1.3 level-2 byte, copied at registration.
    carrier->ownerHasNoTlsDtor = isMainThread; // A36 r32 registration clause: fixed here, immutable, published by the registration lock hold.
    carrier->clientHeap = perThreadClientForCarrierEntry(vm, isMainThread); // §B.2/F1B: main borrows the VM's client, others own a fresh one.
    WTF::storeStoreFence(); // EXIT1.4(b) release-publish half (the registration lock hold is the other half).
    VMLiteRegistry::singleton().registerLite(*carrier, vm);
    // Registration backfills (run AFTER registration so a concurrent install
    // fan cannot be lost; both are idempotent under the per-lite lock):
    carrier->backfillBakedScratchBuffers(); // A16.
    vm.backfillEntryScopeServiceBitsForLiteRegistration(*carrier); // §A.1.5 VM-wide word.

    entry.vmEpoch = vm.vmEpoch();
    entry.carrier = WTF::move(carrier);
    return *entry.carrier;
}

// U-T6 seam for the VM.cpp ~VM A36 walk (same-library linkage; redeclared
// there, not in any header): the calling thread's carrier lite for `vm`, or
// null if this thread never entered it (or the entry is stale). The walk
// uses it to EXEMPT the destroying thread's own carrier from the token-free
// RELEASE_ASSERT — that thread's entry token deliberately survives until the
// final m_lock drop (§F.2 IU row 21).
VMLite* carrierLiteOfCurrentThreadIfExists(VM& vm)
{
    bool isMainThread = WTF::isMainThread();
    CarrierMap& map = isMainThread ? mainThreadCarrierMap() : threadLocalCarrierMap();
    auto it = map.find(&vm);
    if (it == map.end())
        return nullptr;
    if (it->value.vmEpoch != vm.vmEpoch())
        return nullptr; // stale (A36 staleness rule) — never dereferenced
    return it->value.carrier.get();
}

// ============================================================================
// UNGIL §F.1/§F.2/§F.4/§J — entry tokens + the post-GIL lock contract (U-T8;
// DARK: every path below is keyed on vm.gilOff(), which no shipping
// configuration sets — flag-off (useJSThreads=false) behavior is identical).
//
// GIL-off there are two acquisition arms:
//   - SPAWNED Thread (api §5.2; ThreadManager::isJSThreadCurrent()): NO
//     m_lock, ever. lock() installs an entry token {depth, spAtEntry}
//     against the thread's own pre-installed lite — records sp/lastStackTop
//     (§A.1.4), ORs the VM trap + service words into the lite (§A.2.3
//     replaces notifyGrabAllLocks as the late-joiner edge), acquires CLIENT
//     heap access (§B.1, §A.3.2b-gated inside acquireHeapAccess), bumps
//     depth. unlock() is symmetric; depth 0 drains the CURRENT lite's queue
//     (I11), then releases access. JSLockHolder = token.
//   - MAIN/EMBEDDER (ANNEX F1B): m_lock stays REAL (Bun embedder exclusion
//     kept) and acquiring it ALSO takes an entry token (§A.3.1 "entered set"
//     is uniform) + the §A.3.6 carrier+tag+client tuple swap. EVERY lock()
//     runs the gated acquireHeapAccess on THAT carrier's client (idempotent
//     at depth>0, heap F8 step 0); depth-0 unlock releases and retires the
//     token (so §J.3 park sites, which fully release via
//     unlockAllForThreadParking -> willReleaseLock, drop m_lock + token in
//     one shape).
//
// TOKEN STORAGE (recorded deviation, U-T8): §F.1 words the token as living
// "in the VMLite". VMLite.h is OUTSIDE U-T8's owned-file set (L2 appends are
// U-T1/U-T6 territory), so the token record lives in the thread_local vector
// below, keyed by (VM*, vmEpoch). This is semantics-preserving: a token is
// thread-affine by definition (installed/consumed only by its owner thread;
// §F.2's predicate is a current-thread question), conductor predicates read
// lite->clientHeap access state — never token depth — and the §A.3.1 entered
// set is the registry walk. If a later task moves the fields onto the lite
// (L2 append), only this file's helpers change. Records are popped at
// depth-0 release, so the vector is empty at thread death (nothing here
// re-reads walk-freed lites; A36 r32 concerns don't apply).
//
// VM::currentThreadIsHoldingAPILock() (VM.cpp, U-T8) consumes
// currentThreadHoldsEntryToken() below: GIL-off the predicate is REDEFINED
// as "this thread holds an entry token for this VM" (§F.2; DWT §E.7.2 host-
// call meaning). JSLock::currentThreadIsHoldingLock() stays MUTEX-LITERAL.
// ============================================================================

struct VMEntryTokenRecord {
    VM* vm { nullptr };
    uint64_t vmEpoch { 0 };       // A36 staleness rule: tokens never match a reused VM address.
    JSLock* lock { nullptr };     // Non-null => main/embedder arm (F1B); retired by willReleaseLock, keyed by lock so the ~VM-final-unlock (m_vm already nulled) still finds it.
    VMLite* lite { nullptr };     // The lite this token entered with (spawned lite, or the A36 carrier).
    intptr_t depth { 0 };         // Spawned arm: entry depth. Main/embedder arm: 1 (m_lockCount carries recursion under m_lock).
    void* spAtEntry { nullptr };  // §F.1 token half {depth, spAtEntry}.
    // DAL2 composition (review fix, "nested DAL inside a re-entered spawned
    // section"): one frame per OPEN bracket, in LIFO order. ANY bracket
    // entered while this thread holds heap access (the outermost one, and
    // every inner one entered from a nested JSLockHolder re-entry) releases
    // access and records {depthAtEnter, true}; a bracket entered while access
    // is already released is a pure count ({depthAtEnter, false}). The
    // matching exit re-acquires iff its frame released. A nested entry's
    // unlock returning to the TOP frame's depthAtEnter re-releases access
    // (restoring the innermost open bracket's access-released state) — the
    // GIL-on oracle shape, where an inner DropAllLocks at any m_lockCount
    // fully drops the lock and releases access.
    struct DALBracketFrame {
        intptr_t depthAtEnter { 0 };
        bool releasedAccess { false };
    };
    Vector<DALBracketFrame, 1> dalBrackets;
    bool releaseAccessAtDepthZero { false };
    AtomStringTable* entryAtomStringTable { nullptr }; // Spawned arm only (the mutex arm keeps JSLock::m_entryAtomStringTable).
};

static Vector<VMEntryTokenRecord, 2>& entryTokensForCurrentThread()
{
    // Trivial-enough TLS: records are POD and the vector is EMPTY whenever
    // the thread is not entered (depth-0 pops), so late TLS destruction
    // touches no JSC state. GIL-on threads never push, so the flag-off cost
    // is one TLS read + an empty-vector scan on the paths that consult it.
    static thread_local Vector<VMEntryTokenRecord, 2> tokens;
    return tokens;
}

static VMEntryTokenRecord* entryTokenFor(VM& vm)
{
    auto& tokens = entryTokensForCurrentThread();
    for (auto& token : tokens) {
        if (token.vm == &vm && token.vmEpoch == vm.vmEpoch())
            return &token;
    }
    return nullptr;
}

// §F.2 predicate, token meaning. Consumed by VM::currentThreadIsHoldingAPILock
// (VM.cpp; same-library linkage — redeclared there, not in any header, so no
// header outside U-T8's owned set changes).
bool currentThreadHoldsEntryToken(const VM& vm)
{
    auto& tokens = entryTokensForCurrentThread();
    for (auto& token : tokens) {
        if (token.vm == &vm && token.vmEpoch == vm.vmEpoch() && token.depth)
            return true;
    }
    return false;
}

// §F.1 spawned arm: NO m_lock. Caller guarantees vm.gilOff() &&
// ThreadManager::isJSThreadCurrent().
static void spawnedThreadEntryTokenLock(VM& vm, intptr_t lockCount)
{
    // api §5.2: a spawned thread registered + setCurrent its own lite before
    // its first JSLockHolder. A foreign-VM entry from a spawned thread is an
    // embedder-contract violation (§F.5 caller scope / §F.6(e), TERM1.5;
    // A36 single-VM v1): process-abort, not a catchable error.
    VMLite* lite = VMLite::currentIfExists();
    RELEASE_ASSERT(lite && lite->vm == &vm); // §F.6(e): spawned threads are single-VM in v1.

    if (VMEntryTokenRecord* token = entryTokenFor(vm)) {
        ASSERT(token->depth);
        ASSERT(token->lite == lite);
        token->depth += lockCount;
        // §F.1: EVERY lock() runs the gated acquire (idempotent at depth>0,
        // heap F8 step 0). Inside a DAL2 bracket this is the NESTED-ENTRY
        // re-acquire (the canonical DropAllLocks shape: bracketed native code
        // calls back into JS) — correct while JS runs; the matching unlock
        // below restores the bracket's access-released state when depth
        // returns to the innermost open bracket's depthAtEnter (DAL2.2
        // composition; see DALBracketFrame).
        lite->clientHeap->acquireHeapAccess();
        return;
    }

    VMEntryTokenRecord token;
    token.vm = &vm;
    token.vmEpoch = vm.vmEpoch();
    token.lite = lite;
    token.depth = lockCount;
    token.releaseAccessAtDepthZero = true; // §F.1: spawned depth-0 unlock releases access (E.2's drain loop re-acquires; §F.6(c) covers native sections in between).

    // U1/§J.7 (FULL backstop, spawned outermost-entry arm): registered lite
    // for the entered VM, unique nonzero TID, A36C client clause, TLS-tag
    // equality.
    RELEASE_ASSERT(lite->tid);     // tid 0 never installed GIL-off (folded into U1).
    RELEASE_ASSERT(lite->gilOff);  // §A.1.3 level-2 byte agrees with vm.m_gilOff.
    RELEASE_ASSERT(lite->clientHeap && GCClient::Heap::currentThreadClient() == lite->clientHeap); // A36C client clause.
    // U1 TLS-tag clause (ledger row 3 "TLS-tag equality at the backstop"):
    // the REAL tag word(s) must equal this lite's tid << 48 (the api §5.2
    // spawn path ran P5 init + setCurrent before the first JSLockHolder).
    // The jit-side hook is the tag's WRITER, not a checker — a null hook or
    // a desynchronized g_jscButterflyTIDTag fail-stops HERE, before any
    // tier's butterfly fast path consumes a wrong tag.
    assertButterflyTIDTagCoherent();

    auto& thread = Thread::currentSingleton();
    token.entryAtomStringTable = thread.setCurrentAtomStringTable(vm.atomStringTable());

    // §A.1.4: sp/lastStackTop are per-entry-token lite fields (the VM
    // accessors are mode-split per §A.1.3 and route to the CURRENT lite);
    // the token ctor asserts the LITE's slot empty — re-entry restores
    // through the VMEntryRecord chain, not this slot.
    vm.setLastStackTop(thread);
    RELEASE_ASSERT(!vm.stackPointerAtVMEntry());
    token.spAtEntry = currentStackPointer();
    vm.setStackPointerAtVMEntry(token.spAtEntry);

    // §F.1/§A.2.3: token acquisition ORs the VM trap + service words into the
    // lite (the late-joiner delivery edge GIL-off; W0/SD13 carrier-only
    // filtering happens inside, on this — the owner — thread).
    vm.traps().orVMWideTrapBitsIntoLite(*lite);
    vm.backfillEntryScopeServiceBitsForLiteRegistration(*lite);

    // §B.1/§A.3.2b: client heap access, gated (acquireHeapAccess parks on a
    // pending stop per SB1 item 3 and registers the thread for conservative
    // scan, heap I4(b)). Spawned threads are unsampled in v1 (SD18), so no
    // SamplingProfiler notice here.
    lite->clientHeap->acquireHeapAccess();

    entryTokensForCurrentThread().append(token);
}

// §F.1 spawned arm unlock; runs BEFORE any mutex state is consulted (§F.2:
// "Spawned unlock() takes the token branch BEFORE the mutex RELEASE_ASSERT").
static void spawnedThreadEntryTokenUnlock(VM& vm, intptr_t unlockCount)
{
    auto& tokens = entryTokensForCurrentThread();
    for (size_t i = 0; i < tokens.size(); ++i) {
        VMEntryTokenRecord& token = tokens[i];
        if (token.vm != &vm || token.vmEpoch != vm.vmEpoch())
            continue;
        RELEASE_ASSERT(token.depth >= unlockCount);
        if (token.depth > unlockCount) {
            token.depth -= unlockCount;
            // DAL2 composition (U-T8 fix): if this thread is inside a
            // DropAllLocks bracket and this unlock fully unwinds a nested
            // entry (depth back to the depth recorded at the OUTERMOST
            // bracket enter), re-release heap access — otherwise the thread
            // would resume its blocking native section HOLDING access for
            // the rest of the bracket, and a §10.4/§A.3.2 conductor (shared
            // GC, haveABadTime stop) would wait on it for the unbounded
            // duration of the block (DAL2.2 violated; conductor deadlock).
            // Mirrors the GIL-on oracle, where an inner JSLockHolder inside
            // DropAllLocks hits m_lockCount == 0 at its unlock and releases
            // access via willReleaseLock's m_shouldReleaseHeapAccess.
            // Delivery stays deferred to the bracket exit's trap poll (DAL2);
            // nothing is polled here.
            if (!token.dalBrackets.isEmpty()) {
                auto& top = token.dalBrackets.last();
                ASSERT(token.depth >= top.depthAtEnter);
                if (token.depth == top.depthAtEnter)
                    token.lite->clientHeap->releaseHeapAccess();
            }
            return;
        }
        // Depth-0 release. Mirror willReleaseLock with the token still
        // counted (callees may consult the predicate), per-thread state only.
        VMLite* lite = token.lite;
        ASSERT(VMLite::currentIfExists() == lite);
        ASSERT(token.dalBrackets.isEmpty()); // A DAL2 bracket never outlives its entry.
        {
            RefPtr<VM> protectedVM { &vm };
            // §F.1 drain-on-release KEPT GIL-off: the CURRENT lite's queue
            // (I11; §E/U-T9 routes spawned enqueues here — until then the
            // per-lite queue is absent and this is a no-op).
            if (token.dalBrackets.isEmpty()) [[likely]]
                lite->drainDefaultMicrotaskQueue();
            // §A.1.3 mode-split selector: per-lite topCallFrame/lastException.
            if (!protectedVM->group3Primitives().topCallFrame)
                protectedVM->clearLastException();
            // heap.releaseDelayedReleasedObjects() is deliberately NOT
            // mirrored: the delayed-release list is a Cocoa/ObjC-interop
            // facility whose mutation is carrier-affine (IU table row below,
            // EXCLUSIVITY CONSUMER serialized by m_lock); spawned threads
            // never run the ObjC API in v1.
            protectedVM->setStackPointerAtVMEntry(nullptr); // §A.1.4, per-lite.
            if (token.releaseAccessAtDepthZero)
                lite->clientHeap->releaseHeapAccess(); // §F.1: depth 0 releases access.
        }
        Thread::currentSingleton().setCurrentAtomStringTable(token.entryAtomStringTable);
        tokens.removeAt(i);
        return;
    }
    // Unlock without a token: same protocol violation the GIL-on path
    // fail-stops on (lock-not-owned).
    RELEASE_ASSERT_NOT_REACHED();
}

// §F.4 / ANNEX DAL2 (U-T8): the spawned DropAllLocks bracket. A HEAP-ACCESS
// bracket, NOT a lock drop (r20 F1): token, entry depth, m_lock and
// m_lockDropDepth are ALL untouched; JSLock::currentThreadIsHoldingLock()
// stays mutex-literal false throughout; the dropped-lock count is 0 (U14).
// Only the OUTERMOST bracket transitions access (inner = pure count); LIFO is
// NOT required — there is no m_lockDropDepth participation, so the D1
// livelock shape cannot recur. DAL ctor/dtor are access transitions: per the
// §E.2 rule they run holding NO api rank-1..3 lock and no heap 10a/10b lock
// (U20's lint covers DAL sites).
static void spawnedDropAllLocksBracketEnter(VM& vm)
{
    VMEntryTokenRecord* token = entryTokenFor(vm);
    if (!token || !token->depth)
        return; // Not entered: DropAllLocks is a no-op (GIL-on parity, bug 139654#c11).
    // DAL2.1/DAL2.2 (review fix: the access transition is keyed on "access
    // currently held", NOT on bracket depth). The canonical embedder shape —
    // DropAllLocks -> native -> JSLockHolder re-entry (re-acquires access) ->
    // a SECOND blocking DropAllLocks — previously treated the inner bracket
    // as a pure count, so the thread blocked for an unbounded native section
    // HOLDING heap access: the heap §10.4 GC barrier and every §A.3.2
    // conductor predicate (allEnteredThreadsAreQuiescent) would wait on it
    // indefinitely (the DAL2.2 conductor-deadlock class), and the
    // watchdogAssertStopProgress fail-stop turned the hang into a crash. The
    // GIL-on oracle releases access for the inner DropAllLocks too. Each
    // bracket records whether IT released, so exits re-acquire symmetrically
    // (LIFO frames; see DALBracketFrame above).
    bool accessHeld = token->lite->clientHeap->hasHeapAccess();
    token->dalBrackets.append({ token->depth, accessHeld });
    if (accessHeld) {
        // Heap F8 mandatory-revert shape: seq_cst exchange -> NoAccess inside
        // releaseHeapAccess. The thread now counts access-released for the
        // heap §10.4 barrier AND the §A.3.2 conductor predicate. The recorded
        // depth lets a nested JSLockHolder's unwind inside this bracket
        // restore the access-released state (see spawnedThreadEntryTokenUnlock's
        // depth>0 arm).
        token->lite->clientHeap->releaseHeapAccess();
    }
}

static void spawnedDropAllLocksBracketExit(VM& vm)
{
    VMEntryTokenRecord* token = entryTokenFor(vm);
    if (!token || token->dalBrackets.isEmpty())
        return; // Ctor was a no-op (thread was not entered at bracket entry).
    auto frame = token->dalBrackets.takeLast();
    if (frame.releasedAccess) {
        // DAL2.1 dtor: re-acquire the SAME client's access, §A.3.2b/§A.3.8-
        // gated (the SB1 item-3 seq_cst stop-bit poll + park live inside
        // acquireHeapAccess), THEN poll the lite's trap bits before
        // returning to JS — trap delivery during the bracket was deferred to
        // here (same shape as §F.5 nested-entry deferral). U24/DAL2.6 arm:
        // spawned thread blocked in a DAL-bracketed native call while main
        // conducts a shared GC AND a haveABadTime (§K.5) stop — both
        // complete; on release the thread resumes here and observes the
        // deferred traps.
        token->lite->clientHeap->acquireHeapAccess();
        // UNGIL §A.2.2 item 3b (AB-17): lite-then-VM dispatch — the lite's
        // own word carries the rule-3 fanned bits; the VM word carries
        // raises that target it directly (e.g. notifyNeedStopTheWorld's
        // fireTrap). Skip the VM-level service while a termination thrown by
        // the lite service is pending (the caller is about to unwind).
        if (VMTraps* liteTraps = perThreadTrapsIfExists(*token->lite); liteTraps && liteTraps != &vm.traps()) {
            liteTraps->handleTrapsIfNeeded();
            if (!vm.hasPendingTerminationException()) [[likely]]
                vm.traps().handleTrapsIfNeeded();
        } else
            vm.traps().handleTrapsIfNeeded();
    }
}

// Main/embedder (F1B) token retirement at full release — willReleaseLock and
// §J.3's unlockAllForThreadParking both land here, so park sites release
// m_lock + token in one shape (the captured-lite poll rule J.3 keys off the
// PARK lite captured before this runs; it never re-reads VMLite::current()).
// Keyed by lock, not VM: the ~VM final unlock runs after willDestroyVM nulled
// JSLock::m_vm, and the token must survive through ~VM teardown (the
// destroying thread IS entered — DWT stopRunningTasks and friends assert the
// token predicate mid-teardown) and retire only when m_lock actually drops.
static void retireEntryTokenForLock(JSLock* lock)
{
    auto& tokens = entryTokensForCurrentThread();
    if (tokens.isEmpty()) [[likely]] // GIL-on threads never push: flag-off this is the whole cost.
        return;
    for (size_t i = tokens.size(); i--;) {
        if (tokens[i].lock == lock) {
            tokens.removeAt(i);
            return;
        }
    }
}

// ============================================================================
// UNGIL §J.3 captured-lite park records (r10 F5; U-T11). A main/embedder
// park site (join, cond.wait, TA/property Atomics.wait) fully releases via
// unlockAllForThreadParking — which runs the §A.3.6 LIFO tuple restore, so
// for the whole park VMLite::current() is the PRIOR lite (null for a Bun
// thread that entered from native), NOT this VM's carrier. Per-quantum park
// polls must read the CAPTURED carrier lite's trap/stop bits + the
// waiter-state atomic, never VMLite::current(). The capture happens HERE, at
// the §J.3 full-release shape itself (unlockAllForThreadParking stashes the
// lite that was current at the release), and park sites consume it through
// capturedParkLiteOfCurrentThreadIfAny below (same-library seam; consumers
// redeclare — the currentThreadHoldsEntryToken pattern; LockObject.h is
// outside this task's owned-file set).
//
// Lifetime proof (r10 F5, recorded): a carrier dies only at owner TLS death
// or the ~VM A36 walk; the owner is alive mid-park (it IS the parked
// thread), and ~VM while this VM's JS frames are live on a parked thread is
// an embedder error (vmstate M6 precondition) — the captured pointer cannot
// dangle. §A.2.4's D9 clause is re-pointed accordingly (PARK lite; the
// predicate itself lives in VMTraps.cpp, parkLitePollTerminationRequested).
//
// Episode accounting (annex W W1): a record is pushed at every §J.3 full
// release of a gilOff carrier and popped at the matching outermost
// reacquisition (didAcquireLock). The W1 early-service episode therefore
// pops at its inner lock() and re-pushes at its inner
// unlockAllForThreadParking — "exactly once per ACQUISITION EPISODE" is the
// stack discipline, mechanically. GIL-on/flag-off never push (gated on
// vm.gilOff()), so the flag-off cost is zero (no TLS write on those paths).
// ============================================================================

struct ParkedCarrierLiteRecord {
    VM* vm { nullptr };
    uint64_t vmEpoch { 0 }; // A36 staleness rule: a record never matches a reused VM address.
    VMLite* lite { nullptr };
};

static Vector<ParkedCarrierLiteRecord, 2>& parkedCarrierLiteRecordsForCurrentThread()
{
    // Trivial-enough TLS, like entryTokensForCurrentThread: records are POD
    // and the vector is EMPTY whenever the thread is not inside a §J.3 park
    // bracket, so late TLS destruction touches no JSC state.
    static thread_local Vector<ParkedCarrierLiteRecord, 2> records;
    return records;
}

// §J.3 captured-lite seam (U-T11): the carrier lite this thread's innermost
// live §J.3 release of `vm` captured, or null (not parked / GIL-on caller).
// Park sites call this AFTER their unlockAllForThreadParking-shaped release
// (the GILDroppedSection ctor) — equivalent to capturing "BEFORE the
// release" per r10 F5, because the stash IS the value current at release.
VMLite* capturedParkLiteOfCurrentThreadIfAny(VM& vm)
{
    auto& records = parkedCarrierLiteRecordsForCurrentThread();
    for (size_t i = records.size(); i--;) {
        if (records[i].vm == &vm && records[i].vmEpoch == vm.vmEpoch())
            return records[i].lite;
    }
    return nullptr;
}

static void pushParkedCarrierLiteRecord(VM& vm, VMLite* lite)
{
    parkedCarrierLiteRecordsForCurrentThread().append({ &vm, vm.vmEpoch(), lite });
}

static void popParkedCarrierLiteRecordIfAny(VM& vm)
{
    auto& records = parkedCarrierLiteRecordsForCurrentThread();
    if (records.isEmpty()) [[likely]]
        return;
    for (size_t i = records.size(); i--;) {
        if (records[i].vm == &vm && records[i].vmEpoch == vm.vmEpoch()) {
            records.removeAt(i);
            return;
        }
    }
}

// ============================================================================
// UNGIL §G — per-thread blocking policy (U-T11). Replaces the per-VM G11
// gate jsThreadsCanBlockOnCurrentThread as the GIL-off authority:
//   §G.1 spawned TS => true; main/embedder => embedder policy
//        (isAtomicsWaitAllowedOnCurrentThread()).
//   §G.2 governs ALL sync parks: TA/property Atomics.wait (the KEPT G11
//        gate, §C.4), join, contended lock.hold, cond.wait; violations throw
//        the existing TypeErrors (api I18 intact).
//   §G.3 the D4 GIL-dropped main TA wait machinery is GIL-on-only; GIL-off a
//        permitted main sync wait parks per §J.3. D8 per §C.6 (SD6).
// GIL-on (and flag-off) this reduces EXACTLY to the landed
// jsThreadsCanBlockOnCurrentThread body (the spawned arm is gilOff-gated),
// so re-pointing a GIL-on caller at it is behavior-preserving. The G11
// consumer re-points (ThreadAtomics.cpp:876 property-wait gate,
// ThreadObject.cpp/ConditionObject.cpp/LockObject.cpp park gates,
// ThreadObject.h's declaration) live OUTSIDE this task's owned-file set —
// recorded for their owning tasks; this is the predicate of record
// (same-library seam, consumers redeclare).
// ============================================================================
bool mayBlockSynchronously(VM& vm)
{
    if (vm.gilOff() && ThreadManager::isJSThreadCurrent())
        return true; // §G.1: a spawned TS may always park synchronously (post-lift blocking is §G-only; deadlock = user error, ruling recorded r23).
    return vm.m_typedArrayController->isAtomicsWaitAllowedOnCurrentThread();
}

// ============================================================================
// UNGIL annex W W1 — the parked-carrier watchdog service episode (U-T11).
// Caller: a main/embedder carrier parked under §J.3 (m_lock + token + access
// all released by unlockAllForThreadParking) that observed the
// watchdog-check bit on its CAPTURED park lite at a D9 quantum
// (parkLitePollWatchdogCheckRequested, VMTraps.cpp). Contract:
//   - the caller holds NO rank-3 waiter-list lock (W1: reacquisition happens
//     only after the quantum wait returns; listLock is taken only AFTER the
//     episode and dropped BEFORE any re-park) and no other api lock;
//   - this performs the FULL §J.3 exit reacquisition — lock() runs the
//     §A.3.6 carrier/tag/client swap, the §F.1 service-word + trap OR, and
//     the §A.3.2b-gated acquireHeapAccess — then services
//     Watchdog::shouldTerminate under the token on this thread (callback
//     semantics + CPU re-arm identical to an entered carrier, via
//     Watchdog::serviceCheckFromReacquiredParkedCarrier);
//   - terminate => VM-wide termination was raised (rule 3, with the W1
//     consumed-by-servicer shield) — returns true; the caller proceeds to
//     its final park exit (the wait fails per SD8/§E.5);
//   - no terminate => returns false AFTER re-releasing per §J.3
//     (unlockAllForThreadParking — a NEW acquisition episode opens when the
//     caller re-parks); the caller must then run the r15 F2 old-node
//     disposition under its listLock before re-parking.
// The release is unconditional either way: the caller is mid-§J.3 bracket
// and its GILDroppedSection dtor performs the real exit re-lock.
// ============================================================================
bool reacquireParkedCarrierAndServiceWatchdogCheck(VM& vm)
{
    ASSERT(vm.gilOff());
    ASSERT(!ThreadManager::isJSThreadCurrent()); // SD14/W0: spawned threads never service the watchdog.
    JSLock& apiLock = vm.apiLock();
    // The §J.3 release fully dropped m_lock; a holder reaching here would
    // deadlock on itself below — fail-stop the protocol violation.
    RELEASE_ASSERT(!apiLock.currentThreadIsHoldingLock());
    apiLock.lock(); // FULL reacquisition: §A.3.6 swap + §F.1 OR + §A.3.2b-gated access (pops this VM's park record — episode end).
    bool terminated = Watchdog::serviceCheckFromReacquiredParkedCarrier(vm);
    apiLock.unlockAllForThreadParking(); // Re-release per §J.3 (re-pushes the park record — new episode).
    return terminated;
}

// ============================================================================
// UNGIL §F.5 exit-side completion (U-T6): the outer foreign gilOff carrier
// whose gated access re-acquire + deferred trap poll must run AFTER the
// inner VM's m_lock drops. willReleaseLock runs BEFORE m_lock.unlock(), and
// the gated acquireHeapAccess PARKS if the outer VM is mid-stop — parking
// there while still holding the inner mutex closes a deadlock cycle (outer
// conductor waits on an access-holding client; the exiting nested thread
// parks on the conductor's stop while holding the inner m_lock some entering
// thread needs to make progress), and running the outer VM's trap handling
// under the inner mutex is an api-lock-rank inversion besides. So
// willReleaseLock only RE-STAMPS the §10A.1 slot (non-blocking TLS store,
// preserving slot-correct-before-AHA) and stashes the restored lite here;
// unlock() completes the restore after the drop. Thread-local by
// construction (the LIFO restore is a current-thread tuple operation, so no
// other thread's unlock can race the stash), and consumed in the SAME
// unlock() call that set it: willReleaseLock runs exactly when m_lockCount
// is about to reach 0, and that unlock() always proceeds to the m_lock drop.
// The park-site rule this enforces: no api mutex is held at any gated
// acquireHeapAccess park site (the DAL2 dtor and the spawned token paths
// already satisfy it; this was the one violator).
// ============================================================================
static VMLite*& pendingForeignCarrierAccessRestore()
{
    static thread_local VMLite* pending { nullptr };
    return pending;
}

static void completeDeferredForeignCarrierRestoreAfterUnlock()
{
    VMLite* restored = pendingForeignCarrierAccessRestore();
    if (!restored) [[likely]]
        return;
    pendingForeignCarrierAccessRestore() = nullptr;
    // Holding NO mutex here: the inner m_lock just dropped, and the outer
    // VM cannot die under us — this thread still holds its outer entry
    // (token + m_lock/lite), so the outer ~VM's EXIT1.9 fence cannot pass.
    restored->clientHeap->acquireHeapAccess(); // gated; parks if the outer VM is mid-stop; ISB1.2 sync on the success path
    // UNGIL §A.2.2 item 3b (AB-17): lite-then-VM dispatch (see the DAL2 dtor
    // poll above for the rationale).
    VM& restoredVM = *restored->vm;
    if (VMTraps* liteTraps = perThreadTrapsIfExists(*restored); liteTraps && liteTraps != &restoredVM.traps()) {
        liteTraps->handleTrapsIfNeeded();
        if (!restoredVM.hasPendingTerminationException()) [[likely]]
            restoredVM.traps().handleTrapsIfNeeded();
    } else
        restoredVM.traps().handleTrapsIfNeeded();
}

// ============================================================================
// UNGIL §F.2 — predicate-consumer IU table (U-T8 deliverable; ~60 rows).
//
// Both predicates' consumers, classified {assert (token meaning) | BRANCH |
// EXCLUSIVITY CONSUMER (needs a §K serializer — the predicate alone no longer
// implies mutual exclusion GIL-off)}. "MUTEX" rows deliberately consume
// JSLock::currentThreadIsHoldingLock() (mutex-literal, §F.2). ANNEX F2 fixed
// rulings are cited per row. This comment block is the table of record for
// the tree (INTEGRATE-ungil.md is outside U-T8's owned-file set; a later
// close task may relocate it verbatim). Line numbers are as of the U-T8 base
// tree (rows in VM.cpp/JSLock.cpp shift slightly with this task's own edits).
//
// | # | Site | Class | Ruling |
// |---|------|-------|--------|
// | 1 | API/APICast.h:161 toJS | assert (token) | host-call path, U13 |
// | 2 | bytecode/JSThreadsSafepoint.cpp:225 | assert (token) | §A.3 conductor entry (U-T5 replaces the stub) |
// | 3 | heap/WeakSetInlines.h:44 WeakSet::allocate | assert (token+access) | F2 fixed ruling: NOT exclusivity (REFUTED r11 F2) — free-list pop is MSPL-locked under ISS (WeakSetInlines.h:69); deallocate stays lock-free with the recorded WeakSet.h:121-131 soundness argument |
// | 4 | interpreter/InterpreterInlines.h:106,:138 | assert (token) | execution entry |
// | 5 | interpreter/Interpreter.cpp:1032,:1465,:1673 | assert (token) | executeProgram/Call/Eval entries |
// | 6 | interpreter/MicrotaskCallInlines.h:59 | assert (token) | checkpoint runs on the owner (I11) |
// | 7 | heap/Heap.cpp:666 (lastChanceToFinalize path) | assert (token) | ~VM thread keeps its token through teardown (see retireEntryTokenForLock) |
// | 8 | heap/Heap.cpp:789,:801 (reportExtraMemory/deprecatedReport) | assert (token) | mutator-side accounting |
// | 9 | heap/Heap.cpp:2797 | assert (token) | already tolerates the ISS flip (`|| worldIsStoppedForAllClients`) |
// | 10 | heap/Heap.cpp:4260 | BRANCH | sticky-designation diagnostics arm; token meaning correct |
// | 11 | heap/Heap.cpp:4956 | BRANCH | "current thread is a mutator of this heap" query; token meaning |
// | 12 | heap/GCActivityCallback.cpp:63 | assert (token) | timer fires on the owning runloop (carrier) |
// | 13 | heap/HeapIterationScope.h:47 | comment-only | no live consumer |
// | 14 | heap/PreciseSubspace.cpp:81 | assert (token) | allocation requires token+access |
// | 15 | runtime/DeferredWorkTimer.cpp:88,:192,:219,:228,:256,:339,:345 | assert (token) | §E.7.2 token meaning (`|| GC-thread+stopped` arms unchanged) |
// | 16 | runtime/DeferredWorkTimer.cpp:183 runRunLoop | assert (token, NEGATIVE) | §E.7.2: the pump thread must NOT hold a token |
// | 17 | runtime/JSCell.cpp:179-180 validateIsNotSweeping | assert/BRANCH (token) | F2 fixed ruling: token + per-CLIENT mutator state (GCClient::Heap::m_mutatorState via mutatorStateSlot) |
// | 18 | runtime/Watchdog.cpp:56,:77(W4) | assert (token) | r13: token meaning answers the DATA-RACE question; the SEMANTIC ruling is §A.2.8 carrier-only / W4 |
// | 19 | runtime/Watchdog.cpp:52,:165 | assert (MUTEX, kept) | watchdog arm/disarm is a carrier-only API (SD13); mutex-literal is the intended meaning |
// | 20 | runtime/VM.h:1263 (ensureWatchdog-kin),:1437 | assert (token) | |
// | 21 | runtime/VM.cpp:745,:801 ~VM | assert (token) | the destroying thread's token survives until the final m_lock drop |
// | 22 | runtime/VM.cpp:853 primitiveGigacageDisabled | BRANCH (MUTEX, kept) | F2 fixed ruling: MUTEX predicate + §A.1.5 deferred arm — the gigacage-disable service is VM-wide; token holders that are not the mutex holder route through requestEntryScopeService fan-out |
// | 23 | runtime/VM.cpp:1277 (throwException kin) | assert (token) | per-lite exception slots (§A.1.3) |
// | 24 | runtime/VM.cpp:1491,:1514,:1522 | assert (token) | entry-scope service APIs; per-lite records (§A.1.5) |
// | 25 | runtime/VM.cpp:1743 sanitizeStackForVM | BRANCH (token) | F2 fixed ruling: uses the CURRENT lite's lastStackTop — vm.lastStackTop() is the §A.1.3 mode-split selector, so token-true implies the lite slot is this thread's |
// | 26 | runtime/JSLock.cpp:287,:538,:541,:656,:688,:711 | BRANCH/assert (MUTEX, kept) | the m_lock recursion/unwind protocol itself; spawned arms branch away BEFORE these (§F.2) |
// | 27 | runtime/JSLock.cpp:741 DropAllLocks ctor | BRANCH (token) | redefinition intended: a spawned token holder asserts !isCollectorBusyOnCurrentThread |
// | 28 | runtime/LockObject.cpp:77,:82,:93,:108 | assert (MUTEX today) | §J.3/U-T11 row: park-site asserts move to the token meaning when GILDroppedSection grows its GIL-off-by-caller split (J.3 spawned arm = token-only) |
// | 29 | runtime/LockObject.cpp:130 | BRANCH (MUTEX today) | §J.3/U-T11 row: the park full-release branch; spawned arm must branch on the token (carried by U-T11 with GILDroppedSection) |
// | 30 | runtime/VMLiteInlines.h:84, VMLite.cpp:153 | assert (token) | I14 owner asserts |
// | 31 | runtime/ConcurrentButterfly.cpp:463,:872,:1345,:1541,:1683,:1878,:1912,:2190,:2310,:2422,:3028 (+:236 comment) | assert (token) | witness-contract asserts; the OM serializers are the SW/TID machinery, not this predicate |
// | 32 | runtime/ThreadManager.cpp:57-59 ~AsyncTicket | assert (token) | r10 F2 fixed ruling: gains the sweep-context arm GIL-off — the sweeper holds a token, satisfied by the redefinition; the in-lock-sweep Strong free itself is the §F.3(a)/§LK.8 carve-out |
// | 33 | tools/VMInspector.cpp:169,:224 | BRANCH (token) | SD13/SD14 tooling family; U-T8d's off-thread-reader table governs what it may read |
// | 34 | runtime/JSGlobalObject.cpp:1125 | assert (token) | |
// | 35 | heap (delayed release): Heap::releaseDelayedReleasedObjects callers (willReleaseLock here) | EXCLUSIVITY CONSUMER | the ObjC delayed-release list is serialized by m_lock (carrier-affine); the spawned token arm deliberately does NOT call it — no §K serializer needed while spawned threads cannot run the ObjC API (v1) |
// | 36 | wtf/.. JSLock::ownerThread()/ownerThreadUID() consumers (SamplingProfiler, VMTraps signal sender) | BRANCH (MUTEX, kept) | identify the m_lock owner (carrier); spawned threads are reached via the registry walk (§A.2.3), not via lock ownership |
//
// Rows 28/29 are the only consumers whose GIL-off rewrite lands outside this
// task (U-T11, §J.3); every other row is correct under the §F.2 split as
// implemented here, with rows 19/22/26/36 deliberately mutex-literal.
// ============================================================================

// ============================================================================
// UNGIL §E.4 — settle-site lock-context table (U-T8 deliverable; r17 F2
// precondition: AsyncTicket::settle is invoked holding NO api rank-1..3 lock;
// U20 lints rank-3 settles; GIL-on text stands — settle = DWT
// scheduleWorkSoon, no rank-3 lock).
//
// | Site | Caller context | Rank-3 locks at the settle call | Status |
// |------|----------------|--------------------------------|--------|
// | LockObject.cpp:275 (pump -> settleLockGrant) | api 5.5a P "dequeue head, settle" | NONE — the grant is recorded under m_queueLock, the Locker scope CLOSES, then settleLockGrant runs (decide-under-lock / act-after-drop) | COMPLIANT as landed |
// | LockObject.cpp:521 (asyncHold immediate grant -> settleLockGrant) | api 5.5a A "set m_asyncHeld/m_asyncHolder, settle" | NONE — acquiredImmediately is decided under m_queueLock; the settle runs after the scope closes | COMPLIANT as landed |
// | ThreadObject.cpp:246 (completion -> settleJoinTicket) | F5 Compl "drop joinLock; settle moved tkts" | NONE — asyncJoiners are swapped out under joinLock (:239 scope), settled after the drop | COMPLIANT as landed |
// | ThreadObject.cpp:435 (asyncJoin on a non-Running thread -> settleJoinTicket) | F5 asyncJoin | NONE — Phase is re-checked under joinLock (:426 scope) which decides settle-vs-append; the settle runs after the drop (no lost wakeup: completion settles appended tickets) | COMPLIANT as landed |
// | ThreadManager.cpp:78 AsyncTicket::settle body | the settle implementation | NONE held by settle itself; GIL-off (U-T9) it gains the inboxLock open/closed routing per §E.4 — the closed=>main fallback must run AFTER the inboxLock drop (r18 F2), and the §E.7.3 wake fires with NEITHER m_pendingLock NOR any TS::inboxLock held | GIL-on shape landed; GIL-off routing = U-T9 |
//
// All frozen settle sites already conform to the r17 F2 precondition; U-T9
// must preserve the act-after-drop shape when it adds the registrant-inbox
// arm. U20's lint (settle-under-rank-3, wake-under-rank-3) guards regression.
// ============================================================================

// ============================================================================
// UNGIL §F.6 — embedder (Bun) checklist rows (U-T8 deliverable; ANNEX EC1).
// These rows bind OUT-OF-TREE Bun code; they are recorded here because the
// lock contract they audit is this file's. Sign-off split (r21 F2): (b) gates
// U-T9 ENTRY; (a)/(c)/(e) and (d)'s audit are U-T14 close items.
//
// | Row | Obligation | Status |
// |-----|------------|--------|
// | (a) JSLockHolder exclusivity audit | enumerate every Bun JSLockHolder critical section that today relies on excluding ALL JS; GIL-off m_lock excludes only embedder threads (§F.1) — each section must either tolerate concurrent spawned mutators or take a §K serializer | OPEN (Bun-side; U-T14) |
// | (b) SD10 continuation affinity | embedder-registered ordinary-promise reactions settled by a spawned thread run ON THE SETTLER (§E.1b.1, X1.7) — off m_lock, off the embedder loop; a carrier-hop demand = a NEW negotiated SD | HARD precondition of U-T9 ENTRY; ALS slice discharged by ANNEX ALS1 |
// | (c) blocking-site enumeration (DAL2.5/§F.6 delta (c)) | enumerate every Bun spawned-thread blocking native section; each must use §F.4 DAL, §J.3, or RHA/AHA-bracket per heap §9 (SPEC-heap.md:244). In-tree DAL sites are already bracket-correct (this file); known Bun site CLASSES to audit: epoll/kqueue waits in the event loop, synchronous file I/O off the pool, napi blocking calls, postMessage/Atomics.wait shims, dlopen/module-resolution locks | OPEN (Bun-side; U-T14) |
// | (d) FIRST-VM-WINS construction order (U0c/EC1) | the VM intended to spawn Threads MUST be constructed strictly before any other VM (a utility VM constructed first permanently demotes it — no re-designation in v1); recommended boot-assert vm.gilOff()==true right after construction; enumerate every Bun VM-construction site incl. lazy helper VMs | OPEN (Bun-side; U-T14) |
// | (e) no foreign-VM entry from spawned threads | native code on a spawned Thread never enters/creates another VM; enforcement is TWO RELEASE_ASSERTs: the §F.5-scope check in spawnedThreadEntryTokenLock (gilOff target VM) AND the gilOffProcess gate in JSLock::lock() ahead of the mutex arm (loser/GIL-on target VM — the only realizable foreign-VM shape, since the winner is the sole gilOff VM); both process-abort naming §F.6(e) via the lite->vm check | ENFORCED here (both arms); Bun audit OPEN (U-T14) |
// ============================================================================

JSLockHolder::JSLockHolder(JSGlobalObject* globalObject)
    : JSLockHolder(globalObject->vm())
{
}

JSLockHolder::JSLockHolder(VM* vm)
    : JSLockHolder(*vm)
{
}

JSLockHolder::JSLockHolder(VM& vm)
    : m_vm(&vm)
{
    protect(m_vm->apiLock())->lock();
}

JSLockHolder::~JSLockHolder()
{
    RefPtr<JSLock> apiLock(&m_vm->apiLock());
    m_vm = nullptr;
    apiLock->unlock();
}

JSLock::JSLock(VM* vm)
    : m_lockCount(0)
    , m_lockDropDepth(0)
    , m_vm(vm)
    , m_entryAtomStringTable(nullptr)
{
}

JSLock::~JSLock() = default;

void JSLock::willDestroyVM(VM* vm)
{
    ASSERT_UNUSED(vm, m_vm == vm);
    m_vm = nullptr;
}

void JSLock::lock()
{
    lock(1);
}

// Use WTF_IGNORES_THREAD_SAFETY_ANALYSIS because this function conditionally unlocks m_lock, which
// is not supported by analysis.
void JSLock::lock(intptr_t lockCount) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    ASSERT(lockCount > 0);

    // UNGIL §F.1 (U-T8): GIL-off a SPAWNED Thread NEVER touches m_lock —
    // entry is token-only. Branch before any mutex state (the mutex protocol
    // below is the main/embedder F1B arm). Dark flag-off: gilOff() is false
    // in every shipping configuration.
    if (m_vm && m_vm->gilOff() && ThreadManager::isJSThreadCurrent()) [[unlikely]] {
        spawnedThreadEntryTokenLock(*m_vm, lockCount);
        return;
    }

    // UNGIL §F.5 caller scope / §F.6(e) (TERM1.5, U-T8): spawned threads are
    // single-VM in v1. The token-arm assert above only covers a gilOff
    // TARGET VM — under gilOffProcess a LOSER VM (m_gilOff == 0) would
    // otherwise fall through to the m_lock arm and SILENTLY enter: install
    // the loser's shared main carrier (the licensed U0b escape below),
    // swap this thread's TID tag and §10A.1 client slot away from the
    // winner-lite identity it still owns, and run JS on a foreign VM while
    // holding (or interleaving with) the winner's heap access. Fail-stop
    // here instead — an embedder-contract violation naming §F.6(e), a
    // process abort, NOT a catchable error. Dark flag-off: isGILOffProcess()
    // is false in every shipping configuration.
    if (m_vm && VM::isGILOffProcess() && ThreadManager::isJSThreadCurrent()) [[unlikely]] {
        VMLite* lite = VMLite::currentIfExists();
        RELEASE_ASSERT(lite && lite->vm == m_vm); // §F.6(e): no foreign-VM entry from spawned threads (A36 single-VM v1).
    }

#if USE(WEB_THREAD)
    if (m_isWebThreadAware) {
        ASSERT(WebCoreWebThreadIsEnabled && WebCoreWebThreadIsEnabled());
        WebCoreWebThreadLock();
    }
#endif

    bool success = m_lock.tryLock();
    if (!success) [[unlikely]] {
        if (currentThreadIsHoldingLock()) {
            m_lockCount += lockCount;
            // UNGIL §F.1 (F1B): GIL-off EVERY lock() runs the gated
            // acquireHeapAccess on the carrier's client — idempotent at
            // depth>0 (heap F8 step 0); after a conductor-forced revert it
            // re-parks/re-acquires here before JS resumes.
            if (m_vm && m_vm->gilOff()) [[unlikely]] {
                VMLite* lite = VMLite::currentIfExists();
                // §F.5 v1 REFUSAL (recorded deviation, U-T6): recursive
                // re-entry of a HELD gilOff VM while a FOREIGN lite is
                // current — the A->B->A shape, JSLockHolder(A) {
                // JSLockHolder(B) { JSLockHolder(A) } } — is fail-stopped,
                // not honored. The m_lockCount recursion path performs no
                // tuple swap, so honoring it would run A's JS under B's TID
                // tag and B's §10A.1 client (unlocked flat-butterfly
                // transitions under a wrong TID — SPEC-objectmodel §2/§3
                // memory corruption) AND would silently skip the mandated
                // gated acquire on A's client (JS access-released). GIL-on
                // the same shape merely runs A's JS under B's atom table (a
                // pre-existing upstream sharp edge embedders avoid via
                // DropAllLocks); GIL-off the consequence escalates to heap
                // corruption, so the supported nesting discipline is
                // strictly LIFO per VM (§F.5): re-entering A from inside B
                // needs a per-depth tuple-swap stack — not v1.
                RELEASE_ASSERT(lite && lite->vm == m_vm && lite->clientHeap); // §F.5 LIFO nesting discipline (U-T6)
                lite->clientHeap->acquireHeapAccess();
            }
            return;
        }
        // UNGIL §F.5 entry-side EARLY RELEASE (U-T6): this thread is about
        // to BLOCK on a contended m_lock. If a FOREIGN gilOff VM A's carrier
        // is current, the thread still holds A's client heap access, and a
        // plain mutex wait polls none of A's stop machinery — holding access
        // across it would stall A's heap §10.4 barrier and §A.3.2 conductor
        // predicate for the unbounded duration of the inner-lock contention
        // (B's holder can run JS indefinitely), and in the embedder ABBA
        // shape (B's holder entering A) composes into a deadlock the §F.5
        // release exists to break. Release A's access BEFORE the wait;
        // didAcquireLock's §F.5 hook stays as the idempotent uncontended-
        // path release (hasHeapAccess() guard), and the willReleaseLock/
        // unlock LIFO restore re-acquires after the inner m_lock drops.
        // Deliberately NOT on the recursion path above (no blocking there;
        // a release would strand the outer VM access-released through the
        // recursion's unlock, which never reaches depth 0).
        if (Options::useVMLite() && m_vm) [[unlikely]] {
            if (VMLite* outer = VMLite::currentIfExists(); outer && outer->vm && outer->vm != m_vm && outer->gilOff) {
                if (outer->clientHeap && outer->clientHeap->hasHeapAccess())
                    outer->clientHeap->releaseHeapAccess();
            }
        }
        m_lock.lock();
    }

    m_ownerThread = &Thread::currentSingleton();
    m_ownerThreadPtr.store(m_ownerThread.get(), std::memory_order_relaxed);
    m_hasOwnerThread.store(true, std::memory_order_release);
    ASSERT(!m_lockCount);
    m_lockCount = lockCount;

    didAcquireLock();
}

void JSLock::didAcquireLock()
{
    // FIXME: What should happen to the per-thread identifier table if we don't have a VM?
    if (!m_vm)
        return;
    
    auto& thread = Thread::currentSingleton();
    ASSERT(!m_entryAtomStringTable);
    m_entryAtomStringTable = thread.setCurrentAtomStringTable(m_vm->atomStringTable());
    ASSERT(m_entryAtomStringTable);

    bool tookGILOffCarrierPath = false;
    VMLite* gilOffCarrierLite = nullptr;
    if (Options::useVMLite()) [[unlikely]] {
        // UNGIL §F.5 nested foreign-VM entry (U-T6; main/embedder carriers
        // ONLY — a spawned thread RELEASE_ASSERTed in lock() before reaching
        // here, TERM1.5/§F.6(e)): entering VM B while a gilOff VM A's carrier
        // is current FIRST releases A's client heap access (F8 mandatory-
        // revert: seq_cst exchange -> NoAccess inside releaseHeapAccess), so
        // in the nested window this thread counts access-released for A's
        // heap §10.4 barrier AND A's §A.3.2 conductor predicate (heap I4(b):
        // A's JS frames stay alive via the conservative machine-thread
        // scan — the thread remains registered). "FIRST" is enforced at the
        // BLOCKING site: lock()'s entry-side early release runs BEFORE any
        // contended m_lock wait, so A's access is never held across an
        // unbounded inner-lock contention — the release here is the
        // uncontended-tryLock-path twin and an idempotent backstop
        // (hasHeapAccess() guard). A's trap/stop/termination delivery is
        // DEFERRED to the LIFO restore (willReleaseLock re-stamps; the gated
        // re-acquire + poll complete in unlock() AFTER the inner m_lock
        // drops — see completeDeferredForeignCarrierRestoreAfterUnlock).
        // Applies per nesting level (the restores form the LIFO stack of
        // releases). A GIL-on outer lite (gilOff == 0, incl. any
        // m_mainVMLite) keeps the landed handoff protocol — no release here.
        if (VMLite* outer = VMLite::currentIfExists(); outer && outer->vm != m_vm && outer->gilOff) [[unlikely]] {
            if (outer->clientHeap && outer->clientHeap->hasHeapAccess())
                outer->clientHeap->releaseHeapAccess();
        }
        if (m_vm->gilOff()) [[unlikely]] {
            tookGILOffCarrierPath = true;
            // UNGIL §A.3.6 (A36/A36C; U-T1, dark): GIL-off NEVER installs the
            // shared main carrier — every thread (the process main thread
            // included) gets its per-(thread,VM) carrier, lazily created at
            // first entry. The install swaps the full TLS tuple
            // {lite, TID-tag, §10A.1 client slot}: setCurrent swaps lite+tag
            // (jit P5/CS3 hook), and the client slot is re-stamped HERE,
            // before any allocation/OM fast path and before the heap-access
            // acquisition below (preserving §10A.1's slot-correct-before-AHA
            // ordering). A spawned thread (api §5.2) never reaches this
            // path with a foreign lite: spawned Threads are single-VM in v1
            // (TERM1.5; §F.5's nested-entry rules cover main/embedder
            // carriers only and land with U-T6).
            VMLite* current = VMLite::currentIfExists();
            if (!current || current->vm != m_vm) {
                VMLite& carrier = ensureCarrierLiteForCurrentThread(*m_vm);
                m_entryVMLite = VMLite::setCurrent(&carrier);
                m_didInstallCarrierVMLite = true;
                m_entryThreadClient = GCClient::Heap::currentThreadClient();
                // A36C: the §10A.1 client slot is stamped from the CARRIER's
                // client — the per-thread client (U-T6): the VM's original
                // client for the main thread, this thread's own otherwise.
                // This line is the GIL-off stamping authority — see the
                // banner above.
                GCClient::Heap::setCurrentThreadClient(carrier.clientHeap);
                gilOffCarrierLite = &carrier;
            } else {
                // Defensive: a lite for this VM was already current at an
                // outermost acquisition (no path produces this today —
                // restores run at depth-0 unlock and parks fully release).
                gilOffCarrierLite = current;
            }
            // §J.7 (U-T8): the JSLock.cpp:151 L1 backstop, REPLACED — the
            // GIL-off branch RELEASE_ASSERTs the FULL U1 (ledger row 3):
            // lite->vm, unique nonzero TID ("tid 0 never installed" folded
            // in), A36C client clause, AND the TLS-tag equality half.
            RELEASE_ASSERT(gilOffCarrierLite->vm == m_vm);
            RELEASE_ASSERT(gilOffCarrierLite->tid);
            RELEASE_ASSERT(gilOffCarrierLite->gilOff);
            RELEASE_ASSERT(gilOffCarrierLite->clientHeap && GCClient::Heap::currentThreadClient() == gilOffCarrierLite->clientHeap);
            // U1 TLS-tag clause: runs AFTER setCurrent's install above (the
            // P5/CS3 hook is the tag's WRITER; this verifies the written
            // word — incl. the JIT-visible copy where one exists — against
            // the CURRENT lite's tid, fail-stopping a null/broken hook
            // before any butterfly fast path runs under a wrong tag). Also
            // covers the defensive already-current arm.
            assertButterflyTIDTagCoherent();
            // §F.1 (F1B): acquiring m_lock ALSO takes an entry token — the
            // §A.3.1 entered set is uniform across both arms. depth stays 1
            // here; m_lockCount carries mutex-arm recursion. Retired by
            // willReleaseLock (incl. §J.3's unlockAllForThreadParking).
            {
                VMEntryTokenRecord token;
                token.vm = m_vm;
                token.vmEpoch = m_vm->vmEpoch();
                token.lock = this;
                token.lite = gilOffCarrierLite;
                token.depth = 1;
                entryTokensForCurrentThread().append(token);
            }
            // §J.3 (U-T11): an outermost acquisition of a gilOff VM ends any
            // open park episode for it — the only way control proceeds past
            // a §J.3 full release is re-locking this VM (the GILDroppedSection
            // dtor, or the W1 early-service reacquisition). Pop the
            // captured-lite record; no-op when not parked.
            popParkedCarrierLiteRecordIfAny(*m_vm);
        }
        // §6.4.4: install the main carrier iff this thread has no lite or a
        // foreign one (covers main thread, embedder threads, multi-VM per
        // thread). A spawned thread (api §5.2) registered + setCurrent its
        // own lite BEFORE its first JSLockHolder, so cur->vm == m_vm there
        // and nothing is installed (m_didInstallVMLite stays false).
        //
        // SOUNDNESS INVARIANT (Phase A, normative): sharing the main carrier
        // (tid 0) across whichever thread holds the API lock — including
        // embedder threads and threads entering a foreign VM — is sound ONLY
        // because the API lock is mutually exclusive among this VM's mutators
        // (phase-1 GIL). Butterfly TID tags written under tid 0 persist in
        // object headers after the lock is released; if two threads could
        // mutate concurrently while both believing they are TID-0 owners,
        // they would race unlocked flat-butterfly transitions
        // (SPEC-objectmodel §2/§3 assumes TIDs identify a unique live
        // thread). The RELEASE_ASSERT fail-stops any GIL-off VM that still
        // reaches this install path; the gilOff carrier branch above is M4's
        // cross-WS-item-13 replacement (per-thread carriers, unique TIDs).
        //
        // UNGIL SUPERSESSION (U-T1; INTEGRATE-ungil.md ledger): the landed
        // form `RELEASE_ASSERT(!useJSThreads || useThreadGIL)` gains exactly
        // ONE licensed escape — under gilOffProcess a LOSER VM (U0b:
        // m_gilOff == 0) legally installs the shared main carrier because
        // its mutators stay serialized by its own m_lock. The m_gilOff
        // winner never gets here (it took the carrier branch above), and a
        // useJSThreads&&!useThreadGIL config WITHOUT the U0 trio still
        // crashes exactly as before (U0 option validation additionally
        // refuses that shape upstream once landed).
        if (!tookGILOffCarrierPath) {
            VMLite* cur = VMLite::currentIfExists();
            if ((!cur || cur->vm != m_vm) && m_vm->mainVMLite()) {
                RELEASE_ASSERT(!Options::useJSThreads() || Options::useThreadGIL()
                    || (VM::isGILOffProcess() && !m_vm->gilOff()));
                m_entryVMLite = VMLite::setCurrent(m_vm->mainVMLite());
                m_didInstallVMLite = true;
            }
        }
    }

    m_vm->setLastStackTop(thread);

    if (gilOffCarrierLite) [[unlikely]] {
        // UNGIL §F.1 (F1B; U-T8): GIL-off the heap-access bracket is the
        // CARRIER CLIENT's (§B.1), §A.3.2b/§A.3.8-gated inside
        // acquireHeapAccess (F8 step-0 idempotent; stop-pending =>
        // mandatory-revert + park). Symmetric release in willReleaseLock.
        GCClient::Heap* client = gilOffCarrierLite->clientHeap;
        m_shouldReleaseHeapAccess = !client->hasHeapAccess();
        client->acquireHeapAccess();
    } else if (m_vm->heap.hasAccess())
        m_shouldReleaseHeapAccess = false;
    else {
        m_vm->heap.acquireAccess();
        m_shouldReleaseHeapAccess = true;
    }

    // UNGIL §A.1.4 (L7): the accessor is mode-split, so GIL-on this is the
    // landed VM-slot assert, and GIL-off it asserts the CURRENT CARRIER
    // LITE's slot empty (re-entry restores through the VMEntryRecord chain,
    // not this slot) — exactly the §A.1.4 re-keying; one line serves both.
    RELEASE_ASSERT(!m_vm->stackPointerAtVMEntry());
    void* p = currentStackPointer();
    m_vm->setStackPointerAtVMEntry(p);
    if (gilOffCarrierLite) [[unlikely]] {
        // §F.1 token half {depth, spAtEntry}; recorded after the sp write so
        // the token and the lite slot agree.
        if (VMEntryTokenRecord* token = entryTokenFor(*m_vm))
            token->spAtEntry = p;
    }

    if (thread.uid() != m_lastOwnerThread) {
        m_lastOwnerThread = thread.uid();
        if (m_vm->heap.machineThreads().addCurrentThread()) {
            if (isKernTCSMAvailable())
                enableKernTCSM();
        }
    }

    // Note: everything below must come after addCurrentThread().
    if (gilOffCarrierLite) [[unlikely]] {
        // UNGIL §A.2.3/§F.1 (U-T8): GIL-off, token acquisition ORs the VM
        // trap + service words into the entering lite — this REPLACES
        // notifyGrabAllLocks as the late-joiner delivery edge (the VM-wide
        // word stays the fan-out source; carrier-only bits are not filtered
        // here because this arm IS a carrier).
        m_vm->traps().orVMWideTrapBitsIntoLite(*gilOffCarrierLite);
        m_vm->backfillEntryScopeServiceBitsForLiteRegistration(*gilOffCarrierLite);
    } else
        m_vm->traps().notifyGrabAllLocks();

#if ENABLE(SAMPLING_PROFILER)
    {
        SamplingProfiler* samplingProfiler = m_vm->samplingProfiler();
        if (samplingProfiler) [[unlikely]]
            samplingProfiler->noticeJSLockAcquisition();
    }
#endif
}

void JSLock::unlock()
{
    unlock(1);
}

#if PLATFORM(COCOA) && CPU(ADDRESS64) && CPU(ARM64)
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
// FIXME: rdar://168614004
NO_RETURN_DUE_TO_CRASH NEVER_INLINE void JSLock::dumpInfoAndCrashForLockNotOwned() // __attribute__((optnone))
{
    size_t pageSize = WTF::pageSize();
    RELEASE_ASSERT(isPowerOfTwo(pageSize));
    uintptr_t pageMask = ~(static_cast<uintptr_t>(pageSize) - 1);

    uintptr_t* thisAsIntPtr = std::bit_cast<uintptr_t*>(this);
    uintptr_t thisAsInt = std::bit_cast<uintptr_t>(this);
    uintptr_t thisEndAsInt = thisAsInt + sizeof(JSLock);
    uintptr_t blockStartAsInt = thisAsInt & pageMask;
    uintptr_t blockEndAsInt = blockStartAsInt + pageSize;
    char* blockStart = std::bit_cast<char*>(blockStartAsInt);

    register uint64_t dumpState __asm__("x28");

#define updateDumpState(newState, used1, used2, used3) do { \
        WTF::compilerFence(); \
        __asm__ volatile ("mov %0, #" #newState : "=r"(dumpState) : "r"(used1), "r"(used2), "r"(used3)); \
        WTF::compilerFence(); \
    } while (false)

    updateDumpState(0x1111, dumpState, dumpState, dumpState);

    register void* currentThread __asm__("x27") = &Thread::currentSingleton();
    updateDumpState(0x2222, currentThread, dumpState, dumpState);

    // Checks if the this pointer is corrupted. Being out of the page bounds is 1 example of corruption.
    bool lockIsWithinPageBoundary = (blockStartAsInt <= thisAsInt) && (thisEndAsInt <= blockEndAsInt);
    register uintptr_t miscState __asm__("x26") = lockIsWithinPageBoundary;
    updateDumpState(0x3333, miscState, dumpState, dumpState);

    register uintptr_t lockWord0 __asm__("x25") = thisAsIntPtr[0];
    updateDumpState(0x4444, lockWord0, dumpState, dumpState);

    register void* ownerThread __asm__("x24") = m_ownerThreadPtr.load(std::memory_order_relaxed);
    updateDumpState(0x5555, ownerThread, dumpState, dumpState);

    register uintptr_t lockWord2 __asm__("x23") = thisAsIntPtr[2];
    register uintptr_t lockWord3 __asm__("x22") = thisAsIntPtr[3];
    updateDumpState(0x6666, lockWord2, lockWord3, dumpState);

    miscState |= (!!m_vm) << 8; // Check if VM is null.
    updateDumpState(0x7777, miscState, dumpState, dumpState);

    // Check if the page is zero.
    register uintptr_t numZeroBytesBeforeAfter __asm__("x21") = 0;

    uintptr_t totalZeroBytesInPage = 0;
    uintptr_t currentZeroBytes = 0;

    // Count zero bytes before JSLock.
    uintptr_t bytesBeforeLock = thisAsInt - blockStartAsInt;
    for (auto mem = blockStart; mem < blockStart + bytesBeforeLock; mem++) {
        bool byteIsZero = !*mem;
        if (byteIsZero)
            currentZeroBytes++;
    }
    numZeroBytesBeforeAfter = currentZeroBytes;
    numZeroBytesBeforeAfter |= bytesBeforeLock << 16;
    updateDumpState(0x8888, numZeroBytesBeforeAfter, bytesBeforeLock, currentZeroBytes);

    totalZeroBytesInPage += currentZeroBytes;

    // Count zero bytes after JSLock.
    currentZeroBytes = 0;
    uintptr_t bytesAfterBlock = pageSize - (thisAsInt + sizeof(JSLock));
    for (auto mem = blockStart + bytesBeforeLock + sizeof(JSLock); mem < blockStart + pageSize; mem++) {
        bool byteIsZero = !*mem;
        if (byteIsZero)
            currentZeroBytes++;
    }
    numZeroBytesBeforeAfter |= currentZeroBytes << 32;
    numZeroBytesBeforeAfter |= bytesAfterBlock << 48;
    updateDumpState(0x9999, numZeroBytesBeforeAfter, bytesAfterBlock, currentZeroBytes);

    totalZeroBytesInPage += currentZeroBytes;

    register uintptr_t numZeroBytesInLock __asm__("x20") = 0;
    currentZeroBytes = 0;
    for (auto mem = blockStart + bytesBeforeLock; mem < blockStart + bytesBeforeLock + sizeof(JSLock); mem++) {
        bool byteIsZero = !*mem;
        if (byteIsZero)
            currentZeroBytes++;
    }
    numZeroBytesInLock = currentZeroBytes;
    numZeroBytesInLock |= sizeof(JSLock) << 16;
    updateDumpState(0xAAAA, numZeroBytesInLock, currentZeroBytes, dumpState);

    totalZeroBytesInPage += currentZeroBytes;
    numZeroBytesInLock |= totalZeroBytesInPage << 32;
    updateDumpState(0xBBBB, numZeroBytesInLock, totalZeroBytesInPage, currentZeroBytes);

    register VM* vmPtr __asm__("r19") = m_vm;
    register AtomStringTable* atomStringTable __asm__("x15") = m_entryAtomStringTable;
    register JSLock* thisPtr __asm__("x14") = this;
    updateDumpState(0xCCCC, vmPtr, atomStringTable, thisPtr);

    __asm__ volatile (WTF_FATAL_CRASH_INST : : "r"(dumpState), "r"(miscState), "r"(lockWord0), "r"(currentThread), "r"(ownerThread), "r"(lockWord2), "r"(lockWord3), "r"(numZeroBytesBeforeAfter), "r"(numZeroBytesInLock), "r"(vmPtr), "r"(atomStringTable), "r"(thisPtr));
    __builtin_unreachable();

#undef updateDumpState
}
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
#endif

// Use WTF_IGNORES_THREAD_SAFETY_ANALYSIS because this function conditionally unlocks m_lock, which
// is not supported by analysis.
void JSLock::unlock(intptr_t unlockCount) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    // UNGIL §F.2 (U-T8): spawned unlock takes the token branch BEFORE the
    // mutex RELEASE_ASSERT — a spawned thread never owns m_lock GIL-off.
    if (m_vm && m_vm->gilOff() && ThreadManager::isJSThreadCurrent()) [[unlikely]] {
        spawnedThreadEntryTokenUnlock(*m_vm, unlockCount);
        return;
    }

#if PLATFORM(COCOA) && CPU(ADDRESS64) && CPU(ARM64)
    if (!currentThreadIsHoldingLock()) [[unlikely]]
        dumpInfoAndCrashForLockNotOwned();
#else
    RELEASE_ASSERT(currentThreadIsHoldingLock());
#endif

    ASSERT(m_lockCount >= unlockCount);

    // Maintain m_lockCount while calling willReleaseLock() so that its callees know that
    // they still have the lock.
    if (unlockCount == m_lockCount)
        willReleaseLock();

    m_lockCount -= unlockCount;

    if (!m_lockCount) {
        m_hasOwnerThread.store(false, std::memory_order_release);
        m_lock.unlock();
        // UNGIL §F.5 exit-side completion (U-T6): the outer foreign gilOff
        // carrier's gated access re-acquire + deferred trap poll run HERE,
        // with the inner m_lock dropped (no-op unless willReleaseLock's LIFO
        // restore stashed a lite above; see the helper's banner).
        completeDeferredForeignCarrierRestoreAfterUnlock();
    }
}

void JSLock::willReleaseLock()
{
    {
        RefPtr protectedVM { m_vm };
        if (protectedVM) {
            static bool useLegacyDrain = false;
#if PLATFORM(COCOA)
            static std::once_flag once;
            std::call_once(once, [] {
                useLegacyDrain = !linkedOnOrAfterSDKWithBehavior(SDKAlignedBehavior::DoesNotDrainTheMicrotaskQueueWhenCallingObjC);
            });
#endif

            if (!m_lockDropDepth || useLegacyDrain) {
                protectedVM->drainMicrotasks();
                // UNGIL §F.1 (U-T8): drain-on-release KEPT GIL-off — the
                // CURRENT lite's queue (I11). The VM-level drain above stays
                // for embedder-enqueued microtasks (queueMicrotask is not
                // rerouted until §E/U-T9); the per-lite drain is a no-op
                // while that queue is never created.
                if (protectedVM->gilOff()) [[unlikely]] {
                    VMLite* lite = VMLite::currentIfExists();
                    if (lite && lite->vm == protectedVM.get())
                        lite->drainDefaultMicrotaskQueue();
                }
            }

            // UNGIL §A.1.3 (U-T1): this depth-0 "no live frames" guard reads
            // topCallFrame through the SAME mode-split selector as the
            // clearLastException() it gates — GIL-on the VM block
            // (bit-identical to the landed raw read), GIL-off the CURRENT
            // carrier lite (still installed here: the tuple restore below
            // runs after this block). A raw VM-block read would be the inert
            // spare slot (always null) once U-T3/U-T4 emission makes the
            // lite authoritative, unconditionally clearing the lite's
            // m_lastException under live frames. On-thread read — IU table
            // (iv) row, disposition (iii).
            if (!protectedVM->group3Primitives().topCallFrame)
                protectedVM->clearLastException();

            protectedVM->heap.releaseDelayedReleasedObjects();
            protectedVM->setStackPointerAtVMEntry(nullptr);

            if (m_shouldReleaseHeapAccess) {
                if (protectedVM->gilOff()) [[unlikely]] {
                    // §F.1 (F1B; U-T8): symmetric to didAcquireLock — the
                    // CARRIER CLIENT's access bracket, not the server's.
                    // The carrier tuple is still installed here (the restore
                    // below runs after this block).
                    VMLite* lite = VMLite::currentIfExists();
                    RELEASE_ASSERT(lite && lite->vm == protectedVM.get() && lite->clientHeap);
                    lite->clientHeap->releaseHeapAccess();
                } else
                    protectedVM->heap.releaseAccess();
            }
        }
    }

    // UNGIL §F.1/§J.3 (U-T8): retire this lock's entry token (main/embedder
    // F1B arm). Runs for plain depth-0 unlock, DropAllLocks' full drop, AND
    // unlockAllForThreadParking — so §J.3 park sites release m_lock + token
    // in one shape. No-op (empty thread-local scan) on GIL-on threads.
    retireEntryTokenForLock(this);

    if (m_didInstallVMLite) {
        // §6.4.4: restore ONLY IF the installed main carrier is still
        // current — a lite swapped in after our install (e.g. DropAllLocks
        // hand-off to a spawned thread that reacquired with its own lite) is
        // NEVER clobbered; always clear both members. m_vm can only be null
        // here if willDestroyVM already ran, and ~VM calls
        // uninstallVMLiteForVMDestruction() first, which clears the flag —
        // the m_vm guard is belt-and-suspenders.
        if (m_vm && VMLite::currentIfExists() == m_vm->mainVMLite())
            VMLite::setCurrent(m_entryVMLite);
        m_entryVMLite = nullptr;
        m_didInstallVMLite = false;
    }

    if (m_didInstallCarrierVMLite) {
        // UNGIL A36C: LIFO restore of the full tuple — {lite, TID-tag} via
        // setCurrent (tag hook), then the §10A.1 client slot re-stamped to
        // the restored tuple's saved value; restoring to "no lite" clears
        // the slot (m_entryThreadClient was captured as null then).
        ASSERT(!VMLite::currentIfExists() || VMLite::currentIfExists()->vm == m_vm);
        VMLite::setCurrent(m_entryVMLite);
        GCClient::Heap::setCurrentThreadClient(m_entryThreadClient);
        m_entryVMLite = nullptr;
        m_entryThreadClient = nullptr;
        m_didInstallCarrierVMLite = false;
    }

    // UNGIL §F.5 LIFO restore (U-T6): if the restore above brought back a
    // FOREIGN gilOff VM's carrier (this entry was nested inside it), undo the
    // entry-side release — re-stamp the outer client (A36C: every LIFO
    // restore re-stamps the §10A.1 slot through the staleness-checked tuple;
    // the m_didInstallCarrierVMLite arm restored the SAVED slot value, which
    // for a nested entry is exactly the outer client, so the explicit stamp
    // below is idempotent there and load-bearing for the GIL-on-inner-VM
    // restore arm, which does not touch the slot), then DEFER the gated
    // re-acquire of the outer client's access + the outer lite's trap poll
    // to unlock()'s post-m_lock-drop completion
    // (completeDeferredForeignCarrierRestoreAfterUnlock — see its banner:
    // the gated acquire can PARK on the outer VM's stop, and parking while
    // the inner m_lock is still held deadlocks; the poll under the inner
    // mutex is a rank inversion). The outer-VM JS cannot resume before the
    // completion runs: both happen inside the same unlock() call, before it
    // returns to the caller. m_vm may already be null here (~VM final
    // unlock): the deferred outer-VM re-acquire is exactly as required then.
    // Flag-off/GIL-on: no gilOff lite exists, the branch is dead.
    //
    // m_lockDropDepth gate: a DropAllLocks full drop / §J.3
    // unlockAllForThreadParking release of the INNER VM must leave the outer
    // VM access-released too — the thread is entering a blocking native
    // section/park, and re-holding A's access across it would stall A's
    // §10.4 barrier and §A.3.2 predicate for the unbounded block (the DAL2.2
    // shape). The matching grabAllLocks/park re-lock re-enters through
    // didAcquireLock (whose §F.5 entry hook re-releases idempotently), and
    // the eventual REAL depth-0 unlock runs this hook with
    // m_lockDropDepth == 0, restoring A's access + deferred delivery there.
    if (Options::useVMLite() && !m_lockDropDepth) [[unlikely]] {
        if (VMLite* restored = VMLite::currentIfExists(); restored && restored->gilOff && restored->vm && restored->vm != m_vm) [[unlikely]] {
            RELEASE_ASSERT(restored->clientHeap); // EXIT1.4(b): stamped before the outer entry's first acquisition
            GCClient::Heap::setCurrentThreadClient(restored->clientHeap); // A36C restore-side re-stamp (slot-correct-before-AHA: the gated acquire follows post-unlock)
            ASSERT(!pendingForeignCarrierAccessRestore());
            pendingForeignCarrierAccessRestore() = restored; // consumed by this unlock() after the m_lock drop
        }
    }

    if (m_entryAtomStringTable) {
        Thread::currentSingleton().setCurrentAtomStringTable(m_entryAtomStringTable);
        m_entryAtomStringTable = nullptr;
    }
}

void JSLock::uninstallVMLiteForVMDestruction()
{
    // SPEC-vmstate §6.4.4/I20. Caller: TOP of ~VM (M6), API lock held
    // (~VM asserts), m_vm still valid (runs BEFORE
    // m_apiLock->willDestroyVM(this) nulls it).
    if (m_didInstallCarrierVMLite) {
        // UNGIL A36 (U-T1 skeleton): the destroying thread's carrier tuple is
        // restored here; the carrier itself stays in the TLS map and is
        // collected by the ~VM walk / carrier-TLS-death path (U-T6 owns the
        // full EXIT1.9 order).
        VMLite::setCurrent(m_entryVMLite);
        GCClient::Heap::setCurrentThreadClient(m_entryThreadClient);
        m_entryVMLite = nullptr;
        m_entryThreadClient = nullptr;
        m_didInstallCarrierVMLite = false;
        return;
    }
    if (!m_didInstallVMLite)
        return;
    if (VMLite::currentIfExists() == m_vm->mainVMLite())
        VMLite::setCurrent(m_entryVMLite);
    m_entryVMLite = nullptr;
    m_didInstallVMLite = false;
}

unsigned JSLock::unlockAllForThreadParking()
{
    // UNGIL §J.3 (U-T8 note; U-T11 carries the GILDroppedSection split):
    // GIL-off this remains the MAIN/EMBEDDER park-site full release — it
    // drops m_lock AND (via willReleaseLock) the entry token, releasing the
    // carrier client's heap access, exactly the J.3 "release m_lock + token
    // via the unlockAllForThreadParking shape". Spawned threads never own
    // m_lock GIL-off, so their park sites must not reach this (the J.3
    // spawned arm is token-only: access release + §A.3 park cooperation).
    // ORDERING CONSTRAINT (recorded; IU rows 28-29 below): the
    // GILDroppedSection GIL-off-by-caller split (spawned arm = token-only +
    // §A.3.2b post-wake poll, LockObject.cpp) has NOT landed yet; the §C.4
    // 4.5-1a spawned-TA-wait lift (AtomicsObject.cpp) MUST NOT land before
    // it. The RELEASE_ASSERT below is the fail-stop coupling: a spawned
    // GIL-off caller never holds m_lock, so reaching here without the split
    // crashes deterministically in ALL build types instead of corrupting the
    // park protocol — landing the lift first is loudly unshippable, not a
    // silent race.
    //
    // REACH (review fix — do not read the above as "only the §C.4 TA lift
    // is coupled"): GILDroppedSection is reached from EVERY core-api §5.x
    // park site with no gate of its own on spawned threads — thread.join()
    // (ThreadObject.cpp), contended lock.hold (LockObject.cpp), cond.wait
    // (ConditionObject.cpp), property Atomics.wait (ThreadAtomics.cpp) —
    // not just the TA-wait lift. Under GIL-off, ANY spawned-thread park
    // (e.g. two spawned threads contending one Lock) lands on this
    // RELEASE_ASSERT and aborts. That is the AB-13 fail-stop working as
    // intended, but it means NO GIL-off corpus arm that parks may be
    // green-lit before the AB-13 split lands; a partial activation that
    // wires spawn (AB-11/AB-12) without AB-13 fail-stops on the first
    // contended lock, join, or wait.
    RELEASE_ASSERT(currentThreadIsHoldingLock());
    unsigned droppedLockCount = static_cast<unsigned>(m_lockCount);
    // UNGIL §J.3 captured-lite capture (r10 F5; U-T11): record the gilOff
    // carrier lite that is current at THIS release — willReleaseLock's
    // §A.3.6 LIFO restore is about to swap it away, and the park site's
    // per-quantum polls must read the CAPTURED lite's bits, never
    // VMLite::current(). GIL-on/flag-off: no gilOff lite, nothing captured.
    VMLite* parkLite = nullptr;
    if (m_vm && m_vm->gilOff()) [[unlikely]] {
        if (VMLite* current = VMLite::currentIfExists(); current && current->vm == m_vm)
            parkLite = current;
    }
    // Suppress willReleaseLock()'s drainMicrotasks() (guarded on
    // !m_lockDropDepth): a park site must not run user JS mid-host-call.
    // Every other willReleaseLock side effect (atom-table restore,
    // releaseDelayedReleasedObjects, stackPointerAtVMEntry clear,
    // conditional clearLastException, heap-access release) runs exactly as
    // it does for dropAllLocks(). Bump and restore both happen while m_lock
    // is still held, so no other thread can observe the transient depth and
    // the DropAllLocks LIFO protocol is unaffected.
    ++m_lockDropDepth;
    willReleaseLock();
    --m_lockDropDepth;
    m_lockCount = 0;
    m_hasOwnerThread.store(false, std::memory_order_release);
    m_lock.unlock();
    // Push AFTER the full drop: the record exists exactly while the thread
    // is released-and-parked (popped at the matching outermost re-lock in
    // didAcquireLock — see the episode-accounting banner above).
    if (parkLite) [[unlikely]]
        pushParkedCarrierLiteRecord(*m_vm, parkLite);
    return droppedLockCount;
}

void JSLock::lock(JSGlobalObject* globalObject)
{
    protect(globalObject->vm().apiLock())->lock();
}

void JSLock::unlock(JSGlobalObject* globalObject)
{
    protect(globalObject->vm().apiLock())->unlock();
}

// This function returns the number of locks that were dropped.
unsigned JSLock::dropAllLocks(DropAllLocks* dropper)
{
    if (!currentThreadIsHoldingLock())
        return 0;

    ++m_lockDropDepth;

    dropper->setDropDepth(m_lockDropDepth);

    auto& thread = Thread::currentSingleton();
    thread.setSavedStackPointerAtVMEntry(m_vm->stackPointerAtVMEntry());
    thread.setSavedLastStackTop(m_vm->lastStackTop());

    unsigned droppedLockCount = m_lockCount;
    unlock(droppedLockCount);

    return droppedLockCount;
}

void JSLock::grabAllLocks(DropAllLocks* dropper, unsigned droppedLockCount)
{
    // If no locks were dropped, nothing to do!
    if (!droppedLockCount)
        return;

    ASSERT(!currentThreadIsHoldingLock());
    lock(droppedLockCount);

    while (dropper->dropDepth() != m_lockDropDepth) {
        unlock(droppedLockCount);
        Thread::yield();
        lock(droppedLockCount);
    }

    --m_lockDropDepth;

    auto& thread = Thread::currentSingleton();
    m_vm->setStackPointerAtVMEntry(thread.savedStackPointerAtVMEntry());
    m_vm->setLastStackTop(thread);
}

JSLock::DropAllLocks::DropAllLocks(VM* vm)
    : m_droppedLockCount(0)
    // If the VM is in the middle of being destroyed then we don't want to resurrect it
    // by allowing DropAllLocks to ref it. By this point the JSLock has already been
    // released anyways, so it doesn't matter that DropAllLocks is a no-op.
    , m_vm((vm && !vm->heap.isShuttingDown()) ? vm : nullptr)
{
    if (!m_vm)
        return;

    // Contrary to intuition, DropAllLocks does not require that we are actually holding
    // the JSLock before getting here. Its goal is to release the lock if it is held. So,
    // if the lock isn't already held, there's nothing to do, and that's fine.
    // See https://bugs.webkit.org/show_bug.cgi?id=139654#c11.
    RELEASE_ASSERT(!m_vm->currentThreadIsHoldingAPILock() || !m_vm->isCollectorBusyOnCurrentThread(), m_vm->currentThreadIsHoldingAPILock(), m_vm->isCollectorBusyOnCurrentThread());

    // UNGIL §F.4 / ANNEX DAL2 (U-T8): on a SPAWNED thread GIL-off,
    // DropAllLocks is a HEAP-ACCESS bracket, not a lock drop — token, entry
    // depth, m_lock, m_lockDropDepth all untouched; 0 locks dropped (U14).
    // DAL2.4 SUPERSESSION: INTEGRATE-api D1's phase-1 constraint (no
    // DropAllLocks on the shared VM while spawned Threads are live) is
    // LIFTED GIL-off by this bracket; GIL-on (gilOff() false) keeps the
    // constraint until the flip — this branch is unreachable there, so the
    // landed behavior is bit-identical.
    if (m_vm->gilOff() && ThreadManager::isJSThreadCurrent()) [[unlikely]] {
        spawnedDropAllLocksBracketEnter(*m_vm);
        return; // m_droppedLockCount stays 0.
    }

    m_droppedLockCount = protect(m_vm->apiLock())->dropAllLocks(this);
}

JSLock::DropAllLocks::DropAllLocks(JSGlobalObject* globalObject)
    : DropAllLocks(globalObject ? &globalObject->vm() : nullptr)
{
}

JSLock::DropAllLocks::DropAllLocks(VM& vm)
    : DropAllLocks(&vm)
{
}

JSLock::DropAllLocks::~DropAllLocks()
{
    if (!m_vm)
        return;
    // UNGIL §F.4 / ANNEX DAL2 (U-T8): spawned arm — close the access
    // bracket (gated re-acquire + deferred trap poll); never grabAllLocks
    // (nothing was dropped, U14; LIFO/m_lockDropDepth uninvolved).
    if (m_vm->gilOff() && ThreadManager::isJSThreadCurrent()) [[unlikely]] {
        spawnedDropAllLocksBracketExit(*m_vm);
        return;
    }
    protect(m_vm->apiLock())->grabAllLocks(this, m_droppedLockCount);
}

} // namespace JSC
