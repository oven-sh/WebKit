//@ requireOptions("--useSharedGCHeap=1", "--useDollarVM=1")
// SPEC-heap.md T10: I13 — HeapClientSet add/remove cannot complete between
// stop and resume; removal of a stopped client defers to resume; the
// flipping add runs §10B.4 attach quiescence; §10B.4's cross-spec liveness
// rule (creators poll while granted-unserved tickets exist).
//
// clientChurnVsGC: whole GCClient::Heaps are constructed/attached/detached/
// destroyed in a loop on N-1 threads while one long-lived client pounds
// conducted collections.
//
// attachWithPendingTicket: a granted-unserved (legacy, pre-flip) ticket
// exists when the flipping attach starts; the creator side keeps polling
// stopIfNecessary() so the §10B.4 quiescence loop can complete — then, once
// shared, more clients attach while shared RCAC tickets are pending.
//
// Runnable in the no-JIT TSAN config and JIT-on (§5.5).
load("./resources/assert.js", "caller relative");

if (typeof $vm !== "undefined" && typeof $vm.sharedHeapTest === "function") {
    shouldBeTrue($vm.sharedHeapTest("clientChurnVsGC", 4, 64), "clientChurnVsGC");
    shouldBeTrue($vm.sharedHeapTest("attachWithPendingTicket", 3, 4), "attachWithPendingTicket");

    // Deterministic JS-side checksum after the churn.
    let sum = 0;
    for (let i = 0; i < 5000; ++i)
        sum += [i, i + 1, i + 2].reduce((a, b) => a + b, 0);
    shouldBe(sum, 37507500);
}
print("PASS");
