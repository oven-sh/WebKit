//@ requireOptions("--useSharedGCHeap=1", "--useDollarVM=1")
// SPEC-heap.md T10: §10D — sticky-ISS reversion churn.
//
// issRevertChurn: each round attaches one short-lived secondary client
// (sticky ISS flips), lets it die (remove() leaving size() == 1 arms
// m_issRevertPending), then the MAIN client's thread polls SINFAC until the
// reversion lands — a short-lived secondary client must not downgrade GC
// forever. The loop then re-flips ISS on the SAME server (I13 allows it),
// covering the §10B.4 re-flip path, including the stale per-client access
// state left by the post-revert legacy protocol. Items retired while shared
// drain at the legacy runEndPhase site after the reversion.
//
// GIL-off (UNGIL §0 U0c, ANNEX U0C): the §10D reversion arm no-ops — the
// designated server stays ISS for process lifetime and the poll only disarms
// the hint. The harness scenario mode-splits: GIL-off it verifies the U0c
// shape (poll disarms, server stays shared, retired items drain at §10
// step 7 of conducted full cycles) instead of waiting for a reversion the
// spec forbids.
//
// Runnable in the no-JIT TSAN config and JIT-on (§5.5).
load("./resources/assert.js", "caller relative");

if (typeof $vm !== "undefined" && typeof $vm.sharedHeapTest === "function") {
    shouldBeTrue($vm.sharedHeapTest("issRevertChurn", 2, 8), "issRevertChurn");

    // Post-reversion the legacy protocol owns this VM again: plain JS GC
    // churn must behave (deterministic checksum).
    let sum = 0;
    for (let i = 0; i < 5000; ++i) {
        const o = { v: i };
        sum += o.v;
    }
    shouldBe(sum, 12497500);
}
print("PASS");
