/*
 * Copyright (C) 2025 Apple Inc. All rights reserved.
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
#include "VMManager.h"

#include "Heap.h" // UNGIL §A.3 (U-T5): Heap::JSThreadsStopScope (GCL bracket), GCClient::Heap access sampling.
#include "JSCConfig.h"
#include "JSLock.h"
#include "JSThreadsSafepoint.h" // UNGIL §A.3 (U-T5): stop watchdog (annex App. 5.6(d)).
#include "VM.h"
#include "VMEntryScopeInlines.h"
#include "VMLite.h" // UNGIL §A.3.1/EXIT1 (U-T5): the entered-thread set IS the lite registry.
#include "VMLiteShared.h"
#include "VMThreadContext.h"
#include "WasmDebugServerUtilities.h"
#include <atomic>
#include <cstdlib> // BUGHUNT instrumentation (getenv/atexit; NOT FOR LANDING).
#include <mutex> // BUGHUNT instrumentation (call_once; NOT FOR LANDING).
#include <wtf/DataLog.h> // V4 watchdog: arbitration-queue breadcrumb.
#include <wtf/HashMap.h>
#include <wtf/Locker.h>
#include <wtf/MonotonicTime.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/Scope.h>
#include <wtf/Threading.h>

namespace JSC {

VM* VMManager::s_recentVM { nullptr };

VMManager& VMManager::singleton()
{
    static LazyNeverDestroyed<VMManager> manager;
    static std::once_flag onceKey;
    std::call_once(onceKey, [] {
        manager.construct();
    });
    return manager.get();
}

VMThreadContext::VMThreadContext() = default;
VMThreadContext::~VMThreadContext() = default;

bool VMManager::isValidVMSlow(VM* vm)
{
    bool found = false;
    forEachVM([&] (VM& nextVM) {
        if (vm == &nextVM) {
            s_recentVM = vm;
            found = true;
            return IterationStatus::Done;
        }
        return IterationStatus::Continue;
    });
    return found;
}

void VMManager::dumpVMs()
{
    unsigned i = 0;
    WTFLogAlways("Registered VMs:");
    forEachVM([&] (VM& nextVM) {
        WTFLogAlways("  [%u] VM %p", i++, &nextVM);
        return IterationStatus::Continue;
    });
}

void VMManager::iterateVMs(const Invocable<IterationStatus(VM&)> auto& functor) WTF_REQUIRES_LOCK(m_worldLock)
{
    for (auto* context = m_vmList.head(); context; context = context->next()) {
        VM& vm = *VM::fromThreadContext(context);
        IterationStatus status = functor(vm);
        if (status == IterationStatus::Done)
            return;
    }
}

VM* VMManager::findMatchingVMImpl(const ScopedLambda<VMManager::TestCallback>& test)
{
    Locker lock { m_worldLock };
    if (s_recentVM && test(*s_recentVM))
        return s_recentVM;

    VM* result = nullptr;
    iterateVMs(scopedLambda<IteratorCallback>([&] (VM& vm) {
        if (test(vm)) {
            result = &vm;
            s_recentVM = &vm;
            return IterationStatus::Done;
        }
        return IterationStatus::Continue;
    }));
    return result;
}

void VMManager::forEachVMImpl(const ScopedLambda<VMManager::IteratorCallback>& func)
{
    Locker lock { m_worldLock };
    iterateVMs(func);
}

VMManager::Error VMManager::forEachVMWithTimeoutImpl(Seconds timeout, const ScopedLambda<VMManager::IteratorCallback>& func)
{
    if (!m_worldLock.tryLockWithTimeout(timeout))
        return Error::TimedOut;

    Locker locker { AdoptLock, m_worldLock };
    iterateVMs(func);
    return Error::None;
}

void VMManager::Info::dump(PrintStream& out) const
{
    out.print("VMManager::Info(numberOfVMs:", numberOfVMs);
    out.print(", numberOfActiveVMs:", numberOfActiveVMs);
    out.print(", numberOfStoppedVMs:", numberOfStoppedVMs);
    out.print(", worldMode:", worldMode);
    out.print(", targetVM:", RawPointer(targetVM), ")");
}

auto VMManager::info() -> Info
{
    Info info;
    auto& manager = singleton();

    // The reason for locking here is so that we capture a consistent snapshot
    // of all the values in info.
    Locker lock { manager.m_worldLock };
    info.numberOfVMs = manager.m_numberOfVMs;
    info.numberOfActiveVMs = manager.m_numberOfActiveVMs;
    info.numberOfStoppedVMs = manager.m_numberOfStoppedVMs;
    info.worldMode = manager.m_worldMode;
    info.targetVM = manager.m_targetVM;
    return info;
}

void VMManager::setWasmDebuggerOnStop(StopTheWorldCallback callback)
{
    g_jscConfig.wasmDebuggerOnStop = callback;
}

void VMManager::setWasmDebuggerOnResume(PostResumeCallback callback)
{
    g_jscConfig.wasmDebuggerOnResume = callback;
}

void VMManager::setMemoryDebuggerCallback(StopTheWorldCallback callback)
{
    g_jscConfig.memoryDebuggerStopTheWorld = callback;
}

// THREADS-INTEGRATE(heap) manifest 5d (review round 4): file-local
// Atomic statics, deliberately NOT g_jscConfig slots — JSC::Config lives
// in the WTF::Config page that Config::finalize() (run from every VM
// constructor) mprotects read-only, and the sole installer
// (Heap::noteSharedServerSticky, second-client attach) always runs
// post-freeze; a config store would SIGSEGV at the ISS flip. seq_cst
// Atomic: the install happens-before the ISS flip publishes on the
// installing thread, and the hooks are inert (no-op unless ISS && GSP),
// so a load racing the install correctly no-ops.
static Atomic<void (*)(VM&)> s_gcWillParkInStopTheWorld { nullptr };
static Atomic<void (*)(VM&)> s_gcDidResumeFromStopTheWorld { nullptr };

void VMManager::setGCParkCallbacks(void (*willPark)(VM&), void (*didResume)(VM&))
{
    // Heap-owned hooks (JSC::Heap::gcWillParkInStopTheWorld /
    // gcDidResumeFromStopTheWorld); may be null (inert).
    s_gcWillParkInStopTheWorld.store(willPark);
    s_gcDidResumeFromStopTheWorld.store(didResume);
}

#if USE(BUN_JSC_ADDITIONS)
void VMManager::setJSDebuggerCallback(StopTheWorldCallback callback)
{
    g_jscConfig.jsDebuggerStopTheWorld = callback;
}
#endif

// ============================================================================
// UNGIL §A.3 (U-T5): thread-granular stop-the-world for the gilOff VM.
//
// Re-freezes jit R1.c "N threads in ONE VM = thread-granular STW", both
// sides. The counting unit is the ENTERED THREAD (§A.3.1): the entered set
// IS the VMLiteRegistry filtered lite->vm == target (EXIT1.1) — there is no
// second entered-thread structure. Every conductor predicate sample RE-WALKS
// the registry under VMLiteRegistry::lock (EXIT1.2); lite/client pointers
// are never cached across samples; the walk is allocation-free and acquires
// nothing; the registry lock is dropped before the conductor blocks between
// samples.
//
// Conductor order (ANNEX HBT4, normative for ALL §A.3 conductors):
//   release access (R1.i's first step KEPT) -> §A.3.3 arbitration on the
//   park-aware pending-job-slot mutex -> (WINNER ONLY)
//   Heap::JSThreadsStopScope (GCL) -> fan stop bits -> stop -> work ->
//   resume -> drop scope -> re-acquire access.
// Losers park on the job-slot mutex access-released (they count as parked
// for the winner's predicate through the access sample) and never block raw
// on GCL (HBT4.3: at most one thread — the winner — ever blocks in
// GCL.lock(), access-released).
//
// §LK row 4b — s_jsThreadsJobSlotLock: §A.3-conductors ONLY; inner to
// rank 1/token (the requester holds its entry token entering arbitration;
// tokens are ordering-inert, LK.1); OUTER to heap rank 2 (GCL); held across
// the ENTIRE stop window; never held together with any api rank-1..3 lock.
//
// Deviation recorded (§A.3.1's "m_worldLock held for the window"): conductor
// tenure and window serialization are carried by the job-slot mutex per
// HBT4's reordering — m_worldLock is NOT held across the window (holding it
// would block every notifyVMStop park entry, deadlocking the predicate). The
// Mode-machine state m_worldLock guards is untouched by §A.3 windows: a §A.3
// stop sets NO client-visible GC stop state and never transitions
// VMManager::Mode (§A.3.2b; the keep-parked interplay below).
//
// SB1 contract: the stop word's SOLE accessors are the seq_cst pair below
// (U20 lint shape); the conductor's per-sample access loads execute inside
// the registry-lock hold behind a seq_cst fence — with the client's seq_cst
// F8 CAS / seq_cst RHA exchange on the other side, the C++20 SC-fence rule
// puts all four ops in one total order, which is exactly the SB1.4 proof
// (a fence-assisted relaxed sample is used because GCClient::Heap::
// hasHeapAccess() is a relaxed accessor frozen in Heap.h, outside this
// task's writable set; the proof obligation is discharged identically).
// ============================================================================

// Defined in heap/Heap.cpp (U-T5): per-thread park access pairing.
void gcClientWillParkForThreadGranularStop();
void gcClientDidResumeFromThreadGranularStop();
// Defined in runtime/VMLite.cpp (U-T5): ANNEX ISB1.
void jsThreadsBumpStopGeneration();
void jsThreadsNVSExitInstructionSync();

static Lock s_jsThreadsJobSlotLock; // §A.3.3/HBT4 pending-job-slot mutex (§LK row 4b).
// TRUE LEAF (U20; review round on the §LK lint): nothing — not even the
// registry lock — is ever acquired while s_jsThreadsParkLock is held. Waiters
// evaluate their predicates (registry walks, m_worldLock samples) BEFORE
// taking it; the bounded 1ms waits absorb the resulting check-then-wait
// lost-wakeup window (<= one timeout of added latency, never a hang).
static Lock s_jsThreadsParkLock;
static Condition s_jsThreadsParkCondition; // NVS tickets + the conductor's predicate wait.
static std::atomic<VM*> s_jsThreadsStopWord { nullptr }; // SB1 stop word; seq_cst accessors below ONLY (U20).
static std::atomic<WTF::Thread*> s_jsThreadsConductorThread { nullptr }; // §A.3.3 tenure (thread-keyed, not VM-keyed).
static std::atomic<unsigned> s_jsThreadsWorldStoppedDepth { 0 }; // §J.8 witness: window open AND predicate satisfied.
static std::atomic<uint64_t> s_jsThreadsCompletedWindowCount { 0 }; // V4 watchdog progress token: bumped once per COMPLETED §A.3 window (resume path). Relaxed: heuristic re-arm input only, never a soundness input.
static thread_local unsigned t_jsThreadsConductorDepth { 0 }; // R1.h nesting on the conductor thread.

// The gilOff Mode-machine servicing-thread tenure (§A.3.8: the landed
// per-VM machine keyed m_targetVM on the VM, which is ambiguous with two
// same-VM observers — exactly the double-transition/assert hazard the
// handout cites at :218/:580). Keyed PER VM (review round: §A.3.8 makes each
// gilOff VM ONE counting unit with ITS OWN representative — a single
// process-wide slot would let the first-arriving thread of VM A hold the
// tenure while every thread of VM B parks uncounted, so B's stop never
// reaches m_numberOfStoppedVMs == m_numberOfActiveVMs and a two-gilOff-VM
// Mode stop deadlocks). Guarded by m_worldLock; entries are transient (the
// representative removes its own on exit; notifyVMDestruction sweeps).
static HashMap<VM*, WTF::Thread*>& gilOffServicingThreads() // WTF_REQUIRES_LOCK(m_worldLock) by convention.
{
    static NeverDestroyed<HashMap<VM*, WTF::Thread*>> map;
    return map.get();
}

// §A.3.2b(i) gate exemption: true while THIS thread is a gilOff VM's elected
// Mode-machine representative (between election and the post-service
// re-acquire) — the representative must re-acquire heap access to run the
// STW callback, and gating it would self-deadlock. Thread-local because the
// exemption is exact for the asking thread and needs no lock.
static thread_local bool t_gilOffModeStopServicer { false };

// U20: the ONLY loads/stores of the stop word (seq_cst, SB1 item 1/3).
static ALWAYS_INLINE VM* jsThreadsStopWordLoad()
{
    return s_jsThreadsStopWord.load(std::memory_order_seq_cst);
}

static ALWAYS_INLINE void jsThreadsStopWordStore(VM* vm)
{
    s_jsThreadsStopWord.store(vm, std::memory_order_seq_cst);
}

bool jsThreadsStopPendingFor(VM& vm)
{
    return jsThreadsStopWordLoad() == &vm;
}

bool jsThreadsCurrentThreadIsStopConductor()
{
    return s_jsThreadsConductorThread.load(std::memory_order_seq_cst) == &Thread::currentSingleton();
}

void jsThreadsNotifyMutatorQuiesced()
{
    Locker locker { s_jsThreadsParkLock };
    s_jsThreadsParkCondition.notifyAll();
}

// EXIT1.1/EXIT1.2: forEachEnteredThread — THE registry-walk helper; §A.3
// conductor code reaches lites ONLY through it (U20). The functor runs
// inside the registry-lock hold of the walk that found the lite and must not
// let any lite*/client* escape the hold (no caching across samples). The
// walk is allocation-free and acquires nothing.
//
// Entered predicate (EXIT1.4): registered AND state == Live (TEARDOWN or
// absent => EXITED, r28-r30; COLLECTED/DETACHED defensively excluded — they
// are never conductor-visible) AND clientHeap non-null (the write-once
// release-published client pointer; null => not-entered, EXIT1.4(b)). Every
// lite-state read is under the registry lock (r31).
template<typename Functor>
static void forEachEnteredThread(VM& vm, const Functor& functor)
{
    auto& registry = VMLiteRegistry::singleton();
    Locker locker { registry.lock };
    // SB1 item 2: order this sample after the conductor's seq_cst stop-word
    // store and against the clients' seq_cst CAS/exchange (SC-fence leg; see
    // the banner). One fence per walk suffices — every load below is
    // program-ordered after it.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    for (VMLite* lite : registry.lites) {
        if (lite->vm != &vm)
            continue; // §A.1.3 filter.
        if (lite->state != VMLite::State::Live)
            continue; // EXIT1.4(a): counted EXITED before any client deref.
        if (!lite->clientHeap)
            continue; // EXIT1.4(b): not-entered / no-access.
        if (functor(*lite) == IterationStatus::Done)
            break;
    }
}

