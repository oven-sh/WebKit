//@ requireOptions("--useSharedGCHeap=1", "--useDollarVM=1")
// SPEC-congc CG-T1 (C0, flags-off half): the CG-1 window split of the
// conduct path must be byte-for-byte flag-off (CG-I0) — every §13.2 stage
// flag is OFF here (none even exists yet), so a conduct is exactly ONE
// window (open(FirstWindow) ... close(final)) and this file must behave
// exactly as the pre-split corpus did.
//
// Paths exercised (all CG-1-touched):
//  - runSharedGCElection winner arm + F20 ownership-checked deferred clear
//    (syncRequesterStorm: competing sync requesters storm the election;
//    the CGD3.1 wind-down race window is hammered by back-to-back tickets);
//  - tryConductSharedCollectionForPoll (allocation-driven poll conducts);
//  - JSThreadsStopScope ctors (F45 waiter bracket) vs conductors
//    (jsThreadsStopVsGCRequester, gcDuringDebuggerPark,
//    debuggerStopDuringSharedGC: stop-scope churn against elections — the
//    §3.4 election/poll guards must NEVER fire flag-off, since any GCL-free
//    point is NotRunning, ANNEX CGD1.1 flag-off half);
//  - pollIssRevertIfNeeded post-F11-restructure (issRevertChurn: the
//    bounded GEC wait must still run — and only run — in NotRunning).
//
// Determinism: scenario verdicts and the JS-side checksum are the
// byte-identical oracle the CG-T1 gate diffs against the pre-split run.
load("./resources/assert.js", "caller relative");

if (typeof $vm !== "undefined" && typeof $vm.sharedHeapTest === "function") {
    // Election storm: many granted tickets, every wind-down re-checked by a
    // late-granted requester (the F20 deferred-clear window).
    shouldBeTrue($vm.sharedHeapTest("syncRequesterStorm", 4, 24), "syncRequesterStorm");

    // Stop-scope (GCL) vs conductor interleavings: first-window tryLock
    // carve-out (F15) under contention; F45 counter brackets every scope.
    shouldBeTrue($vm.sharedHeapTest("jsThreadsStopVsGCRequester", 4, 24), "jsThreadsStopVsGCRequester");
    shouldBeTrue($vm.sharedHeapTest("gcDuringDebuggerPark", 3, 16), "gcDuringDebuggerPark");
    shouldBeTrue($vm.sharedHeapTest("debuggerStopDuringSharedGC", 3, 16), "debuggerStopDuringSharedGC");

    // Revert-poll restructure (F11/CGD1.2): client churn arms
    // m_issRevertPending; the main client's polls must still complete the
    // revert (flag-off the mid-cycle back-off arm is unreachable).
    shouldBeTrue($vm.sharedHeapTest("issRevertChurn", 3, 12), "issRevertChurn");

    // JS-side churn after the storms: the main client's heap is intact and
    // the world resumed (deterministic checksum, same shape as the
    // heap-allocation-storm.js oracle).
    let sum = 0;
    for (let i = 0; i < 10000; ++i) {
        const o = { a: i, b: i * 2, c: "s" + (i & 7) };
        sum += o.a + o.b;
    }
    shouldBe(sum, 149985000);

    // A final synchronous full collection drives one more clean
    // election -> conduct -> wind-down sequence through the split helpers.
    $vm.gc();
    shouldBeTrue(true, "post-storm full GC completed");
}
print("PASS");
