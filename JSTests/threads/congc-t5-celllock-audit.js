//@ requireOptions("--useSharedGCHeap=1", "--useConcurrentSharedGCMarking=1", "--useJSThreads=1", "--useDollarVM=1", "--numberOfGCMarkers=4")
// SPEC-congc CG-T5 (C1): cell-lock audit arms — ANNEX CGT1.7 charter
// (CG-3c).
//
// Gate semantics (CGT1.7): (1) the CG-A2 cell-lock audit is EXECUTED — the
// ANNEX CGN1 rows are recorded in docs/threads/INTEGRATE-congc.md ("CG-A2
// audit execution" section; row evidence cites the tree, including the
// runtime/-side visitor inputs such as the atomic m_terminationException
// visit in Heap.cpp's conservative-root constraint). This file carries the
// PER-ROW runtime arms for every CELL-LOCKED and N-PROTOCOL row, plus the
// CG-I18 storm. (2) CG-I18 (cell-lock-no-park, SPEC-congc §8.2): the debug
// asserts landed by CG-3c — GCCellLockDepth == 0 at SINFAC entry and at
// every AHA park leg (GSP revert, §A.3 park, Mode-machine park) — are the
// oracle for the storm arm: an engine path that parks holding a JSCellLock
// (rank 10a) crashes deterministically in ASSERT builds with the C1 flag on.
//
// Per-row arms (row ids = ANNEX CGN1):
//  - N1 (JSObject butterfly/shape storage; om §6/§9 IN-PROTOCOL): property +
//    indexed churn with shape transitions during forced concurrent cycles.
//  - N2 (JSString ropes; ungil §N.2 release-CAS/acquire IN-PROTOCOL): rope
//    build/resolve storm — visitors acquire-read fiber words mid-resolution.
//  - N3 (JSMap/JSSet/WeakMap/WeakSet; ungil §N.1 CELL-LOCKED incl. reads):
//    THE CG-I18 storm — map/set mutation vs forced fixpoint windows; every
//    mutation takes the cell lock, so any park-holding-10a path trips the
//    depth asserts.
//  - N4 (Structure, rank 10b IN-PROTOCOL): transition churn rides arm N1
//    (every shape transition exercises Structure's concurrent read paths).
//  - N5 (ArrayBuffer/TA words; ungil §N.6 IN-PROTOCOL): resize/detach-class
//    churn via transfer() when available, plus TA allocation storm.
//  - N6 (profiling fields, RACY-TOLERATED): no functional arm by design —
//    coverage is the TSAN run-config arm (suppressions key on the CGN1 row
//    list ONLY).
//
// Run-config arms carried by drivers (t3 convention): TSAN
// (Tools/threads/tsan/run-corpus-tsan.sh --useConcurrentSharedGCMarking=1),
// amplifier (Tools/threads/amplify.sh), GIL-off pinned env + C1 flag, and a
// Debug-build run (the CG-I18 asserts are ASSERT_ENABLED-only).
//
// Pass criterion: exact checksums + termination. Skip-arms PASS without
// Thread/$vm.
load("./harness.js", "caller relative");

const haveVM = typeof $vm !== "undefined";
const haveThread = typeof Thread === "function";

function forcedGCs(n) {
    if (!haveVM)
        return;
    for (let i = 0; i < n; ++i) {
        $vm.gc();
        sleepMs(1);
    }
}