static unsigned UNUSED_FUNCTION numberOfEnteredThreads(VM& vm)
{
    unsigned count = 0;
    forEachEnteredThread(vm, [&](VMLite&) {
        ++count;
        return IterationStatus::Continue;
    });
    return count;
}

// §A.3.2 conductor predicate, one sample: every entered thread of the target
// VM — other than the conductor itself — is access-released (which subsumes
// parked: gilOff threads release their own client when parking, see
// gcClientWillParkForThreadGranularStop) or not-entered. The conductor's own
// lite is re-derived from TLS per sample, never cached across samples
// (EXIT1.2).
static bool allEnteredThreadsAreQuiescent(VM& vm)
{
    VMLite* conductorLite = VMLite::currentIfExists();
    bool quiescent = true;
    forEachEnteredThread(vm, [&](VMLite& lite) {
        if (&lite == conductorLite)
            return IterationStatus::Continue; // HBT2.1: the conductor may retain/re-acquire access.
        // SB1 item 2 sample (fence-assisted; live client deref is sound
        // under the walk's lock hold per EXIT1.4(b)).
        if (lite.clientHeap->hasHeapAccess()) {
            quiescent = false;
            return IterationStatus::Done;
        }
        return IterationStatus::Continue;
    });
    return quiescent;
}

// Mode-machine service gating (§A.3.8): a latched debugger STW callback must
// not run while any gilOff mutator other than the servicing thread still
// holds heap access. Cheap: gated on the process-level discriminator.
static bool gilOffMutatorsBlockModeStopService()
{
    if (!VM::isGILOffProcess()) [[likely]]
        return false;
    GCClient::Heap* servicingClient = GCClient::Heap::currentThreadClient();
    auto& registry = VMLiteRegistry::singleton();
    Locker locker { registry.lock };
    std::atomic_thread_fence(std::memory_order_seq_cst);
    for (VMLite* lite : registry.lites) {
        if (!lite->gilOff)
            continue;
        if (lite->state != VMLite::State::Live)
            continue;
        GCClient::Heap* client = lite->clientHeap;
        if (!client || client == servicingClient)
            continue;
        if (client->hasHeapAccess())
            return true;
    }
    return false;
}

void jsThreadsParkForStopWindow(VM& vm)
{
    // §A.3.2 NVS ticket. Pre: the calling thread holds NO heap access (the
    // §A.3.2b gate reverted it, or the caller released it). Tokens are KEPT
    // while parked (§A.3.2b) — that is what makes the access-released
    // exemption sound: re-running JS needs re-acquisition, which this
    // window's stop word gates.
    if (jsThreadsCurrentThreadIsStopConductor())
        return; // HBT3.2: a conductor never parks on its own window.
    for (;;) {
        if (jsThreadsStopWordLoad() != &vm)
            break;
        Locker locker { s_jsThreadsParkLock };
        if (jsThreadsStopWordLoad() != &vm)
            break;
        // Bounded wait: resume notifies this condition; the timeout is a
        // belt-and-suspenders backstop against a lost wakeup, not a
        // correctness mechanism (the predicate is re-polled seq_cst).
        s_jsThreadsParkCondition.waitFor(s_jsThreadsParkLock, Seconds::fromMilliseconds(1));
    }
    // R1.d/ISB1: leaving the NVS ticket executes an unconditional
    // context-sync and refreshes the per-thread stop-generation copy.
    jsThreadsNVSExitInstructionSync();
}

