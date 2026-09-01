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

#pragma once

#include "Options.h"
#include <wtf/Forward.h>
#include <wtf/MonotonicTime.h>
#include <wtf/Noncopyable.h>
#include <wtf/RecursiveLockAdapter.h>
#include <wtf/ScopedLambda.h>

namespace JSC {

class VM;

// JSThreadsSafepoint (SPEC-jit R1): the single safepoint primitive consumed by
// code jettison (SPEC-jit section 5.3), Class-A watchpoint fires (section 5.6),
// object-model transition stops, haveABadTime and the debugger's STW walk
// under shared-memory threads. stopTheWorldAndRun has two live paths, chosen
// by the target VM's mode:
//
//  - GIL-on (vm.gilOff() false; the shipping shape): an inline stub. The
//    caller holds this VM's JSLock, so no other thread of this VM executes JS,
//    and no other VM can execute this VM's code (CodeBlocks, Structures and
//    watchpoint sets belong to one VM; every client of a shared server belongs
//    to the VM whose heap it is). `work` therefore runs inline on the caller's
//    stack with no stop requested. Other VMs in the process (Workers, jsc
//    $.agent) may be entered concurrently and patch their own code under
//    their own JSLock, exactly as flag-off. R1.i/CS2: when the server heap the
//    caller's client attaches to (vm.clientHeap.server()) is shared, the stub
//    releases THIS client's heap access (GCClient::Heap::releaseHeapAccess)
//    and holds Heap::JSThreadsStopScope on that server (the rank-2 GC
//    conductor lock) across `work`, so a shared-mode GC can neither start nor
//    be mid-cycle while the closure patches code. F5: an instruction-stream
//    barrier (crossModifyingCodeFence) on the closing edge of the closure.
//
//  - GIL-off (vm.gilOff() true): every request is rerouted to the §A.3
//    thread-granular conductor (jsThreadsThreadGranularStopTheWorldAndRun in
//    runtime/VMManager.cpp: requester arbitration, quiescence of every entered
//    lite of the VM, GC serialization via Heap::JSThreadsStopScope, and the
//    resume-path ISB on every parked mutator). The wait loops there and the
//    Heap::JSThreadsStopScope watchdog constructor call
//    watchdogAssertStopProgress, so the stop watchdog fail-stop below is live.
//
// Common to both: R1.h — a caller that is already world-stopped (a GC's
// stopped window, an outer stopTheWorldAndRun closure, an open thread-granular
// window) runs `work` inline without re-requesting, with the witness raised
// across the closure; see stopTheWorldAndRun's guards for which evidence
// licenses that on a non-conductor thread.
//
// CodeBlock::jettison is the section 5.3 choke point: every flag-on jettison
// with reason != JettisonDueToOldAge routes its entire body through
// stopTheWorldAndRun (reoptimization, watchpoint-fire and debugger triggers
// alike), so callers of jettison never need their own stop.
//
// Caller contract: caller is an entered mutator and holds NO lock from the
// SPEC-jit section 7 order and no cell lock; `work` runs with every mutator
// stopped and must neither allocate in the JS heap nor re-enter the VM. The
// requesting path additionally requires the caller's heap access to be
// releasable (no allocation in flight). An already-world-stopped caller (R1.h
// path) is exempt from the entered-mutator requirement: its safety argument is
// the enclosing stop.
namespace JSThreadsSafepoint {

// Stop every mutator, run `work` on the caller's own stack, resume.
// Idempotent w.r.t. an already-stopped world: a caller that is already running
// world-stopped (e.g. a watchpoint fire reached from a GC's stopped window or a
// nested fire inside an outer stopTheWorldAndRun closure) just runs inline
// without re-requesting (R1.h).
JS_EXPORT_PRIVATE void stopTheWorldAndRun(VM&, const ScopedLambda<void()>& work);

// True while no mutator that can execute this VM's code is running JS other
// than the caller. Disjuncts per SPEC-jit section 5.6, each scoped to that
// VM: the calling thread is inside a GIL-on stub closure or an
// AlreadyStoppedWorldWitnessScope, OR a §A.3 thread-granular window targeting
// this VM is open with its predicate satisfied, OR the VMManager world mode is
// Stopped, OR the legacy per-VM GC stop (vm.heap.worldIsStopped()), OR the
// server this VM's client attaches to reports worldIsStoppedForAllClients().
// Every consumer runs on the thread doing the patching or firing.
JS_EXPORT_PRIVATE bool worldIsStopped(VM&);

// VM-less form for patching sites that have no VM in scope
// (DFG::CommonData::invalidateLinkedCode, DFG::JumpReplacement::fire): the
// process-global stub witness (some thread is inside a stub closure or witness
// scope), any open §A.3 window, or Mode::Stopped. It cannot consult per-heap
// state and cannot tell which VM a witness belongs to, so it is used for
// asserts only.
JS_EXPORT_PRIVATE bool worldIsStopped();

// ===== Already-stopped witness scope =====
//
// RAII over the process-global world-stopped witness for a caller whose own
// evidence that the world is stopped is ALREADY established
// (worldIsStopped(vm) is true) but possibly only via per-heap state that the
// VM-less worldIsStopped() consumers (the patching asserts in
// DFG::CommonData::invalidateLinkedCode / DFG::JumpReplacement::fire) cannot
// see. The constructor:
//   1. if no process-global witness holds yet, RELEASE_ASSERTs that at most
//      one VM attached to the caller's server heap is entered — the per-heap
//      evidence parks every client of that server, so this is the only way
//      a mutator able to execute the caller's code could still be running;
//      independent VMs in the process are not counted; then
//   2. raises the global and the per-thread stub depth witness.
// The destructor issues the F5 instruction-stream barrier
// (crossModifyingCodeFence) and lowers the witness. Nests freely.
//
// User: stopTheWorldAndRun's R1.h already-stopped path, which is also how a
// Class-A watchpoint fire reached on already-stopped evidence runs (its drain
// is the `work` closure).
//
// The constructor's count is a SAMPLED tripwire for the structural fact that
// every server has at most one VM attached (VM::clientHeap and every spawned
// or carrier client attach to vm.heap), not the soundness mechanism.
class AlreadyStoppedWorldWitnessScope {
    WTF_MAKE_NONCOPYABLE(AlreadyStoppedWorldWitnessScope);
public:
    JS_EXPORT_PRIVATE explicit AlreadyStoppedWorldWitnessScope(VM&);
    JS_EXPORT_PRIVATE ~AlreadyStoppedWorldWitnessScope();
};

// SPEC-jit I2: no tier modifies reachable machine code while more than one
// mutator may execute JS, except inside a stop-the-world window. Wired at every
// patching site (invalidateLinkedCode, JumpReplacement::fire,
// rewireStubAsJumpInAccess, DirectCallLinkInfo patching).
ALWAYS_INLINE void assertPatchingIsSafe(VM& vm)
{
    if (Options::useJSThreads()) [[unlikely]]
        RELEASE_ASSERT(worldIsStopped(vm));
}

ALWAYS_INLINE void assertPatchingIsSafe()
{
    if (Options::useJSThreads()) [[unlikely]]
        RELEASE_ASSERT(worldIsStopped());
}

// ===== SPEC-jit section 5.6 stop watchdog (annex App. 5.6(d)) =====
//
// A Class-A watchpoint fire that requests a stop while some OTHER mutator can
// never park (the classic escape: a direct fireAll caller holding a section-7
// or cell lock that a to-be-parked mutator needs, or that prevents the holder
// itself from polling) wedges the stop forever. The watchdog turns that hang
// into a deterministic crash NAMING the escaped set.
//
// Usage: the requester publishes a context (RAII, per-thread, nests) before
// calling stopTheWorldAndRun; every wait loop on the way to a stopped world
// calls watchdogAssertStopProgress(requestStart) on each iteration. Callers:
// the §A.3 conductor loops in runtime/VMManager.cpp and the
// Heap::JSThreadsStopScope watchdog constructor, which the GIL-on stub enters
// whenever the caller's server heap is shared. Any new wait on the requesting
// path MUST call it too.
//
// The context is thread-local: a wedged requester times out on its own thread
// and names the set IT was firing, so concurrent requesters cannot
// misattribute each other's sets.
class ClassAStopWatchdogContext {
    WTF_MAKE_NONCOPYABLE(ClassAStopWatchdogContext);
public:
    JS_EXPORT_PRIVATE ClassAStopWatchdogContext(const void* context, const char* description);
    JS_EXPORT_PRIVATE ~ClassAStopWatchdogContext();

private:
    const void* m_previousContext;
    const char* m_previousDescription;
};

// RELEASE_ASSERTs (crashing with the published context) if the stop requested
// at `requestStart` has not completed within Options::jsThreadsStopWatchdogMs()
// (0 disables the fail-stop and lets the requester wait indefinitely). Safe to
// call repeatedly from the wait loop; cheap when under the timeout. When the
// requester passes its target VM (the §A.3 thread-granular conductor loop
// does), the timeout dump also re-runs the §A.3.2 predicate walk and NAMES
// every entered lite of that VM with its access state — so a timeout
// identifies WHICH participant failed to quiesce, not just the requester's
// own context (review-round root-cause-B localization).
JS_EXPORT_PRIVATE void watchdogAssertStopProgress(MonotonicTime requestStart, VM* vm = nullptr);

// FIX-2 (stw-watchdog-timeout): per-D9-quantum stop poll for gilOff native
// park sites (Atomics.wait per-wait nodes, property-wait, Lock/Condition/
// Thread parks, GC-completion waits). If a §A.3 thread-granular window
// targets `vm` and the caller is not its conductor, releases the caller's
// own client heap access, wakes the conductor's sampler, parks until resume,
// and re-acquires (re-running the §A.3.2b admission gates). Returns true if
// it parked — the caller must re-validate its wait predicate before sleeping
// again. Must be called with no rank-3 lock held. No-op GIL-on.
JS_EXPORT_PRIVATE bool parkSitePollAndParkForStopTheWorld(VM&);

// ===== Conductor heap-fact rewrite epoch =====
//
// GIL-off, DFG/FTL CheckTraps is compiled as an invalidation point at the
// poll's rejoin rather than a heap clobber (DFGClobberize.h), so a heap fact
// hoisted across the poll (butterfly, structure, indexing type) is only valid
// if no stop window rewrote it while the mutator was parked there. Every stop
// window that may rewrite such a fact bumps this process-global epoch; a
// mutator whose park in VMTraps::handleTraps or
// parkSitePollAndParkForStopTheWorld overlapped a bump jettisons its own
// on-stack optimizing-JIT code on resume, which fires the invalidation points
// so the poll OSR-exits before reusing any hoisted fact.
//
// Bump edge: the load-bearing bump is IN-WINDOW, after `work` and before the
// stopped world resumes, on both legs of stopTheWorldAndRun. A publication-time
// bump alone is unsound: a mutator parked BY the window samples the epoch after
// the trap bits that park it are set, hence after any bump made at publication,
// and would compare equal on resume. The ClassAStopWatchdogContext constructor
// and destructor bump as entry and exit edges (covering mutators already inside
// handleTraps at publication), and JSGlobalObject::haveABadTimeImpl and the
// NeedDebuggerBreak service bump explicitly because GIL-on publishes no window.
// A false-positive bump only costs a jettison; a missed bump at a window that
// rewrites heap facts is a correctness bug.
JS_EXPORT_PRIVATE uint64_t conductorHeapFactRewriteEpoch();
JS_EXPORT_PRIVATE void noteConductorHeapFactRewrite();

// RAII, thread-local, nests: while open on this thread, neither the
// ClassAStopWatchdogContext edges nor stopTheWorldAndRun's in-window bump
// touch the epoch. Only for stop windows that rewrite code and no heap fact:
// CodeBlock::jettison opens it around its stop so reoptimization does not
// cascade into an on-stack jettison of every parked mutator's code. A
// heap-fact-rewriting window whose nested jettisons open this scope is still
// covered by its own outer in-window bump.
class PureCodeLifecycleStopWindowScope {
    WTF_MAKE_NONCOPYABLE(PureCodeLifecycleStopWindowScope);
public:
    JS_EXPORT_PRIVATE PureCodeLifecycleStopWindowScope();
    JS_EXPORT_PRIVATE ~PureCodeLifecycleStopWindowScope();
};

// ===== GIL-removal tripwire (review round 1) =====
//
// The jit workstream ships several KNOWN GIL-SOUND-ONLY gaps (consolidated
// list: docs/threads/INTEGRATE-jit.md "GIL-removal preconditions"):
// DFG64/FTL array-element store predicates, the LLInt monomorphic-call record
// form, the MultiDeleteByOffset flag-on bail, allocation tagging, the ARM64
// R7 dest==base residue, the deferred Class-A fire fact-publication ordering
// (precondition 10: the release claim-CAS in WatchpointSet::fireAllSlow(VM&,
// DeferredWatchpointFire*) publishes the invalidation before the caller
// publishes its mutation, but every deferring site still fires only at scope
// exit, after publishing),
// the segmented-butterfly (regime 2) fast paths, and the slow-path
// call-linking writer-writer serialization (precondition 11 — AB18-D:
// LANDED). The mechanical tripwire is WIRED at the gilOff SPAWNED-thread
// second-mutator attach point (CVE-B6 / MC-CODE S8):
//
//     attachSpawnedThreadGCClient (runtime/ThreadManager.cpp):
//     RELEASE_ASSERT(JSThreadsSafepoint::gilRemovalPreconditionsMet()
//                    || Options::useThreadGILOffUnsafe());
//
// RESIDUAL second wiring site (out of B6 file scope; lands with the JSLock
// owner or the GIL-removal commit): the §F.1/§B.2 carrier non-main arm at
// runtime/JSLock.cpp perThreadClientForCarrierEntry (the `new GCClient::Heap`
// for a non-main embedder/carrier thread, reached from
// ensureCarrierLiteForCurrentThread under ASSERT(vm.gilOff())) is ALSO a
// gilOff concurrent-mutator admission point and MUST carry the identical
// assert before the tripwire is considered fully wired.
//
// The predicate is a compile-time constant FALSE today; the bring-up override
// flag (useThreadGILOffUnsafe — also the U0 option-validation gate that admits
// gilOff at all) keeps the ladder running. The tripwire is therefore NOT
// independently load-bearing today: its teeth depend on the U0 gate
// (Options.cpp) and the override flag being retired TOGETHER with the
// predicate flip. The GIL-removal change flips the constant to true in the
// SAME commit that closes (or consciously re-classifies) every listed
// precondition, retires the override flag, AND wires the carrier non-main
// site above. Flipping it without doing so is the recorded violation; a
// production build that admits a second concurrent mutator without the
// override fail-stops at the attach assert rather than running the open gaps
// silently.
constexpr bool gilRemovalPreconditionsMetValue = false;
ALWAYS_INLINE constexpr bool gilRemovalPreconditionsMet() { return gilRemovalPreconditionsMetValue; }

} // namespace JSThreadsSafepoint

// ===== GIL-off compilation lock =====
//
// GIL-off threads of one VM share Executables, UnlinkedCodeBlocks, the
// CodeCache and installed CodeBlocks, but CodeBlock creation, installation and
// tier-up finalization were written for a single mutator. One process-wide
// recursive lock serializes them: prepareForExecution, unlinkedCodeBlockFor,
// the CodeCache lookup/insert pairs and DFG::Plan::finalize all take it
// through GILOffCompilationLocker. It is recursive because
// prepareForExecutionImpl holds it across installCode and Plan::finalize holds
// it across the callback's installCode. At most one gilOff VM exists per
// process, so the lock couples no GIL-on thread.
// Defined in runtime/ScriptExecutable.cpp.
JS_EXPORT_PRIVATE RecursiveLock& gilOffCompilationLock();

// Contended acquisition. A thread blocked in a raw lock() is invisible to the
// gilOff stop protocol: if the holder parks at a safepoint inside the locked
// region, the stop never completes and the holder never resumes. The contended
// path therefore spins on tryLock(), parking on the thread-granular stop word
// first (it cannot throw or run JS, and it works under a caller's DeferTraps
// scope, where trap servicing is a no-op) and servicing only NeedStopTheWorld
// traps between attempts.
JS_EXPORT_PRIVATE void lockGILOffCompilationLockContended(VM&);

// Scoped acquisition gated on the caller's predicate (vm.gilOffWithProcessGate()
// at every site): flag-off and GIL-on pay one predicted-untaken branch and take
// no lock. The fast path is an uncontended tryLock, which also covers
// same-thread recursion.
class GILOffCompilationLocker {
    WTF_MAKE_NONCOPYABLE(GILOffCompilationLocker);
public:
    GILOffCompilationLocker(VM& vm, bool shouldLock)
        : m_shouldLock(shouldLock)
    {
        if (!m_shouldLock) [[likely]]
            return;
        if (gilOffCompilationLock().tryLock()) [[likely]]
            return;
        lockGILOffCompilationLockContended(vm);
    }

    ~GILOffCompilationLocker()
    {
        if (m_shouldLock) [[unlikely]]
            gilOffCompilationLock().unlock();
    }

private:
    bool m_shouldLock;
};

} // namespace JSC
