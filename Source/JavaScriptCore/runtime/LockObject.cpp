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
#include "LockObject.h"

#include "CustomGetterSetter.h"
#include "ExceptionHelpers.h"
#include "InternalFunction.h" // SCALEBENCH §37 R1: createSubclassStructure for the constructLock newTarget!=callee arm.
#include "FunctionCodeBlock.h" // hold-vmEntry-trampoline fast path: codeBlockForCall()->numParameters() / jitCode().
#include "JITCode.h" // hold-vmEntry-trampoline fast path: jitCode()->addressForCall().
#include "JSCInlines.h"
#include "JSLock.h"
#include "LLIntThunks.h" // hold-vmEntry-trampoline fast path: vmEntryToJavaScriptWith0Arguments.
#include "MachineStackMarker.h" // T5-barrier-site: CurrentThreadState / RegisterState / ALLOCATE_AND_GET_REGISTER_STATE for the GILDroppedSection spawned-arm coop root snapshot.
#include "JSNativeStdFunction.h"
#include "JSPromise.h"
#include "ObjectConstructor.h"
#include "ThreadAtomics.h"
#include "ThreadObject.h"
#include "ThreadManager.h"
#include "VMLite.h"
#include "VMManager.h"
#include "TopExceptionScope.h"
#include <wtf/MonotonicTime.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/ParkingLot.h>
#include <wtf/RunLoop.h>

namespace JSC {

// UNGIL §A.2.4 rule 4 / annex W W1 (AB-17 item 4) — park-lite predicates and
// the W1 carrier service episode. Same-library seams (consumers redeclare;
// the predicates of record live in VMTraps.cpp, the episode helper and the
// captured-lite accessor in JSLock.cpp). GIL-on,
// parkLitePollTerminationRequested(vm, nullptr) is byte-equivalent to the
// landed jsThreadParkTerminationRequested (watchdog-check folded in);
// GIL-off it is termination-ONLY against the PARK lite's word, and the
// watchdog-check bit is serviced by the W1 episode instead of being treated
// as a termination verdict.
bool parkLitePollTerminationRequested(VM&, VMLite* parkLite);
bool parkLitePollWatchdogCheckRequested(VM&, VMLite* parkLite);
VMLite* capturedParkLiteOfCurrentThreadIfAny(VM&);
bool reacquireParkedCarrierAndServiceWatchdogCheck(VM&);

// ---------------- S2-parallel-cpu-waste instrumentation + W-adaptive spin ----------------
//
// SCALEBENCH §27 ceiling analysis: at W=16, parallel phases A+B+C consume
// ~96.8k cpu-ms vs ~17.4k at W=1 — a 5.6× overhead, ~79k cpu-ms of pure
// contention waste. cpu_util 0.44 × 16 cores × 14847 ms wall − 7700 ms serial
// = 96.8k; threads ARE running (13.5 cores busy over 7150 ms parallel-wall),
// just burning. Prime suspects (charter S2): (i) the §25-T5 adaptive spin
// below against Zipf-hot K=128 shard locks (Phase A/B/C all do
// shard.lock.hold per term); (ii) Atomics.add(counters, '…', 1) on a single
// shared object every doc/query (the ThreadAtomics counters, dumped together
// with the per-lock numbers below).
//
// Two pieces here:
//   (a) Options::logJSLockContention() — per-NativeLockState {acquires,
//       spinIters, parks} counters + a process-global registry, dumped at
//       std::atexit. The registry holds Ref<NativeLockState> so the dump
//       never reads freed memory; NativeLockState is ThreadSafeRefCounted and
//       only the JS Lock object's destruction would otherwise drop the last
//       ref. Diagnostic option only — flag-off and option-off the registry is
//       never touched (one predicted-not-taken branch on the acquire path).
//   (d) W-adaptive spin bound: the §25-T5 jsLockSpinLimit (40) is halved at
//       clientSet().size() >= 8 (and quartered at >= 16). With K=128 shard
//       locks Zipf-concentrated, at W=16 the hottest few shards see >>2
//       contenders, so a 40-iter spin per failed tryLock is pure burn against
//       a holder whose critical section outlasts the spin anyway. The bound is
//       cached in s_jsLockAdaptiveSpinLimit (relaxed-loaded on the spin path)
//       and refreshed only inside the heavy slow path (after the spin failed
//       and we are about to GILDroppedSection + park anyway), so the
//       rank-6-locked HeapClientSet::size() read never sits on the fast path.
//       gilOff-only — the spin itself is gilOff-gated, so flag-off behavior is
//       byte-identical to the pre-S2 code.
//
// Neither piece weakens any assert or changes flag-off behavior: the option
// defaults off, every counter bump is gated on it, and the adaptive limit is
// read only inside the existing gilOff-gated spin block.

namespace {

struct JSLockContentionRegistry {
    Lock lock; // diagnostic-only; rank: leaf, never held across any other lock.
    Vector<Ref<NativeLockState>> states WTF_GUARDED_BY_LOCK(lock);
    unsigned nextId WTF_GUARDED_BY_LOCK(lock) { 1 };
};

JSLockContentionRegistry& jsLockContentionRegistry()
{
    static LazyNeverDestroyed<JSLockContentionRegistry> registry;
    static std::once_flag onceKey;
    std::call_once(onceKey, [] {
        registry.construct();
        std::atexit([] { dumpJSLockContentionStats(); });
    });
    return registry;
}

} // anonymous namespace

void dumpJSLockContentionStats()
{
    if (!Options::logJSLockContention())
        return;
    auto& registry = jsLockContentionRegistry();
    uint64_t totalAcquires = 0;
    uint64_t totalSpinIters = 0;
    uint64_t totalParks = 0;
    unsigned hottestId = 0;
    uint64_t hottestParks = 0;
    {
        Locker locker { registry.lock };
        dataLogLn("[logJSLockContention] ======== Lock.prototype.hold per-lock stats (", registry.states.size(), " locks) ========");
        for (auto& state : registry.states) {
            uint64_t acquires = state->m_statAcquires.load(std::memory_order_relaxed);
            uint64_t spinIters = state->m_statSpinIters.load(std::memory_order_relaxed);
            uint64_t parks = state->m_statParks.load(std::memory_order_relaxed);
            totalAcquires += acquires;
            totalSpinIters += spinIters;
            totalParks += parks;
            if (parks > hottestParks) {
                hottestParks = parks;
                hottestId = state->m_statId;
            }
            // Suppress cold locks (acquired but never spun or parked) from the
            // per-line dump to keep K=128-shard output readable; they still
            // count toward the totals.
            if (!spinIters && !parks)
                continue;
            dataLogLn("[logJSLockContention]   lock#", state->m_statId,
                " acquires=", acquires,
                " spinIters=", spinIters,
                " parks=", parks,
                " spin/acq=", acquires ? static_cast<double>(spinIters) / static_cast<double>(acquires) : 0.0,
                " park/acq=", acquires ? static_cast<double>(parks) / static_cast<double>(acquires) : 0.0);
        }
    }
    dataLogLn("[logJSLockContention] TOTAL acquires=", totalAcquires,
        " spinIters=", totalSpinIters,
        " parks=", totalParks,
        " hottest=lock#", hottestId, " (", hottestParks, " parks)");
    dumpThreadAtomicsRMWStats();
}

// §25-T5 jsLockSpinLimit, made W-adaptive (charter S2(d)). The constant the
// in-tree T5 spin used (40) is the W<8 default; halved at W>=8, quartered at
// W>=16. Stored as the EFFECTIVE limit so the hot spin path is one relaxed
// load; refreshed by jsLockRefreshAdaptiveSpinLimit() only from the heavy
// slow path (where one rank-6-locked clientSet().size() is noise next to
// GILDroppedSection + ParkingLot). Process-global is correct: a single shared
// VM (5.2), and the limit is purely a heuristic — a slightly stale value
// never affects correctness (the spin's safety bail on jsThreadsStopPendingFor
// and the unchanged slow-path fall-through are what matter).
static constexpr unsigned jsLockSpinLimitBase = 40;
static std::atomic<unsigned> s_jsLockAdaptiveSpinLimit { jsLockSpinLimitBase };
static std::atomic<unsigned> s_jsLockSpinRefreshTick { 0 };

static void jsLockRefreshAdaptiveSpinLimit(VM& vm)
{
    // GIL-off only caller (the spin block is gilOff-gated). Throttled: the
    // refresh reads HeapClientSet::size(), which takes its rank-6 registry
    // lock — at W=16 with hot shard contention the slow path fires millions of
    // times, and an unthrottled global-lock read here would itself be a new
    // contention point. clientSet().size() only changes at thread spawn/exit
    // (rare relative to lock.hold), so sampling once per 256 slow-path entries
    // is plenty fresh; a stale limit is harmless (heuristic only — the spin's
    // safety bail and slow-path fall-through are unchanged). No other lock is
    // held here; we are about to release heap access and park.
    if (s_jsLockSpinRefreshTick.fetch_add(1, std::memory_order_relaxed) & 0xff)
        return;
    unsigned clients = vm.heap.clientSet().size();
    unsigned limit = jsLockSpinLimitBase;
    if (clients >= 16)
        limit = jsLockSpinLimitBase / 4;
    else if (clients >= 8)
        limit = jsLockSpinLimitBase / 2;
    s_jsLockAdaptiveSpinLimit.store(limit, std::memory_order_relaxed);
}

const ClassInfo JSLockObject::s_info = { "Lock"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSLockObject) };

