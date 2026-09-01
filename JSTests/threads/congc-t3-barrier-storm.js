//@ requireOptions("--useSharedGCHeap=1", "--useConcurrentSharedGCMarking=1", "--useJSThreads=1", "--useDollarVM=1", "--numberOfGCMarkers=4")
// SPEC-congc CG-T3 (C1): barrier storm — ANNEX CGT1.5 charter (CG-3a).
//
// N threads store-heavy during forced concurrent marking: every store into
// an OLD (pre-marked) object is a writeBarrier; under C1R
// (useConcurrentSharedGCMarking && ISS) each client thread's barrier appends
// ride its per-client CMS (SPEC-congc §5.2) and are drained into
// m_sharedMutatorMarkStack at every WND-open, BEFORE the window's first
// constraint pass (CGA1 A21). A lost CMS cell is a lost re-mark: the cycle
// under-marks and a later sweep frees a reachable object — the storm's
// post-GC checksum walk then reads garbage or crashes.
//
// Run-config arms carried by the drivers, NOT by extra code here:
//  - TSAN no-JIT: Tools/threads/tsan/run-corpus-tsan.sh with
//    --useConcurrentSharedGCMarking=1 (suppressions = documented
//    RACY-TOLERATED rows only, CGA1/CGN1).
//  - Amplifier (Tools/threads/amplify.sh): the CGT1.5-named perturbation
//    surfaces — WND-open barrier entry, CMS donate, fence republish,
//    m_isMarking resume edge, steal-vs-mark — are slow-path sites already
//    instrumented or adjacent to RaceAmplifier::perturb() hooks; this file
//    is the amplifier's target workload.
//  - F19 sub-arm: the SAME file under the GIL-off env (JSC_useJSThreads=1
//    JSC_useThreadGIL=0 JSC_useVMLite=1 JSC_useSharedAtomStringTable=1
//    JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1) plus the C1 flag.
//    The SERVER fence pair staying tautological is the engine-side §5.3(3)
//    pin (CG-2; ANNEX CGD2.2 reader table): setMutatorShouldBeFenced keeps
//    the always-fenced forcing when GIL-off, so every emitted-code reader
//    stays fenced. The JS-visible consequence asserted below: two full
//    cycles, then a fence storm — every store still barriers correctly and
//    no post-cycle store is lost.
load("./harness.js", "caller relative");

if (typeof Thread === "function" && typeof $vm !== "undefined") {
    const N = 4;            // storm threads
    const ROWS = 64;        // old objects per thread (barrier targets)
    const BURSTS = 40;      // store bursts per thread
    const BURST_LEN = 2000; // stores per burst

    // Build the OLD object graph up front and age it: one full collection
    // makes every row object black/old, so the storm's stores are the
    // barrier-relevant kind (old -> new edges that ONLY the barrier/CMS
    // path can re-grey).
    const rows = [];
    for (let t = 0; t < N; ++t) {
        const mine = [];
        for (let r = 0; r < ROWS; ++r)
            mine.push({ a: 0, b: 0, ref: null, tag: (t << 16) | r });
        rows.push(mine);
    }
    $vm.gc(); // age the graph

    const gate = { go: 0, started: 0, done: 0 };

    const threads = spawnN(N, (t) => {
        const mine = rows[t];
        Atomics.add(gate, "started", 1);
        while (Atomics.load(gate, "go") === 0)
            Atomics.wait(gate, "go", 0, 100);
        let acc = 0;
        for (let burst = 0; burst < BURSTS; ++burst) {
            for (let i = 0; i < BURST_LEN; ++i) {
                const row = mine[i & (ROWS - 1)];
                // Old->new edge: the freshly allocated leaf is reachable
                // ONLY through the barriered store. If the CMS/donate path
                // loses the barrier, marking never sees the leaf.
                row.ref = { v: i, burst, t };
                row.a = row.a + 1;
                row.b = row.a + row.ref.v;
                acc += row.b & 0xff;
            }
            // Allocation churn keeps the cycle marking long enough to take
            // Concurrent windows between this thread's bursts.
            let junk = [];
            for (let i = 0; i < 64; ++i)
                junk.push({ x: i, s: "j" + (i & 7) });
            if (junk.length !== 64)
                throw new Error("churn lost allocations");
        }
        Atomics.add(gate, "done", 1);
        return acc;
    });

    waitUntil(() => Atomics.load(gate, "started") === N);
    Atomics.store(gate, "go", 1);
    Atomics.notify(gate, "go", Infinity);

    // Main: force collection pressure while the storm runs — async requests
    // from allocation churn plus periodic synchronous fulls, so the storm
    // overlaps marking (and, with the C1 flag on, between-window mutator
    // execution).
    let mainChurnSum = 0;
    while (Atomics.load(gate, "done") < N) {
        for (let i = 0; i < 5000; ++i) {
            const o = { a: i, b: i * 2 };
            mainChurnSum += o.a + o.b;
        }
        $vm.gc();
    }
    joinAll(threads);

    // Post-storm verification: every row's invariant b == a + ref.v must
    // hold and every leaf must be intact — a lost barrier shows up as a
    // freed/garbage leaf or a corrupted row.
    for (let t = 0; t < N; ++t) {
        for (let r = 0; r < ROWS; ++r) {
            const row = rows[t][r];
            shouldBe(row.tag, (t << 16) | r);
            shouldBeTrue(row.ref !== null, "row.ref survived");
            shouldBe(row.b, row.a + row.ref.v);
        }
    }

    // F19 sub-arm tail: two more FULL cycles, then a fence storm. Under
    // GIL-off C1 the server pair is pinned always-fenced (CGD2.2), so these
    // stores must still take the barrier slow path and none may be lost.
    $vm.gc();
    $vm.gc();
    let fenceSum = 0;
    for (let i = 0; i < 20000; ++i) {
        const row = rows[i & (N - 1)][i & (ROWS - 1)];
        row.ref = { v: i };
        fenceSum += row.ref.v & 1;
    }
    shouldBe(fenceSum, 10000);
    $vm.gc();
    for (let t = 0; t < N; ++t) {
        for (let r = 0; r < ROWS; ++r)
            shouldBeTrue(rows[t][r].ref !== null, "post-fence-storm leaf survived");
    }
} else if (typeof $vm !== "undefined" && typeof $vm.sharedHeapTest === "function") {
    // No Thread global (flag combination unavailable): still exercise the
    // shared-heap storm surface single-sided so the file is not a silent
    // no-op in reduced configs.
    shouldBeTrue($vm.sharedHeapTest("allocationStorm", 4, 10000), "allocationStorm");
}
print("PASS");
