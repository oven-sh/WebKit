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

#include "ConcurrentJSLock.h"
#include "GCThreadLocalCache.h"

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

namespace JSC {

// Defined in heap/Heap.cpp: per-thread park access pairing consumed by
// parkSitePollAndParkForStopTheWorld below.
void gcClientWillParkForThreadGranularStop();
void gcClientDidResumeFromThreadGranularStop();
bool gcClientReleaseAccessAndBlockForPendingSharedGCStop();
// T5-rootscan-skip-coop-parked-suspend (heap/Heap.cpp): coop root snapshot
// publish/clear bracketing the class-(2) ticket park below.
void gcClientPublishParkedRootSnapshot(CurrentThreadState*);
void gcClientClearParkedRootSnapshot();

// The second long-hold recursive lock (with gilOffCompilationLock, declared in
// JSThreadsSafepoint.h) a conductor can be holding when it requests a stop.
// Owner: runtime/Lookup.cpp. Consumed only on the watchdog fail-stop path
// (isOwner() probe: a held lock here means a contended waiter on that lock is
// the prime suspect for the non-quiescent thread).
RecursiveLock& staticPropertyReificationLock();

namespace JSThreadsSafepoint {

// Heap-access release for the R1.i bracket, scoped to the CALLING thread's
// GCClient::Heap — not Heap::releaseAccess() on the server. Once a server is
// shared, heap access is tracked per client and the server-level
// releaseAccess() forwards to the MAIN client, which is the wrong client
// whenever the stop requester is a non-main client. Destruction re-acquires
// access, blocking if a shared-mode GC stop is pending (heap F8) — the spec's
// resume order.
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

// Raised around every GIL-on stub closure and every AlreadyStoppedWorldWitnessScope
// in the process. Consumed by the VM-less worldIsStopped() (the patching
// asserts) and by AlreadyStoppedWorldWitnessScope to decide whether its
// tripwire runs. Atomic because compiler and GC threads read it; being
// process-global it says only that SOME thread is inside a stopped window,
// which is why worldIsStopped(VM&) and the guards below never accept it.
static std::atomic<unsigned> s_stubWorldStoppedDepth { 0 };

// The same depth counted for the current thread only. Evidence raised on this
// stack cannot evaporate while `work` runs on it, and it belongs to the VM
// this thread is patching; worldIsStopped(VM&) and the foreign-thread and
// GC-conduction guards in stopTheWorldAndRun accept exactly this. Plain:
// written and read only by the owning thread.
static thread_local unsigned t_stubWorldStoppedDepth { 0 };

// The mutators that can execute code owned by `vm` are the mutators of the
// VMs attached to vm's server heap: CodeBlocks, Structures and watchpoint
// sets belong to one VM, and every client of a shared server belongs to the
// VM whose heap that server is (VM::clientHeap and every spawned or carrier
// client attach to vm.heap). An independent VM in the same process (a Worker,
// a jsc $.agent) runs and patches only its own code under its own JSLock or GC
// stop, exactly as flag-off, so it is never counted here. Every VM's server
// therefore has at most one VM attached, and this count is a sampled tripwire
// for that structural fact, not the soundness mechanism.
static unsigned enteredVMsAttachedToServerOf(VM& vm)
{
    JSC::Heap& server = vm.clientHeap.server();
    unsigned entered = 0;
    VMManager::forEachVM([&](VM& candidate) {
        if (candidate.isEntered() && &candidate.clientHeap.server() == &server)
            ++entered;
        return IterationStatus::Continue;
    });
    return entered;
}

// Raising the process-global witness on the strength of per-heap evidence
// (vm.heap.worldIsStopped(), or the server's worldIsStoppedForAllClients()) is
// sound when that evidence covers every mutator that can execute vm's code.
// Both stops park every client of vm's server (a shared server sets
// m_worldIsStopped only after all its clients are stopped), so the only thing
// left to check is that no second VM is attached to that server.
static void assertAlreadyStoppedEvidenceCoversEveryMutator(VM& vm)
{
    RELEASE_ASSERT(enteredVMsAttachedToServerOf(vm) <= 1);
}

AlreadyStoppedWorldWitnessScope::AlreadyStoppedWorldWitnessScope(VM& vm)
{
    ASSERT(worldIsStopped(vm));
    // The tripwire runs only when NO process-global witness holds: under an
    // all-VM stop (VMManager::Mode::Stopped, e.g. the wasm debugger) or an
    // outer closure / witness scope whose own entry already passed it, the
    // global witness is already truthful. VMManager::forEachVM needs no API
    // lock, matching the R1.h no-API-lock contract.
    if (!worldIsStopped())
        assertAlreadyStoppedEvidenceCoversEveryMutator(vm);
    s_stubWorldStoppedDepth.fetch_add(1, std::memory_order_relaxed);
    ++t_stubWorldStoppedDepth;
}

AlreadyStoppedWorldWitnessScope::~AlreadyStoppedWorldWitnessScope()
{
    // F5: the patcher's own instruction-stream barrier after any
    // cross-modifying code write inside the scope. The data-side flush is
    // performed by the patching primitives themselves.
    WTF::crossModifyingCodeFence();
    --t_stubWorldStoppedDepth;
    s_stubWorldStoppedDepth.fetch_sub(1, std::memory_order_relaxed);
}

// Already-stopped evidence that is THREAD-STABLE for the current caller: it
// cannot evaporate while `work` runs on this stack. That is the stub depth
// raised on THIS thread (an enclosing stub closure or witness scope whose own
// entry passed the guards) and the VMManager all-VM stop. Deliberately
// EXCLUDES (a) the process-global stub depth — a conductor raises it inside
// its own window (nested fires run under a witness scope), so for any other
// thread it is evidence of a window that can close underneath it; (b) the
// §A.3 thread-granular window, whose inline licence belongs to its conductor
// alone (r33 guard + the conductor early-out at the call site); and (c) the
// per-heap GC-stop disjuncts (vm.heap.worldIsStopped() /
// worldIsStoppedForAllClients()), whose stability is exactly what the
// GC-conduction check at the call site establishes: those stops are ended by
// the GC conductor, which clears WSAC and resumes its clients WITHOUT
// consulting our witness depth.
static bool worldIsStoppedEvidenceIsThreadStable()
{
    if (t_stubWorldStoppedDepth)
        return true;
    return VMManager::info().worldMode == VMManager::Mode::Stopped;
}

// R1.h foreign-thread guard (review r33): the "already stopped" disjuncts of
// worldIsStopped(VM&) MINUS the §A.3 thread-granular window witness, with the
// stub depth restricted to this thread for the reason above.
static bool worldIsStoppedEvidenceExcludingThreadGranularWindow(VM& vm)
{
    if (worldIsStoppedEvidenceIsThreadStable())
        return true;
    if (vm.heap.worldIsStopped())
        return true;
    return vm.clientHeap.server().worldIsStoppedForAllClients();
}

// ===== checktraps-dejank-invalidation-point: conductor heap-fact rewrite epoch =====
// See the header comment (JSThreadsSafepoint.h). Process-global: stop windows are
// process-rare and a false-positive bump only costs an on-stack jettison, so
// per-VM precision is not worth the plumbing. acq_rel/acquire keeps the
// counter itself coherent; the cross-thread ordering guarantee rides the
// park/resume edge.
//
// BUMP-EDGE LAW: the load-bearing bump happens IN-WINDOW, AFTER `work` and
// BEFORE the world resumes, on both legs of stopTheWorldAndRun below: the
// wrapped closure of a conducted §A.3 window, and the post-work bump of the
// already-stopped inline path (which runs on the conductor or collector of
// the outer stop, hence before that stop's resume edge). A publication-time
// (ClassAStopWatchdogContext ctor) bump is NOT sufficient on its own: a
// mutator parked BY the window samples the epoch in VMTraps::handleTraps
// strictly AFTER the publication bump (the trap bits that send it there are
// set after the ctor runs), so its exit compare would see an unchanged epoch —
// exactly the victim class the mechanism exists for. The ctor and dtor bumps
// are the entry and exit edges: they cover mutators that were ALREADY inside
// handleTraps when the window published, and are otherwise redundant with the
// in-window bump. Defined here, above stopTheWorldAndRun, because the reroute
// branch references them.
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

// Requests that reached a conductor (GIL off) or the GIL-on stub, not the
// nested/inline R1.h cases. Test-only observable ($vm.jsThreadsStopRequestCount).
static std::atomic<uint64_t> s_stopTheWorldRequestCount { 0 };
uint64_t stopTheWorldRequestCount() { return s_stopTheWorldRequestCount.load(std::memory_order_relaxed); }

void stopTheWorldAndRun(VM& vm, const ScopedLambda<void()>& work)
{
    // R1.h FIRST (load-bearing for SPEC-jit section 5.3, Task 5): a caller that
    // is ALREADY world-stopped — a jettison reached from a GC's stopped window
    // (legacy per-VM stop or shared-server stop) or from inside an outer
    // stopTheWorldAndRun closure (e.g. a Class-A watchpoint fire's section 5.6
    // step 5 jettisons, or an object-model transition stop's body) — runs
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
        // the §A.3 thread-granular window targeting this VM
        // (jsThreadsThreadGranularWorldIsStopped()), inline execution is
        // licensed solely for the CONDUCTOR thread — the §A.3 window
        // quiesces only entered lites of the target VM holding heap access,
        // so a non-participant thread (a worklist compiler thread's
        // finalize/install leg, another server's GC context, a mutator
        // between its access release and its park) that observed the window
        // and patched inline here would run concurrently with the
        // conductor's own work body: two unsynchronized patchers inside one
        // "stopped" window. The evidence accepted instead is only what this
        // thread raised itself or a stop it can see end (GC stop of its own
        // heap, all-VM stop) — fail-stop, never patch.
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
            JSC::Heap& gcStopServer = vm.clientHeap.server();
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
                auto workThenBumpHeapFactRewriteEpochFunctor = [&] {
                    work();
                    if (!t_pureCodeLifecycleStopWindowDepth)
                        noteConductorHeapFactRewrite();
                };
                ScopedLambda<void()> workThenBumpHeapFactRewriteEpoch(workThenBumpHeapFactRewriteEpochFunctor);
                return jsThreadsThreadGranularStopTheWorldAndRun(vm, workThenBumpHeapFactRewriteEpoch);
            }
        }
        // The attached-VM tripwire and the witness raise/lower + F5 barrier
        // live in AlreadyStoppedWorldWitnessScope. Class-A watchpoint fires
        // reached on already-stopped evidence arrive here too (their drain is
        // the `work` closure), so they pass the guards above like any other
        // caller. See the scope's comments above.
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