static JSC_DECLARE_HOST_FUNCTION(callLock);
static JSC_DECLARE_HOST_FUNCTION(constructLock);
static JSC_DECLARE_HOST_FUNCTION(lockProtoFuncHold);
static JSC_DECLARE_HOST_FUNCTION(lockProtoFuncAsyncHold);
static JSC_DECLARE_CUSTOM_GETTER(lockLockedGetter);

JSLockObject::JSLockObject(VM& vm, Structure* structure)
    : Base(vm, structure)
    , m_state(NativeLockState::create())
{
}

JSLockObject* JSLockObject::create(VM& vm, Structure* structure)
{
    JSLockObject* object = new (NotNull, allocateCell<JSLockObject>(vm)) JSLockObject(vm, structure);
    object->finishCreation(vm);
    if (Options::logJSLockContention()) [[unlikely]] {
        // S2 instrumentation: register the state for the atexit dump. The
        // registry holds a Ref so the dump never reads freed memory; reachable
        // only with useJSThreads (Lock is not exposed otherwise), so flag-off
        // codegen is untouched.
        auto& registry = jsLockContentionRegistry();
        Locker locker { registry.lock };
        object->lockState().m_statId = registry.nextId++;
        registry.states.append(Ref { object->lockState() });
    }
    return object;
}

void JSLockObject::destroy(JSCell* cell)
{
    static_cast<JSLockObject*>(cell)->JSLockObject::~JSLockObject();
}

GILParkSavedExecutionState::GILParkSavedExecutionState(VM& vm)
    : m_vm(vm)
    , m_gilOff(vm.gilOff())
{
    if (m_gilOff) {
        // UNGIL §J.2 (U-T11): dead GIL-off. Group-3 execution state is
        // per-lite (§A.1.3), and — since obligation 10 landed the
        // exceptionScopeVerificationState() mode split — so is the
        // EXCEPTION_SCOPE_VERIFICATION scope chain, making the "per-lite
        // words" premise of this early return TRUE for every word this
        // class would otherwise save: no other mutator can clobber this
        // thread's topCallFrame/topEntryFrame/exception-scope words while
        // it parks, so there is nothing to save (see the keying rationale
        // on the member declaration, LockObject.h). A save/restore through
        // the raw VM-block members would be a data race shared by every
        // concurrently parking spawned thread. Park-site asserts move to the token
        // meaning (JSLock.cpp IU row 28): a spawned thread holds an entry
        // token; a main/embedder carrier still holds m_lock here, which
        // satisfies the token predicate too.
        ASSERT(vm.currentThreadIsHoldingAPILock());
#if ASSERT_ENABLED
        // Premise check (reviewer amendment): the raw VM-block words must be
        // UNCHANGED across the park — main carrier: its own per-lite live
        // words, untouched while it parks; spawned: GIL-off "inert spare
        // storage" nobody writes (all writers route per-lite). Capture for
        // the dtor's equality assert; reads of never-written (or
        // self-owned) words race with nothing.
        m_topCallFrame = vm.topCallFrame;
        m_topEntryFrame = vm.topEntryFrame;
#endif
        return;
    }
    ASSERT(vm.apiLock().currentThreadIsHoldingLock());
    m_topCallFrame = vm.topCallFrame;
    m_topEntryFrame = vm.topEntryFrame;
#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    // Obligation 10: GIL-on arm only (GIL-off took the §J.2 early return
    // above). The accessor resolves the VM copy here — bit-identical to the
    // former raw members.
    m_topExceptionScope = vm.exceptionScopeVerificationState().m_topExceptionScope;
    m_needExceptionCheck = vm.exceptionScopeVerificationState().m_needExceptionCheck;
#endif
}

GILParkSavedExecutionState::~GILParkSavedExecutionState()
{
    if (m_gilOff) {
        // §J.2: nothing was saved; this thread's per-lite state survived the
        // park untouched. Token-meaning assert (runs AFTER ~GILDroppedSection's
        // body re-established the entered state — member dtors run last).
        ASSERT(m_vm.currentThreadIsHoldingAPILock());
#if ASSERT_ENABLED
        // The "unchanged across the park" premise the skip rests on.
        ASSERT(m_vm.topCallFrame == m_topCallFrame);
        ASSERT(m_vm.topEntryFrame == m_topEntryFrame);
#endif
        return;
    }
    ASSERT(m_vm.apiLock().currentThreadIsHoldingLock());
    m_vm.topCallFrame = m_topCallFrame;
    m_vm.topEntryFrame = m_topEntryFrame;
#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    // Obligation 10: GIL-on arm only (see ctor).
    m_vm.exceptionScopeVerificationState().m_topExceptionScope = m_topExceptionScope;
    m_vm.exceptionScopeVerificationState().m_needExceptionCheck = m_needExceptionCheck;
#endif
}

void GILParkSavedExecutionState::resetForFreshThread(VM& vm)
{
    if (vm.gilOff()) {
        // IU row 28 (§J.2): token meaning. GIL-off a fresh thread's Group-3
        // execution state is its own lite's, clean at lite creation
        // (§A.1.3); writing the raw VM-block spare storage from a spawned
        // thread would be a cross-thread store into words other parkers
        // concurrently read. Nothing to reset.
        ASSERT(vm.currentThreadIsHoldingAPILock());
        return;
    }
    ASSERT(vm.apiLock().currentThreadIsHoldingLock());
    vm.topCallFrame = nullptr;
    vm.topEntryFrame = nullptr;
#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    // Obligation 10: GIL-on arm only (GIL-off early-returned above — a
    // fresh GIL-off thread's verification state is its own lite's,
    // zero-initialized at lite creation).
    vm.exceptionScopeVerificationState().m_topExceptionScope = nullptr;
    vm.exceptionScopeVerificationState().m_needExceptionCheck = false;
#endif
}

