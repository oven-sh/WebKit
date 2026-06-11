/*
 * Copyright (C) 2013-2023 Apple Inc. All rights reserved.
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
#include "VMEntryScope.h"

#include "ConcurrentButterflyOperations.h"
#include "Heap.h"
#include "JSThreadsSafepoint.h"
#include "Options.h"
#include "SamplingProfiler.h"
#include "VM.h"
#include "VMLite.h"
#include "VMLiteShared.h" // VMLiteRegistry: the gilOff entered-record stores run under its lock.
#include "VMEntryScopeInlines.h"
#include "WasmCapabilities.h"
#include "WasmMachineThreads.h"
#include "Watchdog.h"
#include <atomic>

namespace JSC {

// UNGIL §A.3.4/§J.8 (U-T5): the pre-M4 M7 structural entered-VM tripwire
// (the s_jsThreadsEntered* counters + sticky shared-server slot that used to
// live here) is DELETED, as licensed by SPEC-ungil §A.3.4. GIL-off, entry
// during a §A.3 stop PARKS instead of crashing: a thread completing entry
// acquires heap access through GCClient::Heap::acquireHeapAccess, whose
// §A.3.2b stop-word gate (SB1 seq_cst poll beside the F8 step-2 GSP load)
// mandatory-reverts and parks the entrant on its own NVS ticket until the
// conductor resumes — fresh acquisition is gated, so a §A.3 window never
// admits a new mutator (annex SB1/EXIT1; VMManager.cpp owns the window).
// The phase-1 single-entered-mutator premise the tripwire enforced is
// replaced wholesale by the thread-granular protocol.

void VMEntryScope::setUpSlow()
{
    // UNGIL §A.1.5 (re-frozen at U-T5): GIL-off, the per-entry record lives
    // ONLY on the CURRENT lite — the transitional VM-member shadow is
    // DROPPED (IU obligation 1). With N concurrently-entered threads a
    // VM-wide shadow would be a last-writer-wins data race, and a stale
    // shadow would make a sibling's exit invisible to VM-wide consumers.
    // VM-wide "entered" consumers go through VM::isEntered()/
    // isAnyThreadEntered() (registry walk) and per-thread consumers through
    // VM::currentThreadEntryScope() (both routed by U-T1).
    //
    // RE-KEYED (review fix; closes the former OPEN-BLOCKER, landed in the
    // same wave as the JSThreadsSafepoint.cpp stub reroute):
    // VMEntryScopeInlines.h's ctor/dtor fast paths now gate
    // setUpSlow/tearDownSlow on the CURRENT lite's record when m_vm.gilOff()
    // (ctor: `!VMLite::current().entryScope`; dtor: `VMLite::current().
    // entryScope == this`), so these bodies run exactly once per outermost
    // per-thread entry/exit and the RELEASE_ASSERTs below are genuine
    // protocol tripwires (double-record / foreign-lite), not always-trip
    // gates. The raw vm.entryScope shadow stays GIL-on-only.
    if (m_vm.gilOff()) [[unlikely]] {
        VMLite& lite = VMLite::current();
        RELEASE_ASSERT(lite.vm == &m_vm);
        // UNGIL review fix: the entered-record store runs under the registry
        // lock (leaf — nothing acquired under it) so the cross-thread walks
        // that key off it (anyOtherLiteOfVMEntered, the TERM1.2 retire
        // decision, fanOutTerminationToSiblingLites, isAnyThreadEntered)
        // are serialized against entry/exit transitions. See the field's
        // comment in VMLite.h. GIL-on keeps the plain VM-member shadow.
        auto& registry = VMLiteRegistry::singleton();
        Locker locker { registry.lock };
        RELEASE_ASSERT(!lite.entryScope.load(std::memory_order_relaxed));
        // UNGIL AB-17 interim fail-stop (GIL-removal review round 4): the
        // no-other-entered check and the entered-record publication run
        // under ONE registry-lock hold, so the SECOND concurrent top-level
        // JS entry of this VM aborts HERE, deterministically. The earlier
        // updateStackLimits-time walk (VM.cpp) is necessary but not
        // sufficient: it samples sibling entryScopes BEFORE this store
        // happens (TOCTOU — two entrants can both pass it pre-publication),
        // and a token-holding thread RE-entering JS through a fresh
        // VMEntryScope never re-runs updateStackLimits at all. Both holes
        // close here because every gilOff outermost per-thread entry funnels
        // through this store. Deleted by the same change that lands the
        // §A.2.2 per-lite soft-stack-limit reroute (AB-17) — BUT see the
        // alias coupling below; deletion alone is not licensed.
        //
        // GIL-removal round 5 (TERM1.2 <-> AB-17 ordering, MECHANICAL):
        // VMTraps.cpp's TERM1.2 interim retires the shared NeedTermination
        // bit when no OTHER lite of this VM has a live entryScope — but the
        // delivery obligation is TOKEN-scoped, and a token-holding sibling
        // with NO live entry scope (between its fn-return teardown and the
        // completion drain, or between drainMicrotasks iterations) would
        // RE-ENTER through a fresh VMEntryScope with the bit already
        // cleared and lose the termination permanently
        // (orVMWideTrapBitsIntoLite re-ORs only at TOKEN acquisition, not
        // at entry-scope re-entry — and is a no-op under the alias anyway).
        // That race is unreachable ONLY while this walk refuses the second
        // concurrent entry. Its recorded deletion trigger (§A.2.2) and the
        // TERM1.2 interim's retirement trigger (§A.2.1 per-lite trap words)
        // are DIFFERENT chartered changes, so the walk is keyed on BOTH:
        // the §A.2.2 change flips the constant below, and the walk then
        // still self-retains while perThreadTrapsIfExists aliases the VM
        // word (i.e. §A.2.1 not landed). Delete the whole gate only when
        // both legs are false. INTEGRATE-ungil.md AB-17 records this
        // ordering dependency.
        //
        // AB-17 STATUS (this change): the COMPLETE §A.2.2 reroute is LANDED
        // in this one diff — generated-code soft-limit reads (LLInt shared
        // prologue trap-aware site + 64/32_64 doVMEntry/arity sites via the
        // formerly-staged chained offsets and Group-3 discriminators;
        // Baseline/DFG/FTL/thunk/varargs/Yarr emission sites via
        // AssemblyHelpers::branchPtrAgainstSoftStackLimit), the C++
        // VM::softStackLimit() readers (VMInlines.h helper + the
        // out-of-line VM::softStackLimitForCurrentThreadSlow for JSString
        // ropes/JSONObject/LiteralParser/Yarr), the VMTraps per-lite stop
        // fan in its single-controller form (checklist item 3c: VM-level
        // updateThreadStopRequestIfNeeded fans each registered lite's OWN
        // update, whose shouldStop derives from the lite word PLUS the VM
        // word; cancel restores the PER-LITE saved value via the lite's own
        // StackManager), the item-3b servicing dispatch
        // (handleTrapsForCurrentThreadIfNeeded at every poll site +
        // VMTraps::vm() consulting m_liteOwnerVM), the §F.1
        // lite-registration backfill (VMLiteRegistry::registerLite stamps
        // setLiteOwnerVM and ORs+derives pending VM-wide bits), and the
        // W1/D9 park-site split (LockObject/ConditionObject/ThreadObject
        // re-pointed at the park-lite predicates with the W1 episode).
        // VM::updateStackLimits' N-entered refusal walk is deleted; the
        // VM-level word is now published by CARRIER entries only
        // (serialized under m_lock — no cross-thread clobber; spawned lites
        // publish only their own word).
        //
        // AB-17 ACCEPTANCE STATUS (review round; READ BEFORE TRUSTING
        // "LANDED" ABOVE): the reroute legs are landed, but the headline
        // GIL-off acceptance rung is NOT green. The pinned command
        //   jsc --useJSThreads=1 --useThreadGIL=0 --useVMLite=1
        //       --useSharedAtomStringTable=1 --useSharedGCHeap=1
        //       --useThreadGILOffUnsafe=1 JSTests/threads/smoke.js
        // fails intermittently (6/10 Release runs in the round's
        // measurement) on DOWNSTREAM N-entry legs, not the soft-limit
        // reroute itself:
        //   (i) Debug/ASAN, deterministic: stack-use-after-return in
        //       ThrowScope::~ThrowScope -> ExceptionScope::stackPosition()
        //       on a spawned thread — the VM-level exception-scope
        //       verification chain (m_topExceptionScope) was shared across
        //       lites and pointed into the carrier's stack. FIXED:
        //       INTEGRATE-ungil.md obligation 10 landed — the verification
        //       bookkeeping is per-lite GIL-off via the
        //       VM::exceptionScopeVerificationState() mode-split accessor
        //       (VMLite debug-only L2 tail append; VM copy GIL-on,
        //       bit-identical).
        //   (ii) Release, intermittent: SIGSEGV inside Baseline-JIT'd code
        //       on a spawned thread, and a tier-up RELEASE_ASSERT
        //       ("... result = CompilationInvalidated but our replacement
        //       is ...") — concurrent compilation/replacement legs.
        //   (iii) flag-off serial bench gate (Tools/threads/bench-gate.sh):
        //       transition-heavy-constructor is consistently +2.4-3.4% over
        //       the 2026-06-05 baseline (3x15-run medians; threshold 1%).
        //       Measured NOT attributable to the AB-17 LLInt Group-3
        //       discriminator sites: --useLLInt=0 (LLInt prologue never
        //       runs) shows the same delta. Other 7 benchmarks within
        //       threshold. Needs an A/B bisect against the pre-AB-17 tree.
        // FIXED at the same review: the W1 watchdog parked-carrier livelock
        // (condition-wait/property-wait termination hung GIL-off; Watchdog
        // CallerState::ParkedCarrier verdict), the signed-vs-unsigned
        // branchPtrAgainstSoftStackLimit conditions at all nine JIT-tier
        // sites, and the AB-15 wasm call-path hole (jsCallICEntrypoint +
        // wasm call trampoline/thunk/CallWasm gated under useJSThreads; SD7
        // refusal in callWebAssemblyFunction — a spawned thread calling a
        // carrier-created export now throws TypeError instead of running
        // with carrier stack limits). Until (i)/(ii)/(iii) are root-caused,
        // GIL-off N-entry is LANDED-WITH-KNOWN-FAILING-RUNGS, not accepted;
        // do not record AB-17 as verification-complete downstream of this
        // block.
        //
        // AB-17 round-2 amendment (review finding): the LOL tier
        // (lol/LOLJIT.cpp, --useLOLJIT, replaces Baseline) had a missed
        // prologue soft-limit read. It is now (a) rerouted through
        // branchPtrAgainstSoftStackLimit like the other JIT-tier sites and
        // (b) additionally forced off under GIL-off at option
        // canonicalization (Options.cpp U0 block) until the tier passes the
        // full §A.1.3 COMPILED-FOR-VM audit.
        //
        // AB-17 round-3 amendment (review findings; AB-17 remains
        // LANDED-WITH-KNOWN-FAILING-RUNGS, NOT accepted):
        //  (1) Bench-gate suspect (iii) FIXED at the prime-suspect site:
        //      VM::softStackLimitForCurrentThreadSlow was an unconditional
        //      out-of-line JS_EXPORT_PRIVATE call on flag-off hot paths
        //      (every RegExp match via YarrMatchingContextHolder, JSString
        //      rope resolution, JSON/LiteralParser) — consistent with the
        //      --useLLInt=0 A/B showing the same delta. It is now an
        //      ALWAYS_INLINE VM.h wrapper whose flag-off arm is the single
        //      traps().softStackLimit() load behind one m_gilOff branch;
        //      only the gilOff arm (softStackLimitForCurrentThreadGilOffSlow)
        //      is out-of-line. The bench gate must be RE-RUN before (iii)
        //      is closed; do not assume green.
        //  (2) TERM1.2 per-lite consume-side shield FIXED: handleTraps'
        //      carrier trim now consults the VM-LEVEL
        //      m_carrierTookSharedTermination on per-lite servicing
        //      instances (the set-side always stored it there) and drops the
        //      re-ORed per-lite NeedTermination echo, closing the spurious
        //      re-termination of the host's clear-and-re-enter via the
        //      orVMWideTrapBitsIntoLite -> per-lite-dispatch-first channel.
        //  (3) VM::updateStackLimits spawned arm: the early return now
        //      RELEASE_ASSERTs that a no-publish (liteTraps null) only
        //      happens when no lite of THIS VM is current (silent stale
        //      per-lite limit fail-stops), and the per-lite publish
        //      precommits stack memory on OS(WINDOWS) (spawned lites never
        //      reach the VM-word precommit block).
        // Rungs (i) per-lite exception-state split and (ii) concurrent
        // compilation/replacement remain OPEN; downstream must still not
        // record AB-17 verification-complete.
        //
        // AB-17 round-4 amendment (review findings; bench-gate (iii)
        // RE-RUN PERFORMED as the round-3 text mandates — numbers attached;
        // AB-17 remains LANDED-WITH-KNOWN-FAILING-RUNGS, NOT accepted):
        //  (1) Bench gate (iii) re-run, post-ALWAYS_INLINE-wrapper, 3x15-run
        //      medians vs the 2026-06-05 baseline:
        //      transition-heavy-constructor +2.30% / +0.63% / +1.93%
        //      (improved from the recorded +2.4-3.4%; 2 of 3 passes over the
        //      1% threshold). Other 7 benchmarks within threshold every pass
        //      (megamorphic-access -13.5%, i.e. FASTER than baseline).
        //  (2) The mandated A/B bisect EXONERATES the residual C++
        //      m_gilOff branches: with all three C++ reader sites (the VM.h
        //      wrapper, VMInlines.h softStackLimitForCurrentThread, and
        //      hasExceptionsAfterHandlingTraps) temporarily forced to their
        //      bare pre-AB-17 flag-off forms, the benchmark measured
        //      +12.5% / +13.7% / +15.9% — REMOVING the branches measured
        //      slower, so the delta is not their executed cost (consistent
        //      with the round-3 --useLLInt=0 A/B exonerating the LLInt
        //      discriminator sites).
        //  (3) Measurement-validity probe: a source-identical rebuild of the
        //      unmodified tree then measured +7.9% / +11.8%, and repeated
        //      runs of ONE binary spread 54.4-73.7ms (35%) on this (shared,
        //      load ~8-12) host. CONCLUSION: rung (iii) is reclassified from
        //      "regression" to NOT-MEASURABLE-AT-1%-ON-THIS-HOST; the
        //      recorded +2.4-3.4% red and this re-run's +0.6-2.3% are inside
        //      the observed noise envelope. (iii) stays OPEN; closing it
        //      requires a quiet/pinned host with baseline and candidate
        //      recorded interleaved in the same session.
        //  (4) Finding-(h) coverage hole CLOSED: the TERM1.2 carrier-trim
        //      registry-lock acquisition in VMTraps::handleTraps (the one
        //      trap-machinery registry acquisition missing the re-rank
        //      assert) now calls
        //      assertNoPerLiteTrapSignalingLockHeldOnCurrentThread like the
        //      other five sites (debug-only; release no-op).
        //  (5) Golden-disasm scope note (LLInt evidence channel): the three
        //      AB-17 LLInt discriminator expansions (shared prologue,
        //      doVMEntry, functionArityCheck) are NOT capturable by the
        //      golden-disasm gate BY ITS OWN CONTRACT (LLInt is asm, not
        //      --dumpDisassembly output; see golden-disasm.sh header), and
        //      no golden baseline exists in-tree to diff against. Their
        //      chartered flag-off-cost license is the --useJIT=0 bench-gate
        //      run (UNGIL-HANDOUT delta-(a) text) — subject to the same
        //      host-noise blocker as (1)-(3).
        //
        // AB-17 round-5 amendment (review findings):
        //  (1) WASM FORCED OFF UNDER GIL-OFF (Options.cpp U0 block, LOLJIT
        //      precedent): the wasm<->JS glue still emits raw VM-block
        //      VM::exceptionOffset() loads (WasmToJS.cpp x6, JSToWasm.cpp
        //      x3, WebAssemblyBuiltinTrampoline.cpp x1) — inert spare
        //      storage GIL-off, i.e. silently missed exceptions on a
        //      CARRIER-executed wasm<->JS call. The AB-15 SD7 refusal below
        //      only covers spawned threads. Delete the Options.cpp refusal
        //      once those ten sites are rerouted through the mode-keyed
        //      exception-slot pattern and the wasm tier passes §A.1.3.
        //  (2) Rung (iii) note: RETURN_IF_EXCEPTION's flag-off expansion now
        //      evaluates gilOffWithProcessGate() (two read-only-Config-page
        //      byte tests) before the legacy maybeNeedHandling() load. Same
        //      class as the round-4 item-(2) exonerated m_gilOff branches;
        //      rung (iii) remains OPEN and blocked on a quiet host per
        //      round-4 item (3) — do not record it green without that run.
        //
        // AB18-G disposition (V5 bench worsening; reviewed 3/3):
        //  (1) The two failing V5 gates were PROTOCOL-INVALID: --runs 5 vs
        //      the bench-gate.sh default 9, on this shared load ~8-12 host
        //      whose documented noise envelope (round-4 item (3): 35%
        //      single-binary spread; +7.9%/+11.8% source-identical rebuild)
        //      strictly contains the reported +4.18%/+6.35%. The
        //      WORSE-than-+1.78% claim is superseded as protocol-invalid,
        //      not as measured-and-closed. V5/AB18-G must NOT be recorded
        //      green on this argument or on the code edits below alone;
        //      closure requires actually running the round-4-mandated
        //      interleaved A/B (taskset on an isolated core, loadavg < 1
        //      checked first, >= 15 runs, baseline and candidate alternated
        //      B,C,B,C in ONE session) and appending the numbers here. Do
        //      not re-record baseline.json from this host/session.
        //  (2) Code change (JITOperations.cpp operationHandleTraps /
        //      operationOptimize + its companion RELEASE_ASSERT): the last
        //      raw vm.gilOff() loads in that file now use
        //      gilOffWithProcessGate(), and the operationOptimize
        //      conjunction is reordered so flag-off never touches the
        //      m_gilOff line (Bench item I3 false sharing with the
        //      fetch_or'd m_entryScopeServicesRawBits). These edits SHRINK
        //      the I3 window at two cold sites (traps fire rarely; tier-up
        //      is warmup-only) and do NOT close it: a real GIL-off process
        //      still loads m_gilOff from the contended line, and other
        //      readers of that VM member cluster still take the coherence
        //      miss. Structural closure is a FOLLOW-UP, only after the
        //      interleaved A/B proves an attributable delta: isolate
        //      m_entryScopeServicesRawBits on its own cache line
        //      (alignas/padding) or relocate immutable m_gilOff into a
        //      read-mostly cluster.
        //  (3) If the interleaved A/B still shows a reproducible delta,
        //      AB18-G stays open and the next proposal is the out-of-scope
        //      residual: hoisting one group3Primitives() materialization
        //      per JIT operation in FrameTracers.h / the
        //      RETURN_IF_EXCEPTION flag-off expansion — not more edits to
        //      the two files scoped here.
        //
        // AB18-G follow-up (post-ab17b bench-regression round; measurements
        // appended per item (1)'s mandate — NO code changes this round):
        //  (1) CODEGEN RULE VERIFIED AT MACHINE-CODE LEVEL (new evidence
        //      class; closes "does flag-off emit today's code" for this
        //      bench): both hot FTL bodies of transition-heavy-constructor
        //      (make: inline-capacity-12 NewObject bump-allocate + 12 inline
        //      stores + 2 StructureID stores; run: structure check + two
        //      inline loads + checked adds + direct call) were dumped from a
        //      live Release process (gdb dump of the JIT region named by
        //      --dumpDisassembly ranges; objdump decode) and are the
        //      pre-threads instruction stream exactly: no TLS reads, no
        //      fences, no extra loads/branches; the prologue soft-stack-limit
        //      check is the single absolute-address compare (the AB-17
        //      reroute's flag-off arm). Repeat the gdb/objdump procedure if
        //      this rung reds again — it is cheap and decisive where
        //      --dumpDisassembly has no in-process disassembler.
        //  (2) The residual is not stalls either: a local 250-iteration copy
        //      (BENCH.md "local copy" rule) measures marginal IPC 3.70 and
        //      ~17.1M marginal instructions/iteration, ruling out the I3
        //      false-sharing class and code-layout stall theories for the
        //      observed delta. Eden cadence is policy-stable (23 GCs/run =
        //      allocation / 32MB cycle allowance; ~142kb survivors/GC).
        //  (3) Host-sensitivity re-confirmed on a LOW-LOAD session (loadavg
        //      ~1.7/64): with the 7 non-allocating benchmarks within +-0.6%
        //      of the 2026-06-05 baseline ALL session (megamorphic-access
        //      -12..-13.6%, i.e. faster), the one allocating/GC-heavy bench
        //      drifted +6.12% (5-run median) -> +11.36% (9-run median,
        //      ~90 min later) on the SAME binary, plain runs 57.8 -> 64.0ms
        //      within the session. Measured-region composition: ~56-75% FTL,
        //      remainder split kernel (anon-page first-touch faults, futex/
        //      timer/scheduler traffic from parallel eden marking) + GC/
        //      allocator C++ — i.e. exactly the shared-kernel/SMP resources
        //      a multi-tenant host perturbs, which the non-allocating
        //      benches never touch. The ab17b-verify +10.59%/+8.04% reds sit
        //      inside this same-binary drift envelope.
        //  (4) Disposition: rung (iii) stays NOT-MEASURABLE-AT-1%-ON-THIS-
        //      HOST (round-4 item (3)); AB18-G structural edits remain
        //      blocked on the interleaved A/B per item (2), which now
        //      additionally requires REBUILDING a pre-ab17b baseline binary
        //      from VCS — no such binary survives on this host (Fuzz build
        //      is ASAN; TSan build is TSAN; both incomparable).
        constexpr bool perLiteSoftStackLimitRerouteLanded = true; // COMPLETE §A.2.2 reroute landed (AB-17; this change).
        bool perLiteTrapWordsStillAliasVMTrapWord = perThreadTrapsIfExists(lite) == &m_vm.traps(); // §A.2.1 landed: false for gilOff lites.
        if (!perLiteSoftStackLimitRerouteLanded || perLiteTrapWordsStillAliasVMTrapWord) {
            for (VMLite* other : registry.lites) {
                if (other->vm == &m_vm && other != &lite) {
                    RELEASE_ASSERT_WITH_MESSAGE(!other->entryScope.load(std::memory_order_relaxed),
                        "GIL-off second concurrent JS entry refused: VM-level soft stack limit is shared (AB-17 not landed) or per-lite trap words still alias the VM word (TERM1.2 interim, A.2.1 not landed)");
                }
            }
        } else {
            // Self-verifying go-live (review round 6 amendment): when the
            // refusal walk retires, the entering thread's PER-LITE soft
            // stack limit must already have been published by its own pass
            // through updateStackLimits (JSLock::didAcquireLock ->
            // setStackPointerAtVMEntry). A null word here means the §A.2.2
            // reroute or the lite-registration backfill is missing or
            // regressed — refuse at the same deterministic trip point as
            // the fail-stop above instead of running generated code against
            // a never-published limit.
            RELEASE_ASSERT_WITH_MESSAGE(lite.threadContext.traps().softStackLimit(),
                "GIL-off N-entered go-live refused: entering lite's per-thread soft stack limit was never published (AB-17 §A.2.2 reroute or registration backfill missing/regressed)");
        }
        lite.entryScope.store(this, std::memory_order_relaxed);
    } else
        m_vm.entryScope = this;

#if ASSERT_ENABLED
    // SPEC-vmstate I14: an installed VMLite always belongs to the VM whose
    // JSLock this thread holds.
    if (Options::useVMLite()) {
        if (VMLite* lite = VMLite::currentIfExists())
            ASSERT(lite->vm == &m_vm);
    }
    // SPEC-jit I19: the per-thread butterfly TID tag must be coherent before
    // any JS runs on this thread (CS3; zero-init is correct only for the
    // main thread).
    if (Options::useJSThreads()) [[unlikely]]
        assertButterflyTIDTagCoherent();
#endif

    auto& thread = Thread::currentSingleton();
    if (!thread.isJSThread()) [[unlikely]] {
        Thread::registerJSThread(thread);

        if (Wasm::isSupported())
            Wasm::startTrackingCurrentThread();
#if HAVE(MACH_EXCEPTIONS)
        registerThreadForMachExceptionHandling(thread);
#endif
    }

    if (m_vm.hasAnyEntryScopeServiceRequest() || m_vm.hasTimeZoneChange()) [[unlikely]]
        m_vm.executeEntryScopeServicesOnEntry();
}

void VMEntryScope::tearDownSlow()
{
    ASSERT_WITH_MESSAGE(!m_vm.hasCheckpointOSRSideState(), "Exitting the VM but pending checkpoint side state still available");

    // UNGIL §A.1.5 (U-T5): GIL-off, clear ONLY the per-lite record (the
    // CURRENT lite is the one this scope was recorded on — the dtor runs on
    // the ctor's thread). The VM-member shadow is dropped (see setUpSlow);
    // GIL-on keeps the landed single write.
    if (m_vm.gilOff()) [[unlikely]] {
        VMLite& lite = VMLite::current();
        RELEASE_ASSERT(lite.vm == &m_vm);
        // Registry-lock-serialized for the same reason as setUpSlow's store
        // (see VMLite.h's entryScope comment).
        Locker locker { VMLiteRegistry::singleton().lock };
        RELEASE_ASSERT(lite.entryScope.load(std::memory_order_relaxed) == this);
        lite.entryScope.store(nullptr, std::memory_order_relaxed);
    } else
        m_vm.entryScope = nullptr;

    // §A.1.5: executeEntryScopeServicesOnExit uses the CURRENT lite's bits
    // when gilOff (VM::has/clearEntryScopeService route there).
    if (m_vm.hasAnyEntryScopeServiceRequest()) [[unlikely]]
        m_vm.executeEntryScopeServicesOnExit();
}

} // namespace JSC
