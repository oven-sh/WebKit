//@ requireOptions("--useSharedGCHeap=1", "--useDollarVM=1")
// SPEC-heap.md T10: I1 (block handles single-owner), I8 (steal atomicity),
// I12 (N-stack conservative roots) via the §12.1 harness.
//
// allocationStorm: N standalone clients allocate pattern-checked cells over
// the shared BlockDirectories while one client conducts collections; every
// retained cell lives only on a harness thread's STACK, so surviving a
// conducted stop proves the §10.6 suspend-and-copy scan covered all N
// mutator stacks (I12). A lost/doubled block handout corrupts a pattern and
// RELEASE_ASSERTs (I1).
//
// stealRace: alternating size-class bursts with full collections between
// phases force findEmptyBlockToSteal through the MSPL'd
// sweep/removeFromDirectory/addBlock sequence (I8).
//
// Runnable in the no-JIT TSAN config; JIT-on exercises the §5.5 rule (server
// allocator tables never populated => JS-side allocation in this very file
// takes the slow path into the main client's TLC).
load("./resources/assert.js", "caller relative");

if (typeof $vm !== "undefined" && typeof $vm.sharedHeapTest === "function") {
    shouldBeTrue($vm.sharedHeapTest("allocationStorm", 4, 20000), "allocationStorm");
    shouldBeTrue($vm.sharedHeapTest("stealRace", 4, 16), "stealRace");

    // JS-side churn after the storm: the main client's heap is intact and
    // the world resumed (deterministic checksum).
    let sum = 0;
    for (let i = 0; i < 10000; ++i) {
        const o = { a: i, b: i * 2, c: "s" + (i & 7) };
        sum += o.a + o.b;
    }
    shouldBe(sum, 149985000);
}
print("PASS");
