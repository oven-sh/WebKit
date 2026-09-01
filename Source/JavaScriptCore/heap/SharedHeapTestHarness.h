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

#include <wtf/text/WTFString.h>

namespace JSC {

class Heap;

// Multi-client shared-heap test harness (SPEC-heap.md §12.1): K raw
// WTF::Threads, each constructing a standalone GCClient::Heap over the given
// server on its own stack (markStandalone() + attachCurrentThread()), running
// C-level allocation/steal/detach/epoch loops with no JS/VM entry. Exposed to
// JS via $vm.sharedHeapTest(name, threads, iters) (INTEGRATE-heap.md item 8).
// The $vm function is registered UNCONDITIONALLY (normal useDollarVM gating
// only — never gated on Options::useSharedGCHeap()); option gating is
// PER-SCENARIO, inside run(): shared-mode scenarios refuse (return false)
// when the option is off, while epochReclaim deliberately runs the legacy
// (!ISS, incl. option-off) reclamation path (T7/I10 exemption) and so MUST
// remain callable with the option off (heap-option-off.js asserts it).
//
// Caller contract: run() is called on the main VM's mutator thread with the
// API lock and (legacy or forwarded) heap access held — the JSDollarVM seam
// guarantees this. Scenarios that spawn clients release the caller's access
// around the multi-threaded section (ReleaseHeapAccessScope) so a
// standalone conductor's §10.4 access barrier can complete while this thread
// is parked in waitForCompletion().
class SharedHeapTestHarness {
public:
    // Runs the named scenario (§12.1). Returns true on success; false for an
    // unknown scenario or a scenario whose preconditions don't hold (e.g. a
    // shared-mode scenario with Options::useSharedGCHeap() off, or
    // epochReclaim once the server is already sticky-shared). Hard invariant
    // violations RELEASE_ASSERT inside the scenario.
    //
    // Scenarios (SPEC-heap.md §12.1):
    //   allocationStorm           I1/I12 — pattern-checked alloc storm + GCs
    //   preciseAllocationStorm    I16 — precise-registry storm + GCs
    //   stealRace                 I1/I8 — empty-block steals across size classes
    //   clientChurnVsGC           I13 — ctor/dtor churn vs stop windows
    //   epochReclaim              I11 — T7 unit test (1-client !ISS config)
    //   structureLockVsSTW        I14 — STW-forbidden scope + deferred allocation
    //   blockedInNativeVsGC       F8 — release/sleep/re-acquire vs conducted stops
    //   syncRequesterStorm        §10.2 — concurrent sync requesters (election)
    //   noEnteredVMsGC            §12.1 — zero-entered-VMs stop path
    //   attachWithPendingTicket   §10B.4 — attach quiescence + creator liveness
    //   deferralVsAllocationStorm I17 — per-client DeferGC depth vs stops
    //   debuggerStopDuringSharedGC §10C(b)/(c) — non-GC GCL stop vs GC conductor
    //   gcDuringDebuggerPark      §10C(a)/(e) — GC requested while GCL stop held
    //   jsThreadsStopVsGCRequester §10C(e) — stop-scope churn vs requesters (G13)
    //   issRevertChurn            §10D — sticky-ISS reversion + re-flip churn
    JS_EXPORT_PRIVATE static bool run(JSC::Heap& server, const String& scenarioName, unsigned threadCount, unsigned iterations);

private:
    // §11/I11 unit tests (T7). epochReclaim covers, in the 1-client !ISS
    // configuration: (a) retire -> legacy GC (runEndPhase reclamation site)
    // -> NOT freed by the retiring cycle -> second legacy GC -> freed; and
    // (b) via a stop-the-world safepoint hook, that a conducted cycle's own
    // periphery suspension (world stopped, compiler threads suspended by
    // stopThePeriphery) does NOT satisfy I11 — the reclaimer bracket alone
    // licenses bumpAndReclaim().
    static bool runEpochReclaimScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations);
    // Installed once via Heap::addStopTheWorldSafepointHook; fires once per
    // collection in both protocols, inside the stop, BEFORE the reclaimer
    // bracket opens.
    static void recordEpochHookObservation(JSC::Heap&);

    // §12.1 multi-client scenarios (T10).
    static bool runAllocationStormScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations);
    static bool runPreciseAllocationStormScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations);
    static bool runStealRaceScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations);
    static bool runClientChurnVsGCScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations);
    static bool runStructureLockVsSTWScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations);
    static bool runBlockedInNativeVsGCScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations);
    static bool runSyncRequesterStormScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations);
    static bool runNoEnteredVMsGCScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations);
    static bool runAttachWithPendingTicketScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations);
    static bool runDeferralVsAllocationStormScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations);
    static bool runDebuggerStopDuringSharedGCScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations);
    static bool runGCDuringDebuggerParkScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations);
    static bool runJSThreadsStopVsGCRequesterScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations);
    static bool runIssRevertChurnScenario(JSC::Heap& server, unsigned threadCount, unsigned iterations);
};

} // namespace JSC