// SPEC-ungil §A.3.2b(i) (review round): the lite's stop bit — under the
// §A.2.1 interim seam, the VM trap word's NeedStopTheWorld bit — gates FRESH
// heap-access acquisition for MODE-MACHINE stops too, not just §A.3 windows.
// The §A.3.8 service-gating conjunct (gilOffMutatorsBlockModeStopService)
// samples sibling access states and is sound only if a sibling that released
// cannot silently re-acquire and run JS while the debugger STW callback is
// in flight. Consumed by GCClient::Heap::acquireHeapAccess (Heap.cpp seam).
//
// Exemptions: (a) the elected representative (it re-acquires to service);
// (b) the free-running RunOne target VM (landed RunOne semantics resume the
// target wholesale; its trap bit stays set by design); (c) Mode::RunAll
// (a residual or pre-Stopping-sliver bit — poll-site delivery and
// notifyVMActivation cover the sliver; the §A.3 conductor's post-window
// clear/re-check retires residuals).
//
// Cost: one fence-assisted relaxed trap-bit load on the gilOff AHA path; the
// m_worldLock-taking info() snapshot runs only with the bit set (i.e. only
// while some stop is actually in flight or RunOne is active — debugger
// modes, where "peek performance is not a concern" per notifyVMStop).
bool jsThreadsModeStopGatesCurrentThread(VM& vm)
{
    if (t_gilOffModeStopServicer)
        return false;
    // HBT2.1/HBT3.2: an open §A.3 window's conductor may retain/re-acquire
    // access inside its own window while holding the job-slot mutex (and
    // GCL). Parking IT here would deadlock a Mode stop that arrived
    // mid-window: every sibling is ticket-parked on the §A.3 word and none
    // reaches the representative election until the window closes — so the
    // conductor must finish its window first and parks for the Mode stop at
    // its post-window re-acquire instead (tenure is already dropped there).
    if (jsThreadsCurrentThreadIsStopConductor())
        return false;
    // SB1-shape Dekker leg: the fan side is fireTrap's seq_cst RMW followed
    // by a fenced access sample; this side is the caller's seq_cst access
    // CAS followed by this fenced bit load (the trap-bit accessor is a
    // relaxed load frozen in VMTraps.h, outside this task's writable set —
    // fence-assisted, same discharge as the banner's hasHeapAccess note).
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (!vm.traps().needHandling(VMTraps::NeedStopTheWorld)) [[likely]]
        return false;
    auto info = VMManager::info();
    if (info.worldMode == VMManager::Mode::RunAll)
        return false;
    if (info.worldMode == VMManager::Mode::RunOne && info.targetVM == &vm)
        return false;
    return true;
}

void jsThreadsParkForModeStop(VM& vm)
{
    // §A.3.2b(i) NVS ticket for Mode-machine stops. Pre: the calling thread
    // holds NO heap access (the AHA gate reverted it). The predicate is
    // evaluated OUTSIDE the park-lock hold (leaf discipline, see the lock's
    // comment); resumeTheWorld notifies this condition, and the bounded wait
    // backstops the check-then-wait window and the RunOne edges.
    for (;;) {
        if (!jsThreadsModeStopGatesCurrentThread(vm))
            break;
        Locker locker { s_jsThreadsParkLock };
        s_jsThreadsParkCondition.waitFor(s_jsThreadsParkLock, Seconds::fromMilliseconds(1));
    }
    // R1.d/ISB1: same NVS-exit contract as the §A.3 ticket above.
    jsThreadsNVSExitInstructionSync();
}

// §J.8 witness for patching asserts (replaces the stub depth counter's role
// post-ungil): true while a §A.3 window is open AND its predicate has been
// satisfied. OPEN (cross-file): JSThreadsSafepoint::worldIsStopped() gains
// this disjunct when the stub is deleted (bytecode/JSThreadsSafepoint.cpp is
// outside U-T5's writable set — see the task summary).
bool jsThreadsThreadGranularWorldIsStopped()
{
    return s_jsThreadsWorldStoppedDepth.load(std::memory_order_relaxed);
}

