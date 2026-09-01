//@ requireOptions("--useSharedGCHeap=1", "--useConcurrentSharedGCMarking=1", "--useJSThreads=1", "--useDollarVM=1", "--numberOfGCMarkers=4")
// SPEC-congc CG-T4 (C1): allocation/steal storm during marking — ANNEX
// CGT1.6 charter (CG-3a).
//
// The §6.2 N-client rules under stage C1: out-of-window allocation DURING
// marking must come out live (allocate-black via versioned newlyAllocated,
// §6.1), every sweep-to-freelist and block steal stays under MSPL with a
// stable isMarking (§6.2(2)), and the per-client flush at EVERY WND-open
// covers freelist cells (§6.2(3)/(5)). The endMarking liveness assert
// (§6.2(5)): the conservative-stack-root snapshot walk in Heap::endMarking()
// — every snapshot cell must carry a version-current liveness bit before the
// newlyAllocated version retires; it RELEASE_ASSERTs on the guilty cycle.
// That walk is live in the current tree for every ISS cycle (the
// fix-shared-heap-corruption instrumentation), so this storm runs with the
// assert enabled by construction; the CGT1.9 re-gating (debug,
// stage-flag-gated) is tracked in INTEGRATE-congc.md.
//
// Two arms:
//  1. Harness arm: the C++ §12.1 scenarios (allocationStorm /
//     preciseAllocationStorm / stealRace) with the C1 stage flag ON — N
//     standalone clients allocate pattern-checked cells over the shared
//     BlockDirectories while collections now take Concurrent windows; stack
//     retention across windows proves the §10.6 scan + §6.2(5) liveness.
//  2. JS-thread arm: N Threads allocate/steal-pressure size classes during
//     forced marking — alternating size-class bursts force
//     findEmptyBlockToSteal through sweep/removeFromDirectory/addBlock
//     against mid-cycle windows (steal-vs-mark, CG-T4's named race).
load("./harness.js", "caller relative");

if (typeof $vm !== "undefined" && typeof $vm.sharedHeapTest === "function") {
    // Harness arm (runs whether or not the Thread global is available).
    shouldBeTrue($vm.sharedHeapTest("allocationStorm", 4, 20000), "allocationStorm under C1");
    shouldBeTrue($vm.sharedHeapTest("preciseAllocationStorm", 4, 2000), "preciseAllocationStorm under C1");
    shouldBeTrue($vm.sharedHeapTest("stealRace", 4, 16), "stealRace under C1");
}

if (typeof Thread === "function" && typeof $vm !== "undefined") {
    const N = 4;
    const PHASES = 12;
    const gate = { go: 0, started: 0, done: 0 };

    // Retained ring per thread: cells allocated DURING marking must survive
    // the cycle that observed them (allocate-black / WND-open flush). The
    // ring is the §6.2(5) witness — each slot is reachable only via this
    // array, and slots are overwritten round-robin so every phase both
    // creates marking-era cells and frees older ones for the sweep/steal
    // path to rehandle.
    const RING = 256;
    const rings = [];
    for (let t = 0; t < N; ++t)
        rings.push(new Array(RING).fill(null));

    const threads = spawnN(N, (t) => {
        const ring = rings[t];
        Atomics.add(gate, "started", 1);
        while (Atomics.load(gate, "go") === 0)
            Atomics.wait(gate, "go", 0, 100);
        let cursor = 0;
        for (let phase = 0; phase < PHASES; ++phase) {
            // Alternate size classes phase by phase: small properties vs
            // butterfly-heavy vs string-carrying — different directories, so
            // emptied blocks from one phase are steal candidates in the
            // next (I8 under marking).
            for (let i = 0; i < 4000; ++i) {
                let cell;
                if (phase & 1)
                    cell = { k: i, arr: [i, i + 1, i + 2, i + 3] };
                else
                    cell = { k: i, s: "c" + (i & 15), pad0: 0, pad1: 1, pad2: 2 };
                cell.check = (t << 24) ^ (phase << 16) ^ i;
                ring[cursor & (RING - 1)] = cell;
                cursor++;
            }
            // Verify the surviving ring slice: a cell freed under us by an
            // under-marked cycle reads a corrupted/garbage check.
            for (let r = 0; r < RING; ++r) {
                const cell = ring[r];
                if (cell === null)
                    continue;
                if ((cell.check ^ (t << 24)) < 0)
                    throw new Error("ring corruption at thread " + t + " slot " + r);
            }
        }
        Atomics.add(gate, "done", 1);
        return cursor;
    });

    waitUntil(() => Atomics.load(gate, "started") === N);
    Atomics.store(gate, "go", 1);
    Atomics.notify(gate, "go", Infinity);

    // Main: keep cycles coming so thread allocation overlaps marking and
    // between-window execution.
    while (Atomics.load(gate, "done") < N) {
        let junk = [];
        for (let i = 0; i < 3000; ++i)
            junk.push({ m: i });
        if (junk.length !== 3000)
            throw new Error("main churn lost allocations");
        $vm.gc();
    }
    const cursors = joinAll(threads);
    for (let t = 0; t < N; ++t)
        shouldBe(cursors[t], PHASES * 4000);

    // Final full cycle over the retained rings, then a last integrity walk.
    $vm.gc();
    for (let t = 0; t < N; ++t) {
        let live = 0;
        for (let r = 0; r < RING; ++r) {
            if (rings[t][r] !== null)
                live++;
        }
        shouldBe(live, RING);
    }
}
print("PASS");