    // UNGIL §A.3.3 LICENSED REROUTE: gilOff Class-A requests take the real
    // thread-granular conductor (HBT4/SB1/ISB1 sequence, R1.a-i). The stub
    // below is the GIL-on path only — its premise ("the caller's JSLock makes
    // it the sole mutator of this VM") does not hold for N entered threads of
    // one gilOff VM. Nested fires inside an open thread-granular window do
    // not reach here: jsThreadsThreadGranularWorldIsStopped() feeds the
    // worldIsStopped() disjunct above, so they run inline under the witness
    // scope (R1.h).
    s_stopTheWorldRequestCount.fetch_add(1, std::memory_order_relaxed);
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
        auto workThenBumpHeapFactRewriteEpochFunctor = [&] {
            work();
            if (!t_pureCodeLifecycleStopWindowDepth)
                noteConductorHeapFactRewrite();
        };
        ScopedLambda<void()> workThenBumpHeapFactRewriteEpoch(workThenBumpHeapFactRewriteEpochFunctor);
        return jsThreadsThreadGranularStopTheWorldAndRun(vm, workThenBumpHeapFactRewriteEpoch);
    }

    // GIL-on inline stub (SPEC-jit R1). The caller holds this VM's JSLock, so
    // no other thread of this VM executes JS, and no other VM can execute this
    // VM's code (enteredVMsAttachedToServerOf above): "the world" for the code
    // being patched is exactly the calling thread, and `work` runs inline on
    // this stack. Of the R1.a-i sequence only the GC-serialization bracket
    // (R1.i, release heap access + Heap::JSThreadsStopScope) is needed here;
    // there is no stop/resume and no requester arbitration.

    // Caller must be an entered mutator (R1 contract).
    RELEASE_ASSERT(vm.currentThreadIsHoldingAPILock());

    // Sampled tripwire for the structural premise above: a second VM attached
    // to this VM's server would be a mutator the JSLock does not serialize.
    // Independent VMs in the process (Workers, jsc $.agent) are not counted;
    // they patch their own code under their own JSLock, as flag-off.
    RELEASE_ASSERT(enteredVMsAttachedToServerOf(vm) <= 1);

    // R1.i (SPEC-jit section 5.3 / CS2): bracket the ENTIRE stopped window for
    // a shared-server heap — release this thread's heap access FIRST
    // (JSThreadsStopScope's precondition: a conductor must never stop the
    // world while still counted as a heap-accessing mutator), then hold the
    // rank-2 GC conductor lock across `work` so no shared-mode GC can start or
    // be mid-cycle while we patch code. Destruction order is the spec's resume
    // order: drop the stop scope (unlock GCL), then re-acquire heap access.
    // `work` runs without heap access; it must not allocate in the JS heap
    // (OM O4) — heap-metadata WRITES without access are explicitly allowed
    // (heap section 10A exemption). NEVER calls bumpAndReclaim (G13/CS4):
    // JSThreads stops enqueue a GC request instead; reclamation rides the GC.
    // Non-shared heap: no-op per R1.i — today's jettisons already run with
    // heap access held, and the legacy concurrent collector tolerates that
    // exactly as it does in tip-of-tree.
    //
    // The bracket is keyed on, and the stop scope taken against, the server
    // heap this thread's client attaches to (vm.clientHeap.server(), which is
    // vm.heap for every VM in this tree) and the heap-access release is
    // client-scoped (ClientHeapAccessReleaseScope above): once a server is
    // shared, a non-main client's access is tracked on that client, not on
    // the server's main client. Heap::JSThreadsStopScope self-gates on
    // !isSharedServer() internally, so passing the resolved server is safe in
    // every config.
    JSC::Heap& server = vm.clientHeap.server();
    // Declaration order is load-bearing: destruction runs stop scope first
    // (unlock GCL), then re-acquires heap access — the spec's resume order.
    std::optional<ClientHeapAccessReleaseScope> releaseHeapAccess;
    std::optional<JSC::Heap::JSThreadsStopScope> jsThreadsStopScope;
    if (server.isSharedServer()) [[unlikely]] {
        // The WATCHDOG stop-scope ctor, not the blocking one: a jettison
        // requester queued on the GCL behind a wedged shared GC must fail-stop
        // under the standard 30s stop watchdog instead of hanging unwatched.
        // requestStart is sampled strictly BEFORE the
        // ClientHeapAccessReleaseScope (reaching the bracket is part of
        // reaching a stopped world), so the whole release+GCL leg is covered,
        // and the ctor threads the target VM into the timeout diagnostics.
        MonotonicTime requestStart = MonotonicTime::now();
        releaseHeapAccess.emplace(vm.clientHeap);
        jsThreadsStopScope.emplace(server, requestStart);
    }

    s_stubWorldStoppedDepth.fetch_add(1, std::memory_order_relaxed);
    ++t_stubWorldStoppedDepth;
    work();
    // F5: patcher-side instruction-stream barrier before any possibility of
    // this (sole) mutator re-entering JIT'd code.
    WTF::crossModifyingCodeFence();
    --t_stubWorldStoppedDepth;
    s_stubWorldStoppedDepth.fetch_sub(1, std::memory_order_relaxed);
}