// The real R1.a-i sequence (§A.3, HBT4 order). GIL-off ONLY: gilOn callers
// keep the JSThreadsSafepoint.cpp path. The §A.3.3 licensed reroute has
// LANDED: JSThreadsSafepoint::stopTheWorldAndRun routes here when
// vm.gilOff() (after its R1.h already-stopped inline branch, which now also
// consults jsThreadsThreadGranularWorldIsStopped() so nested fires inside an
// open window run inline), and JSThreadsSafepoint::worldIsStopped() gained
// the §J.8 disjunct. The stub and its entered-VM tripwire remain GIL-on-only.
void jsThreadsThreadGranularStopTheWorldAndRun(VM& vm, const ScopedLambda<void()>& work)
{
    RELEASE_ASSERT(vm.gilOff());
#if ASSERT_ENABLED
    {
        // R1 contract: the requester is an entered mutator of this VM
        // (token holder; GIL-off spawned threads hold no m_lock, §F.1, so
        // currentThreadIsHoldingAPILock is NOT the right assert here).
        VMLite* selfLite = VMLite::currentIfExists();
        ASSERT(selfLite && selfLite->vm == &vm);
    }
#endif

    // R1.h: a nested request on the conductor thread inside its own open
    // window runs inline — the world is already stopped for us.
    if (t_jsThreadsConductorDepth) {
        RELEASE_ASSERT(jsThreadsCurrentThreadIsStopConductor());
        ++t_jsThreadsConductorDepth;
        work();
        --t_jsThreadsConductorDepth;
        WTF::crossModifyingCodeFence(); // Patcher-side fence for the nested patch (F5).
        return;
    }

    // R1.i step 1 — KEPT FIRST (HBT4.1): release this thread's own client
    // access before arbitration, so a losing requester parks access-released
    // and the winner's predicate counts it.
    GCClient::Heap* selfClient = GCClient::Heap::currentThreadClient();
    bool releasedAccess = false;
    if (selfClient && selfClient->hasHeapAccess()) {
        selfClient->releaseHeapAccess();
        releasedAccess = true;
    }
    // Watchdog budget (V4-stw-watchdog revision): requestStart is sampled
    // BEFORE the arbitration park — reaching conductor tenure is part of
    // reaching a stopped world, and the pre-tenure leg was previously an
    // unbounded, unwatched block. But the budget is PROGRESS-AWARE, not one
    // wall-clock window end-to-end: the arbitration leg re-arms whenever
    // another conductor COMPLETES a window (a legitimate fire-storm queue
    // serializing N full stop windows is progress, not a wedge), and each
    // post-tenure leg (GCL bracket; §A.3.2 predicate wait) gets its own
    // fresh 30s budget. A true wedge completes no windows and makes no
    // per-leg progress, so it still fail-stops in <= 30s of NO progress —
    // detection unchanged; only the queue-mistaken-for-wedge false positive
    // is retired.
    MonotonicTime requestStart = MonotonicTime::now();
    const MonotonicTime originalRequestStart = requestStart;
    const MonotonicTime bhRequestBegin = originalRequestStart; // BUGHUNT (NOT FOR LANDING): request-to-resume latency.
    {
        // HBT4 step 2: arbitration. Exactly one requesting THREAD is
        // released as conductor; losers PARK here in bounded 1ms tryLock
        // quanta (watchdog-covered, see above), access-released, then retry
        // the whole sequence as later winners.
        //
        // V4-stw-watchdog (transition-vs-write, tier-forced): this tryLock
        // poll has NO queue position — unlike Lock::lock()'s parked
        // FIFO/handoff, a sleeping poller can lose every race to
        // freshly-arriving requesters. Under a Class-A fire storm
        // (tier-forced thresholds multiply transition fires; jettisoned
        // code recompiles at warmup 10 and re-arms fresh watched sets) the
        // job slot is held nearly continuously by a SUCCESSION of live,
        // completing windows, so a starved loser's single end-to-end budget
        // expires with the system fully live — the watchdog then fail-stops
        // and its participant dump prints the CURRENT conductor's AB-21
        // window access as "NON-QUIESCENT". Distinguish queue from wedge:
        // a completed window is PROGRESS; re-arm the budget whenever the
        // completion count advances. A true wedge completes no windows and
        // still crashes at 30s of no progress, detection unchanged.
        // Accepted trade: progress-based re-arm makes arbitration
        // starvation formally unbounded (tryLock has no fairness); wedge
        // detection, not fairness, is the watchdog's contract — the
        // breadcrumb below keeps long queues observable.
        uint64_t observedWindows = s_jsThreadsCompletedWindowCount.load(std::memory_order_relaxed);
        unsigned loggedQueueBreadcrumbs = 0;
        while (!s_jsThreadsJobSlotLock.tryLock()) {
            uint64_t windows = s_jsThreadsCompletedWindowCount.load(std::memory_order_relaxed);
            if (windows != observedWindows) {
                observedWindows = windows;
                requestStart = MonotonicTime::now(); // Progress: another conductor completed a window.
            }
            // Starvation observability: a re-armed loser can now queue far
            // past one budget with zero output; log a breadcrumb at 60s and
            // 120s of TOTAL queueing so a future fairness bug reads as a
            // loud queue, not a silent harness timeout.
            Seconds totalQueued = MonotonicTime::now() - originalRequestStart;
            if ((loggedQueueBreadcrumbs == 0 && totalQueued > Seconds(60)) || (loggedQueueBreadcrumbs == 1 && totalQueued > Seconds(120))) {
                ++loggedQueueBreadcrumbs;
                dataLogLn("JSThreads §A.3 arbitration: requester queued ", totalQueued.seconds(), "s total (budget re-armed on window completions; completed-window count now ", observedWindows, ") — live fire-storm queue, not a wedge.");
            }
            JSThreadsSafepoint::watchdogAssertStopProgress(requestStart, &vm);
            WTF::sleep(Seconds::fromMilliseconds(1));
        }
        Locker arbitration { AdoptLock, s_jsThreadsJobSlotLock };
        // Per-leg budget: winning tenure IS progress. The GCL bracket and
        // the §A.3.2 predicate wait each get a fresh 30s window (a wedge in
        // any single leg still fail-stops in <= 30s of no progress; total
        // request latency stays bounded by progress, not by one wall-clock
        // budget that a legitimate fire-storm queue can exhaust).
        requestStart = MonotonicTime::now();

        // HBT4 step 3 (WINNER ONLY) — the LICENSED REORDER of the landed
        // R1.i bracket: the GCL bracket comes strictly AFTER arbitration
        // (the landed order "GCL then arbitrate" deadlocks: a loser blocked
        // raw on GCL would violate HBT4.3 and could deadlock against a GC
        // conductor queued behind the same lock). At most one thread — this
        // winner — ever blocks in the GCL acquisition, and it blocks
        // access-released and watchdog-covered; it queues behind any
        // in-progress shared GC (§10C(b)/(e)).
        JSC::Heap& server = vm.clientHeap.server();
        Heap::JSThreadsStopScope stopScope(server, requestStart);
        requestStart = MonotonicTime::now(); // Per-leg: predicate wait gets its own budget (GCL leg was covered by the value passed into stopScope).

        // Fan (§A.2.3 / SB1 item 1): conductor tenure, then the seq_cst stop
        // word, then the per-lite stop bits. Under the U-T2 interim seam the
        // per-lite bits ALIAS the single VM-wide trap word (VMLite.cpp
        // §A.2.1), so the fan is one requestStop(); the seq_cst stop WORD is
        // the load-bearing half of the SB1 Dekker pair with re-acquirers.
        s_jsThreadsConductorThread.store(&Thread::currentSingleton(), std::memory_order_seq_cst);
        jsThreadsStopWordStore(&vm);
        vm.requestStop(); // Poll-site delivery: running mutators trap to notifyVMStop.
        WTF::storeLoadFence();
        jsThreadsNotifyMutatorQuiesced(); // Wake ticket-parked threads to observe the word.

        // §A.3.2 predicate wait: per-sample EXIT1.2 registry walks; the
        // registry lock is dropped before every block/yield between samples,
        // and the walk NEVER runs under s_jsThreadsParkLock (leaf discipline,
        // see the lock's comment — the bounded wait absorbs the
        // check-then-wait lost-wakeup window). requestStart was re-sampled
        // after the GCL bracket (per-leg budget, see the arbitration block
        // above): this predicate wait has its own fresh 30s window, with NO
        // progress re-arm — a conductor whose predicate cannot converge
        // (bucket-iii lock-holding fire, unpolled access-holding native
        // wait) still fail-stops in <= 30s.
        for (;;) {
            if (allEnteredThreadsAreQuiescent(vm))
                break;
            // §A.3.2 fan re-assertion (review round): under the §A.2.1 alias
            // the per-lite stop bits ARE the single VM-wide trap word, and
            // VMTraps' take rule clears NeedStopTheWorld when the FIRST
            // trapping thread latches it — with no per-lite delivery record,
            // a still-JS-spinning sibling would otherwise never trap after a
            // sibling consumed the bit, and this predicate would hang until
            // the watchdog fail-stops. Re-fire on every non-quiescent sample
            // (idempotent seq_cst RMW, bounded by this loop). RETIRED when
            // the per-lite trap words land (VMTraps.h activation checklist).
            vm.requestStop();
            WTF::storeLoadFence();
            JSThreadsSafepoint::watchdogAssertStopProgress(requestStart, &vm); // Pass the target VM so a timeout names the non-quiescent lite(s).
            Locker parkLocker { s_jsThreadsParkLock };
            s_jsThreadsParkCondition.waitFor(s_jsThreadsParkLock, Seconds::fromMilliseconds(1));
        }

        // AB-21 fix (GIL-removal review round): re-acquire the conductor's
        // OWN client access for the window before running the fire bodies.
        // Class-A fire bodies (WatchpointSet::fireAllSlow via
        // drainClassAFireQueue, Debugger walk closures) take
        // DeferGC/DeferGCForAWhile and run write barriers, whose per-client
        // slots assert `client->hasHeapAccess() ||
        // worldIsStoppedForAllClients()` (Heap::deferralDepthSlot, Heap.h) —
        // the §A.3 thread-granular witness is invisible to them, so a
        // no-access conductor aborted at the FIRST Class-A fire (the AB-21
        // gilOff boot crash). Sound and non-blocking here: the AHA §A.3 and
        // Mode-stop legs exempt the conductor (jsThreadsCurrentThreadIs-
        // StopConductor — tenure was published above), GSP cannot be
        // pending while the JSThreadsStopScope GCL bracket is held, and
        // allEnteredThreadsAreQuiescent exempts the conductor's own lite
        // (HBT2.1), so the satisfied predicate stays satisfied.
        GCClient::Heap* windowClient = GCClient::Heap::currentThreadClient();
        bool reacquiredForWindow = false;
        if (windowClient && !windowClient->hasHeapAccess()) {
            windowClient->acquireHeapAccess();
            reacquiredForWindow = true;
        }

        // World stopped: run `work` on this stack. Default-conductor closure
        // rules stand (R1.i): allocation-free, own client only. HBT2.2
        // NO-GC-IN-WINDOW: bracket the window in heap I14's STW-forbidden
        // counter so CSAC/SINFAC entries assert; GC initiation inside the
        // window is FORBIDDEN — deferred-GC checks enqueue and re-run after
        // resume.
        s_jsThreadsWorldStoppedDepth.fetch_add(1, std::memory_order_relaxed);
        ++t_jsThreadsConductorDepth;
        server.incrementSTWForbiddenScope();
        work();
        server.decrementSTWForbiddenScope();
        --t_jsThreadsConductorDepth;

        // AB-21 fix: drop the window-scoped access BEFORE resume publication
        // so a sibling observing the cleared word never samples the
        // conductor as an access-holding mutator mid-resume; the R1.i tail
        // below re-acquires for the requester iff it held access on entry.
        if (reacquiredForWindow)
            windowClient->releaseHeapAccess();

        // Resume (R1.i order): patcher-side data->ifetch publication first,
        // then the ISB1.1 stop-generation bump INSIDE the window before
        // resume, then drop the witness and clear the word (seq_cst — its
        // synchronizes-with edge is what publishes the bump to gated
        // re-acquirers), then wake every ticket.
        WTF::crossModifyingCodeFence();
        jsThreadsBumpStopGeneration();
        s_jsThreadsWorldStoppedDepth.fetch_sub(1, std::memory_order_relaxed);
        jsThreadsStopWordStore(nullptr);
        s_jsThreadsConductorThread.store(nullptr, std::memory_order_seq_cst);
        s_jsThreadsCompletedWindowCount.fetch_add(1, std::memory_order_relaxed); // V4 watchdog progress token (see the arbitration loop).
        // BUGHUNT INSTRUMENTATION (stw-watchdog evidence pack; env-gated; NOT FOR LANDING):
        // JSC_CLASSA_FIRE_STATS=1 also dumps the completed SectionA.3 window count and
        // request-to-resume latency aggregates at exit.
        if (getenv("JSC_CLASSA_FIRE_STATS")) [[unlikely]] {
            static std::atomic<double> bhTotalMs { 0 };
            static std::atomic<double> bhMaxMs { 0 };
            double ms = (MonotonicTime::now() - bhRequestBegin).milliseconds();
            double cur = bhTotalMs.load(std::memory_order_relaxed);
            while (!bhTotalMs.compare_exchange_weak(cur, cur + ms)) { }
            double curMax = bhMaxMs.load(std::memory_order_relaxed);
            while (ms > curMax && !bhMaxMs.compare_exchange_weak(curMax, ms)) { }
            static std::once_flag bhOnce;
            std::call_once(bhOnce, [] {
                std::atexit([] {
                    dataLogLn("BUGHUNT-A3-WINDOWS completed=", s_jsThreadsCompletedWindowCount.load(std::memory_order_relaxed),
                        " totalRequestToResumeMs=", bhTotalMs.load(std::memory_order_relaxed),
                        " maxMs=", bhMaxMs.load(std::memory_order_relaxed));
                });
            });
        }
        // Retire this window's stop bits (review round): leaving the
        // NeedStopTheWorld trap + entry-scope-service bits set forever would
        // (a) route EVERY later VMEntryScope entry/exit of every thread
        // through the entry-scope service and its m_worldLock takes — a
        // permanent serialization point — and (b) wedge the §A.3.2b(i)
        // Mode-stop AHA gate on a stale bit. The bit may be co-owned by an
        // in-flight Mode-machine stop, so the clear is CLEAR-THEN-RECHECK-
        // THEN-RESTORE rather than a raw cancel: requestStopAllInternal
        // publishes Mode::Stopping and (re)fires the trap bit under ONE
        // m_worldLock hold, so the info() lock acquisition below observes a
        // non-RunAll mode for any fire our cancel could have raced with, and
        // the restore re-delivers it (requestStop is idempotent; a duplicate
        // delivery is one benign notifyVMStop trip). RunOne deliberately
        // keeps its bits (the restore re-arms them). A queued §A.3 loser
        // re-fires its own fan as the next winner.
        vm.cancelStop();
        if (VMManager::info().worldMode != VMManager::Mode::RunAll)
            vm.requestStop();
        jsThreadsNotifyMutatorQuiesced();
    } // ~JSThreadsStopScope: drop the GCL bracket (resume order), then...
    if (releasedAccess)
        selfClient->acquireHeapAccess(); // ...re-acquire access LAST (R1.i).
}

