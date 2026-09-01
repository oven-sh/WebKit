//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// havebadtime-vs-indexed-fastpath.js — gc-stress suite: thread B (here: the
// main thread, which can hold the GIL through the transition) triggers
// haveABadTime() on the SHARED realm by defining an indexed accessor on
// Array.prototype, while worker threads A1..A3 hammer indexed stores/reads
// on plain dense arrays through hot fast paths.
//
// haveABadTime flips every array structure in the realm to SlowPutArrayStorage
// and invalidates the realm's bad-time watchpoints; every indexed fast path
// compiled or cached before the flip must stop being used (or must remain
// semantically correct). JSC fires haveABadTime for an indexed accessor at
// ANY index on Array.prototype, so the trap lives at TRAP_INDEX — far outside
// every index any worker ever touches. That matters for spec correctness:
// a worker's first store to a hole consults the prototype chain (OrdinarySet),
// so an accessor at an index workers write to would LEGALLY swallow their
// stores. With the trap out of range, post-flip hole stores at 0..LEN-1 must
// still create own elements, and every read must keep returning exactly what
// that worker wrote, before, during, and after the flip.
//
// Post-flip overlap is GUARANTEED, not wall-clock luck: workers block at
// round POST_FLIP_WAIT_ROUND until the main thread publishes the "flipped"
// flag (set immediately after the defineProperty), so rounds
// POST_FLIP_WAIT_ROUND..TOTAL_ROUNDS-1 provably execute against the bad-time
// realm; each worker returns its post-flip round count and the join asserts
// the exact expected tail.
//
// Post-state coherence asserted:
//   - every worker's private array reads back its full expected contents,
//   - the shared array's disjoint per-worker ranges are read back EVERY
//     round (so the replace-path store site has coverage inside the actual
//     conversion window, not just steady-state) and hold each worker's
//     final values at join (no lost or misdirected indexed stores),
//   - reads/stores at TRAP_INDEX observe the prototype accessor (proves the
//     bad-time transition actually happened — the test cannot pass vacuously),
//   - the realm reports bad time via $vm.isHavingABadTime when available.
//
// Runtime: bounded — workers run a fixed number of rounds; rendezvous waits
// capped by waitUntil's 30s default.

load("../harness.js", "caller relative");

const WORKERS = 3;
const ARRAY_LEN = 512;
const WARMUP_ROUNDS = 40;
const POST_FLIP_WAIT_ROUND = 80;
const TOTAL_ROUNDS = 160;
const SENTINEL = "from-bad-time-accessor";
// The accessor index: far outside [0, ARRAY_LEN) and [0, WORKERS*ARRAY_LEN)
// so no worker store or read ever consults it via the prototype chain.
const TRAP_INDEX = 1 << 20;

const sharedArray = new Array(WORKERS * ARRAY_LEN).fill(0);
const progress = { warmed: 0, flipped: 0 };

function verifyPrivate(a, t, round, where) {
    for (let i = 0; i < ARRAY_LEN; ++i) {
        const expected = t * 1000000 + round * 1000 + (i & 7);
        if (a[i] !== expected)
            throw new Error("worker " + t + " " + where + " round " + round
                + ": a[" + i + "] = " + describe(a[i]) + ", expected " + expected);
    }
}

const workers = spawnN(WORKERS, function (t) {
    const base = t * ARRAY_LEN;
    let lastPrivate = null;
    let postFlipRounds = 0;
    for (let round = 0; round < TOTAL_ROUNDS; ++round) {
        if (round === POST_FLIP_WAIT_ROUND) {
            // Guarantee the tail of the run executes against the flipped
            // realm: block until the main thread has installed the accessor.
            waitUntil(() => Atomics.load(progress, "flipped") === 1, 30000);
        }
        if (Atomics.load(progress, "flipped") === 1)
            ++postFlipRounds;

        // Fresh array each round: contiguous indexed stores 0..LEN-1
        // (the int32/contiguous fast path pre-flip; post-flip these are hole
        // stores into SlowPutArrayStorage, but with no accessor at 0..LEN-1
        // they must still create own elements), then full read-back.
        const a = new Array(ARRAY_LEN);
        for (let i = 0; i < ARRAY_LEN; ++i)
            a[i] = t * 1000000 + round * 1000 + (i & 7);
        verifyPrivate(a, t, round, "read-back");

        // Shared array: each worker owns a disjoint range [base, base+LEN).
        // (Own elements exist from the .fill(0) — these are never hole
        // stores, pre- or post-flip.)
        for (let i = 0; i < ARRAY_LEN; ++i)
            sharedArray[base + i] = t * 1000000 + round * 1000 + (i & 7);

        // Immediate read-back of this worker's disjoint sharedArray range
        // (nobody else writes it). This gives the long-lived dense-array
        // REPLACE-path store site per-round coverage, including the rounds
        // racing the haveABadTime conversion window itself — without this, a
        // store lost or misdirected during the flip would be overwritten by
        // later rounds before the final post-join verify could see it.
        for (let i = 0; i < ARRAY_LEN; ++i) {
            const expected = t * 1000000 + round * 1000 + (i & 7);
            if (sharedArray[base + i] !== expected)
                throw new Error("worker " + t + " sharedArray read-back round " + round
                    + ": [" + (base + i) + "] = " + describe(sharedArray[base + i])
                    + ", expected " + expected);
        }

        lastPrivate = a;
        if (round === WARMUP_ROUNDS) {
            Atomics.add(progress, "warmed", 1);
            Atomics.notify(progress, "warmed");
        }
        if (!(round % 8))
            sleepMs(0); // cooperative-GIL yield: let the flip land mid-run
    }
    // Re-verify the final private array AFTER all rounds (post-flip reads of
    // a post-flip-written array).
    verifyPrivate(lastPrivate, t, TOTAL_ROUNDS - 1, "final");
    return postFlipRounds;
});

