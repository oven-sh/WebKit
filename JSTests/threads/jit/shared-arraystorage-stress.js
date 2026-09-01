//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// shared-arraystorage-stress.js — SPEC-jit Task 13: I20 shared-ArrayStorage
// stress (GIL-interleaved pre-integration).
//
// I20 (mirrors OM I31): flag-on, no generated code may make an unlocked
// butterfly access reachable by an SW=1 ArrayStorage butterfly — every AS
// fast path is (a) E2-elided, (b) SW-tested with SW=1 routed to the locked
// R3 ops, or (c) statically non-AS. This test makes SW=1 AS butterflies a
// certainty and drives all the generated AS paths at them:
//   - foreign-thread element writes (SW flip, then SW=1 stores),
//   - owner-thread shift/unshift between waves (AS-COPY relayout under the
//     cell lock per the AS-rule; superseded storage never rewritten),
//   - sparse/hole reads and in-bounds reads from every thread,
//   - JIT-warmed get_by_val/put_by_val over AS shapes (LLInt->Baseline->DFG).
//
// Correctness oracle (GIL today, true concurrency at the integration gate):
// disjoint-slot writes must ALL land (no lost elements — a write into
// superseded AS storage after an AS-COPY relayout would lose it), and reads
// must never see values outside the written domain.

load("../harness.js", "caller relative");

if (typeof $vm === "undefined" || !$vm.ensureArrayStorage) {
    print("shared-arraystorage-stress: SKIP ($vm.ensureArrayStorage unavailable)");
} else {
    const THREADS = 4;
    const LEN = 128;
    const WAVES = 20;
    const WRITES_PER_WAVE = 64;

    const arr = new Array(LEN).fill(0);
    $vm.ensureArrayStorage(arr);
    arr[LEN + 50] = 1; // sparse tail => SlowPut-ish AS territory stays AS
    shouldBeTrue(($vm.indexingMode(arr) || "").indexOf("ArrayStorage") >= 0,
        "test precondition: arr must have ArrayStorage indexing");

    function readAt(a, i) { return a[i]; }
    noInline(readAt);
    function writeAt(a, i, v) { a[i] = v; }
    noInline(writeAt);

    // Warm the by-val paths on the AS shape from the main thread.
    for (let i = 0; i < 20000; ++i) {
        writeAt(arr, i % LEN, 0);
        if (readAt(arr, i % LEN) !== 0)
            throw new Error("warmup readback");
    }

    // Annex T2 (no preemptive-GIL reliance): the rendezvous counters are
    // Atomics - a plain `count++` is a two-step RMW that loses increments
    // under true parallelism and hangs the wave loop (the oracle below is
    // untouched; these are scaffolding only).
    const wave = { n: 0 };
    const done = { count: 0 };

    function encode(slot, w, v) { return slot * 1000000 + w * 1000 + v; }

    const writers = spawnN(THREADS, function (slot) {
        // Each thread owns a disjoint stripe: indices i with i % THREADS == slot.
        for (let w = 0; w < WAVES; ++w) {
            waitUntil(() => Atomics.load(wave, "n") >= w, 30000);
            for (let k = 0; k < WRITES_PER_WAVE; ++k) {
                const i = (slot + k * THREADS) % LEN;
                writeAt(arr, i, encode(slot, w, k & 7)); // foreign write: SW path
                const back = readAt(arr, i);
                // AB17e oracle restore: exact equality, not membership. The
                // wave barrier makes intervening relayout impossible — the
                // owner only shift/unshifts BETWEEN waves while every writer
                // is parked (it blocks in waitUntil(done >= (w+1)*THREADS)
                // during wave w), and stripes are disjoint
                // (LEN % THREADS == 0, so i % THREADS == slot). Nothing can
                // run between our write and this read on index i; anything
                // but the exact value is a LOST or torn write.
                if (back !== encode(slot, w, k & 7))
                    throw new Error("lost/torn write at " + i + " (wave " + w
                        + ", slot " + slot + ", k " + k + "): expected "
                        + encode(slot, w, k & 7) + " got " + String(back));
            }
            Atomics.add(done, "count", 1);
        }
        return slot;
    });

    // Owner thread: drive waves, relayout between them (AS-COPY), and read
    // hot in the meantime.
    for (let w = 0; w < WAVES; ++w) {
        Atomics.store(wave, "n", w);
        waitUntil(() => Atomics.load(done, "count") >= (w + 1) * THREADS, 30000);
        // No writer is mid-wave now: verify the stripe writes all landed.
        for (let slot = 0; slot < THREADS; ++slot) {
            const lastK = WRITES_PER_WAVE - 1;
            const i = (slot + lastK * THREADS) % LEN;
            const v = readAt(arr, i);
            // AB17e oracle restore: exact equality — a LOST write (slot
            // still holding warmup 0 or a wave w-1 epoch) must FAIL here,
            // not pass silently. Only stripe `slot` ever writes index i
            // (LEN % THREADS == 0); the k values colliding on i within the
            // wave are k ≡ lastK (mod LEN/THREADS), all from this same
            // stripe and all earlier than k = lastK, so the wave's final
            // write to i is encode(slot, w, lastK & 7); no writer is
            // mid-wave now and the owner has not yet relayouted.
            if (v !== encode(slot, w, lastK & 7))
                throw new Error("lost write at " + i + " (wave " + w
                    + ", slot " + slot + "): expected "
                    + encode(slot, w, lastK & 7) + " got " + String(v));
        }
        // Owner relayout between waves: shift + unshift (vector move /
        // indexBias change => AS-COPY under the cell lock flag-on).
        const head = arr.shift();
        arr.unshift(head);
        // Hole + boundary churn.
        delete arr[LEN - 2];
        arr[LEN - 2] = 0;
    }

    joinAll(writers);

    // Final integrity: length unchanged, sparse tail survived, all values in
    // domain.
    shouldBe(arr.length, LEN + 51);
    shouldBe(arr[LEN + 50], 1, "sparse tail element must survive relayouts");
    for (let i = 0; i < LEN; ++i) {
        const v = arr[i];
        if (v !== undefined && typeof v !== "number")
            throw new Error("out-of-domain value at " + i + ": " + String(v));
    }
    print("shared-arraystorage-stress: PASS");
}