// UNGIL §J.3 spawned arm (U-T11; the JSLock.cpp unlockAllForThreadParking
// "ORDERING CONSTRAINT" / AB-13 split, now landed): a spawned thread GIL-off
// NEVER owns m_lock (JSLock's token-only entry arm), so a park site has no
// GIL to drop. The §J.3 release is "access release + §A.3 park cooperation +
// §A.3.2b post-wake poll" — which is EXACTLY the §F.4/ANNEX DAL2 heap-access
// bracket: enter releases the client's heap access (a §10.4 GC barrier /
// §A.3.2 conductor never waits on a parked thread); exit re-acquires
// §A.3.2b/§A.3.8-gated (parks across an in-flight stop-the-world) and then
// polls the lite's deferred trap bits before returning to JS. Token, entry
// depth, sp/lastStackTop, m_lockDropDepth are all untouched (per-lite /
// uninvolved), so the D1 LIFO-livelock shape cannot recur (0 locks dropped).
struct GILDroppedSectionSpawnedArm {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(GILDroppedSectionSpawnedArm);
    explicit GILDroppedSectionSpawnedArm(VM& vm)
        : bracket(vm)
    {
    }
    JSLock::DropAllLocks bracket;
    // T5-barrier-site (SCALEBENCH §32 RUN-3.7, t5verify): coop root-snapshot
    // STORAGE for the JS-level park span. Lives here (heap, via the
    // makeUnique<GILDroppedSectionSpawnedArm> in GILDroppedSection's ctor)
    // because the GILDroppedSection ctor's own stack frame RETURNS before the
    // caller actually parks — a stack-local CurrentThreadState there would be
    // dead memory by the time the conductor's gatherStackRoots reads it. The
    // GILDroppedSection OBJECT itself is the caller's RAII local and stays
    // live across the whole park, so `stackTop` is set to that address (see
    // ctor below); the struct + RegisterState bytes live here until the dtor
    // clears the snapshot and resets m_spawnedArm. alignas matches the
    // RegisterState.h jmp_buf-fallback ALLOCATE_AND_GET_REGISTER_STATE so the
    // consumer's roundUpToMultipleOf<sizeof(CPURegister)> end-pointer math is
    // sound regardless of which RegisterState definition is selected.
    CurrentThreadState parkedRootSnapshot;
    alignas(alignof(void*) > alignof(RegisterState) ? alignof(void*) : alignof(RegisterState)) RegisterState parkedRootRegisterState;
};

// Defined in heap/Heap.cpp (T5-rootscan-skip-coop-parked-suspend): publish /
// clear the cooperative root snapshot bracketing each pure-park span. Same
// forward-decl shape as VMManager.cpp's existing T5 sites; kept out of Heap.h
// so flag-off TUs that never reach a publish site see no symbol.
void gcClientPublishParkedRootSnapshot(CurrentThreadState*);
void gcClientClearParkedRootSnapshot();

GILDroppedSection::GILDroppedSection(VM& vm)
    : m_vm(vm)
    , m_savedExecutionState(vm)
    , m_stackPointerAtVMEntry(vm.stackPointerAtVMEntry())
{
    // UNGIL §J.3 (U-T11): GIL-off-by-caller split. The predicate is
    // textually identical to JSLock::DropAllLocks' own GIL-off arm
    // (JSLock.cpp), so the outer split and the inner bracket can never
    // disagree. (m_savedExecutionState keys on gilOff ALONE — see its
    // declaration comment for why that is sound for gilOff MAIN carriers
    // too.)
    if (vm.gilOff() && ThreadManager::isJSThreadCurrent()) [[unlikely]] {
        // Spawned arm: token-only (see GILDroppedSectionSpawnedArm above).
        // Reaching unlockAllForThreadParking here would trip its AB-13
        // RELEASE_ASSERT (a spawned GIL-off thread never holds m_lock).
        m_spawnedArm = makeUnique<GILDroppedSectionSpawnedArm>(vm);
        // T5-barrier-site (SCALEBENCH §32 RUN-3.7, t5verify pc1+pc2
        // coopParked=0/16 accessReleased=16/16 on every one of 31 stops →
        // 465 wasted SIGUSR2 suspend/resume round-trips). EVERY JS-level park
        // — Condition.wait, contended Lock.hold, Thread.join, property/SAB
        // Atomics.wait, jsThreadGILHandoffYield — funnels through THIS
        // spawned-arm branch and lands in spawnedDropAllLocksBracketEnter
        // (JSLock.cpp), which does releaseHeapAccess() ONLY: the sibling
        // counts access-released for the §10.4 barrier but never publishes a
        // coop root snapshot, so the conductor's gatherStackRoots falls back
        // to suspend() for it. The earlier T5 wiring covered only
        // safepoint/stop-protocol parks (JSThreadsSafepoint / VMManager
        // notifyVMStop / Heap F8 acquire), all of which require a GC ALREADY
        // in flight; a sibling parked at a JS Condition BEFORE the GC starts
        // never reaches them. Publish HERE, at the single common wire-point.
        //
        // ORDERING (matches the protocol comment at Heap.cpp gatherStackRoots
        // / GCClient::Heap::publishParkedRootSnapshot in Heap.h): the
        // SpawnedArm ctor above has just run the DAL2 bracket's seq_cst
        // releaseHeapAccess(); the seq_cst snapshot store below is the
        // Dekker-paired publish. The dtor clears seq_cst BEFORE re-acquiring
        // access (before m_spawnedArm = nullptr → spawnedDropAllLocksBracketExit
        // → acquireHeapAccess), so a conductor that re-loads the snapshot at
        // use time (Heap.cpp coopParkedSnapshotLookup) sees either the
        // still-valid heap-backed struct or null (falls back to suspend).
        //
        // STORAGE: CurrentThreadState + RegisterState live in the
        // heap-allocated m_spawnedArm (see GILDroppedSectionSpawnedArm above)
        // so they outlive this ctor frame; `stackTop` is `this` — the
        // GILDroppedSection object sits in the PARK-SITE CALLER's stack frame
        // (the deepest frame guaranteed live across the whole park) and the
        // span is pure-park (no HelperDrain entry, no JSCell* below it: only
        // ParkingLot / futex / condvar machinery), so [this, stackOrigin] is
        // an at-least-as-conservative superset of the thread's JS roots
        // (tryCopyCooperativelyParkedThreadStack deliberately does NOT extend
        // by the OS red-zone for the same reason). Callee-saves are spilled
        // into a local via ALLOCATE_AND_GET_REGISTER_STATE (the per-arch asm
        // macro DECLARES its destination, so it cannot target a member
        // directly) and then byte-copied into the heap-backed member; the
        // values are plain register words at the spill instant — copying them
        // is sound for conservative scanning, including the jmp_buf fallback
        // (never longjmp'd, just scanned).
        //
        // Flag-off / W=1: the enclosing predicate is `vm.gilOff() &&
        // ThreadManager::isJSThreadCurrent()` — a SPAWNED GIL-off JS thread.
        // Its existence implies main + ≥1 spawned client (clients ≥ 2), so
        // this site structurally satisfies the W≥2-only / not-the-conductor
        // gate the gcClientPublishParkedRootSnapshot contract (Heap.cpp)
        // requires; flag-off and W=1 never enter this [[unlikely]] block at
        // all. No clientSet().size() probe (would take the rank-6 registry
        // lock on every JS park).
        ALLOCATE_AND_GET_REGISTER_STATE(t5SpilledCalleeSaves);
        memcpy(&m_spawnedArm->parkedRootRegisterState, &t5SpilledCalleeSaves, sizeof(RegisterState));
        m_spawnedArm->parkedRootSnapshot.stackOrigin = Thread::currentSingleton().stack().origin();
        m_spawnedArm->parkedRootSnapshot.stackTop = static_cast<void*>(this);
        m_spawnedArm->parkedRootSnapshot.registerState = &m_spawnedArm->parkedRootRegisterState;
        gcClientPublishParkedRootSnapshot(&m_spawnedArm->parkedRootSnapshot);
        return;
    }
    // Main/embedder carriers (GIL-off) and every GIL-on caller: m_lock is
    // genuinely held; full §J.3 release (m_lock + token, drain suppressed,
    // park-record push for the W1 watchdog episode happens inside).
    JSLock& apiLock = vm.apiLock();
    ASSERT(apiLock.currentThreadIsHoldingLock());
    // 9.2-9: depth-suppressed release — no microtask drain at park sites
    // (the D11 fix); see JSLock::unlockAllForThreadParking.
    m_lockCount = apiLock.unlockAllForThreadParking();
}