bool worldIsStopped()
{
    if (s_stubWorldStoppedDepth.load(std::memory_order_relaxed))
        return true;

    // UNGIL §J.8: a §A.3 thread-granular window (VMManager.cpp conductor)
    // counts as a stopped world for the VM-less patching asserts; this form
    // cannot tell which VM it targets, which is why it is for asserts only.
    // An object-model transition stop's body runs inside one of the windows
    // above (or inside the GIL-on stub), so it needs no disjunct of its own.
    if (jsThreadsThreadGranularWorldIsStopped()) [[unlikely]]
        return true;

    return VMManager::info().worldMode == VMManager::Mode::Stopped;
}

// ===== SPEC-jit section 5.6 stop watchdog (annex App. 5.6(d)) =====

// Options::jsThreadsStopWatchdogMs(). The default is generous: it covers slow
// CI/ASAN/valgrind-grade parking latencies, and an escaped lock-holding fire
// site wedges forever, so any finite bound catches it. Zero disables the
// fail-stop: an embedder whose native sections legitimately hold heap access
// longer than the default, without an access-release bracket, trades the
// crash for a requester that waits until the section returns to a poll.
static Seconds stopTheWorldWatchdogTimeout()
{
    return Seconds::fromMilliseconds(Options::jsThreadsStopWatchdogMs());
}

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
    // bump is IN-WINDOW, pre-resume, on both legs of stopTheWorldAndRun (see
    // the BUMP-EDGE LAW comment above). Flag-off: contexts are only published
    // flag-on, but gate anyway so an accidental flag-off publication changes
    // nothing.
    if (Options::useJSThreads() && !t_pureCodeLifecycleStopWindowDepth) [[likely]]
        noteConductorHeapFactRewrite();
}

