//@ skip
// SKIPPED 2026-06-07: the heap-cap flags this test depends on are inert in
// this tree. The hard cap in CompleteSubspace.cpp (lines ~218-221 and
// ~284-287) compares heap capacity against WTF::ramSize() DIRECTLY, not
// against Heap::m_ramSize (which is the only thing --forceRAMSize overrides,
// Heap.cpp:372). So --maxHeapSizeAsRAMSizeMultiple=1 caps the heap at the
// HOST's physical RAM, and on any normally-sized machine the cap never
// fires: smoke-verified by retaining a ~1.5GB live hoard under
// --forceRAMSize=67108864 --maxHeapSizeAsRAMSizeMultiple=1 with zero OOM.
// The test's own vacuity guard (outcome 2 below) correctly catches this and
// fails, so it cannot join the corpus until either (a) CompleteSubspace
// honors the forced RAM size, or (b) a real per-VM heap-cap option exists.
// Re-enable by restoring this header line:
//   requireOptions("--useJSThreads=1", "--forceRAMSize=67108864", "--maxHeapSizeAsRAMSizeMultiple=1")
//
// semantics/oom-one-thread.js — one hog thread allocates toward OOM under a
// small heap cap (--forceRAMSize=64MB with --maxHeapSizeAsRAMSizeMultiple=1
// caps the GC heap at ~RAM size) while sibling threads keep doing small
// allocations. The heap is SHARED, so the cap is a shared resource.
//
// ACCEPTABLE OUTCOMES (documented; all asserted-as-allowed below):
//   1. The hog catches an out-of-memory error (Error/RangeError whose
//      message mentions memory) — the expected common case.
//   2. The hog reaches its iteration cap while one or more SMALL allocators
//      absorbed the OOM — allowed; the test then still verifies coherence.
//      (A "cap" finish with ZERO OOMs anywhere is NOT allowed: the hog's
//      live hoard is 4x the configured cap, so a no-OOM run means the
//      RAM-cap flags were inert and the test would pass vacuously.)
//   3. One or more SMALL allocators catch the OOM instead of (or as well
//      as) the hog — allowed: with a shared heap the cap can fire on
//      whichever thread allocates at the high-water moment. They catch,
//      record, and finish.
// HARD REQUIREMENTS (failures):
//   - no crash/abort of the VM, no hang (everything bounded and joined);
//   - any thrown error IS an out-of-memory-shaped error, not a corrupt
//     value, and is catchable by the thread that allocated;
//   - threads that did not OOM produce exactly-correct data (their small
//     allocations are never silently dropped or torn);
//   - after the hog releases its hoard, EVERY thread (and main) can
//     allocate again — the OOM state is not sticky.
load("../harness.js", "caller relative");

const SMALL_WORKERS = 3;
const SMALL_ROUNDS = 400;
// HOG_CAP bounds the WORST-CASE live memory when the RAM-cap flags do not
// bite (documented outcome 2): 1024 chunks x ~256KB = ~256MB, still 4x the
// 64MB configured heap so outcome 1 (OOM) is unaffected, but the no-cap
// fallback can no longer balloon to multi-GB RSS on a shared machine.
const HOG_CAP = 1024;          // hard bound on hog iterations
const HOG_CHUNK = 1 << 15;     // 32k doubles per chunk ~ 256KB payload

function isOOMError(e) {
    if (!(e instanceof Error || (e && typeof e.name === "string")))
        return false;
    return /memory/i.test(String(e)) || e.name === "RangeError";
}

const gate = { ready: 0, go: 0, hogDone: 0 };
const board = { hogOOM: 0, smallOOM: 0 }; // shared tallies (Atomics only)

const hog = new Thread(() => {
    Atomics.add(gate, "ready", 1);
    while (Atomics.load(gate, "go") === 0)
        Atomics.wait(gate, "go", 0, 2);
    const hoard = [];
    let outcome = "cap";
    try {
        for (let i = 0; i < HOG_CAP; ++i) {
            const a = new Array(HOG_CHUNK);
            for (let j = 0; j < HOG_CHUNK; j += 64)
                a[j] = i + j * 0.5; // touch pages, defeat lazy/CoW tricks
            hoard.push(a);
        }
    } catch (e) {
        if (!isOOMError(e))
            throw new Error("hog: allocation threw a non-OOM error: " + e);
        outcome = "oom";
        Atomics.add(board, "hogOOM", 1);
    }
    // Release and prove recovery on this thread.
    hoard.length = 0;
    let recovered;
    try {
        recovered = new Array(1024).fill(1).reduce((s, v) => s + v, 0);
    } catch (e) {
        // One failed retry is tolerated (GC may not have caught up); a
        // second failure after an explicit yield is not. (go is 1 here, so
        // this wait really parks ~50ms and drops the GIL.)
        Atomics.wait(gate, "go", 1, 50);
        recovered = new Array(1024).fill(1).reduce((s, v) => s + v, 0);
    }
    Atomics.store(gate, "hogDone", 1);
    return outcome + ":" + recovered;
});