GILDroppedSection::~GILDroppedSection()
{
    if (m_spawnedArm) [[unlikely]] {
        // T5-barrier-site: clear the coop root snapshot seq_cst BEFORE the
        // DAL2 bracket exit re-acquires heap access below (m_spawnedArm =
        // nullptr → ~GILDroppedSectionSpawnedArm → ~DropAllLocks →
        // spawnedDropAllLocksBracketExit → acquireHeapAccess). Once access is
        // re-acquired the thread is a live mutator again and MUST be a
        // suspend() target; the conductor's seq_cst re-load at use time
        // (Heap.cpp gatherStackRoots coopParkedSnapshotLookup) sees null and
        // falls back. Idempotent vs gcClientDidResumeFromThreadGranularStop's
        // own clear. Runs before the storage in m_spawnedArm is freed, so the
        // published pointer is never observed dangling.
        gcClientClearParkedRootSnapshot();
        // §J.3 spawned exit: close the DAL2 bracket NOW (the §A.3.2b/§A.3.8-
        // gated heap-access re-acquire — may park across an in-flight
        // stop-the-world — then the deferred lite trap poll), explicitly
        // rather than via the member dtor so the poll's effects can be
        // normalized below before control returns to the park site.
        m_spawnedArm = nullptr;
        // The bracket-exit trap poll (spawnedDropAllLocksBracketExit,
        // JSLock.cpp) runs handleTrapsIfNeeded, which can INSTALL the
        // termination exception inside this host call. Park-site epilogues
        // were written under the recorded premise "only trap BITS were
        // raised while we slept" (request-then-throw), and a value-return
        // with a pending termination exception trips their
        // EXCEPTION_SCOPE_VERIFICATION scopes. Convert the delivery back to
        // the bits+request form: the site's own
        // jsThreadParkTerminationRequested / parkLitePollTerminationRequested
        // check (every park epilogue runs one) or the caller's next trap
        // poll surfaces it — never lost, single canonical throw, no
        // double-throw.
        // (hasPendingTerminationException is the non-mutating per-lite read;
        // vm.exception() would flip this thread's m_needExceptionCheck
        // verification bit.)
        if (m_vm.hasPendingTerminationException()) [[unlikely]] {
            auto topScope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
            topScope.clearException();
            m_vm.setHasTerminationRequest();
            m_vm.traps().fireTrapVMWide(VMTraps::NeedTermination); // Idempotent OR; guarantees the next handleTraps re-delivers.
        }
        // Early return BEFORE the sp/lastStackTop restore below: for a
        // spawned thread mid-entry those are LIVE per-lite §A.1.4 slots
        // (token + entry depth untouched across the park); re-stamping
        // lastStackTop with this dtor frame would shrink conservative-scan
        // coverage of the thread's deeper live JS frames for the rest of the
        // entry — a GC-miss UAF. Nothing was unlocked (0 locks dropped), so
        // there is nothing to reacquire or restore.
        return;
    }

    JSLock& apiLock = m_vm.apiLock();
    for (unsigned i = 0; i < m_lockCount; ++i)
        apiLock.lock();

    // Same restoration grabAllLocks performs after its LIFO spin; the
    // intervening holders left THEIR stack-entry bookkeeping in the VM.
    // (m_savedExecutionState's destructor then restores
    // topCallFrame/topEntryFrame, running after this body.)
    m_vm.setStackPointerAtVMEntry(m_stackPointerAtVMEntry);
    m_vm.setLastStackTop(Thread::currentSingleton());
}

void jsThreadGILHandoffYield(VM& vm)
{
    // UNGIL IU row 29 (JSLock.cpp; §J.3/U-T11): the park full-release branch
    // moves to the TOKEN meaning for GIL-off spawned callers — mutex-literal
    // it no-ops (a spawned thread never owns m_lock), and the
    // jsThreadYieldForPendingParkResumptions loop below would busy-spin its
    // full deadline HOLDING heap access, stalling §A.3.2 conductor stops.
    // The spawned GILDroppedSection arm releases heap access for the yield.
    if (vm.gilOff() && ThreadManager::isJSThreadCurrent()) [[unlikely]] {
        if (!vm.currentThreadIsHoldingAPILock())
            return;
    } else if (!vm.apiLock().currentThreadIsHoldingLock())
        return;
    GILDroppedSection droppedSection(vm);
    Thread::yield();
}

// See the declaration comment (LockObject.h). Every increment is paired
// with exactly one decrement on the parker's resume path (all exits of the
// park scopes, including termination), so the counter cannot leak or
// underflow.
static std::atomic<unsigned> s_pendingParkResumptions { 0 };

void jsThreadNoteParkResumptionPending()
{
    s_pendingParkResumptions.fetch_add(1, std::memory_order_relaxed);
}

void jsThreadNoteParkResumptionDone()
{
    s_pendingParkResumptions.fetch_sub(1, std::memory_order_relaxed);
}

void jsThreadYieldForPendingParkResumptions(VM& vm)
{
    // Bounded, monotonic-progress wait: parkers normally resume within a
    // few milliseconds (1ms tryLock quanta + a GIL handoff), so the
    // deadline is pure slack against scheduler stalls. It must NOT be
    // unbounded: a parker can be pinned behind a holder that itself waits
    // on THIS thread's completion (e.g. join under hold), and waiting for
    // it here would deadlock — on deadline expiry we fall back to the
    // pre-existing drain-while-parked timing instead of hanging. Only a
    // DECREASE of the pending count is progress and resets the deadline:
    // the counter is process-global, so an increase (or mere churn) can
    // come from unrelated park traffic and must not extend the wait —
    // otherwise storm workloads (continuous contended-hold/notify churn)
    // could defer this thread's completion sequence unboundedly.
    static constexpr Seconds maxStall = Seconds::fromMilliseconds(500);
    unsigned lastPending = s_pendingParkResumptions.load(std::memory_order_acquire);
    MonotonicTime deadline = MonotonicTime::now() + maxStall;
    while (lastPending) {
        // AB-17 item 4: park-lite form, termination only — this caller is
        // ENTERED (it holds its token between handoff yields), so the
        // watchdog check bit is serviced at its regular trap polls, never
        // treated as termination. GIL-on byte-equivalent (folded form).
        VMLite* yieldParkLite = nullptr;
        if (vm.gilOff())
            yieldParkLite = ThreadManager::isJSThreadCurrent() ? VMLite::currentIfExists() : capturedParkLiteOfCurrentThreadIfAny(vm);
        if (parkLitePollTerminationRequested(vm, yieldParkLite))
            return;
        if (MonotonicTime::now() > deadline)
            return;
        jsThreadGILHandoffYield(vm);
        unsigned pending = s_pendingParkResumptions.load(std::memory_order_acquire);
        if (pending < lastPending)
            deadline = MonotonicTime::now() + maxStall;
        lastPending = pending;
    }
}

bool jsThreadParkTerminationRequested(VM& vm)
{
    // See the declaration comment (LockObject.h): hasTerminationRequest()
    // is only ever set by trap handling on an executing mutator, so a park
    // must read the trap bits themselves.
    if (vm.hasTerminationRequest())
        return true;
    return vm.traps().needHandling(VMTraps::NeedTermination | VMTraps::NeedWatchdogCheck);
}

// ---------------- T5 fair handoff (wake-on-release for sync lock.hold) ----------------

void NativeLockState::wakeOneSyncHoldParker()
{
    // Pairs with lockProtoFuncHold's contended path: the parker increments
    // m_syncHoldParkers (seq_cst) BEFORE its tryLock retries and parks on
    // &m_lock only after a ParkingLot-bucket-locked validation that m_lock is
    // still held. The seq_cst fence here orders the caller's preceding
    // m_lock.unlock() store before our counter load (Dekker shape: parker's
    // increment->tryLock vs releaser's unlock->load). Even if this check
    // races and skips the unpark, the parker's 1ms park timeout bounds the
    // miss to one pre-T5 poll quantum — correctness never depends on the wake.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (!m_syncHoldParkers.load(std::memory_order_relaxed)) [[likely]]
        return;
    WTF::ParkingLot::unparkOne(&m_lock);
}

// ---------------- NativeLockState pump machinery (SPEC-api 5.5a) ----------------