ClassAStopWatchdogContext::~ClassAStopWatchdogContext()
{
    // checktraps-dejank-invalidation-point: the EXIT-EDGE bump. A context is
    // published around a stopTheWorldAndRun call, which bumps in-window on
    // both of its legs, so this bump is redundant with that one: for a request
    // that ran inline under an outer stop it lands before the outer stop's
    // resume edge like the in-window bump; for a request that conducted its
    // own §A.3 window it is post-resume. A post-resume bump can at worst cause
    // a spurious jettison on an unrelated concurrently-parked thread — sound,
    // perf-only. Same suppression gate as the ctor.
    if (Options::useJSThreads() && !t_pureCodeLifecycleStopWindowDepth) [[likely]]
        noteConductorHeapFactRewrite();

    t_pendingClassAStopContext = m_previousContext;
    t_pendingClassAStopContextDescription = m_previousDescription;
}

void watchdogAssertStopProgress(MonotonicTime requestStart, VM* vm) WTF_IGNORES_THREAD_SAFETY_ANALYSIS // Manual bounded tryLock of the registry leaf lock below.
{
    Seconds timeout = stopTheWorldWatchdogTimeout();
    if (!timeout) [[unlikely]]
        return;
    if (MonotonicTime::now() - requestStart < timeout) [[likely]]
        return;

    const void* context = t_pendingClassAStopContext;
    const char* description = t_pendingClassAStopContextDescription;
    dataLogLn("JSThreads stop-the-world failed to reach a stopped world within ",
        timeout.seconds(), "s. Pending Class-A fire context: ",
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
                // (the stop-protocol parks: VMManager.cpp sibling poll,
                // JSThreadsSafepoint.cpp class-(2) park, Heap.cpp F8 wait)
                // and never cleared independently — under U-T6 per-thread
                // clients it is the stable identity once set. Null if the
                // lite has never parked at one of those sites; JS-level parks
                // (Atomics.wait, Lock.hold, Condition.wait, join) publish no
                // snapshot and do not record it.
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
//       callers: lockGILOffCompilationLockContended (ScriptExecutable.cpp, the
//       GILOffCompilationLocker tryLock spin), JSObject.h:2005. Any NEW
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
#if ASSERT_ENABLED
    // A thread that waits for a cell lock or a ConcurrentJSLock waits without
    // a safepoint, so a holder that parks here (or would, were a stop pending)
    // can keep a stop from ever completing. Checked on every poll, pending or
    // not, so the Debug corpus finds such a holder without needing the race.
    ASSERT(!GCCellLockDepth::current());
    ASSERT(!ConcurrentJSLockDepth::current());
#endif
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
                vm.trapsForCurrentThread().jettisonOptimizedCodeOnStackAfterConductorHeapFactRewrite();
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
            vm.trapsForCurrentThread().jettisonOptimizedCodeOnStackAfterConductorHeapFactRewrite();
    }
    return true;
}