const smalls = spawnN(SMALL_WORKERS, t => {
    Atomics.add(gate, "ready", 1);
    while (Atomics.load(gate, "go") === 0)
        Atomics.wait(gate, "go", 0, 2);
    let sum = 0;
    let myOOMs = 0;
    for (let r = 0; r < SMALL_ROUNDS; ++r) {
        try {
            const o = { idx: r, arr: new Array(64).fill(r), s: "small_" + t + "_" + r };
            // Exactness: anything we successfully allocated must be intact.
            if (o.arr[63] !== r || o.idx !== r || o.s.length < 8)
                throw new Error("small worker " + t + ": torn small allocation at round " + r);
            sum += o.arr[0];
        } catch (e) {
            if (e instanceof Error && /torn small/.test(e.message))
                throw e; // real failure
            if (!isOOMError(e))
                throw new Error("small worker " + t + ": non-OOM error: " + e);
            ++myOOMs; // outcome 3: acceptable, keep going
            Atomics.wait(gate, "hogDone", 0, 5); // back off until the hog releases
            --r; // retry the round after backoff (bounded: OOM clears once hog releases)
            if (myOOMs > 2000) // ~10s of 5ms backoffs: well inside the timeout
                throw new Error("small worker " + t + ": OOM never cleared");
        }
        if ((r & 63) === 63)
            Atomics.wait(gate, "hogDone", 0, 1); // GIL-dropping yield
    }
    Atomics.add(board, "smallOOM", myOOMs > 0 ? 1 : 0);
    return sum;
});

waitUntil(() => Atomics.load(gate, "ready") === 1 + SMALL_WORKERS);
Atomics.store(gate, "go", 1);
Atomics.notify(gate, "go", Infinity);

const hogReport = hog.join();
const smallSums = joinAll(smalls);

// Hog outcome: oom or cap, and its post-release allocation worked.
const [hogOutcome, hogRecovered] = hogReport.split(":");
shouldBeTrue(hogOutcome === "oom" || hogOutcome === "cap", "hog outcome legal: " + hogOutcome);
shouldBe(Number(hogRecovered), 1024, "hog allocated again after releasing the hoard");

// Small workers: every completed round contributed exactly; sum is the
// closed form regardless of how many OOM-retries happened along the way.
const expectedSum = (SMALL_ROUNDS - 1) * SMALL_ROUNDS / 2;
for (let t = 0; t < SMALL_WORKERS; ++t)
    shouldBe(smallSums[t], expectedSum, "small worker " + t + " exact work despite pressure");

// Vacuity guard: the hog holds a LIVE ~256MB hoard (HOG_CAP x ~256KB)
// against a 64MB configured cap, so on a build where the RAM-cap flags are
// honored the cap MUST fire on some thread. If NO thread ever observed an
// OOM-shaped error, the flags were ignored (e.g. the shared-VM/per-lite
// heap-config path dropped or mis-scoped them) and none of this test's OOM
// requirements were actually exercised — that is a failure, not a pass.
shouldBeTrue(Atomics.load(board, "hogOOM") + Atomics.load(board, "smallOOM") >= 1,
    "heap cap fired on at least one thread (hog=" + hogOutcome
    + "); a 4x-oversubscribed live hoard with zero OOMs means the RAM-cap flags were inert");

// Post-storm: main thread allocates comfortably.
const big = new Array(1 << 16).fill(2);
shouldBe(big[big.length - 1], 2);
// No print: the hog/small OOM split is interleaving-dependent, and
// Tools/threads/amplify.sh flags ANY stdout divergence across reruns. The
// asserts above encode every requirement.

// WOULD-FAIL-IF: shared-heap OOM handling under threads is not safe — the
// cap firing while N threads allocate concurrently aborts the process or
// corrupts the heap instead of throwing a catchable out-of-memory error on
// the allocating thread (the VM crash fails the run outright; a non-OOM-
// shaped error fails isOOMError), an OOM unwinding on one thread poisons a
// sibling's in-progress small allocation (the torn-small-allocation check:
// successfully-returned objects must be fully formed), the OOM state is
// sticky per-VM so threads can never allocate again after the hog releases
// its hoard (hog recovery / small-worker retry cap / main's post-storm
// allocation), per-lite heap accounting double-frees the hoard on release,
// OR the per-lite/shared-VM heap-config path drops or mis-scopes the
// --forceRAMSize/--maxHeapSizeAsRAMSizeMultiple options so the cap never
// fires (the vacuity guard: a 4x-oversubscribed live hoard finishing with
// zero OOM sightings fails outright instead of passing without exercising
// any OOM path). The exact closed-form sums prove no small allocation was
// silently dropped under pressure.