void NativeLockState::schedPumpLocked(VM& fallbackVM)
{
    if (m_pumpPending)
        return;
    m_pumpPending = true;
    // SPEC-api 5.5a schedPump: dispatch P on the HEAD ticket's vm.runLoop()
    // (G28), at most one pump per lock (m_pumpPending). In phase 1 there is
    // a single shared VM so this equals the caller's VM; the head-ticket
    // routing is what survives GIL removal (the pump must run on the run
    // loop able to grant the head acquirer). Both callers (R, A-failure)
    // only schedule with m_asyncWaiters non-empty.
    ASSERT(!m_asyncWaiters.isEmpty());
    VM& dispatchVM = m_asyncWaiters.isEmpty() ? fallbackVM : m_asyncWaiters.first()->vm();
    // Capture Ref<VM>, mirroring the D5 waitAsync-timer fix
    // (docs/threads/INTEGRATE-api.md "Landed deviations"): WTF::RunLoop is
    // independently ref-counted and outlives the VM, so this task can
    // otherwise run after embedder VM teardown — pump() -> settleLockGrant
    // -> AsyncTicket::settle() dereferences the ticket's VM
    // (deferredWorkTimer->scheduleWorkSoon). The Ref also forestalls DWT
    // shutdown cancellation racing settle()'s isCancelled() check:
    // cancelPendingWork(VM&) only runs during VM teardown (GC End phase /
    // ~VM), which cannot begin while this task pins the VM; and even a
    // ticket cancelled by other means is tolerated downstream
    // (DeferredWorkTimer::doWork drops cancelled tickets' tasks,
    // DeferredWorkTimer.cpp:121-124, destroying the wrapper — and its
    // promise Strong — under the JSLock).
    dispatchVM.runLoop().dispatch([state = Ref { *this }, protectedVM = Ref { dispatchVM }] {
        state->pump();
    });
}

void NativeLockState::releasePump(VM& vm)
{
    Locker queueLocker { m_queueLock };
    if (!m_asyncWaiters.isEmpty())
        schedPumpLocked(vm);
}

void NativeLockState::enqueueAsyncAcquirer(Ref<AsyncTicket>&& ticket, VM& vm)
{
    Locker queueLocker { m_queueLock };
    m_asyncWaiters.append(WTF::move(ticket));
    schedPumpLocked(vm);
}

void NativeLockState::asyncReleaseInternal(AsyncTicket& ticket, VM& vm)
{
    {
        Locker queueLocker { m_queueLock };
        ASSERT_UNUSED(ticket, m_asyncHolder == &ticket);
        m_asyncHolder = nullptr;
        m_asyncHeld.store(false, std::memory_order_release);
        // The grant is no longer live: a sync hold from the delivered fn's
        // remaining body is legal again (D10; see LockObject.h). Harmless
        // when the runner was never set (no-fn arm, release from another
        // thread).
        m_asyncGrantRunner.store(nullptr, std::memory_order_relaxed);
    }
    m_lock.unlock();
    // T5: wake a parked sync-hold contender immediately (covers the post-fn
    // implicit release E and cond.asyncWait's 4.3(b) consumption). Done
    // before releasePump so a parked SYNC contender gets first shot at the
    // freed lock — matching the pre-T5 ordering, where the parker's tryLock
    // (sub-ms) raced a pump that only runs on a later run-loop turn; the
    // FIFO contract is unchanged (only sync holds may barge, 4.2).
    wakeOneSyncHoldParker();
    releasePump(vm);
}

void NativeLockState::pump()
{
    {
        Locker queueLocker { m_queueLock };
        m_pumpPending = false; // clear-before-tryLock is normative (5.5a P)
    }
    if (!m_lock.tryLock())
        return; // holder's release will re-run R with pump-pending false
    RefPtr<AsyncTicket> grant;
    {
        Locker queueLocker { m_queueLock };
        if (m_asyncWaiters.isEmpty()) {
            // Unlock stays under m_queueLock (an enqueue interleaved between
            // the emptiness check and this unlock would otherwise strand its
            // ticket: its scheduled pump tryLocks against the lock we still
            // hold and relies on OUR release to re-run R). The T5 wake runs
            // after the locker drops — no need to hold m_queueLock across
            // the ParkingLot bucket lock.
            m_lock.unlock();
        } else {
            grant = m_asyncWaiters.takeFirst();
            m_asyncHeld.store(true, std::memory_order_release);
            m_asyncHolder = grant;
        }
    }
    if (!grant) {
        wakeOneSyncHoldParker(); // T5: the lock went free with no async grant.
        return;
    }
    settleLockGrant(*this, *grant);
}

// Settle a granted async acquisition. The settle task runs on a run-loop
// turn holding the JSLock.
void settleLockGrant(NativeLockState& state, AsyncTicket& ticket)
{
    Ref<NativeLockState> protectedState { state };
    Ref<AsyncTicket> protectedTicket { ticket };
    if (ticket.grantWithFunction) {
        JSObject* function = ticket.extraDependency();
        ticket.settle([state = WTF::move(protectedState), ticket = WTF::move(protectedTicket), function](DeferredWorkTimer::Ticket dwtTicket) {
            JSPromise* promise = uncheckedDowncast<JSPromise>(dwtTicket->target());
            JSGlobalObject* globalObject = promise->realm();
            VM& vm = globalObject->vm();
            // Delivery point: from here on the hold is observable by JS, so
            // cond.asyncWait's (b) arm may consume it (the delivered gate in
            // conditionProtoFuncAsyncWait guarantees the ticket could NOT
            // have been consumed before this line — fn therefore always
            // starts with the lock genuinely held; E's consumed-CAS failure
            // below can only mean fn itself gave the hold away, I23).
            ticket->markGrantDelivered();
            // D10 (LockObject.h m_asyncGrantRunner): while fn runs, a sync
            // hold on this lock from this thread must throw "Lock is not
            // recursive" instead of self-deadlocking in m_lock. Cleared by
            // asyncReleaseInternal — via E below, or earlier via
            // cond.asyncWait's 4.3(b) consumption inside fn (after which the
            // lock is free and a sync hold is legal again).
            state->m_asyncGrantRunner.store(&Thread::currentSingleton(), std::memory_order_relaxed);
            auto callData = JSC::getCallData(function);
            MarkedArgumentBuffer args;
            NakedPtr<Exception> exception;
            JSValue result = JSC::call(globalObject, function, callData, jsUndefined(), args, exception);
            // E: implicit post-fn release (unless cond.asyncWait consumed the hold).
            if (ticket->tryConsume())
                state->asyncReleaseInternal(ticket.get(), vm);
            if (exception)
                promise->reject(vm, exception->value());
            else
                promise->resolve(globalObject, vm, result);
        });
        return;
    }
    ticket.settle([state = WTF::move(protectedState), ticket = WTF::move(protectedTicket)](DeferredWorkTimer::Ticket dwtTicket) {
        JSPromise* promise = uncheckedDowncast<JSPromise>(dwtTicket->target());
        JSGlobalObject* globalObject = promise->realm();
        VM& vm = globalObject->vm();
        // Delivery point (see the with-fn arm): the resolved release fn is
        // minted against an unconsumed ticket, so its first call never
        // throws the 4.2 "called more than once" Error spuriously.
        ticket->markGrantDelivered();
        JSNativeStdFunction* releaseFunction = JSNativeStdFunction::create(vm, globalObject, 0, "release"_s, [state, ticket](JSGlobalObject* lexicalGlobalObject, CallFrame*) -> EncodedJSValue {
            VM& innerVM = lexicalGlobalObject->vm();
            auto scope = DECLARE_THROW_SCOPE(innerVM);
            if (!ticket->tryConsume())
                return throwVMError(lexicalGlobalObject, scope, createError(lexicalGlobalObject, "Lock release function called more than once"_s));
            state->asyncReleaseInternal(ticket.get(), innerVM);
            return JSValue::encode(jsUndefined());
        });
        promise->resolve(globalObject, vm, releaseFunction);
    });
}

// ---------------- host functions ----------------

JSC_DEFINE_HOST_FUNCTION(callLock, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return throwVMTypeError(globalObject, scope, "calling Lock constructor without new is invalid"_s);
}

