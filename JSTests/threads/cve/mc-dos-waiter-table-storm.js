//@ requireOptions("--useJSThreads=1", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1")
// MC-DOS S4 (docs/threads/cve/map-MC-DOS.md): property-waiter table growth,
// drain correctness, and — the load-bearing assertion — RECLAMATION.
//
// PropertyWaiterTable (runtime/ThreadAtomics.cpp, SPEC-api 5.6) is a
// process-global singleton with no quota: per-(cell,uid) lists, unbounded
// deques, a Strong cellProtect per list and a Strong promise per async
// ticket. The containment argument (map S4) is that every drain path funnels
// through removeListIfEmpty / the D5 timer / sweepCellAtFinalization and
// clears BOTH Strongs. If any drain path skips that, the table becomes a
// monotonic GC-root set keyed by (live cell x every key ever waited on):
// silent unbounded growth — the MC-DOS failure shape — invisible to pure
// correctness tests but GC-OBSERVABLE: a waited-on-then-drained object whose
// last reference dies MUST become collectable.
//
// Arms:
//  1. Deep-deque storm on few keys: many finite-timeout async waiters +
//     partial notify; counts must be exact (notify returns flips, <= count).
//  2. Wide-key storm from spawned threads: distinct (cell,uid) pairs, all
//     drained by timeout; cross-thread settle goes through the registrant
//     inbox / dead-registrant main drain (SPEC-api 5.5).
//  3. Reclamation probe: K objects each waited on (finite timeout) then
//     fully drained and dropped; FinalizationRegistry must observe a
//     majority collected under repeated gc(). ZERO collected = leaked
//     cellProtect Strongs = the S4 hole; fail loudly.
//
// Deterministic in outcome (counts + eventual collection), storm-shaped in
// schedule; amplifier knobs are DEPTH/KEYS/K (rule 1: do not shrink to make
// a window "too small").
load("../harness.js", "caller relative");

asyncTestStart(3);

// ---- Arm 1: deep deque on few keys, exact notify accounting ----
{
    const DEPTH = 512;
    const o = { a: 0, b: 0 };
    const promises = [];
    for (let i = 0; i < DEPTH; ++i) {
        const r = Atomics.waitAsync(o, "a", 0, 60000);
        shouldBe(r.async, true, "arm1: waiter " + i + " must enqueue");
        promises.push(r.value);
    }
    // Notify half; the rest must NOT settle "ok".
    const flipped = Atomics.notify(o, "a", DEPTH / 2);
    shouldBe(flipped, DEPTH / 2, "arm1: notify flips exactly count");
    // Drain the remainder deterministically.
    const flippedRest = Atomics.notify(o, "a", Infinity);
    shouldBe(flippedRest, DEPTH / 2, "arm1: remainder count exact");
    shouldBe(Atomics.notify(o, "a", Infinity), 0, "arm1: deque fully drained");
    Promise.all(promises).then(values => {
        for (const v of values)
            shouldBe(v, "ok", "arm1: every notified waiter settles ok");
        asyncTestPassed();
    });
}

// ---- Arm 2: wide keys from spawned threads, drained by timeout ----
{
    const THREADS = 4;
    const KEYS = 64;
    const shared = {};
    for (let t = 0; t < THREADS; ++t)
        for (let k = 0; k < KEYS; ++k)
            shared["k" + t + "_" + k] = 0;

    const threads = [];
    for (let t = 0; t < THREADS; ++t) {
        threads.push(new Thread((obj, tid, keys) => {
            // Each registration creates a fresh (cell,uid) list entry; the
            // 80ms D5 timer is the sole drain. The registrant finishes
            // immediately: dead-registrant tickets must still settle
            // (SPEC-api 4.6.2 / 5.5 residue drain to main).
            const ps = [];
            for (let k = 0; k < keys; ++k) {
                const r = Atomics.waitAsync(obj, "k" + tid + "_" + k, 0, 80);
                if (r.async !== true)
                    throw new Error("arm2: expected async waiter");
                ps.push(r.value);
            }
            return Promise.all(ps);
        }, shared, t, KEYS));
    }
    const joins = threads.map(t => t.asyncJoin().then(p => p));
    Promise.all(joins).then(results => {
        for (const values of results) {
            shouldBe(values.length, KEYS, "arm2: all waiters settled");
            for (const v of values)
                shouldBe(v, "timed-out", "arm2: timer drains every waiter");
        }
        // After full drain, every list must be empty: notify finds nothing.
        let residue = 0;
        for (let t = 0; t < THREADS; ++t)
            for (let k = 0; k < KEYS; ++k)
                residue += Atomics.notify(shared, "k" + t + "_" + k, Infinity);
        shouldBe(residue, 0, "arm2: no waiter survives its drain");
        asyncTestPassed();
    });
}

// ---- Arm 3: reclamation probe (the MC-DOS assertion) ----
//
// WeakRef-based, polled across MICROTASK turns: WeakRef keepDuringJob
// re-protects a target only until the end of the job that called deref(),
// so a later turn's gc() can still collect it. We never park synchronously
// here (a sync park would stop the run loop and starve nothing we need —
// but it also proves nothing), and we never rely on FinalizationRegistry
// callback scheduling.
{
    const K = 128;
    const weakRefs = [];

    function makeWaitedOnGarbage() {
        const ps = [];
        for (let i = 0; i < K; ++i) {
            const cell = { v: 0 };
            weakRefs.push(new WeakRef(cell));
            // One notified waiter and one not-equal probe per cell:
            // exercises the notify drain against removeListIfEmpty and the
            // never-enqueued fast path.
            const r = Atomics.waitAsync(cell, "v", 0, 60000);
            if (r.async !== true)
                throw new Error("arm3: expected async waiter");
            ps.push(r.value);
            if (Atomics.notify(cell, "v", Infinity) !== 1)
                throw new Error("arm3: notify must flip the one waiter");
            const ne = Atomics.waitAsync(cell, "v", 999); // not-equal: never enqueues
            if (ne.async !== false || ne.value !== "not-equal")
                throw new Error("arm3: not-equal probe must not enqueue");
        }
        return Promise.all(ps);
        // All direct cell references die with this frame.
    }

    function countCleared() {
        let cleared = 0;
        for (const wr of weakRefs) {
            if (wr.deref() === undefined)
                cleared++;
        }
        return cleared;
    }

    makeWaitedOnGarbage().then(async values => {
        for (const v of values)
            shouldBe(v, "ok", "arm3: notified waiters settle ok");
        // Every list is drained: cellProtect must be cleared and the table
        // entry removed, so the cells are garbage now. Conservative stack
        // scanning may pin a few; a MAJORITY must be collectable. Zero
        // collected after sustained gc() = leaked Strong roots = the S4
        // hole. Each loop iteration is a separate job (await), so
        // keepDuringJob protection from the previous deref poll expires.
        let cleared = 0;
        for (let turn = 0; turn < 2000 && cleared < K / 2; ++turn) {
            gc();
            await Promise.resolve();
            cleared = countCleared();
        }
        shouldBeTrue(cleared >= K / 2,
            "arm3: waited-on-then-drained cells must be collectable (got "
            + cleared + "/" + K + "; 0 means PropertyWaiterTable leaked its Strong roots)");
        asyncTestPassed();
    });
}
