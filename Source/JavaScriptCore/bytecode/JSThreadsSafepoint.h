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
#include <wtf/ScopedLambda.h>

namespace JSC {

class VM;

// JSThreadsSafepoint (SPEC-jit R1): the single safepoint primitive consumed by
// code jettison (SPEC-jit section 5.3) and Class-A watchpoint fires (section 5.6)
// under shared-memory threads. The real mechanics are a veneer over the
// VMManager stop-the-world machinery plus integration manifest M4
// (requester-as-conductor arbitration, StopReason::JSThreads, GC serialization
// via Heap::JSThreadsStopScope, and a resume-path ISB on every mutator leaving
// notifyVMStop).
//
// INTERIM STUB until M4 lands (SPEC-jit Task 1; mirrors SPEC-objectmodel
// manifest entry 6): stopTheWorldAndRun RELEASE_ASSERTs that at most one VM is
// concurrently entered (the phase-1 GIL guarantees this) and runs `work` inline
// on the caller's stack. worldIsStopped() reports true inside that closure and
// while the object-model workstream's interim stub witness is raised, so every
// world-stopped assert (I2/I8 and the OM fire asserts) is exercised with today's
// single-mutator builds and with the GIL'd Thread() stub.
//
// Already live ahead of M4 (SPEC-jit Task 5, section 5.3):
//  - R1.h: a caller that is already world-stopped runs `work` inline without
//    re-requesting, with the witness raised across the closure;
//  - R1.i/CS2: when the SERVER heap the caller's client attaches to
//    (vm.clientHeap.server() — NOT the VM's own, possibly idle, heap member;
//    R4-1) is a shared server, the requesting path releases THIS client's
//    heap access (GCClient::Heap::releaseHeapAccess) and holds
//    Heap::JSThreadsStopScope on that server (the rank-2 GC conductor lock)
//    across `work`, so a shared-mode GC can neither start nor be mid-cycle
//    while the closure patches code;
//  - F5 (stub form): an instruction-stream barrier (crossModifyingCodeFence)
//    on the closing edge of the closure — the single-mutator stand-in for the
//    per-mutator ISB that M4's NVS resume tail issues (R1.d).
//
// CodeBlock::jettison is the section 5.3 choke point: every flag-on jettison
// with reason != JettisonDueToOldAge routes its entire body through
// stopTheWorldAndRun (reoptimization, watchpoint-fire and debugger triggers
// alike), so callers of jettison never need their own stop.
//
// Caller contract (unchanged when M4 lands): caller is an entered mutator and
// holds NO lock from the SPEC-jit section 7 order and no cell lock; `work` runs
// with every mutator stopped and must neither allocate in the JS heap nor
// re-enter the VM. The requesting path additionally requires the caller's heap
// access to be releasable (no allocation in flight). An already-world-stopped
// caller (R1.h path) is exempt from the entered-mutator requirement: its
// safety argument is the enclosing stop.
namespace JSThreadsSafepoint {

// Stop every mutator, run `work` on the caller's own stack, resume.
// Idempotent w.r.t. an already-stopped world: a caller that is already running
// world-stopped (e.g. a watchpoint fire reached from a GC's stopped window or a
// nested fire inside an outer stopTheWorldAndRun closure) just runs inline
// without re-requesting (R1.h).
JS_EXPORT_PRIVATE void stopTheWorldAndRun(VM&, const ScopedLambda<void()>& work);

// True while no other mutator can be executing JS. Disjuncts per SPEC-jit
// section 5.6: VMManager world mode is Stopped, OR the shared GC heap reports
// worldIsStoppedForAllClients() (once the heap workstream lands), OR the legacy
// per-VM GC stop (vm.heap.worldIsStopped()), OR (pre-M4 only) an interim stub
// witness is raised.
JS_EXPORT_PRIVATE bool worldIsStopped(VM&);

// VM-less conservative form for patching sites that have no VM in scope
// (DFG::CommonData::invalidateLinkedCode, DFG::JumpReplacement::fire). Covers
// the VMManager mode and the interim stub witnesses only; it cannot consult
// per-heap state, so it is strictly weaker than worldIsStopped(VM&) and is used
// for asserts only.
JS_EXPORT_PRIVATE bool worldIsStopped();

// ===== Pre-M4 already-stopped witness scope (review round 3, R3-1/R3-11) =====
//
// RAII over the interim stub's process-global world-stopped witness for a
// caller whose own evidence that the world is stopped is ALREADY established
// (worldIsStopped(vm) is true) but possibly only via per-heap state that the
// VM-less worldIsStopped() consumers (the patching asserts in
// DFG::CommonData::invalidateLinkedCode / DFG::JumpReplacement::fire) cannot
// see. The constructor:
//   1. if no process-global witness holds yet, RELEASE_ASSERTs that the
//      per-heap evidence in fact covers every mutator in the process (the
//      R2-4 tripwire, with the R3-11 shared-server scoping: entered VMs that
//      are clients of a shared server currently stopped-for-all-clients are
//      parked by that stop and do not count); then
//   2. raises the global stub depth witness.
// The destructor issues the F5 instruction-stream barrier
// (crossModifyingCodeFence) and lowers the witness. Nests freely.
//
// Users: stopTheWorldAndRun's R1.h already-stopped path, and
// WatchpointSet::fireAllUnderClassAStop branch (1) — the inline fire on
// already-stopped evidence, which previously ran with neither the tripwire
// nor the witness (review round 3, R3-1). Deleted at M4 with the rest of the
// stub counter.
//
// NOTE (R3-4): the constructor's entered-VM count is a SAMPLED tripwire, not
// a structural guarantee — nothing stops a thread from entering another VM
// right after the count. The structural enforcement point pre-M4 is VM entry
// itself: manifest M7 (docs/threads/INTEGRATE-jit.md) adds a process-global
// entered-VM counter to VMEntryScope that RELEASE_ASSERTs sole-entry under
// useJSThreads, making a second concurrent entry crash deterministically on
// the ENTERING thread regardless of timing. Until M7 is applied, flag-on with
// more than one concurrently-enterable VM is an unsupported configuration.
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
// calling stopTheWorldAndRun; the requester's wait loop calls
// watchdogAssertStopProgress(requestStart) on every iteration while awaiting
// Mode::Stopped. Pre-M4 the interim stub never waits, so the watchdog is
// dormant by construction; M4's real parking loop MUST call it.
// THREADS-INTEGRATE(jit): wire watchdogAssertStopProgress into the
// requester-side wait loop when M4 replaces the stub.
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
// at `requestStart` has not completed within a generous timeout. Safe to call
// repeatedly from the wait loop; cheap when under the timeout. When the
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

// ===== GIL-removal tripwire (review round 1) =====
//
// The jit workstream ships several KNOWN GIL-SOUND-ONLY gaps (consolidated
// list: docs/threads/INTEGRATE-jit.md "GIL-removal preconditions"):
// DFG64/FTL array-element store predicates, the LLInt monomorphic-call record
// form, the MultiDeleteByOffset flag-on bail, allocation tagging, the ARM64
// R7 dest==base residue, the deferred Class-A fire fact-publication ordering
// (precondition 10 — B5: MECHANISM landed,
// DeferredWatchpointFire::fireEarlyForGILOff + the release claim-CAS in
// WatchpointSet::fireAllSlow(VM&, DeferredWatchpointFire*); per-site Task-11
// "fact published before fire?" classification is the residual),
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

} // namespace JSC