void VMManager::incrementActiveVMs(VM& vm) WTF_REQUIRES_LOCK(m_worldLock)
{
    RELEASE_ASSERT(m_worldMode != Mode::RunAll);

    if (!vm.traps().m_hasBeenCountedAsActive) {
        m_numberOfActiveVMs++;
        vm.traps().m_hasBeenCountedAsActive = true;
    }
}

void VMManager::decrementActiveVMs(VM& vm) WTF_REQUIRES_LOCK(m_worldLock)
{
    // UNGIL §A.3.8 (U-T5): the gilOff VM is ONE counting unit, represented
    // by its gilOffServicingThreads() entry (see notifyVMStop). A SIBLING
    // thread of that VM deactivating mid-stop must neither decrement the
    // VM's count nor trigger the RunOne resume-all arm (the :218
    // m_targetVM==&vm tenure assert is VM-keyed and ambiguous with two
    // same-VM observers) — the VM stays active while any thread is still
    // entered. GIL-on: branch dead.
    if (vm.gilOff() && m_worldMode != Mode::RunAll) [[unlikely]] {
        bool currentThreadIsServicing = gilOffServicingThreads().get(&vm) == &Thread::currentSingleton();
        if (!currentThreadIsServicing && vm.isEntered())
            return;
    }

    // We only need to track m_numberOfActiveVMs changes if we're in RunOne
    // mode. If we're running because the world was resumed with RunAll,
    // then m_numberOfActiveVMs is invalid, and resumeTheWorld() would set
    // it to a token value of invalidNumberOfActiveVMs (to aid debugging).
    if (m_worldMode == Mode::RunAll) {
        RELEASE_ASSERT(m_numberOfActiveVMs == invalidNumberOfActiveVMs);
        RELEASE_ASSERT(!vm.traps().m_hasBeenCountedAsActive);
    } else if (vm.traps().m_hasBeenCountedAsActive) {
        m_numberOfActiveVMs--;
        vm.traps().m_hasBeenCountedAsActive = false;
    }

    auto shouldResumeAll = [&] WTF_REQUIRES_LOCK(m_worldLock) {
        if (m_worldMode != Mode::RunAll && !m_numberOfActiveVMs)
            return true;
        if (m_worldMode == Mode::RunOne) {
            RELEASE_ASSERT(m_targetVM == &vm);
            return true;
        }
        return false;
    };

    if (shouldResumeAll()) {
        if (m_targetVM) {
            // There's a designated targetVM thread to continue in, but we don't have the
            // ability to just wake the desired one up. So, wake up all the threads and let
            // them sort themselves out.
            //
            // But if the targetVM thread is this thread, then pass the control to another
            // thread, any thread. That's because this thread is dying imminently.
            if (m_targetVM == &vm) {
                m_targetVM = nullptr;
                m_useRunOneMode = false;
            }
            m_worldConditionVariable.notifyAll();
        } else {
            // There's no designated targetVM thread. So, just waking up any one thread will do.
            m_worldConditionVariable.notifyOne();
        }
    }
}

CONCURRENT_SAFE void VMManager::requestStopAllInternal(StopReason reason)
{
    // StopReason is synonymous with "StopRequest".
    // From the client's perspective, it is the reason for a stop request.
    // From the VMManager's perspective, it is the type of stop request.
    auto requestBits = static_cast<StopRequestBits>(reason);
    m_pendingStopRequestBits.exchangeOr(requestBits);
    {
        Locker lock { m_worldLock };
        if (m_worldMode >= Mode::Stopping) {
            // THREADS-INTEGRATE(heap) manifest 5g(ii): a GC stop
            // requested while another stop is already in progress must
            // still (1) trap entered VMs — a RunOne targetVM keeps
            // executing through an in-progress debugger stop and would
            // otherwise never reach a poll — and (2) wake parked VMs so
            // their wait loops observe the new GC bit and run the 5g(i)
            // park hook (release heap access). Without this, a GC
            // requested during a non-GC stop hangs the §10.4 barrier.
            if (reason == StopReason::GC) [[unlikely]] {
                iterateVMs(scopedLambda<IteratorCallback>([&] (VM& vm) {
                    if (vm.isEntered()) {
                        vm.requestStop();
                        WTF::storeLoadFence();
                    }
                    return IterationStatus::Continue;
                }));
                m_worldConditionVariable.notifyAll();
            }
            return;
        }

        if (m_worldMode == Mode::RunAll) {
            // RunOne mode allows execution of 1 VM without resumeTheWorld(). We did not clear
            // the m_hasBeenCountedAsActive flags on each VM on resuming with RunOne. As a
            // result, m_numberOfActiveVMs is still valid in RunOne mode. We don't want
            // to reset m_numberOfActiveVMs to 0 here because we won't be re-calculating
            // it on stop like we do for RunAll mode.
            //
            // For RunAll mode, do want to reset m_numberOfActiveVMs, and incrementActiveVMs()
            // below will re-calculate the current true value of m_numberOfActiveVMs.
            m_numberOfActiveVMs = 0;
        }

        // INVARIANT (load-bearing for the §A.3 conductor's resume recheck,
        // jsThreadsThreadGranularStopTheWorldAndRun cancelStop/recheck/
        // restore): the Mode::Stopping publication below and the per-VM
        // requestStop() fan in the iterateVMs loop further down MUST both
        // happen under THIS single m_worldLock hold. The conductor's
        // post-cancel VMManager::info() read acquires m_worldLock, so any
        // fire its cancel could have raced with is observed as a non-RunAll
        // mode and the bits are re-delivered (vm.requestStop() restores BOTH
        // the trap bit and the entry-scope-service bit, VM.h). Splitting the
        // publication and the fan across separate lock holds reintroduces a
        // lost-stop-bit window (silent stop-delivery loss).
        ASSERT(m_worldLock.isHeld());
        m_worldMode = Mode::Stopping;

        bool isWasmDebugger = reason == StopReason::WasmDebugger;

        // Have to use iterateVMs() instead of forEachVM() because we're already
        // holding the m_worldLock.
        iterateVMs(scopedLambda<IteratorCallback>([&] (VM& vm) {
            vm.requestStop();
            WTF::storeLoadFence();

            if (isWasmDebugger) [[unlikely]]
                dispatchStopHandler(vm);

            if (vm.isEntered()) {
                // incrementActiveVMs() relies on m_worldLock being held, which it
                // obviously is above. However, Clang is not smart enough to see this.
                // So, we need to suppress this warning here.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wthread-safety-analysis"
#endif
                incrementActiveVMs(vm);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
            }
            return IterationStatus::Continue;
        }));
    }
}

// Dispatch a callback to VM's RunLoop to handle Stop-The-World for idle VMs.
// Idle VMs (not executing code) never check traps, so they can't respond to requestStop().
// Dispatching to RunLoop ensures the callback executes when VM processes events, allowing
// idle VMs to call notifyVMStop(). Uses atomic flag to prevent duplicate dispatches.
void VMManager::dispatchStopHandler(VM& vm)
{
    if (vm.traps().m_hasDispatchedIdleStopHandler.exchange(true))
        return;

    // Use JSLock coordination pattern (like JSRunLoopTimer) to safely detect VM destruction.
    Ref<JSLock> apiLock = vm.apiLock();
    vm.runLoop().dispatch([apiLock = WTF::move(apiLock)]() {
        Locker locker { apiLock.get() };

        RefPtr<VM> vm = apiLock->vm();
        if (!vm)
            return;

        // Clear flag before VMEntryScope so new stop requests during VMEntryScope can dispatch.
        // Must clear before, not after: stops during destruction can't be handled by this scope.
        vm->traps().m_hasDispatchedIdleStopHandler.exchange(false);

        RELEASE_ASSERT(!vm->isEntered());
        VMEntryScope scope(*vm, nullptr);
    });
}