// Wait until every worker is hot (fast paths compiled/cached), then flip the
// shared realm into bad time mid-storm.
waitUntil(() => Atomics.load(progress, "warmed") === WORKERS, 30000);

Object.defineProperty(Array.prototype, TRAP_INDEX, {
    get() { return SENTINEL; },
    set(v) { /* swallow */ },
    configurable: true,
});

// Publish the flip so workers' rounds POST_FLIP_WAIT_ROUND.. are provably
// post-flip.
Atomics.store(progress, "flipped", 1);
Atomics.notify(progress, "flipped");

const results = joinAll(workers);
for (let t = 0; t < WORKERS; ++t) {
    shouldBeTrue(results[t] >= TOTAL_ROUNDS - POST_FLIP_WAIT_ROUND,
        "worker " + t + " must have executed at least "
        + (TOTAL_ROUNDS - POST_FLIP_WAIT_ROUND) + " post-flip rounds (got " + results[t] + ")");
}

// ---- post-state coherence ----

// 1. Shared array: every worker's final round survived the flip intact.
for (let t = 0; t < WORKERS; ++t) {
    const base = t * ARRAY_LEN;
    for (let i = 0; i < ARRAY_LEN; ++i) {
        const expected = t * 1000000 + (TOTAL_ROUNDS - 1) * 1000 + (i & 7);
        shouldBe(sharedArray[base + i], expected,
            "sharedArray[" + (base + i) + "] (worker " + t + ")");
    }
}

// 2. The accessor really is installed and reachable through the prototype
//    chain: a read at TRAP_INDEX on an array with no such own element routes
//    to the getter.
const holey = new Array(4);
holey[3] = "tail";
shouldBe(holey[TRAP_INDEX], SENTINEL, "read at TRAP_INDEX must observe the Array.prototype accessor");
shouldBe(holey[3], "tail");

// 3. Stores at TRAP_INDEX route to the (swallowing) setter: no own element is
//    created and length does not grow.
const holey2 = [];
holey2[TRAP_INDEX] = "should-be-swallowed";
shouldBe(holey2[TRAP_INDEX], SENTINEL, "store at TRAP_INDEX must hit the SlowPut setter");
shouldBe(holey2.length, 0, "swallowed store must not create an own element");

// 4. Realm-level confirmation when the introspection hook exists.
if (typeof $vm !== "undefined" && typeof $vm.isHavingABadTime === "function")
    shouldBeTrue($vm.isHavingABadTime(holey), "realm must report bad time");

// 5. Post-flip indexed stores/reads on the main thread still behave: own
//    elements at in-range indices are created and read back.
const dense = [1, 2, 3];
dense[0] = 42;
shouldBe(dense[0], 42, "in-range own element store must work post-flip");

print("havebadtime-vs-indexed-fastpath: PASS");

// WOULD-FAIL-IF: haveABadTime on the shared realm is not propagated safely
// to other threads' indexed fast paths — e.g. another thread keeps executing
// a cached contiguous store/load path after the realm flipped to
// SlowPutArrayStorage (lost store into a detached/converted butterfly, read
// of a stale pre-conversion spine), or the structure flip tears so a worker
// observes an array that is neither valid dense nor valid SlowPut state.
// The trap accessor lives at TRAP_INDEX, outside every index the workers
// touch, so spec semantics REQUIRE all worker stores (including post-flip
// hole stores at 0..LEN-1) to create/update own elements; any lost or
// misdirected store changes a cell in a worker's private array or its
// disjoint sharedArray range away from the closed-form expected value, and
// the full-contents verifies report the exact index and value. BOTH store
// sites have per-round (transition-window) coverage: the fresh private
// arrays (hole-store path) via verifyPrivate every round, and the
// long-lived sharedArray (existing-element replace path) via the immediate
// per-round read-back of each worker's disjoint range — so a regression
// confined to the flip window cannot be masked by later rounds overwriting
// the same cells before the final verify. The flipped-flag rendezvous guarantees every worker
// executes rounds POST_FLIP_WAIT_ROUND..TOTAL_ROUNDS-1 against the bad-time
// realm (asserted via the returned post-flip round count), so the overlap
// cannot be vacuous; the TRAP_INDEX checks (2)/(3) keep the test from
// passing when the flip never happened.
