//@ requireOptions("--useSharedGCHeap=1", "--useDollarVM=1")
// SPEC-heap.md T10: I16 — the precise-allocation registry
// (m_preciseAllocations vectors, preciseAllocationSet, indexInSpace stamps)
// is mutated only under MSPL or world-stopped.
//
// preciseAllocationStorm: N standalone clients allocate
// larger-than-largeCutoff cells (the §5.6 tryCreate+register critical
// section) interleaved with small-block handout under the same MSPL, while
// full collections iterate the registry world-stopped (I5). The race
// amplifier's precise-registration hook widens the indexInSpace window.
//
// Runnable in the no-JIT TSAN config and JIT-on (§5.5).
load("./resources/assert.js", "caller relative");

if (typeof $vm !== "undefined" && typeof $vm.sharedHeapTest === "function") {
    shouldBeTrue($vm.sharedHeapTest("preciseAllocationStorm", 4, 2000), "preciseAllocationStorm");

    // JS-side precise allocations afterwards (large arrays force precise/aux
    // large paths in the main client too); deterministic checksum.
    let sum = 0;
    for (let i = 0; i < 16; ++i) {
        const big = new Array(64 * 1024).fill(i);
        sum += big[0] + big[big.length - 1];
    }
    shouldBe(sum, 240);
}
print("PASS");