CONCURRENT_SAFE void VMManager::requestResumeAllInternal(StopReason reason)
{
    // StopReason is synonymous with StopRequest.
    // From the client's perspective, it is the reason for a stop request.
    // From the VMManager's perspective, it is the type of stop request.
    auto requestBits = static_cast<StopRequestBits>(reason);
    m_pendingStopRequestBits.exchangeAnd(~requestBits);

    // THREADS-INTEGRATE(heap) manifest 5e: VMs parked by the GC
    // keep-parked rule (5b) wait on m_worldConditionVariable with no
    // latched m_currentStopReason and (with no other stop in progress) no
    // targetVM — NOTHING else will ever wake them once the GC bit
    // clears. So for reason == GC, ALWAYS notifyAll under m_worldLock:
    // even if other stop bits remain pending (woken VMs re-evaluate
    // shouldStop() and re-park for the remaining reasons), and even if
    // resumeTheWorld() early-returns (RunOne mode, or already RunAll).
    // Getting this wrong (e.g. notifying only when the GC bit was the
    // last pending bit) is a silent shared-mode resume deadlock.
    if (reason == StopReason::GC) {
        Locker lock { m_worldLock };
        if (!hasPendingStopRequests())
            resumeTheWorld();
        m_worldConditionVariable.notifyAll();
        return;
    }

    if (hasPendingStopRequests())
        return; // There are still pending stop requests. Nothing more to do.

    Locker lock { m_worldLock };
    resumeTheWorld();
}

void VMManager::resumeTheWorld() WTF_REQUIRES_LOCK(m_worldLock)
{
    // We can call resumeTheWorld() more than once. Hence, we may already be in RunAll mode.
    if (m_worldMode == Mode::RunAll)
        return; // Already resumed. Nothing more to do.

    // If we're in RunOne mode, then we want to still call into notifyVMStop() all
    // the time. So, we don't want to resumeTheWorld() just yet as that will disable
    // all the stop checks yet.
    if (m_useRunOneMode)
        return;

    // Have to use iterateVMs() instead of forEachVM() because we're already
    // holding the m_worldLock.
    iterateVMs(scopedLambda<IteratorCallback>([&] (VM& vm) {
        vm.cancelStop();
        vm.traps().m_hasBeenCountedAsActive = false;
        return IterationStatus::Continue;
    }));

    m_targetVM = nullptr;
    m_numberOfActiveVMs = invalidNumberOfActiveVMs; // invalid when not Stopped.
    m_worldMode = Mode::RunAll;
    m_worldConditionVariable.notifyAll();

    // §A.3.2b(i) (review round): Mode-stop-gated acquirers park on the NVS
    // ticket condition, not on m_worldConditionVariable — wake them on
    // resume (their bounded wait is only a backstop). The park lock is a
    // true leaf, so taking it under m_worldLock is ordered.
    if (VM::isGILOffProcess()) [[unlikely]]
        jsThreadsNotifyMutatorQuiesced();
}