bool worldIsStopped(VM& vm)
{
    // SPEC-jit section 5.6 disjuncts, each scoped to the mutators that can
    // execute vm's code. Another VM's stub closure or thread-granular window
    // stops nothing that runs vm's code, so unlike the VM-less form this one
    // consults neither the process-global depth nor an unscoped window. Every
    // consumer (stopTheWorldAndRun's entry, the patching and fire asserts,
    // the jettison fold) runs on the thread doing the patching, so the
    // per-thread depth is exactly the enclosing-closure evidence it needs.
    if (t_stubWorldStoppedDepth)
        return true;

    // A §A.3 thread-granular window parks the entered lites of the VM it
    // targets and no other VM's.
    if (jsThreadsThreadGranularWorldIsStopped() && jsThreadsStopPendingFor(vm)) [[unlikely]]
        return true;

    if (VMManager::info().worldMode == VMManager::Mode::Stopped)
        return true;

    // Legacy per-VM GC stop: true from when the mutator is stopped through the
    // End phase, which covers finalizeUnconditionally/visitWeak-driven fires.
    if (vm.heap.worldIsStopped())
        return true;

    // Shared-server GC stop (SPEC-heap section 9): every client of the server
    // this thread's client heap is attached to is parked.
    return vm.clientHeap.server().worldIsStoppedForAllClients();
}

} // namespace JSThreadsSafepoint
} // namespace JSC