JSC_DEFINE_HOST_FUNCTION(constructLock, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // SCALEBENCH §37 R1: use the per-realm cached instance Structure (set in
    // createLockProperty). The constructor's "prototype" is ReadOnly|DontDelete
    // (linkConstructorAndPrototype-style install below), so re-reading it via
    // jsCallee()->get(prototype) — the old per-call shape — is observably
    // identical to this cache; the difference is one StructureID instead of one
    // per `new Lock()`, which keeps the bench's K=128 shard-lock `.hold` GetById
    // monomorphic instead of permanently GaveUp. Subclassing goes through
    // InternalFunction::createSubclassStructure so `class L extends Lock {}`
    // gets the right prototype (the old code never honored newTarget at all —
    // this is the standard host-constructor pattern). callFrame->newTarget() is
    // always an object inside a [[Construct]] host function. Flag-off this
    // function is unreachable (only installed under useJSThreads).
    Structure* structure = globalObject->lockObjectStructure();
    ASSERT(structure);
    JSValue newTarget = callFrame->newTarget();
    if (newTarget != callFrame->jsCallee()) [[unlikely]] {
        structure = InternalFunction::createSubclassStructure(globalObject, asObject(newTarget), structure);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(JSLockObject::create(vm, structure));
}

JSC_DEFINE_HOST_FUNCTION(lockProtoFuncHold, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSLockObject* lockObject = dynamicDowncast<JSLockObject>(callFrame->thisValue());
    if (!lockObject)
        return throwVMTypeError(globalObject, scope, "Lock.prototype.hold called on incompatible receiver"_s);
    JSValue functionValue = callFrame->argument(0);

    const bool gilOff = vm.gilOff();
    // hold-vmEntry-trampoline (SCALEBENCH §34 flat-gap): gilOff fast-callee
    // detection. The overwhelmingly common shape is `lock.hold(() => …)` — a
    // fresh JSFunction over ONE FunctionExecutable that compiles once and is
    // then re-entered millions of times via this host call. perf at W=16
    // attributes 10.7% self to executeCallImpl + 5.8% self here (16.5% of all
    // on-CPU samples), and the holdtime micro measures 117 ns per
    // lock.hold(()=>x++) vs 12.6 ns for an FTL-inlined pure-JS callFn — the
    // ~104 ns is the CallData→JSC::call→executeCallImpl→VMEntryScope/
    // DeferTraps/prepareForExecution/ProtoCallFrame/vmEntryToJavaScript
    // round-trip plus two extra ThrowScopes and a MarkedArgumentBuffer. For a
    // non-host JSFunction we (a) skip getCallData entirely (it is callable by
    // construction) and (b) after the lock is acquired, dispatch the body
    // through vmEntryToJavaScriptWith0Arguments against the executable's
    // already-installed codeBlockForCall — exactly the CachedCall /
    // MicrotaskCall::tryCallWithArguments fast shape (InterpreterInlines.h /
    // MicrotaskCallInlines.h), keyed on the FunctionExecutable instead of a
    // separate cache. Flag-off this whole block is dead (gilOff false):
    // fastFunction stays null and execution falls through byte-for-byte to the
    // pre-existing getCallData / JSC::call path below.
    JSFunction* fastFunction = nullptr;
#if (CPU(ARM64) || CPU(X86_64)) && CPU(ADDRESS64) && !ENABLE(C_LOOP)
    if (gilOff) {
        fastFunction = dynamicDowncast<JSFunction>(functionValue);
        if (fastFunction && fastFunction->isHostFunction()) [[unlikely]]
            fastFunction = nullptr;
    }
#endif
    CallData callData;
    if (!fastFunction) {
        callData = JSC::getCallData(functionValue);
        if (callData.type == CallData::Type::None)
            return throwVMTypeError(globalObject, scope, "Lock.prototype.hold requires a callable argument"_s);
    }

    NativeLockState& state = lockObject->lockState();
    // Recursion guard: a sync hold (m_holder) OR a live asyncHold(fn) grant
    // being delivered on this very thread (m_asyncGrantRunner, D10 — the
    // sync-in-async self-deadlock; see LockObject.h) both mean "held by the
    // current thread" for 4.2 purposes. Thread::currentSingleton() is hoisted
    // and reused for the m_holder store and the epilogue holder check — pure
    // refactor (the TLS lookup is idempotent), so flag-off observable
    // behaviour is identical; it just stops paying four TLS reads per hold.
    Thread* currentThread = &Thread::currentSingleton();
    if (state.m_holder.load(std::memory_order_relaxed) == currentThread
        || (state.m_asyncGrantRunner.load(std::memory_order_relaxed) == currentThread
            && state.m_asyncHeld.load(std::memory_order_acquire))) [[unlikely]]
        return throwVMError(globalObject, scope, createError(globalObject, "Lock is not recursive"_s));

    const bool logContention = Options::logJSLockContention();
    bool needsSlowPath = !state.m_lock.tryLock();
    if (needsSlowPath && gilOff) {
        // T5-jslock-adaptive-spin: textbook thin-lock briefly-contended fast
        // path. The uncontended tryLock above is the existing fast path; this
        // covers the next case down — the holder's critical section is shorter
        // than the contender's GILDroppedSection bookkeeping (release heap
        // access, possibly park for a pending stop, ParkingLot enqueue), so a
        // bounded spin acquires before the slow path would even finish its
        // prologue. Off-CPU sampling at W=16 attributes ~17% of parked time to
        // this function with K=128 shard locks averaging well under two
        // contenders, i.e. most parks are sub-microsecond races a spin wins.
        // GIL-on this is dead (gilOff() false): the contended path is rare and
        // dropping the GIL IS the point there, so flag-off behaviour is the
        // pre-spin code byte-for-byte. The genuinely-contended tail still
        // falls through to the unchanged m_syncHoldParkers/ParkingLot loop.
        //
        // Safety: we spin WITH heap access (no GILDroppedSection yet), so the
        // loop must not delay an STW rendezvous — bail to the slow path the
        // moment jsThreadsStopPendingFor observes a pending stop; the
        // GILDroppedSection there releases heap access and parks for it. The
        // spin is otherwise a pure tryLock retry: it never sets m_holder,
        // never touches m_syncHoldParkers, never opens the park-resumption
        // window — on success we fall straight through to the single
        // m_holder.store below exactly as the first-tryLock fast path does.
        // S2(d): W-adaptive bound — relaxed-loaded so the fast spin path adds
        // no global contention; refreshed only in the heavy slow path below.
        // gilOff-gated by the enclosing branch, so flag-off this block is dead
        // and jsLockSpinLimitBase (= the pre-S2 constant 40) is never even
        // consulted there.
        unsigned jsLockSpinLimit = s_jsLockAdaptiveSpinLimit.load(std::memory_order_relaxed);
        unsigned spin = 0;
        for (; spin < jsLockSpinLimit; ++spin) {
            if (jsThreadsStopPendingFor(vm)) [[unlikely]]
                break;
            WTF::spinLoopPause();
            if (state.m_lock.tryLock()) {
                needsSlowPath = false;
                break;
            }
        }
        if (logContention) [[unlikely]]
            state.m_statSpinIters.fetch_add(spin, std::memory_order_relaxed);
    }
    if (needsSlowPath) {
        // S2(d): refresh the cached W-adaptive spin limit here, where one
        // rank-6-locked HeapClientSet::size() read is noise relative to the
        // GILDroppedSection + ParkingLot work that follows. gilOff-only (the
        // spin block this feeds is gilOff-gated; flag-off never reads the
        // cache, so flag-off behavior is byte-identical regardless).
        if (gilOff)
            jsLockRefreshAdaptiveSpinLimit(vm);
        if (!jsThreadsCanBlockOnCurrentThread(vm))
            return throwVMTypeError(globalObject, scope, "Lock.prototype.hold cannot block the current thread"_s);
        // SPEC-api 5.3: contended path. Drop the GIL (depth-free; see
        // GILDroppedSection — never block on m_lock while holding the GIL:
        // the holder needs the GIL to run fn and release), then park in
        // m_lock. The GILDroppedSection destructor reacquires the GIL WITH
        // m_lock held — the one permitted rank-4-leaf shape of 5.9(e).
        // Deadlock-free: every contender blocks on m_lock only after fully
        // releasing the GIL, and the holder releases m_lock under the GIL
        // in its hold epilogue (or pump/release path), so the woken
        // acquirer's GIL reacquisition never waits on a thread that needs
        // m_lock. (Unlike DropAllLocks, GILDroppedSection has no
        // strict-LIFO unwind, so carrying m_lock across the reacquire is
        // safe; see the rationale in LockObject.h.)
        //
        // D9 (docs/threads/INTEGRATE-api.md "Landed deviations"): park in
        // bounded tryLock + timed-park quanta (1ms max), polling the
        // termination predicate between quanta — VMTraps cannot wake a
        // thread blocked in WTF::Lock::lock(), so an unbounded park here is
        // unkillable under the watchdog when the holder can never release
        // (e.g. an asyncHold grant whose release fn is never delivered, or a
        // holder that was itself terminated). Mirrors the mandatory 5.6-4
        // property-wait termination poll. T5 layers wake-on-release on top:
        // each quantum is a ParkingLot park that the unlock paths cut short,
        // so the bounded quantum is only the poll backstop, not the handoff
        // latency.
        bool acquired = false;
        // Open this parker's park->resumed window (see
        // jsThreadNoteParkResumptionPending, LockObject.h): a completing
        // thread must not run its 4.6.1 microtask drain between the
        // holder's release and this parker's resume
        // (park-no-microtask-drain.js, contended-hold block).
        jsThreadNoteParkResumptionPending();
        // T5 fair handoff: register as a wake-on-release parker BEFORE the
        // first retry (seq_cst — see wakeOneSyncHoldParker's fence pairing),
        // so every release from here on either sees the counter and unparks
        // us, or completed its unlock before our registration — in which
        // case the next tryLock (or the bucket-locked park validation)
        // observes the free lock and never sleeps. Decremented on every loop
        // exit below, mirroring the jsThreadNoteParkResumptionDone pairing.
        state.m_syncHoldParkers.fetch_add(1, std::memory_order_seq_cst);
        {
            GILDroppedSection droppedSection(vm);
            // UNGIL §A.2.4 rule 4 / annex W W1 (AB-17 item 4): GIL-off the
            // D9 poll re-points at the polling thread's PARK lite (spawned =
            // CURRENT lite; carriers = the §J.3-captured lite), and the
            // watchdog-check bit is split out of "termination": a parked
            // CARRIER observing it runs the full W1 reacquire-service-
            // re-release episode (terminating only on a terminate verdict)
            // instead of failing the park. GIL-on: byte-equivalent to the
            // landed jsThreadParkTerminationRequested loop.
            const bool isSpawnedParker = gilOff && ThreadManager::isJSThreadCurrent();
            VMLite* parkLite = nullptr;
            if (gilOff)
                parkLite = isSpawnedParker ? VMLite::currentIfExists() : capturedParkLiteOfCurrentThreadIfAny(vm);
            // NOT WTF::Lock::tryLockWithTimeout: that helper is a
            // signal-handler-safe PROBE (for MachineStackMarker) — it sleeps
            // in whole-second quanta and returns isHeld(), i.e. "held by
            // ANYONE", not "acquired by me". Under contention it returns
            // true while another holder (e.g. an undelivered asyncHold
            // grant) still owns m_lock, so this hold would steal the lock
            // and its epilogue would unlock a lock it never acquired — the
            // WTF::Lock double-release abort ("Invalid value for lock: 0")
            // and every corruption downstream of two believed holders. Park
            // in honest tryLock + bounded timed-park quanta instead (T5:
            // ParkingLot parks that release unparks immediately).
            while (!(acquired = state.m_lock.tryLock())) {
                if (parkLitePollTerminationRequested(vm, parkLite))
                    break;
                if (gilOff && !isSpawnedParker && parkLitePollWatchdogCheckRequested(vm, parkLite)) [[unlikely]] {
                    // W1 episode: no native lock held here (tryLock failed,
                    // GIL fully released by the dropped section). A
                    // terminate verdict raised VM-wide termination — the
                    // poll above observes it on the next iteration's check;
                    // break directly to the terminated epilogue.
                    if (reacquireParkedCarrierAndServiceWatchdogCheck(vm))
                        break;
                    continue;
                }
                // T5 fair handoff: park on this lock's ParkingLot queue
                // instead of a blind 1ms sleep. The release paths
                // (releaseSyncHold / asyncReleaseInternal / pump) unpark one
                // waiter immediately after m_lock.unlock(), so the common
                // handoff costs a wakeup, not up-to-1ms of idle, and waiters
                // are served in ParkingLot FIFO order. The honest-tryLock
                // structure is UNCHANGED: parking never acquires m_lock — we
                // only ever own it through the tryLock above, so the
                // double-release shape the tryLockWithTimeout comment warns
                // about cannot occur. The 1ms timeout keeps the exact D9
                // poll cadence as a backstop: termination and the W1
                // watchdog-check episode are still observed within ~1ms even
                // if a wake is lost or a release site bypasses the helpers,
                // and the bucket-locked validation (m_lock still held)
                // closes the failed-tryLock-then-release window — a release
                // that beats our enqueue makes validation fail and we retry
                // immediately rather than sleeping through a free lock.
                if (logContention) [[unlikely]]
                    state.m_statParks.fetch_add(1, std::memory_order_relaxed);
                WTF::ParkingLot::parkConditionally(
                    &state.m_lock,
                    [&] { return state.m_lock.isHeld(); },
                    [] { },
                    MonotonicTime::now() + Seconds::fromMilliseconds(1));
            }
        }
        // T5: deregister on every exit (acquired or terminated); pairs with
        // the fetch_add above so the releaser-side counter check stays exact.
        state.m_syncHoldParkers.fetch_sub(1, std::memory_order_seq_cst);
        // Back under the GIL: close the window on every exit (acquired or
        // terminated) — the pairing invariant keeps the global count exact.
        jsThreadNoteParkResumptionDone();
        if (!acquired) {
            // Termination observed while parked: GIL is reacquired (the
            // dropped section ended), m_lock is NOT held. Same surfacing as
            // the 5.6-7 property-wait path, in the handleTraps shape
            // (request-then-throw: throwTerminationException ASSERTs the
            // request flag, which a parked thread never had set — only trap
            // BITS were raised while we slept).
            vm.setHasTerminationRequest();
            vm.throwTerminationException();
            return { };
        }
    }
    state.m_holder.store(currentThread, std::memory_order_relaxed);
    if (logContention) [[unlikely]]
        state.m_statAcquires.fetch_add(1, std::memory_order_relaxed);

#if (CPU(ARM64) || CPU(X86_64)) && CPU(ADDRESS64) && !ENABLE(C_LOOP)
    // hold-vmEntry-trampoline fast dispatch (gilOff only — see the prologue
    // comment): when the closure's FunctionExecutable already has a compiled
    // call CodeBlock and declares no formal parameters (numParameters == 1
    // for the implicit |this|; the 0-arg entry thunk does no arity fixup),
    // call straight through vmEntryToJavaScriptWith0Arguments. This is the
    // CachedCall / MicrotaskCall::tryCallWithArguments shape minus the cache
    // object: codeBlockForCall() is itself the warm-state cache (one
    // FunctionExecutable per closure literal, written once by installCode and
    // re-read here every iteration). We are inside a host call from JS, so
    // vm.entryScope is already set (no VMEntryScope needed) and the thunk's
    // own soft-stack check covers recursion — exactly the invariants the
    // CachedCall path relies on. ANNEX CBI item 3: gilOff derives the entry
    // address THROUGH the one codeBlock snapshot so the (entry, codeBlock)
    // pair is matched by construction against a concurrent installCode/tier-up;
    // the snapshot is conservatively-live on this stack and its JITCode is
    // pinned by the codeBlock, so no DeferTraps is needed for the
    // already-compiled fast path.
    //
    // The epilogue (releaseSyncHold/releasePump) is exception-agnostic C++
    // (WTF::Lock, ParkingLot, an m_queueLock-guarded Deque::isEmpty), so it
    // runs identically with a pending VM exception; we surface the exception
    // to the caller after the lock is released, matching the slow path's
    // release-then-propagate ordering.
    //
    // Cold misses (first call before LLInt install, or a closure that declares
    // parameters) fall through to the unchanged JSC::call path with callData
    // populated lazily.
    if (fastFunction) {
        FunctionCodeBlock* codeBlock = fastFunction->jsExecutable()->codeBlockForCall();
        if (codeBlock && codeBlock->numParameters() <= 1) [[likely]] {
            // SCALEBENCH §36 jitcode-refptr-bounce-14pct: this is the 4.68 M
            // call/run hot path; perf annotate attributes ~37% of
            // lockProtoFuncHold's 11.9% W=16 self time to the `lock incl/decl`
            // pair from the by-value jitCode(). codeBlock is the snapshot we
            // already hold conservatively-live (ANNEX CBI item 3 above); raw
            // read is the same pointer. gilOff-only path so flag-off codegen
            // is unchanged.
            void* entry = codeBlock->jitCodeRawPtr()->addressForCall();
            EncodedJSValue encodedResult = vmEntryToJavaScriptWith0Arguments(entry, &vm, codeBlock, fastFunction, jsUndefined(), nullptr);
            // Epilogue guard: cond.asyncWait may have consumed the hold (4.3(a)).
            if (state.m_holder.load(std::memory_order_relaxed) == currentThread) {
                state.releaseSyncHold();
                state.releasePump(vm);
            }
            RETURN_IF_EXCEPTION(scope, { });
            return encodedResult;
        }
        callData = JSC::getCallData(functionValue);
    }
#endif

    MarkedArgumentBuffer args;
    NakedPtr<Exception> exception;
    JSValue result = JSC::call(globalObject, functionValue, callData, jsUndefined(), args, exception);

    // Epilogue guard: cond.asyncWait may have consumed the hold (4.3(a)).
    if (state.m_holder.load(std::memory_order_relaxed) == currentThread) {
        state.releaseSyncHold();
        state.releasePump(vm);
    }

    if (exception) {
        throwException(globalObject, scope, exception.get());
        return { };
    }
    return JSValue::encode(result);
}

JSC_DEFINE_HOST_FUNCTION(lockProtoFuncAsyncHold, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSLockObject* lockObject = dynamicDowncast<JSLockObject>(callFrame->thisValue());
    if (!lockObject)
        return throwVMTypeError(globalObject, scope, "Lock.prototype.asyncHold called on incompatible receiver"_s);

    JSValue functionValue = callFrame->argument(0);
    bool hasFunction = !functionValue.isUndefined();
    if (hasFunction) {
        auto callData = JSC::getCallData(functionValue);
        if (callData.type == CallData::Type::None)
            return throwVMTypeError(globalObject, scope, "Lock.prototype.asyncHold requires a callable argument when one is provided"_s);
    }

    NativeLockState& state = lockObject->lockState();
    // Sync-hold check only, deliberately NOT asyncGrantRunByCurrentThread()
    // (D10): per the frozen 4.2 text "async-held is NOT recur (callers
    // queue)" — an asyncHold from inside a delivered fn queues a ticket
    // that the post-fn implicit release (E) lets the pump grant later; no
    // deadlock, so no Error.
    if (state.heldByCurrentThread())
        return throwVMError(globalObject, scope, createError(globalObject, "Lock is not recursive"_s));

    JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
    Vector<JSCell*> dependencies;
    if (hasFunction)
        dependencies.append(asObject(functionValue));
    dependencies.append(lockObject);
    Ref<AsyncTicket> ticket = AsyncTicket::create(globalObject, promise, WTF::move(dependencies));
    ticket->grantWithFunction = hasFunction;

    // A: try to acquire immediately; otherwise queue FIFO. The immediate
    // grant is only legal when no async acquirer is already queued:
    // m_asyncWaiters can be non-empty while m_lock is transiently free (a
    // release already ran R, but the scheduled pump only runs on a run-loop
    // turn), and a tryLock here would barge an UNDELIVERED grant ahead of
    // the FIFO head (violating the 4.2 FIFO contract — async tickets are
    // FIFO; only SYNC holds may barge, api/lock-async-hold.js test 5; the
    // forcing case is api/condition-async-wait.js's final block). Worse,
    // that undelivered grant pins m_lock for the rest of the synchronous
    // turn, so a thread parked in a contended sync hold can only be freed
    // by run-loop turns the main thread may never reach — the
    // api/condition-async-wait.js sync+async rendezvous deadlock.
    // Emptiness check and tryLock are done under m_queueLock so no acquirer
    // can be enqueued in between (taking rank-4 m_lock under rank-3
    // m_queueLock follows rank order, 5.9; tryLock never blocks, so the
    // pump's m_lock-then-queueLock shape cannot deadlock against this).
    bool acquiredImmediately = false;
    {
        Locker queueLocker { state.m_queueLock };
        if (state.m_asyncWaiters.isEmpty() && state.m_lock.tryLock()) {
            state.m_asyncHeld.store(true, std::memory_order_release);
            state.m_asyncHolder = ticket.ptr();
            acquiredImmediately = true;
        }
    }
    if (acquiredImmediately)
        settleLockGrant(state, ticket);
    else
        state.enqueueAsyncAcquirer(ticket.copyRef(), vm);

    return JSValue::encode(promise);
}

JSC_DEFINE_CUSTOM_GETTER(lockLockedGetter, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSLockObject* lockObject = dynamicDowncast<JSLockObject>(JSValue::decode(thisValue));
    if (!lockObject)
        return throwVMTypeError(globalObject, scope, "Lock.prototype.locked called on incompatible receiver"_s);
    NativeLockState& state = lockObject->lockState();
    return JSValue::encode(jsBoolean(state.m_lock.isLocked() || state.m_asyncHeld.load(std::memory_order_acquire)));
}

JSValue createLockProperty(VM& vm, JSObject* globalObjectArg)
{
    JSGlobalObject* globalObject = uncheckedDowncast<JSGlobalObject>(globalObjectArg);
    JSObject* prototype = constructEmptyObject(globalObject);
    prototype->putDirectNativeFunction(vm, globalObject, Identifier::fromString(vm, "hold"_s), 1, lockProtoFuncHold, ImplementationVisibility::Public, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));
    prototype->putDirectNativeFunction(vm, globalObject, Identifier::fromString(vm, "asyncHold"_s), 0, lockProtoFuncAsyncHold, ImplementationVisibility::Public, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));
    prototype->putDirectCustomAccessor(vm, Identifier::fromString(vm, "locked"_s), CustomGetterSetter::create(vm, lockLockedGetter, nullptr), static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::CustomAccessor | PropertyAttribute::ReadOnly));

    // SCALEBENCH §37 R1: stamp the per-realm instance Structure once, here,
    // against the canonical prototype just built. constructLock reads this
    // instead of minting a fresh Structure per `new Lock()` (which made every
    // shard lock its own StructureID and forced the §37 4.45%
    // operationGetByIdGaveUp slow-path at the `.hold` call site). This runs
    // only inside the `if (Options::useJSThreads())` block in
    // JSGlobalObject::init — flag-off the slot stays null and untouched.
    globalObject->setLockObjectStructure(vm, JSLockObject::createStructure(vm, globalObject, prototype));

    JSFunction* constructor = JSFunction::create(vm, globalObject, 0, "Lock"_s, callLock, ImplementationVisibility::Public, NoIntrinsic, constructLock);
    constructor->putDirect(vm, vm.propertyNames->prototype, prototype, static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly));
    prototype->putDirect(vm, vm.propertyNames->constructor, constructor, static_cast<unsigned>(PropertyAttribute::DontEnum));
    prototype->putDirect(vm, vm.propertyNames->toStringTagSymbol, jsNontrivialString(vm, "Lock"_s), static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::ReadOnly));
    return constructor;
}

} // namespace JSC