void VMManager::notifyVMStop(VM& vm, StopTheWorldEvent event)
{
    // ========================================================================
    // UNGIL §A.3/§A.3.8 (U-T5): thread-granular NVS for the gilOff VM.
    //
    // With N entered threads in ONE VM the landed per-VM machine
    // double-transitions and trips its own asserts (two same-VM observers:
    // the :454 stopped<=active counter invariant, the :218/:580
    // m_targetVM==&vm tenure asserts, the per-VM m_hasBeenCountedAsActive
    // flag). Disposition here:
    //   - §A.3 windows (no Mode-machine involvement): every thread of the
    //     gilOff VM parks on its OWN ticket, access-released, until the
    //     conductor clears the stop word. Mode/counters are never touched.
    //   - Mode-machine stops (GC keep-parked, debugger latch/service): the
    //     gilOff VM participates as ONE counting unit, represented by
    //     exactly one thread — the first to arrive, recorded in
    //     gilOffServicingThreads() (per-VM) under m_worldLock — which runs the landed
    //     per-VM protocol verbatim (idempotent per-VM flag, latch, RunOne
    //     tenure). Every SIBLING thread parks access-released on its own
    //     ticket below, untouched by the counters ("Mode keyed on
    //     all-parked / released / not-entered": the shouldStop quiescence
    //     conjunct keeps any service from running before the siblings'
    //     access release, and §A.3.2b gates their re-acquisition).
    // GIL-on / flag-off: the [[unlikely]] branch is dead; the landed machine
    // runs byte-for-byte.
    // ========================================================================
    bool isGilOffRepresentative = false;
    if (vm.gilOff()) [[unlikely]] {
        for (;;) {
            // (a) §A.3 ticket: an open thread-granular window parks us first.
            if (jsThreadsStopPendingFor(vm) && !jsThreadsCurrentThreadIsStopConductor()) {
                gcClientWillParkForThreadGranularStop();
                jsThreadsNotifyMutatorQuiesced();
                jsThreadsParkForStopWindow(vm);
                continue; // Re-evaluate: a Mode-machine stop may have arrived meanwhile.
            }
            // (b) Mode-machine stop: representative election under m_worldLock.
            bool modeStopActive = false;
            bool isServicingThread = false;
            {
                Locker lock { m_worldLock };
                modeStopActive = m_worldMode != Mode::RunAll || hasPendingStopRequests();
                // §A.3.8 RunOne (review round): when this gilOff VM IS the
                // free-running RunOne target and nothing further pends, its
                // threads are licensed to run — landed RunOne semantics
                // resume the target VM wholesale, and the §A.3.2b(i) AHA
                // gate carries the same exemption. Without this, every
                // sibling of the target would stay parked for the whole
                // RunOne episode.
                if (m_worldMode == Mode::RunOne && m_targetVM == &vm && !hasPendingStopRequests())
                    modeStopActive = false;
                if (modeStopActive) {
                    // Per-VM representative election (§A.3.8: one counting
                    // unit PER gilOff VM; add() keeps an existing tenant).
                    auto addResult = gilOffServicingThreads().add(&vm, &Thread::currentSingleton());
                    isServicingThread = addResult.iterator->value == &Thread::currentSingleton();
                }
            }
            if (!modeStopActive)
                break; // Nothing pending in either machine.
            if (isServicingThread) {
                isGilOffRepresentative = true;
                // §A.3.2b(i) exemption ON for the service tenure: the
                // representative re-acquires heap access to run the STW
                // callback (and again on the cleanup path) while the trap
                // bit is still set; gating it would self-deadlock. Cleared
                // by the cleanup scope below AFTER its re-acquire.
                t_gilOffModeStopServicer = true;
                break; // Fall through to the landed per-VM machine as the representative.
            }
            // Sibling: park on our own ticket, access-released; never counted.
            gcClientWillParkForThreadGranularStop();
            {
                Locker lock { m_worldLock };
                // Bounded wait: resume paths notify this condition; the
                // timeout keeps the §A.3-word/representative re-evaluation
                // live (the §A.3 conductor and the representative cannot
                // reach every parked sibling's condition deterministically).
                m_worldConditionVariable.waitFor(m_worldLock, Seconds::fromMilliseconds(1));
            }
        }
        if (!isGilOffRepresentative) {
            // NVS exit (sibling / no-op path): re-acquire any released
            // access (F8 + the §A.3.2b gate absorb a freshly-arrived stop),
            // then the R1.d unconditional context-sync + ISB1 refresh.
            gcClientDidResumeFromThreadGranularStop();
            jsThreadsNVSExitInstructionSync();
            return;
        }
    }
    // Representative cleanup runs on EVERY exit from the landed body below
    // (including the early RunAll return): drop the tenure, re-pair any §A.3
    // access release made inside the wait loop, and execute the NVS-exit
    // context-sync.
    auto gilOffRepresentativeCleanup = makeScopeExit([&] {
        if (!isGilOffRepresentative)
            return;
        {
            Locker lock { m_worldLock };
            auto& servicers = gilOffServicingThreads();
            auto it = servicers.find(&vm);
            if (it != servicers.end() && it->value == &Thread::currentSingleton())
                servicers.remove(it);
        }
        gcClientDidResumeFromThreadGranularStop();
        jsThreadsNVSExitInstructionSync();
        // Drop the §A.3.2b(i) exemption only AFTER the re-acquire above —
        // the trap bit may legitimately still be set (RunOne).
        t_gilOffModeStopServicer = false;
    });

    // Ensure VM is counted as active before executing the body of notifyVMStop.
    // This is needed for concurrent stop requests where entry services
    // may have missed the increment due to flag races with dispatch callbacks.
    // The guard in incrementActiveVMs makes this idempotent - if VM was already
    // counted (entry services succeeded), this does nothing.
    {
        Locker lock { m_worldLock };
        // Guard against a late-arriving notifyVMStop() after resumeTheWorld() has already
        // completed. Once the world is back in RunAll, touching the counters would corrupt
        // the m_numberOfStoppedVMs == m_numberOfActiveVMs invariant used in the following stop section.
        if (m_worldMode == Mode::RunAll)
            return;

        incrementActiveVMs(vm);
        ++m_numberOfStoppedVMs;

        // Must be inside this lock. Once m_numberOfStoppedVMs == m_numberOfActiveVMs,
        // the STW callback fires and the debugger assumes isStopped() on every VM.
        // A VM preempted between releasing this lock and calling setStopped() would
        // cause isStopped() to return false after the STW callback fires.
#if ENABLE(WEBASSEMBLY_DEBUGGER)
        if (Options::enableWasmDebugger()) [[unlikely]]
            vm.debugState()->setStopped();
#endif
    }

    // THREADS-INTEGRATE(heap) manifest 5a: GC park hook (entry side). Called
    // with NO m_worldLock held (the hook takes heap locks; L6). Heap-owned,
    // idempotent: no-op unless ISS && GSP && this VM's client holds access.
    if (auto willParkHook = s_gcWillParkInStopTheWorld.load()) [[unlikely]]
        willParkHook(vm);

    // Due to races, we may end up calling notifyVMStop() even when there is no stop to be serviced.
    // It should always be safe to call notifyVMStop() as many times as we like. The only cost is
    // is performance.
    //
    // In Mode::RunOne, we will call notifyVMStop() even if there are no requested stops. The code
    // below will simply determine that there's nothing to do and return back out. This is fine
    // since Mode::RunOne is only used by debuggers, and peek performance is not a concern.
    // We need to ensure that StopTheWorld VMTraps remained installed and that notifyVMStop() gets
    // called when in Mode::RunOne because new VM thread can be started, and we want those new
    // threads to also stop since they aren't the targetVM thread.


    for (;;) {
        {
            Locker lock { m_worldLock };

            RELEASE_ASSERT(m_numberOfStoppedVMs <= m_numberOfActiveVMs);

            auto fetchTopPriorityStopReason = [&] {
                auto pendingRequests = m_pendingStopRequestBits.loadRelaxed();
                // THREADS-INTEGRATE(heap) manifest 5c: the GC bit is never
                // latched/serviced by notifyVMStop — it only keeps VMs
                // parked (5b). The conductor resumes them via
                // requestResumeAll(GC) (5e).
                pendingRequests &= ~static_cast<StopRequestBits>(StopReason::GC);
                for (unsigned i = 0; i < NumberOfStopReasons; ++i) {
                    auto requestToCheck = static_cast<StopRequestBits>(1 << i);
                    if (pendingRequests & requestToCheck)
                        return static_cast<StopReason>(requestToCheck);
                }
                return StopReason::None;
            };

            auto shouldStop = [&] WTF_REQUIRES_LOCK(m_worldLock) {
                // THREADS-INTEGRATE(heap) manifest 5b: a pending GC stop
                // keeps every VM parked until requestResumeAll(GC) — the GC
                // bit is never latched into m_currentStopReason (5c).
                if (m_pendingStopRequestBits.loadRelaxed() & static_cast<StopRequestBits>(StopReason::GC))
                    return true;

                // 1. If the targetVM is already selected, and we're not the targetVM, then stop.
                //    We need to check this first because in RunOne mode, even if there is no more
                //    STW request to service, any VM that is not the targetVM still needs to stop.
                if (m_targetVM) {
                    if (m_targetVM != &vm)
                        return true;
                    // UNGIL §A.3.8 (U-T5): the target's service/RunOne run
                    // must not begin while a gilOff sibling mutator still
                    // holds heap access (transient by construction: the trap
                    // fan drives every sibling to its access-released park,
                    // and §A.3.2b gates re-acquisition).
                    return gilOffMutatorsBlockModeStopService();
                }

                // 2. If there's no more STW requests, then we don't need to stop.
                //    This is superseded by the condition above during RunOne mode.
                if (m_currentStopReason == StopReason::None)
                    return false;

                // UNGIL §A.3.8 (U-T5): "Mode keyed on all-parked / released /
                // not-entered" — a latched request is serviceable only once
                // every gilOff entered thread other than this one is
                // access-released (siblings are not represented in the
                // per-VM counters; this conjunct is their stopping-point
                // evidence). GIL-on processes: compiled to one static-flag
                // load (VM::isGILOffProcess()).
                if (gilOffMutatorsBlockModeStopService())
                    return true;

                // 3. We have a STW request. If not all active VMs are at the stopping point yet,
                //    then stop and wait for the last VM to stop.
                //    FIXME: rdar://173360944 Any VM may serve the STW callback, not just the last to stop,
                //    since once the counter lock above is released, any VM can observe m_numberOfStoppedVMs == m_numberOfActiveVMs.
                return m_numberOfStoppedVMs != m_numberOfActiveVMs;
            };

            // Fetch the top priority stop request and finish servicing it
            // before entertaining another one. THREADS-INTEGRATE(heap)
            // manifest 5f: the fetch precedes the FIRST shouldStop() AND
            // re-runs after every wake — a stop request that arrives while
            // we are parked must be latched by SOME stopped VM or no one
            // services it.
            bool calledGCParkHook = false;
            bool ranGCResumeHook = false;
            for (;;) {
                if (m_currentStopReason == StopReason::None)
                    m_currentStopReason = fetchTopPriorityStopReason();
                if (!shouldStop()) {
                    // THREADS-INTEGRATE(heap) manifest 5g(iii) (review round
                    // 3): if a GC park hook released this VM's heap access —
                    // EITHER the entry-side 5a insertion-A call (GC bit
                    // already pending when we parked) OR the mid-park 5g(i)
                    // call below — re-acquire it BEFORE leaving the wait
                    // loop. The dispatch below may run a non-GC STW callback
                    // (wasmDebuggerOnStop / memoryDebuggerStopTheWorld —
                    // stop reasons that never take the heap's GCL
                    // JSThreadsStopScope bracket) which reads, or even
                    // allocates from, the heap; with access still released a
                    // shared-mode conductor's §10.4 barrier would treat this
                    // VM as not-accessing and collect/sweep under the
                    // callback's feet (and any allocation would violate I2).
                    // The hook is idempotent via m_releasedByGCPark (a no-op
                    // when nothing was released, so gating on ranGCResumeHook
                    // rather than calledGCParkHook is safe AND necessary —
                    // the entry-side release never sets calledGCParkHook) and
                    // F8-blocks if a NEW GC stop pends; calledGCParkHook is
                    // reset and we re-evaluate (continue) so a GC bit that
                    // arrived during the re-acquire re-runs 5g(i) instead of
                    // leaving the new conductor's barrier waiting on us.
                    auto didResumeHook = s_gcDidResumeFromStopTheWorld.load();
                    if (!ranGCResumeHook && didResumeHook) [[unlikely]] {
                        ranGCResumeHook = true;
                        calledGCParkHook = false;
                        {
                            DropLockForScope dropper(lock);
                            didResumeHook(vm);
                        }
                        continue;
                    }
                    break;
                }
                // THREADS-INTEGRATE(heap) manifest 5g(i): a VM that parked
                // BEFORE the GC stop was requested (e.g. for a debugger
                // stop) still holds heap access, and its entry-side 5a hook
                // ran when no GC bit existed — it must release access NOW or
                // the conductor's §10.4 barrier never completes
                // (§10C(a)/(c)/(d)). The hook must run without m_worldLock
                // (it takes heap locks); after re-taking the lock we
                // re-evaluate (continue) rather than wait, because the 5e
                // resume-notify may have fired while the lock was dropped.
                // calledGCParkHook bounds this to one call per GC stop (the
                // hook is idempotent — m_releasedByGCPark — so a re-fire
                // would be harmless but would busy-spin the loop); 5g(iii)
                // resets it after the matching re-acquire so a LATER GC bit
                // in the same notifyVMStop invocation re-runs this block,
                // and this block resets ranGCResumeHook so the matching
                // 5g(iii) re-acquire runs again at the next loop exit.
                bool gcBitPending = m_pendingStopRequestBits.loadRelaxed() & static_cast<StopRequestBits>(StopReason::GC);
                auto willParkHook = s_gcWillParkInStopTheWorld.load();
                if (gcBitPending && !calledGCParkHook && willParkHook) [[unlikely]] {
                    calledGCParkHook = true;
                    ranGCResumeHook = false;
                    {
                        DropLockForScope dropper(lock);
                        willParkHook(vm);
                    }
                    continue;
                }
                // UNGIL §A.3/§A.3.8 (U-T5): a gilOff representative parks
                // access-released, ALWAYS — a §A.3 conductor's predicate and
                // any other thread's service-gating conjunct sample access
                // states, and a representative sleeping with access held
                // would wedge both. Idempotent vs the 5a/5g(i) GC hooks
                // (whichever releases first wins; both resume paths pair on
                // their own flag). Runs without m_worldLock (it takes heap
                // locks).
                if (vm.gilOff()) [[unlikely]] {
                    DropLockForScope dropper(lock);
                    gcClientWillParkForThreadGranularStop();
                    jsThreadsNotifyMutatorQuiesced();
                }
                if (VM::isGILOffProcess()) [[unlikely]] {
                    // Bounded wait (U-T5): gilOff-process stop coordination
                    // has wakeup edges (the §A.3 stop word, sibling access
                    // releases, representative tenure) that deliberately do
                    // not plumb into this condition; the timeout keeps the
                    // predicate re-evaluation live. GIL-on processes keep
                    // the landed untimed wait.
                    m_worldConditionVariable.waitFor(m_worldLock, Seconds::fromMilliseconds(1));
                } else
                    m_worldConditionVariable.wait(m_worldLock);
            }

            // We can only get here under one the following possible circumstance:
            // 1. No targetVM thread was specified (therefore, any thread may service this stop)
            //    and this is the last thread that stopped. Or ...
            // 2. This is a subsequent iteration through this loop after context switches (see the
            //    m_worldConditionVariable.notifyAll() at the bottom of the loop). In which case,
            //    the targetVM thread is the only one that can get past the wait() above. Or ...
            // 3. We're executing in RunOne mode and entering this function due to a subsequent
            //    stop request. In that case, all other threads remained stopped, and only the
            //    targetVM thread is allowed to run.
            RELEASE_ASSERT(!m_targetVM || m_targetVM == &vm);

            // Now we can break out of the handler loop is there are no more requests.
            if (m_currentStopReason == StopReason::None) {
                if (m_useRunOneMode) {
                    m_worldMode = Mode::RunOne;
                    RELEASE_ASSERT(m_targetVM);
                } else if (m_worldMode != Mode::RunAll)
                    resumeTheWorld(); // Sets m_worldMode = Mode::RunAll.
                break; // Exit this loop.
            }

            m_targetVM = &vm;
            m_worldMode = Mode::Stopped;
        }

        // UNGIL §A.3.8 (U-T5): a gilOff representative re-acquires its own
        // client's access before servicing — the STW callback may read or
        // allocate from the heap (the same reasoning as the 5g(iii)
        // re-acquire above, for the thread-granular release pairing). The
        // F8/§A.3.2b gates absorb any freshly-arrived stop.
        if (vm.gilOff()) [[unlikely]]
            gcClientDidResumeFromThreadGranularStop();

        auto status = STW_RESUME();
        switch (m_currentStopReason) {
        case StopReason::GC:
            RELEASE_ASSERT_NOT_REACHED();
        case StopReason::WasmDebugger:
            status = g_jscConfig.wasmDebuggerOnStop(vm, event);
            break;
        case StopReason::MemoryDebugger:
            status = g_jscConfig.memoryDebuggerStopTheWorld(vm, event);
            break;
#if USE(BUN_JSC_ADDITIONS)
        case StopReason::JSDebugger:
            status = g_jscConfig.jsDebuggerStopTheWorld(vm, event);
            break;
#endif
        case StopReason::None:
            RELEASE_ASSERT_NOT_REACHED();
        }

        if (status.first == IterationStatus::Done) {
            // Done servicing this request. We can't just exit the loop here yet because there
            // may be other requests that need to be serviced. So, we'll just clear the
            // current request and go back to the top of the loop to check if there are other
            // requests. It's safe to clear m_currentStopReason without acquiring m_worldLock
            // here because currently, all other VM threads are already stopped.
            // Same reason for why it's safe to set m_useRunOneMode here.
            auto requestBits = static_cast<StopRequestBits>(m_currentStopReason);
            m_pendingStopRequestBits.exchangeAnd(~requestBits);
            if (m_currentStopReason == StopReason::WasmDebugger)
                m_needsWasmDebuggerOnResume.store(true);
            m_currentStopReason = StopReason::None;

            // targetVM not being specified means that we should not change m_useRunOneMode.
            if (status.second)
                m_useRunOneMode = status.second != STW_RESUME_ALL_TOKEN;
        }

        if (status.second && status.second != STW_RESUME_ALL_TOKEN && status.second != m_targetVM) {
            // A context switch was requested. Wake all so that a context switch can occur, and
            // continue on the targetVM thread.
            Locker lock { m_worldLock };
            m_targetVM = status.second;
            m_worldConditionVariable.notifyAll();
        }
    }

    unsigned numberOfStoppedVMs = UINT_MAX;

    {
        Locker lock { m_worldLock };

        // If we get here, we're either transitioning to RunOne or Running mode.
        RELEASE_ASSERT(!m_targetVM || m_targetVM == &vm);

        numberOfStoppedVMs = --m_numberOfStoppedVMs;

#if ENABLE(WEBASSEMBLY_DEBUGGER)
        if (Options::enableWasmDebugger()) [[unlikely]] {
            // WasmAtomicsWaitBlocked: thread is still sleeping inside memory.atomic.wait and
            // may participate in multiple STW cycles. Keep stopData so each cycle shows the
            // correct state; waitForSync() calls clearStop() when the wait ends.
            if (event != StopTheWorldEvent::WasmAtomicsWaitBlocked)
                vm.debugState()->clearStop();
        }
#endif
    }

    // THREADS-INTEGRATE(heap) manifest 5a: GC park hook (resume side). NO
    // m_worldLock held. Heap-owned, idempotent: iff m_releasedByGCPark ->
    // re-acquire heap access (F8-blocking if a NEW stop pends), then clear.
    if (auto didResumeHook = s_gcDidResumeFromStopTheWorld.load()) [[unlikely]]
        didResumeHook(vm);

    // Call post-resume callback once when last VM exits and all VMs are running.
    if (!numberOfStoppedVMs && m_needsWasmDebuggerOnResume.exchange(false))
        g_jscConfig.wasmDebuggerOnResume();
}

