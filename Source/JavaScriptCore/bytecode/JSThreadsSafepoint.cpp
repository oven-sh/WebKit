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
#include "JSThreadsSafepoint.h"

#include "Heap.h"
#include "HeapInlines.h"
#include "MachineContext.h" // B16 watchdog triage: foreign-thread PC/FP capture for the fail-stop backtrace dump.
#include "MachineStackMarker.h" // T5-rootscan-skip: DECLARE_AND_COMPUTE_CURRENT_THREAD_STATE at the class-(2) park.
#include "VM.h"
#include "VMLiteShared.h" // VMLiteRegistry: watchdog timeout participant dump.
#include "VMManager.h"
#include <atomic>
#include <optional>
#include <wtf/Atomics.h>
#include <wtf/DataLog.h>
#include <wtf/IterationStatus.h>
#include <wtf/RecursiveLockAdapter.h> // B16 watchdog triage: gilOffCompilationLock / staticPropertyReificationLock isOwner() probe.
#include <wtf/Seconds.h>
#include <wtf/Threading.h> // B16 watchdog triage: ThreadSuspendLocker / PlatformRegisters for the fail-stop backtrace dump.

// Pre-M4 stub witness of the object-model workstream (SPEC-objectmodel manifest
// entry 6 / SPEC-jit section 5.6 disjunct 4, CS6). The witness global
// g_jsThreadsStubWorldStopped is owned by runtime/ConcurrentButterfly.cpp; we
// read it only when that workstream has landed AND its header advertises the
// witness. Deleted at M4 integration (the disjunct goes away entirely).
// THREADS-INTEGRATE(jit): if ConcurrentButterfly.h declares the witness without
// defining JSC_OM_PROVIDES_JSTHREADS_STUB_WITNESS, the integrator either adds
// that define next to the declaration or (preferred, CS6) has the OM veneer
// delegate to stopTheWorldAndRun below, which makes this disjunct redundant
// (our own depth counter then covers OM stop windows too).
#if __has_include("ConcurrentButterfly.h")
#include "ConcurrentButterfly.h"
#endif

#if __has_include("HeapClientSet.h")
// Heap workstream landed (SPEC-heap manifest): Heap::worldIsStoppedForAllClients()
// is available (SPEC-heap F7). THREADS-INTEGRATE(jit)
#define JSC_JIT_HAS_SHARED_HEAP_SERVER 1
#endif

namespace JSC {

// UNGIL §A.3 thread-granular conductor (VMManager.cpp; same-library seam
// redeclarations — VMManager.h is not the owner of these per the U-T5 record).
// stopTheWorldAndRun below routes EVERY gilOff Class-A request here (the
// §A.3.3 licensed reroute): the interim stub's soundness premise ("at most
// one entered mutator") does not hold for N entered threads of one VM, and
// its entered-VM tripwire counts VMs, not threads, so it would PASS and run
// `work` inline while sibling mutators execute the very code being patched.
void jsThreadsThreadGranularStopTheWorldAndRun(VM&, const ScopedLambda<void()>&);
bool jsThreadsThreadGranularWorldIsStopped();
// FIX-2 (stw-watchdog-timeout): additional same-library §A.3 seams consumed by
// parkSitePollAndParkForStopTheWorld below. Owners: VMManager.cpp (stop word,
// conductor tenure, sampler wakeup, NVS ticket) and heap/Heap.cpp (per-thread
// park access pairing) — same U-T5 seam discipline as the two above.
bool jsThreadsStopPendingFor(VM&);
bool jsThreadsCurrentThreadIsStopConductor();
void jsThreadsNotifyMutatorQuiesced();
void jsThreadsParkForStopWindow(VM&);
void gcClientWillParkForThreadGranularStop();
void gcClientDidResumeFromThreadGranularStop();
bool gcClientReleaseAccessAndBlockForPendingSharedGCStop();
// T5-rootscan-skip-coop-parked-suspend (heap/Heap.cpp): coop root snapshot
// publish/clear bracketing the class-(2) ticket park below.
void gcClientPublishParkedRootSnapshot(CurrentThreadState*);
void gcClientClearParkedRootSnapshot();

// B16 (CVE-AUDIT Tier-B / map-MC-SAFE cross-family): the two long-hold
// recursive locks a §A.3 conductor can be holding when it requests an OM
// transition stop (FIX-2 mech (1) "escaped lock-holding fireAll caller"
// candidates). Owners: runtime/ScriptExecutable.cpp,
// runtime/Lookup.cpp. Consumed only on the watchdog fail-stop path
// (isOwner() probe — a held lock here means a contended waiter on that lock
// is the prime suspect for the non-quiescent lite). Same-library seam.
RecursiveLock& gilOffCompilationLock();
RecursiveLock& staticPropertyReificationLock();

namespace JSThreadsSafepoint {

#if defined(JSC_HEAP_HAS_STW_FORBIDDEN_SCOPE)
// R4-1 (review round 4): heap-access release for the R1.i bracket, scoped to
// the CALLING VM's GCClient::Heap — not Heap::releaseAccess() on the server.
// Per heap §10A, once a server is ISS heap access is tracked per client, and
// the server-level releaseAccess() forwards to the MAIN client, which is the
// wrong client whenever the stop requester is a non-main client of a shared
// server. GCClient::Heap::{release,acquire}HeapAccess always act on the
// requester's own client (and coincide with the forwarded path when the
// requester IS the main client). Destruction re-acquires access, blocking if
// a shared-mode GC stop is pending (heap F8) — the spec's resume order.
class ClientHeapAccessReleaseScope {
    WTF_MAKE_NONCOPYABLE(ClientHeapAccessReleaseScope);
public:
    explicit ClientHeapAccessReleaseScope(GCClient::Heap& client)
        : m_client(client)
    {
        m_client.releaseHeapAccess();
    }