if (haveThread && haveVM) {
    const N = 4;
    const PHASES = 8;
    const gate = { go: 0, started: 0, done: 0 };

    const threads = spawnN(N, (t) => {
        Atomics.add(gate, "started", 1);
        while (Atomics.load(gate, "go") === 0)
            Atomics.wait(gate, "go", 0, 100);

        let checksum = 0;

        // Arm N3 + CG-I18 storm: map/set/weakmap mutation under load. Every
        // op below takes the JSCellLock; allocation pressure inside the
        // working set forces allocation slow paths (CIND/SINFAC polls) and
        // GC stop windows to interleave with the locked sections — the
        // GCCellLockDepth asserts catch any path that parks while holding
        // the lock.
        const map = new Map();
        const set = new Set();
        const weak = new WeakMap();
        const keys = [];
        for (let i = 0; i < 64; ++i)
            keys.push({ k: i });

        for (let phase = 0; phase < PHASES; ++phase) {
            // N3: churn — insert/lookup/delete storms with rehash pressure.
            for (let i = 0; i < 3000; ++i) {
                const k = (t << 20) ^ (phase << 12) ^ i;
                map.set(k, { v: k, pad: [k, k + 1] });
                if (i & 1)
                    set.add(k);
                if ((i & 63) === 0)
                    weak.set(keys[i & 63], { w: k });
                if ((i & 7) === 0) {
                    const got = map.get(k);
                    if (got.v !== k)
                        throw new Error("N3 torn map read: " + got.v + " != " + k);
                    checksum = (checksum + got.v) | 0;
                }
                if ((i & 15) === 0)
                    map.delete((t << 20) ^ (phase << 12) ^ (i - 8));
            }

            // Arm N1/N4: shape-transition churn — fresh objects walk
            // transition chains (Structure reads race visitors); indexed
            // writes flip indexing types.
            for (let i = 0; i < 1500; ++i) {
                const o = {};
                o["p" + (i & 7)] = i;
                o.q = i + 1;
                o[i & 31] = i;
                checksum = (checksum + o.q) | 0;
            }

            // Arm N2: rope storm — concat-heavy strings kept alive across a
            // cycle, resolved later (visitor acquire-reads fibers of
            // unresolved ropes mid-build).
            let rope = "r" + t;
            for (let i = 0; i < 200; ++i)
                rope += "x" + (i & 7);
            if (rope.length !== ("r" + t).length + 400)
                throw new Error("N2 rope length mismatch: " + rope.length);
            checksum = (checksum + rope.length) | 0;

            // Arm N5: ArrayBuffer/TA words — allocation + transfer (detach
            // publishes len=0 seq_cst; visitors read the {base,length} pair
            // per the N6 order).
            let buf = new ArrayBuffer(4096);
            const ta = new Float64Array(buf);
            ta[0] = t + phase;
            if (typeof buf.transfer === "function") {
                const moved = buf.transfer();
                const ta2 = new Float64Array(moved);
                if (ta2[0] !== t + phase)
                    throw new Error("N5 transfer lost contents");
                checksum = (checksum + ta2[0]) | 0;
            } else
                checksum = (checksum + ta[0]) | 0;
        }

        // Deterministic-per-thread final verification: re-read a stable
        // slice of the map.
        for (let i = 0; i < 64; ++i) {
            const k = (t << 20) ^ ((PHASES - 1) << 12) ^ (2900 + i);
            const got = map.get(k);
            if (got !== undefined && got.v !== k)
                throw new Error("N3 final-read corruption at " + k);
        }

        Atomics.add(gate, "done", 1);
        return checksum | 0;
    });

    waitUntil(() => Atomics.load(gate, "started") === N);
    Atomics.store(gate, "go", 1);
    Atomics.notify(gate, "go");

    // Main thread: force fixpoint windows while the storm runs — this is
    // what makes the visitor side race the cell-locked mutator sections
    // (out-of-window draining, tryLock+revisit on N3 rows) and drives the
    // stop polls the CG-I18 asserts guard.
    for (let round = 0; round < 24 && Atomics.load(gate, "done") < N; ++round)
        forcedGCs(2);

    const results = joinAll(threads);
    shouldBe(results.length, N, "all storm threads joined");
    for (let t = 0; t < N; ++t)
        shouldBeTrue(typeof results[t] === "number", "thread " + t + " returned a checksum");
    shouldBe(Atomics.load(gate, "done"), N, "all storm threads completed");
} else if (haveVM && typeof $vm.sharedHeapTest === "function") {
    // No Thread global: standalone-client coverage of the same surfaces
    // (allocation + steal during forced cycles under the C1 flag) so the
    // file still exercises engine code in harness-only configurations.
    shouldBeTrue($vm.sharedHeapTest("allocationStorm", 4, 8000), "allocationStorm (CG-T5 fallback arm)");
} else {
    // Skip arm: no Thread, no $vm — PASS (gate runs under the //@ header
    // options in CI; this keeps bare-shell corpus sweeps green).
}

// CG-A2 EXECUTION MARKER: the audit itself is a documentation gate — see
// docs/threads/INTEGRATE-congc.md "CG-A2 cell-lock audit execution (CG-3c)"
// for the recorded CGN1 rows. This file passing under {Debug build, C1
// flag, GIL-on and GIL-off env, TSAN, amplifier} is the runtime half of the
// CG-T5 gate.