void VMManager::notifyVMConstruction(VM& vm)
{
    bool needsStopping = false;
    {
        Locker locker { m_worldLock };
        s_recentVM = &vm;
        m_vmList.append(vm.threadContext());
        m_numberOfVMs++;
        needsStopping = m_worldMode != Mode::RunAll;
    }
    if (needsStopping) {
        // If a stop is in progress, we cannot proceed onto initializing (i.e. mutating)
        // the heap in the VM constructor. GlobalGC may be expecting a quiescent world
        // state at this point. So, go park this thread if needed.
        notifyVMStop(vm, StopTheWorldEvent::VMCreated); // Cannot be called while holding m_worldLock.

        Locker locker { m_worldLock };
        decrementActiveVMs(vm);
    }
}

void VMManager::notifyVMDestruction(VM& vm)
{
    bool worldIsStopped = false;
    {
        Locker locker { m_worldLock };
        if (s_recentVM == &vm)
            s_recentVM = nullptr;
        m_vmList.remove(vm.threadContext());
        m_numberOfVMs--;
        // §A.3.8 tenure sweep: a representative entry normally removes
        // itself; this covers a VM destroyed while its entry lingers.
        if (vm.gilOff()) [[unlikely]]
            gilOffServicingThreads().remove(&vm);

        worldIsStopped = (m_worldMode != Mode::RunAll);
    }
    if (worldIsStopped) {
        // If a stop is in progress, some threads may have stopped, and may need to be
        // woken up.
        handleVMDestructionWhileWorldStopped(vm);
    }
}

void VMManager::notifyVMActivation(VM& vm)
{
    // The main concern for this notification is that if we are currently Stopping or Stopped,
    // then we need to block this newly activated VM from executing.
    bool needsStopping = false;
    {
        Locker lock { m_worldLock };
        s_recentVM = &vm;
        needsStopping = m_worldMode != Mode::RunAll;
    }
    if (needsStopping)
        notifyVMStop(vm, StopTheWorldEvent::VMActivated);
}

void VMManager::notifyVMDeactivation(VM& vm)
{
    // The main concern for this notification is that if we are currently Stopping or Stopped,
    // then we may need to wake up another thread to potentially service the StopTheWorld
    // request. That's because this may be the last thread that STW is waiting on.
    Locker lock { m_worldLock };
    decrementActiveVMs(vm);
}

void VMManager::handleVMDestructionWhileWorldStopped(VM& vm)
{
    Locker lock { m_worldLock };
    if (m_worldMode == Mode::RunAll) {
        // World has been resumed already. Nothing more to do.
        return;
    }

    if (!m_numberOfVMs) {
        // We're the last VM, and we're about to shutdown. So, there's nothing to
        // resume. Fix m_worldMode to reflect this.
        m_worldMode = Mode::RunAll;
        return;
    }

    // If we get here, then the world is either in Stopping / Stopped / RunOne state,
    // and there's at least one other VM thread in play out there. Wake them up so
    // that the right thread can take next step.
    if (m_targetVM) {
        // There's a designated targetVM thread to continue in, but we don't have the
        // ability to just wake the desired one up. So, wake up all the threads and let
        // them sort themselves out.
        //
        // But if the targetVM thread is this thread, then pass the control to another
        // thread, any thread. That's because this thread is dying imminently.
        if (m_targetVM == &vm) {
            m_targetVM = nullptr;
            m_useRunOneMode = false;
        }
        m_worldConditionVariable.notifyAll();
    } else {
        // There's no designated targetVM thread. So, just waking up any one thread will do.
        m_worldConditionVariable.notifyOne();
    }
}

} // namespace JSC