    ~ClientHeapAccessReleaseScope()
    {
        m_client.acquireHeapAccess();
    }

private:
    GCClient::Heap& m_client;
};
#endif // defined(JSC_HEAP_HAS_STW_FORBIDDEN_SCOPE)

// Raised while a stub stopTheWorldAndRun closure runs on this process. A plain
// global (not thread-local) is correct for the interim stub: the phase-1 GIL
// guarantees at most one entered mutator, which the RELEASE_ASSERT below
// enforces. Atomic only so concurrent compiler/GC threads reading
// worldIsStopped() are race-free.
static std::atomic<unsigned> s_stubWorldStoppedDepth { 0 };

// Review rounds 2 and 3 (R2-4, revised by R3-11): raising the PROCESS-GLOBAL
// stub witness on the strength of already-stopped evidence is only sound if
// that evidence in fact covers every mutator in the process. Two of the
// worldIsStopped(vm) disjuncts are per-heap:
//   - vm.heap.worldIsStopped(): the legacy GC stop of THIS VM only. Sound to
//     globalize only under the phase-1 single-entered-VM premise; otherwise
//     (flag-on + Workers, pre-M4) another thread's VM-less
//     assertPatchingIsSafe() would pass spuriously and Class-A fires would run
//     inline while a foreign mutator executes.
//   - the shared-server all-clients stop (worldIsStoppedForAllClients()): a
//     GENUINE stop of every mutator of that server (heap F7) — its entered-
//     but-parked client VMs are legitimate and MUST NOT trip the count
//     (R3-11: pre-R3 this path RELEASE_ASSERTed on a perfectly legal
//     GC-end-finalizer jettison in the {useJSThreads=1, useSharedGCHeap=1,
//     N clients} config). Only an entered VM OUTSIDE the stopped server's
//     client set breaks the premise.
// So: count entered VMs, excluding clients of a shared server that is
// currently stopped for all clients. Without such a stop, the caller itself
// may be the one entered VM (<= 1); with one, every OTHER heap's entered VM
// is genuinely concurrent (== 0 allowed besides none — the caller's VM is a
// client of the stopped server and is excluded).
//
// R3-4 caveat: this count is SAMPLED (check-then-act) — a thread can enter
// another VM right after it. It is a tripwire for misconfiguration, not the
// soundness mechanism; the structural enforcement point is VM entry (manifest
// M7's VMEntryScope entered-VM counter, which crashes the second ENTERING
// thread deterministically). See the matching note in JSThreadsSafepoint.h.
static void assertAlreadyStoppedEvidenceCoversEveryMutator(VM& vm)
{
#if defined(JSC_JIT_HAS_SHARED_HEAP_SERVER)
    JSC::Heap* stoppedServer = nullptr;
    {
        JSC::Heap& server = vm.clientHeap.server();
        if (server.isSharedServer() && server.worldIsStoppedForAllClients())
            stoppedServer = &server;
    }
#else
    UNUSED_PARAM(vm);
    constexpr void* stoppedServer = nullptr;
#endif
    unsigned enteredVMsNotCoveredByStop = 0;
    VMManager::forEachVM([&](VM& candidate) {
        if (!candidate.isEntered())
            return IterationStatus::Continue;
#if defined(JSC_JIT_HAS_SHARED_HEAP_SERVER)
        if (stoppedServer && &candidate.clientHeap.server() == stoppedServer)
            return IterationStatus::Continue; // Parked client of the stopped shared server (R3-11).
#endif
        ++enteredVMsNotCoveredByStop;
        return IterationStatus::Continue;
    });
    // With a stopped shared server covering the caller's clients, ANY other
    // entered VM is genuinely concurrent (count must be 0); otherwise the one
    // allowed entered VM is the caller itself.
    RELEASE_ASSERT(enteredVMsNotCoveredByStop <= (stoppedServer ? 0u : 1u));
}

AlreadyStoppedWorldWitnessScope::AlreadyStoppedWorldWitnessScope(VM& vm)
{
    ASSERT(worldIsStopped(vm));
    // Scope of the tripwire: only when NO process-global witness holds — under
    // a genuine all-VM stop (VMManager::Mode::Stopped, e.g. the wasm debugger,
    // or an outer stopTheWorldAndRun closure / witness scope whose own entry
    // already passed this check) multiple entered-but-parked VMs are
    // legitimate and the global witness is already truthfully process-wide.
    // VMManager::forEachVM needs no API lock, matching the R1.h no-API-lock
    // contract.
    if (!worldIsStopped())
        assertAlreadyStoppedEvidenceCoversEveryMutator(vm);
    s_stubWorldStoppedDepth.fetch_add(1, std::memory_order_relaxed);
}

AlreadyStoppedWorldWitnessScope::~AlreadyStoppedWorldWitnessScope()
{
    // F5 (stub form): the patcher's own instruction-stream barrier after any
    // cross-modifying code write inside the scope. The data-side flush is
    // performed by the patching primitives themselves; under M4 the
    // resume-path NVS-exit hook (R1.d) issues the per-mutator ISB for
    // JSThreads AND GC stops. // THREADS-INTEGRATE(jit)
    WTF::crossModifyingCodeFence();
    s_stubWorldStoppedDepth.fetch_sub(1, std::memory_order_relaxed);
}

// R1.h foreign-thread guard (review r33): the "already stopped" disjuncts of
// worldIsStopped()/worldIsStopped(VM&) MINUS the §A.3 process-global
// thread-granular window witness. Keep in sync with those two predicates.
static bool worldIsStoppedEvidenceExcludingThreadGranularWindow(VM& vm)
{
    if (s_stubWorldStoppedDepth.load(std::memory_order_relaxed))
        return true;
#if defined(JSC_OM_PROVIDES_JSTHREADS_STUB_WITNESS)
    if (g_jsThreadsStubWorldStopped)
        return true;
#endif
    if (VMManager::info().worldMode == VMManager::Mode::Stopped)
        return true;
    if (vm.heap.worldIsStopped())
        return true;
#if defined(JSC_JIT_HAS_SHARED_HEAP_SERVER)
    if (vm.clientHeap.server().worldIsStoppedForAllClients())
        return true;
#endif
    return false;
}

// cve-structureid-decontaminate-stop (corpus: mc-safe-gcwait-vs-classa-stop):
// already-stopped evidence that is THREAD-STABLE for the current caller —
// evidence that cannot evaporate while `work` runs on this stack: the
// process-global stub depth (raised by this stack or an outer closure whose
// own entry passed the tripwire), the OM stub witness, and the VMManager
// all-VM stop. Deliberately EXCLUDES (a) the SectionA.3 thread-granular window —
// inline licensing for that belongs to its conductor alone (r33 guard + the
// conductor early-out at the call site) — and (b) the per-heap GC-stop
// disjuncts (vm.heap.worldIsStopped() / worldIsStoppedForAllClients()),
// whose stability is exactly what the GC-conduction check at the call site
// establishes: those stops are ended by the GC conductor, which clears WSAC
// and resumes its clients (the gcwait resume edge) WITHOUT consulting our
// witness depth.
static bool worldIsStoppedEvidenceIsThreadStable()
{
    if (s_stubWorldStoppedDepth.load(std::memory_order_relaxed))
        return true;
#if defined(JSC_OM_PROVIDES_JSTHREADS_STUB_WITNESS)
    if (g_jsThreadsStubWorldStopped)
        return true;
#endif
    return VMManager::info().worldMode == VMManager::Mode::Stopped;
}

// ===== checktraps-dejank-invalidation-point: conductor heap-fact rewrite epoch =====
// See the header comment (runtime/VMTraps.h). Process-global: stop windows are
// process-rare and a false-positive bump only costs an on-stack jettison, so
// per-VM precision is not worth the plumbing. acq_rel/acquire keeps the
// counter itself coherent; the cross-thread ordering guarantee rides the
// park/resume edge.
//
// BUMP-EDGE LAW (review blockers, amend round): the load-bearing bump for a
// thread-granular (§A.3) window happens IN-WINDOW, AFTER the window's work and
// BEFORE the world resumes (the wrapped closure in stopTheWorldAndRun's gilOff
// reroute below). A publication-time (ClassAStopWatchdogContext ctor) bump is
// NOT sufficient on its own: a mutator parked BY the window samples the epoch
// in VMTraps::handleTraps strictly AFTER the publication bump (the trap bits
// that send it there are set after the ctor runs), so its exit compare would
// see an unchanged epoch — exactly the victim class the mechanism exists for.
// The ctor bump is kept as the entry-edge half (it covers mutators that were
// ALREADY inside handleTraps when the window published, plus the GIL-on legs),
// and the dtor bump covers windows that ran INLINE under an outer
// already-stopped world (there the requester's dtor runs before the OUTER
// stop's resume edge, so it IS pre-resume for every mutator that outer stop
// parked). Defined here, above stopTheWorldAndRun, because the reroute branch
// references them.
static std::atomic<uint64_t> s_conductorHeapFactRewriteEpoch { 0 };

// Thread-local nesting depth for PureCodeLifecycleStopWindowScope. Plain
// (non-atomic): written and read only by the owning thread.
static thread_local unsigned t_pureCodeLifecycleStopWindowDepth { 0 };

uint64_t conductorHeapFactRewriteEpoch()
{
    return s_conductorHeapFactRewriteEpoch.load(std::memory_order_acquire);
}

void noteConductorHeapFactRewrite()
{
    s_conductorHeapFactRewriteEpoch.fetch_add(1, std::memory_order_acq_rel);
}

PureCodeLifecycleStopWindowScope::PureCodeLifecycleStopWindowScope()
{
    ++t_pureCodeLifecycleStopWindowDepth;
}

PureCodeLifecycleStopWindowScope::~PureCodeLifecycleStopWindowScope()
{
    ASSERT(t_pureCodeLifecycleStopWindowDepth);
    --t_pureCodeLifecycleStopWindowDepth;
}

void stopTheWorldAndRun(VM& vm, const ScopedLambda<void()>& work)
{
    // R1.h FIRST (load-bearing for SPEC-jit section 5.3, Task 5): a caller that
    // is ALREADY world-stopped — a jettison reached from a GC's stopped window
    // (legacy per-VM stop or shared-server stop), from inside an outer
    // stopTheWorldAndRun closure (e.g. a Class-A watchpoint fire's section 5.6
    // step 5 jettisons), or from the object-model stub's witness window — runs
    // `work` inline without re-requesting a stop. We still bump the depth
    // counter around it so the VM-less worldIsStopped() witness (consumed by
    // the VM-less patching asserts in DFG::CommonData::invalidateLinkedCode and
    // DFG::JumpReplacement::fire) holds across the closure even when the
    // "already stopped" evidence is per-heap state those asserts cannot see.
    // No API-lock or GIL assertion here: e.g. GC-end finalizers jettisoning
    // dead-weak-reference code run with the collector's stop as their safety
    // argument, not the caller contract of the requesting path below.
    if (worldIsStopped(vm)) {
        // R1.h foreign-thread guard (review r33): when the ONLY evidence is
        // the §A.3 process-global thread-granular window
        // (jsThreadsThreadGranularWorldIsStopped()), inline execution is
        // licensed solely for the CONDUCTOR thread — the §A.3 window
        // quiesces only entered lites of the target VM holding heap access,
        // so a non-participant thread (a worklist compiler thread's
        // finalize/install leg, another server's GC context, a mutator
        // between its access release and its park) that observes the global
        // depth and patched inline here would run concurrently with the
        // conductor's own work body: two unsynchronized patchers inside one
        // "stopped" window. Such a caller must queue at arbitration (the
        // requesting path below) instead — fail-stop, never patch.
        if (jsThreadsThreadGranularWorldIsStopped() && !jsThreadsCurrentThreadIsStopConductor()) [[unlikely]]
            RELEASE_ASSERT(worldIsStoppedEvidenceExcludingThreadGranularWindow(vm));
        // cve-structureid-decontaminate-stop (corpus:
        // mc-safe-gcwait-vs-classa-stop): stop-CONDUCTION check for the
        // per-heap GC-stop disjuncts, gilOff only. The legacy
        // vm.heap.worldIsStopped() and shared-server
        // worldIsStoppedForAllClients() evidence is conducted by the GC, not
        // by this caller: the GC conductor clears WSAC pre-resume and wakes
        // its clients (the gcwait resume edge) without consulting the
        // witness depth this branch is about to raise, so for a caller that
        // does not itself conduct that GC the "stopped world" can evaporate
        // mid-`work` — Class-A nuking/patching then races freshly resumed
        // mutators (observed: StructureID decontaminate assert via
        // JSCell::structure on a transiently-cleared cell, rope fiber-sum
        // assert, ASAN through JSC::call). Inline execution on GC-stop
        // evidence is licensed only for the thread CONDUCTING that GC, whose
        // stop cannot end underneath it because it is the thread that ends
        // it. Conduction is read from mutatorState(), per-calling-thread
        // once ISS (a shared-mode conductor marks only its own thread
        // Collecting/Sweeping via CollectingScope/SweepingScope; a worklist
        // compiler thread or a foreign mutator reads Running), OR from the
        // calling thread being a designated GC thread (collector/helper,
        // Thread::mayBeGCThread()) — CollectingScope exists only on the
        // mutator-conducted leg, so an End phase conducted by the concurrent
        // collector thread reads Running from the server slot yet is the
        // thread that ends the stop. Together this matches
        // Heap::currentThreadIsDoingGCWork() minus the Allocating disjunct
        // (an Allocating mutator does not conduct a GC stop). Every other
        // gilOff caller queues at SectionA.3 arbitration instead: the
        // thread-granular conductor takes the GC conductor lock (HBT4.5),
        // so it naturally waits out the in-flight GC before opening its own
        // window — fail-closed, never patch on evaporable evidence.
        if (vm.gilOff() && !jsThreadsCurrentThreadIsStopConductor()
            && !worldIsStoppedEvidenceIsThreadStable()) [[unlikely]] {
#if defined(JSC_JIT_HAS_SHARED_HEAP_SERVER)
            JSC::Heap& gcStopServer = vm.clientHeap.server();
#else
            JSC::Heap& gcStopServer = vm.heap;
#endif
            bool currentThreadConductsTheGCStop = Thread::mayBeGCThread();
            switch (gcStopServer.mutatorState()) {
            case MutatorState::Collecting:
            case MutatorState::Sweeping:
                currentThreadConductsTheGCStop = true;
                break;
            case MutatorState::Running:
            case MutatorState::Allocating:
                break;
            }
            if (!currentThreadConductsTheGCStop) {
                // checktraps-dejank-invalidation-point (amend round 2, review
                // blocker): this reroute CONDUCTS a fresh §A.3 window for a
                // Class-A nuking/patching caller exactly like the gilOff
                // reroute below, so it falls under the same BUMP-EDGE LAW
                // (comment above): the load-bearing bump must run IN-WINDOW,
                // after `work` and before the conductor publishes resume.
                // Handing the RAW `work` closure to the conductor here left
                // this branch's victims covered only by the requester's
                // ClassAStopWatchdogContext dtor bump — post-resume for a
                // conducted window — so a victim parked BY this window that
                // sampled the epoch post-publication could compare equal on
                // resume, skip the on-stack jettison, and reuse stale hoisted
                // butterfly/structure facts. Wrap identically to the gilOff
                // reroute (suppression depth is this requester's own
                // thread-local; the wrapped closure runs on this stack).
                auto workThenBumpHeapFactRewriteEpoch = scopedLambda<void()>([&] {
                    work();
                    if (!t_pureCodeLifecycleStopWindowDepth)
                        noteConductorHeapFactRewrite();
                });
                return jsThreadsThreadGranularStopTheWorldAndRun(vm, workThenBumpHeapFactRewriteEpoch);
            }
        }
        // Review rounds 2/3 (R2-4, R3-1, R3-11): the entered-VM tripwire, the
        // shared-server scoping, and the witness raise/lower + F5 barrier all
        // live in AlreadyStoppedWorldWitnessScope (shared with
        // WatchpointSet::fireAllUnderClassAStop branch (1), which fires inline
        // on the same kind of evidence). See the scope's comments above.
        AlreadyStoppedWorldWitnessScope witnessScope(vm);
        work();
        // checktraps-dejank-invalidation-point (amend round): this inline
        // `work` ran under an OUTER stopped world (GC stop, shared-server
        // stop, or an open §A.3 window). Bump the heap-fact rewrite epoch on
        // the way out: the outer stop's resume edge is still ahead of us (we
        // run on the conductor/collector of that stop, which resumes only
        // after this caller returns), so the bump is pre-resume for every
        // mutator that outer stop parked. Pure code-lifecycle callers
        // (GC-end finalizer jettisons via CodeBlock::jettison) are
        // suppressed via the requester-thread depth as everywhere else.
        if (vm.gilOff() && !t_pureCodeLifecycleStopWindowDepth) [[unlikely]]
            noteConductorHeapFactRewrite();
        return;
    }

    // UNGIL §A.3.3 LICENSED REROUTE (closes the blocker-grade OPEN item
    // recorded in VMManager.cpp at jsThreadsThreadGranularStopTheWorldAndRun):
    // gilOff Class-A requests take the real thread-granular conductor
    // (HBT4/SB1/ISB1 sequence, R1.a-i). The stub below remains the GIL-on
    // path only — its premise ("at most one entered mutator") and its
    // entered-VM tripwire are incompatible with N entered threads of one VM
    // (the tripwire counts VMs, so it would PASS under N threads and patch
    // code under running siblings). Nested fires inside an open
    // thread-granular window do not reach here: jsThreadsThreadGranular-
    // WorldIsStopped() feeds the worldIsStopped() disjunct above, so they
    // run inline under the witness scope (R1.h).
    if (vm.gilOff()) [[unlikely]] {
        // checktraps-dejank-invalidation-point (review blocker fix, amend
        // round — see the BUMP-EDGE LAW comment above): bump the conductor
        // heap-fact rewrite epoch IN-WINDOW, after `work` and strictly before
        // the conductor publishes resume (clears the stop word / wakes
        // tickets). Every mutator parked BY this window therefore observes
        // the bump on its resume edge: its handleTraps entry sample (taken
        // after the trap bits were set, i.e. after publication but before the
        // window's work) is ordered before this bump, and its exit compare is
        // ordered after the resume publication, which is ordered after this
        // bump — so the compare fires and the on-stack jettison runs. The
        // suppression depth is the REQUESTER's thread-local and `work` runs
        // on this same stack, so pure code-lifecycle windows
        // (CodeBlock::jettison) stay epoch-silent exactly as before. Nested
        // requests inside an open window run this wrapped closure inline on
        // the conductor (R1.h branch of the reroute) — a nested in-window
        // bump is pre-resume too, hence sound and merely redundant.
        auto workThenBumpHeapFactRewriteEpoch = scopedLambda<void()>([&] {
            work();
            if (!t_pureCodeLifecycleStopWindowDepth)
                noteConductorHeapFactRewrite();
        });
        return jsThreadsThreadGranularStopTheWorldAndRun(vm, workThenBumpHeapFactRewriteEpoch);
    }

    // INTERIM STUB until integration manifest M4 (SPEC-jit R1, Task 1).
    // Real sequence (R1.a-i), restored at integration:
    //   1. release this VM's heap access;
    //   2. Heap::JSThreadsStopScope over the GC conductor lock (CS2; no-op for
    //      a non-shared heap);
    //   3. VMManager::requestStopAllWithConductor(StopReason::JSThreads, &vm)
    //      and park in notifyVMStop; arbitration releases exactly this
    //      requester as conductor (R1.c);
    //   4. run `work` on this stack, world stopped;
    //   5. resume; every mutator leaving notifyVMStop executes an ISB (F5);
    //   6. drop the stop scope, re-acquire heap access.
    // Of these, steps 1-2 and 6 (the R1.i GC-serialization bracket) are live
    // below for shared-server heaps; only the actual stop/resume (steps 3 and
    // 5) and the requester-vs-requester park-aware mutex (R1.g) remain stubbed.
    // // THREADS-INTEGRATE(jit)

    // Caller must be an entered mutator (R1 contract).
    RELEASE_ASSERT(vm.currentThreadIsHoldingAPILock());

    // Phase-1 GIL: no second VM may be concurrently entered. This is the load-
    // bearing soundness argument for running `work` inline: with at most one
    // entered mutator, "the world" is exactly the calling thread. (We count
    // entered VMs directly; VMManager::info().numberOfActiveVMs is only
    // meaningful while a stop is in progress.)
    //
    // R3-4 (review round 3): this count is a SAMPLED tripwire (check-then-act)
    // — VM entry does not consult this stub, so a thread could enter another
    // VM after the count and before `work` completes. The STRUCTURAL
    // enforcement point pre-M4 is VM entry itself: manifest M7
    // (docs/threads/INTEGRATE-jit.md) adds a process-global entered-VM counter
    // to VMEntryScope that RELEASE_ASSERTs sole-entry under useJSThreads, so a
    // second concurrent entry crashes deterministically on the ENTERING thread
    // regardless of interleaving. Until M7 is applied, flag-on with more than
    // one concurrently-enterable VM is an unsupported configuration; this
    // sampled count merely catches it with high probability. Deleted at M4
    // (real parking makes entry during a stop park instead of crash).
    unsigned enteredVMs = 0;
    VMManager::forEachVM([&](VM& candidate) {
        if (candidate.isEntered())
            ++enteredVMs;
        return IterationStatus::Continue;
    });
    RELEASE_ASSERT(enteredVMs <= 1);

#if defined(JSC_HEAP_HAS_STW_FORBIDDEN_SCOPE)
    // R1.i (SPEC-jit section 5.3 / CS2, RESOLVED-AS-PROVIDED by heap manifest
    // 10b): bracket the ENTIRE stopped window for a shared-server heap —
    // release this VM's heap access FIRST (JSThreadsStopScope's precondition:
    // a conductor must never stop the world while still counted as a
    // heap-accessing mutator), then hold the rank-2 GC conductor lock across
    // `work` so no shared-mode GC can start or be mid-cycle while we patch
    // code. Destruction order is the spec's resume order: drop the stop scope
    // (unlock GCL), then re-acquire heap access. `work` runs without heap
    // access; it must not allocate in the JS heap (OM O4) — heap-metadata
    // WRITES without access are explicitly allowed (heap section 10A
    // exemption). NEVER calls bumpAndReclaim (G13/CS4): JSThreads stops
    // enqueue a GC request instead; reclamation rides the GC.
    // Non-shared heap: no-op per R1.i — today's jettisons already run with
    // heap access held, and the legacy concurrent collector tolerates that
    // exactly as it does in tip-of-tree.
    //
    // R4-1 (review round 4): the bracket is keyed on, and the stop scope taken
    // against, the SERVER heap this VM's client attaches to — NOT the VM's own
    // `heap` member. Under useSharedGCHeap a CLIENT VM's vm.heap is not the
    // shared server (m_isSharedServer is set on the server Heap only; see the
    // R3-11 notes at worldIsStopped(VM&) below and at
    // assertAlreadyStoppedEvidenceCoversEveryMutator above). Keying on vm.heap
    // silently skipped the whole bracket for every client of a shared server:
    // a reoptimization jettison or Class-A fire requested from such a client
    // would patch code with no GC conductor lock held while a shared-mode GC
    // could start or be mid-cycle on the server (the exact CS2 race). For the
    // 1:1 case server() == vm.heap, so behavior there is unchanged.
    // Heap-access release is client-scoped (see ClientHeapAccessReleaseScope
    // above). Note Heap::JSThreadsStopScope self-gates on !isSharedServer()
    // internally, so passing the resolved server is safe in every config.
    //
    // Gate interplay (recorded in docs/threads/INTEGRATE-jit.md, R4-1):
    // JSC_HEAP_HAS_STW_FORBIDDEN_SCOPE (defined by Heap.h itself) and
    // JSC_JIT_HAS_SHARED_HEAP_SERVER (defined at the top of this file iff
    // HeapClientSet.h is present) are independent #ifdefs. If the former held
    // without the latter, the fallback below keys on vm.heap — sound, because
    // without the heap workstream's client-set machinery no foreign-client
    // shared server can exist, so vm.heap is the only candidate server.
#if defined(JSC_JIT_HAS_SHARED_HEAP_SERVER)
    JSC::Heap& server = vm.clientHeap.server();
#else
    JSC::Heap& server = vm.heap;
#endif
    // Declaration order is load-bearing: destruction runs stop scope first
    // (unlock GCL), then re-acquires heap access — the spec's resume order.
    std::optional<ClientHeapAccessReleaseScope> releaseHeapAccess;
    std::optional<JSC::Heap::JSThreadsStopScope> jsThreadsStopScope;
    if (server.isSharedServer()) [[unlikely]] {
        // [r34] F-A item (2) (SPEC-ungil-history rev 34; carried by congc
        // CG-1): this bracket previously used the BLOCKING stop-scope ctor —
        // a raw GCL lock() with NO watchdog sampling, so a jettison requester
        // queued behind a wedged shared GC hung unwatched forever (or
        // surfaced as a queued bystander's nil-context 30s fire). Re-pointed
        // at the WATCHDOG ctor: requestStart is sampled strictly BEFORE the
        // ClientHeapAccessReleaseScope (reaching the bracket is part of
        // reaching a stopped world), so the whole release+GCL leg sits under
        // the standard 30s stop watchdog, and the ctor threads the target VM
        // into the timeout diagnostics (F-A item (3), heap-side).
        MonotonicTime requestStart = MonotonicTime::now();
        releaseHeapAccess.emplace(vm.clientHeap);
        jsThreadsStopScope.emplace(server, requestStart);
    }
#endif

    s_stubWorldStoppedDepth.fetch_add(1, std::memory_order_relaxed);
    work();
    // F5 (stub form): patcher-side instruction-stream barrier before any
    // possibility of this (sole) mutator re-entering JIT'd code. Under M4 this
    // is subsumed by the per-mutator ISB in the NVS resume tail (R1.d; M4's
    // fence first, then the heap's gcDidResumeFromStopTheWorld hook, manifest
    // 5a). // THREADS-INTEGRATE(jit)
    WTF::crossModifyingCodeFence();
    s_stubWorldStoppedDepth.fetch_sub(1, std::memory_order_relaxed);
}

bool worldIsStopped()
{
    if (s_stubWorldStoppedDepth.load(std::memory_order_relaxed))
        return true;

    // UNGIL §J.8: a §A.3 thread-granular window (VMManager.cpp conductor)
    // counts as a stopped world for the VM-less patching asserts and for the
    // R1.h inline-nested-fire branch of stopTheWorldAndRun above. This is
    // the disjunct the U-T5 record said this predicate "gains when the stub
    // is deleted"; it lands with the §A.3.3 reroute (the stub itself stays
    // for GIL-on callers).
    if (jsThreadsThreadGranularWorldIsStopped()) [[unlikely]]
        return true;

#if defined(JSC_OM_PROVIDES_JSTHREADS_STUB_WITNESS)
    // SPEC-jit section 5.6 disjunct 4 (pre-M4 only; deleted at M4, CS6).
    if (g_jsThreadsStubWorldStopped)
        return true;
#endif

    return VMManager::info().worldMode == VMManager::Mode::Stopped;
}

// ===== SPEC-jit section 5.6 stop watchdog (annex App. 5.6(d)) =====

// Generous: covers slow CI/ASAN/valgrind-grade parking latencies; an escaped
// lock-holding fire site wedges forever, so any finite bound catches it.
static constexpr Seconds stopTheWorldWatchdogTimeout { 30 };

// Thread-local so a wedged requester names the set IT is firing (concurrent
// requesters cannot misattribute). Plain (non-atomic) is correct: written and
// read only by the owning thread.
static thread_local const void* t_pendingClassAStopContext { nullptr };
static thread_local const char* t_pendingClassAStopContextDescription { nullptr };

ClassAStopWatchdogContext::ClassAStopWatchdogContext(const void* context, const char* description)
    : m_previousContext(t_pendingClassAStopContext)
    , m_previousDescription(t_pendingClassAStopContextDescription)
{
    t_pendingClassAStopContext = context;
    t_pendingClassAStopContextDescription = description;

    // checktraps-dejank-invalidation-point (amend round): every published
    // stop-window request is conservatively treated as a potential conductor
    // heap-fact rewrite (WatchpointSet Class-A fire / OM transition stop /
    // Debugger STW walk), EXCEPT pure code-lifecycle windows
    // (CodeBlock::jettison opens the suppression scope).
    //
    // THIS CTOR BUMP IS THE ENTRY-EDGE HALF ONLY — it is deliberately NOT the
    // load-bearing bump. A publication-time bump lands strictly BEFORE the
    // trap bits that park the window's own victims, so every mutator parked
    // BY this window samples the epoch post-bump in handleTraps and its exit
    // compare would see no change (the original "sequenced before the window
    // opens, hence before any parked mutator resumes" reasoning was inverted
    // — being before the window also puts it before those mutators' ENTRY
    // samples). What this bump DOES cover: (a) mutators already inside
    // handleTraps when the window published (entry sample pre-bump), and
    // (b) the GIL-on flag-on legs, belt-and-braces. The load-bearing GIL-off
    // bump is IN-WINDOW, pre-resume: stopTheWorldAndRun's gilOff reroute
    // wraps `work` to bump after the window's body and before the conductor
    // clears the stop word (see the BUMP-EDGE LAW comment above), and the
    // dtor below bumps for windows that ran inline under an outer stop.
    // Flag-off: contexts are only published flag-on, but gate anyway so an
    // accidental flag-off publication changes nothing.
    if (Options::useJSThreads() && !t_pureCodeLifecycleStopWindowDepth) [[likely]]
        noteConductorHeapFactRewrite();
}

ClassAStopWatchdogContext::~ClassAStopWatchdogContext()
{
    // checktraps-dejank-invalidation-point (amend round, blocker fix): the
    // EXIT-EDGE bump. For a request that ran INLINE under an outer
    // already-stopped world (WatchpointSet::fireAllUnderClassAStop branch
    // (1), nested fires inside an open window), this dtor runs after the
    // heap-fact rewrite completed and BEFORE the outer stop's resume edge —
    // i.e. pre-resume for every mutator that outer stop parked, which the
    // ctor bump cannot cover (their entry samples post-date it). For a
    // request that conducted its own §A.3 window this bump is post-resume
    // and therefore only belt-and-braces (the wrapped-closure in-window bump
    // in stopTheWorldAndRun is the one those victims observe); a post-resume
    // bump can at worst cause a spurious jettison on an unrelated
    // concurrently-parked thread — sound, perf-only. Same suppression gate
    // as the ctor.
    if (Options::useJSThreads() && !t_pureCodeLifecycleStopWindowDepth) [[likely]]
        noteConductorHeapFactRewrite();

    t_pendingClassAStopContext = m_previousContext;
    t_pendingClassAStopContextDescription = m_previousDescription;
}

void watchdogAssertStopProgress(MonotonicTime requestStart, VM* vm) WTF_IGNORES_THREAD_SAFETY_ANALYSIS // Manual bounded tryLock of the registry leaf lock below.
{
    if (MonotonicTime::now() - requestStart < stopTheWorldWatchdogTimeout) [[likely]]
        return;

    const void* context = t_pendingClassAStopContext;
    const char* description = t_pendingClassAStopContextDescription;
    dataLogLn("JSThreads stop-the-world failed to reach a stopped world within ",
        stopTheWorldWatchdogTimeout.seconds(), "s. Pending Class-A fire context: ",
        RawPointer(context), " (", description ? description : "<no Class-A fire pending on this thread>",
        "). Either an escaped lock-holding direct fireAll caller (SPEC-jit annex App. 5.6(c) bucket iii; Task-11 audit table in docs/threads/INTEGRATE-jit.md / manifest M6), or a mutator parked in a native wait that holds heap access without an access-release bracket or per-quantum parkSitePollAndParkForStopTheWorld poll (FIX-2 banner, mechanisms (1)/(2)). All stopTheWorldAndRun requesters publish a ClassAStopWatchdogContext (watchpoint fire / CodeBlock jettison / OM transition stop / Debugger STW), so a nil context here means the wedged requester is NOT this thread, or a new context-less call site escaped review.");

    // Review-round root-cause-B localization: name the participant(s) that
    // failed the §A.3.2 predicate. Same walk shape as the conductor's
    // per-sample predicate (registry leaf lock + SC fence; allocation-free),
    // run once on the way into the fail-stop, so the crash log identifies
    // WHICH entered lite still holds access instead of only the requester's
    // (often nil) Class-A context.
    // B16 triage (CVE-AUDIT Tier-B / map-MC-SAFE cross-family): collected
    // (tid, owning WTF::Thread*) for every NON-QUIESCENT lite, captured under
    // the registry lock and walked AFTER releasing it (the suspend/backtrace
    // below must not run while holding the registry leaf lock — the suspended
    // thread may be spinning on it, and the SIGUSR2 ack handshake would then
    // never complete). Fixed-size to stay allocation-free under the registry
    // lock; the watchdog only needs to name the wedge, so a few entries
    // suffice.
    constexpr unsigned watchdogMaxWedgedDump = 8;
    struct { uint16_t tid; WTF::Thread* thread; } wedged[watchdogMaxWedgedDump];
    unsigned numWedged = 0;

    if (vm) {
        VMLite* requesterLite = VMLite::currentIfExists();
        auto& registry = VMLiteRegistry::singleton();
        // tryLock with a short bounded spin, NOT a blocking Locker: this is
        // the watchdog's fail-stop path — if the wedge under diagnosis is a
        // thread stuck while HOLDING the registry leaf lock (or the lock is
        // corrupted by the same bug), an unconditional acquire would convert
        // the deterministic 30s crash into an indefinite hang, the exact
        // behavior this watchdog exists to prevent. Lose the dump, keep the
        // crash.
        bool lockedRegistry = false;
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            if (registry.lock.tryLock()) {
                lockedRegistry = true;
                break;
            }
            Thread::yield();
        }
        if (!lockedRegistry) {
            dataLogLn("  (registry lock unavailable after bounded spin — possible registry-lock-holding wedge; skipping participant dump)");
            RELEASE_ASSERT_NOT_REACHED();
        }
        {
            Locker locker { AdoptLock, registry.lock };
            std::atomic_thread_fence(std::memory_order_seq_cst);
            // A foreign §A.3 window open at dump time means the access-holding
            // lite below may be that window's conductor (AB-21 re-acquire), not
            // a wedged mutator — i.e. the caller is a starved/queued requester,
            // not a conductor whose predicate cannot converge.
            bool foreignWindowOpen = jsThreadsThreadGranularWorldIsStopped() && !jsThreadsCurrentThreadIsStopConductor();
            if (foreignWindowOpen)
                dataLogLn("  NOTE: another thread's §A.3 stop window is OPEN (witness depth nonzero, opened by a different thread) — this requester never reached/held tenure; access-holders below may be that live conductor.");
            for (VMLite* lite : registry.lites) {
                if (lite->vm != vm)
                    continue;
                if (lite->state != VMLite::State::Live)
                    continue;
                if (!lite->clientHeap)
                    continue;
                bool nonQuiescent = lite != requesterLite && lite->clientHeap->hasHeapAccess();
                // B16: parkedRootSnapshotThread() is the per-thread client's
                // owning WTF::Thread*, set on its first coop-snapshot publish
                // (LockObject.cpp GILDroppedSection spawned-arm,
                // JSThreadsSafepoint.cpp class-(2) park, Heap.cpp F8 wait)
                // and never cleared independently — under U-T6 per-thread
                // clients it is the stable identity once set. Null only if
                // the lite has never parked once (rare; both B16 repros'
                // workers Atomics.wait at startup).
                WTF::Thread* liteThread = lite->clientHeap->parkedRootSnapshotThread();
                dataLogLn("  entered lite ", RawPointer(lite), " tid=", lite->tid,
                    lite == requesterLite ? " [requester/conductor — exempt]" : "",
                    " clientHeap=", RawPointer(lite->clientHeap),
                    " hasHeapAccess=", lite->clientHeap->hasHeapAccess(),
                    " ownerThread=", RawPointer(liteThread),
                    " ownerThreadUID=", liteThread ? liteThread->uid() : 0,
                    nonQuiescent
                        ? (foreignWindowOpen ? "  <== access-holding (possibly the OPEN window's conductor — see NOTE above)" : "  <== NON-QUIESCENT (blocking the stop)")
                        : "");
                if (nonQuiescent && numWedged < watchdogMaxWedgedDump) {
                    wedged[numWedged].tid = lite->tid;
                    wedged[numWedged].thread = liteThread;
                    ++numWedged;
                }
            }
        }
        // Registry lock dropped (Locker scope closed). Everything below is
        // best-effort fail-stop diagnostics: this thread is about to
        // RELEASE_ASSERT_NOT_REACHED, so allocation / suspend / signal use
        // here cannot regress live behaviour. None of this is reachable
        // flag-off (every watchdogAssertStopProgress caller is on a gilOff-
        // only path: VMManager.cpp:683/:770 §A.3 conductor loops and the
        // Heap::JSThreadsStopScope watchdog ctor at Heap.cpp:7750), so
        // flag-off byte-identical is preserved.

        // B16 conductor-side held-lock facts (FIX-2 mech (1) discriminator).
        // A TRUE here means the conductor entered jsThreadsStopTheWorldAndRun
        // while holding a long-hold recursive lock that other mutators may
        // contend RAW — i.e. the wedged lite is most likely blocked inside
        // that lock's contended-acquire path. A FALSE for both rules them
        // out and points at FIX-2 mech (2) (an unbracketed access-holding
        // native wait on the wedged lite's stack).
        dataLogLn("  conductor thread uid=", Thread::currentSingleton().uid(),
            " isCurrentlyTenuredConductor=", jsThreadsCurrentThreadIsStopConductor(),
            " holdsGilOffCompilationLock=", gilOffCompilationLock().isOwner(),
            " holdsStaticPropertyReificationLock=", staticPropertyReificationLock().isOwner());
        {
            JSC::Heap& server = vm->clientHeap.server();
            dataLogLn("  server heap ", RawPointer(&server),
                " isSharedServer=", server.isSharedServer(),
                " gcStopPendingForAllClients=", server.gcStopPendingForAllClients(),
                " worldIsStoppedForAllClients=", server.worldIsStoppedForAllClients());
        }

        // B16 wedged-thread native backtrace. The single most useful
        // localization datum: WHERE the non-quiescent lite is sitting in
        // native code with heap access held for 30s. Suspend it (the same
        // SIGUSR2 machinery the conservative root scan uses —
        // MachineThreads::tryCopyOtherThreadStacks), capture PC + walk the
        // frame-pointer chain inside that thread's real stack bounds, then
        // resume so the subsequent RELEASE_ASSERT crash sees a consistent
        // process. The walk reads only the suspended thread's own stack
        // memory (validated against thread.stack()) and is malloc-free, so
        // it is signal-suspension-safe in the same sense the root-scan copy
        // is. Addresses are dumped raw for offline addr2line/llvm-symbolizer
        // — the binary is the build under test, so symbolization is
        // build-stable (unlike the existing
        // mc-gc-weakgcmap-registry-vs-prune.stw-variant.txt whose REQUESTER
        // backtrace is from the WTFCrash on this thread and says nothing
        // about the wedged sibling).
        for (unsigned i = 0; i < numWedged; ++i) {
            WTF::Thread* thread = wedged[i].thread;
            dataLogLn("  -- B16 native backtrace for NON-QUIESCENT lite tid=", wedged[i].tid, " thread=", RawPointer(thread), " --");
            if (!thread) {
                dataLogLn("     (no owning Thread* recorded — lite never published a coop root snapshot; see parkedRootSnapshotThread())");
                continue;
            }
            if (thread == &Thread::currentSingleton()) {
                dataLogLn("     (wedged thread IS the watchdog caller — stale parkedRootSnapshotThread, skipping self-suspend)");
                continue;
            }
#if HAVE(MACHINE_CONTEXT) || OS(WINDOWS)
            ThreadSuspendLocker suspendLocker;
            auto suspended = thread->suspend(suspendLocker);
            if (!suspended) {
                dataLogLn("     (suspend() failed — thread may be exiting)");
                continue;
            }
            PlatformRegisters regs;
            thread->getRegisters(suspendLocker, regs);
            void* pc = nullptr;
            if (auto pcPtr = MachineContext::instructionPointer(regs))
                pc = pcPtr->untaggedPtr();
            void* fp = MachineContext::framePointer(regs);
            const StackBounds& stackBounds = thread->stack();
            dataLogLn("     pc=", RawPointer(pc), " fp=", RawPointer(fp),
                " stack=[", RawPointer(stackBounds.end()), ", ", RawPointer(stackBounds.origin()), "]");
            // Frame-pointer walk: standard {fp[0]=caller-fp, fp[1]=ret-addr}
            // layout (x86_64 SysV / arm64 AAPCS, the platforms the threads
            // ladder runs on). Bounded; each step validated against the
            // suspended thread's real stack so an FP that escaped into heap
            // (ASAN fake-stack, JIT frames without an FP) terminates the
            // walk instead of faulting. JIT frames at the leaf are expected
            // and harmless — the wedge is in NATIVE code (a JS-executing
            // thread would have hit a CheckTraps poll within ~1ms of the
            // conductor's per-sample re-fire, VMManager.cpp:768), so the
            // frames above any JIT leaf are the load-bearing ones.
            for (unsigned frame = 0; frame < 64; ++frame) {
                if (!fp || !stackBounds.contains(fp))
                    break;
                void** frameWords = reinterpret_cast<void**>(fp);
                // fp[1] read also stays inside the stack (origin is
                // exclusive-ish; one word past fp is fine while fp itself is
                // contained — the thread's own frame wrote it).
                void* retAddr = frameWords[1];
                void* callerFP = frameWords[0];
                dataLogLn("     #", frame, " ret=", RawPointer(retAddr), " fp=", RawPointer(fp));
                // Stack grows down; the FP chain walks toward origin
                // (higher addresses). A non-monotone link is a broken chain
                // (or a leaf JIT frame) — stop.
                if (reinterpret_cast<uintptr_t>(callerFP) <= reinterpret_cast<uintptr_t>(fp))
                    break;
                fp = callerFP;
            }
            thread->resume(suspendLocker);
#else
            dataLogLn("     (no MACHINE_CONTEXT on this platform — foreign-thread backtrace unavailable)");
#endif
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// ===== FIX-2 (stw-watchdog-timeout): gilOff park-site stop poll =====
// The §A.3.2 conductor predicate is purely access-based ("parked implies
// access-released"). There are TWO discharge mechanisms, by park class
// (review-round banner correction — the original banner wrongly claimed the
// D9 quantum loops call this helper; they never did and must not need to):
//
//   (1) JS-level D9 park sites (LockObject.cpp hold, ThreadAtomics.cpp
//       property-wait, ThreadObject.cpp join) park strictly INSIDE a
//       GILDroppedSection bracket, whose ctor releases this thread's
//       per-client heap access for the whole wait (spawned arm:
//       JSLock::DropAllLocks; carrier arm: unlockAllForThreadParking ->
//       willReleaseLock's gilOff lite->clientHeap->releaseHeapAccess(),
//       JSLock.cpp). Access-released for the entire wait satisfies the
//       predicate with no per-quantum stop poll; re-acquisition at bracket
//       exit funnels through the gated AHA (F8/§A.3.2b), which parks on a
//       still-open window. These loops poll only TERMINATION and
//       WATCHDOG-CHECK, by design.
//
//   (2) Compile-side / runtime waits that hold heap access across an
//       unbounded native wait and have NO bracket — THESE are this helper's
//       callers: BytecodeGenerator.cpp + DFGPlan.cpp (GILOffCompilationLocker
//       tryLock spins), ScriptExecutable.cpp, JSObject.h:2005. Any NEW
//       access-holding unbounded wait must either sit inside a
//       GILDroppedSection-class bracket per (1) or call this helper once per
//       wait quantum — otherwise the conductor's predicate cannot converge
//       and the 30s watchdog fail-stops (the residual counter-lock 5/5
//       signature is exactly an unfound class-(2) wait).
//
// This helper discharges TWO stop families for class-(2) waiters: the §A.3
// thread-granular window (stop-word leg below) AND a pending SHARED-GC stop
// (GSP/F8 leg below, via gcClientReleaseAccessAndBlockForPendingSharedGCStop).
// The GC leg is load-bearing, not defense-in-depth: a §A.3 requester that
// already owns gilOffCompilationLock queues on the GCL BEHIND a shared-GC
// conductor with its stop word still unpublished, so a stop-word-only
// predicate leaves the spinners' held access wedging the GC's §10.4 barrier
// forever (the counter-lock contgc watchdog signature; see the leg's comment
// in the function body).
//
// Called once per wait quantum with NO rank-3 (waiter-list/queue) lock held.
// Returns true if it parked; the caller must treat that as a fresh
// acquisition episode (re-validate its wait predicate / re-enqueue per the
// W1 disposition rules) before sleeping again. Cost when no window is
// pending: one immutable-byte branch plus one seq_cst load; GIL-on: one
// branch.
bool parkSitePollAndParkForStopTheWorld(VM& vm)
{
    if (!vm.gilOff()) [[likely]]
        return false;
    if (!jsThreadsStopPendingFor(vm)) [[likely]] {
        // counter-lock contgc wedge fix (root cause; H1xH2 composition): a
        // class-(2) access-holding wait must quiesce for a pending
        // SHARED-GC stop (GSP/F8) too, not only a published §A.3 window.
        // The closed cycle this discharges: an §A.3 requester that already
        // owns gilOffCompilationLock (ScriptExecutable::prepareForExecution
        // -> CodeBlock::finishCreation -> fireTTLSetsForSharedTransition)
        // queues on the GCL (Heap::JSThreadsStopScope watchdog ctor) BEHIND
        // a shared-GC conductor; the §A.3 stop word is still UNPUBLISHED at
        // that point (VMManager takes the GCL strictly before
        // jsThreadsStopWordStore), so the §A.3-word predicate above stays
        // false forever, while these spinners' held access keeps the GC's
        // §10.4 barrier from converging:
        //   GCL -> client-access -> gilOffCompilationLock -> GCL
        // — permanent, 30s watchdog fail-stop (JSThreadsSafepoint.cpp:732
        // signature, "OM transition stop" context). Releasing for the GC
        // here breaks the client-access edge: the GC converges and
        // completes, the GCL frees, the §A.3 requester publishes its word,
        // and the next poll quantum parks on it through the leg below.
        // Epoch bracket mirrors the §A.3 leg (P10/P10b): a thread-granular
        // window can run and rewrite heap facts while this thread is still
        // blocked inside the re-acquire's F8/§A.3.2b gates.
        uint64_t heapFactRewriteEpochOnEntry = conductorHeapFactRewriteEpoch();
        if (!gcClientReleaseAccessAndBlockForPendingSharedGCStop()) [[likely]]
            return false;
        if (conductorHeapFactRewriteEpoch() != heapFactRewriteEpochOnEntry) [[unlikely]] {
            VMLite* selfLite = VMLite::currentIfExists();
            if (selfLite && selfLite->vm == &vm)
                vm.trapsForCurrentThread().jettisonOptimizedCodeOnStackAfterConductorHeapFactRewrite(vm.topCallFrame);
        }
        return true;
    }
    // HBT3.2: the conductor never parks on its own window. A D9 wait reached
    // from inside the conductor's `work` closure is already world-stopped;
    // parking it here would self-deadlock the window.
    if (jsThreadsCurrentThreadIsStopConductor())
        return false;
    // Same order as the AHA §A.3.2b gate (Heap.cpp): publish access-released
    // (seq_cst RHA via the U-T5 pairing helper — idempotent, owns the
    // thread_local pairing flag), wake the conductor's predicate sampler,
    // ticket-park until the stop word clears, then re-acquire. Re-acquisition
    // funnels through acquireHeapAccess's F8/§A.3.2b gates, so a back-to-back
    // window (or a GC stop that arrived meanwhile) re-parks this thread
    // instead of admitting it; the ISB1.2 stop-generation sync on the AHA
    // path covers any code the window patched before this thread can re-enter
    // JIT code.
    // checktraps-dejank-invalidation-point (amend round, P10 coverage fix):
    // this park is a class-(2) wait — the parked thread can have DFG/FTL
    // frames on its stack whose slow path led here through nodes that do NOT
    // clobber the heap in DFGClobberize.h, so heap facts hoisted across the
    // (now non-clobbering) CheckTraps polls can be live across this park.
    // Mirror handleTraps' epoch bracket: sample before publishing
    // quiescence, compare after resume, jettison this thread's on-stack
    // optimizing code on overlap. Caveat recorded in
    // docs/threads/AUDIT-checktraps.md §4 (P10b): unlike the handleTraps
    // park, this rejoin point carries no invalidation point, so the jettison
    // narrows but does not by itself close the window for facts used between
    // this rejoin and the next IP — see the audit's open item P10c for the
    // structural disposition. Compiler-side callers (DFGPlan/
    // BytecodeGenerator on worklist threads) have no lite and no JS stack:
    // skip (the VM-word topEntryFrame there may belong to a RUNNING foreign
    // thread; walking it would be unsound).
    uint64_t heapFactRewriteEpochOnEntry = conductorHeapFactRewriteEpoch();
    gcClientWillParkForThreadGranularStop();
    jsThreadsNotifyMutatorQuiesced();
    {
        // T5-rootscan-skip-coop-parked-suspend: spill callee-saves + record
        // stackTop in THIS frame and publish the coop snapshot for the
        // conductor's root scan; jsThreadsParkForStopWindow is pure stripe
        // condvar machinery below this frame (no JSCell*). Cleared on wake;
        // didResume's redundant clear (idempotent) covers any future
        // early-exit edge. W=1: the enclosing branch already required
        // !jsThreadsCurrentThreadIsStopConductor() — unreachable with one
        // thread.
        DECLARE_AND_COMPUTE_CURRENT_THREAD_STATE(parkedRootSnapshot);
        gcClientPublishParkedRootSnapshot(&parkedRootSnapshot);
        jsThreadsParkForStopWindow(vm);
        gcClientClearParkedRootSnapshot();
    }
    gcClientDidResumeFromThreadGranularStop();
    if (conductorHeapFactRewriteEpoch() != heapFactRewriteEpochOnEntry) [[unlikely]] {
        VMLite* selfLite = VMLite::currentIfExists();
        if (selfLite && selfLite->vm == &vm)
            vm.trapsForCurrentThread().jettisonOptimizedCodeOnStackAfterConductorHeapFactRewrite(vm.topCallFrame);
    }
    return true;
}

bool worldIsStopped(VM& vm)
{
    // SPEC-jit section 5.6 disjuncts.
    if (worldIsStopped())
        return true;

    // Legacy per-VM GC stop: true from when the mutator is stopped through the
    // End phase, which covers finalizeUnconditionally/visitWeak-driven fires.
    if (vm.heap.worldIsStopped())
        return true;

#if defined(JSC_JIT_HAS_SHARED_HEAP_SERVER)
    // Shared-server GC stop (SPEC-heap section 9). Consult the SERVER this
    // VM's client heap is attached to, not the VM's own (possibly idle) Heap
    // member: under useSharedGCHeap a client VM's vm.heap is not the shared
    // server (R3-11). For the 1:1 non-shared case server() == vm.heap.
    // THREADS-INTEGRATE(jit)
    if (vm.clientHeap.server().worldIsStoppedForAllClients())
        return true;
#endif

    return false;
}

} // namespace JSThreadsSafepoint
} // namespace JSC
