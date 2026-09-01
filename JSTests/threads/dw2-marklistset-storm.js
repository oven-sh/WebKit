//@ requireOptions("--useJSThreads=1")
// dw2-marklistset-storm.js — deepwater LEDGER row 2 (DW-2) regression:
// markListSet UAF under --useSharedGCHeap=1.
//
// Mechanism under test: with a shared GC heap, every Thread's
// MarkedVector/MarkedArgumentBuffer spill path registers in the per-Heap
// mark-list set (MarkedVector.h fill/fillWith on the malloc'd-buffer path,
// MarkedVectorBase::addMarkSet via slowAppend/expandCapacity) and
// unregisters in ~MarkedVectorBase. Pre-fix that was ONE unsynchronized
// UncheckedKeyHashSet (Heap::m_markListSet) — the dive saw a hard SEGV
// (zero-page read on a freed table) in HashTable::removeIterator/add via
// MarkedVector::fill <- sortImpl <- arrayProtoFuncSort on spawned Thread T5
// at W=16. The landed fix routes shared-mode registrations through
// Heap::markListSetShard() (address-hashed shards, per-shard Lock); flag-off
// keeps the historical lock-free single set.
//
// Storm shape (W >= 16 per the ledger's reproduction sizing):
//  - sort lane: arrays well past MarkedVector's inline capacity, sorted with
//    a comparator so sortImpl takes the MarkedVector::fill spill path; the
//    vector registers on entry and unregisters at scope exit — every round
//    is an add/remove pair on the shared structure from W threads at once;
//  - apply lane: fn.apply(null, args) with arguments past
//    MarkedArgumentBuffer's inline capacity (8) so slowAppend/expandCapacity
//    registers via addMarkSet;
//  - GC lane: periodic allocation churn plus explicit gc() on a subset of
//    threads so the Msr marking constraint walks the shards (markLists) while
//    sibling threads register/unregister concurrently.
// Every round self-checks (sorted order + exact apply sum), so silent
// corruption — not just the crash — fails the test. Flag-on GIL'd and
// flag-off single-thread runs must also pass.

load("./harness.js", "caller relative");

const HAVE_THREADS = typeof Thread === "function";
const HAVE_GC = typeof gc === "function";
const W = HAVE_THREADS ? 16 : 1;
const ROUNDS = 120;
const SORT_LEN = 257; // >> inline capacity: forces the malloc'd-buffer fill path
const APPLY_ARGS = 64; // > MarkedArgumentBuffer inline capacity of 8

function sum64() {
    let s = 0;
    for (let i = 0; i < arguments.length; ++i)
        s += arguments[i];
    return s;
}

function worker(seed) {
    let check = 0;
    for (let r = 0; r < ROUNDS; ++r) {
        // Sort lane: MarkedVector::fill registration storm.
        const a = new Array(SORT_LEN);
        for (let i = 0; i < SORT_LEN; ++i)
            a[i] = ((i * 2654435761) ^ (seed * 40503) ^ (r * 9973)) | 0;
        a.sort((x, y) => x - y);
        for (let i = 1; i < SORT_LEN; ++i) {
            if (a[i - 1] > a[i])
                throw new Error("seed " + seed + " round " + r + ": sort order corrupted at " + i);
        }
        check = (check + a[0] + a[SORT_LEN - 1]) | 0;

        // Apply lane: MarkedArgumentBuffer slowAppend/addMarkSet storm.
        const args = new Array(APPLY_ARGS);
        let expected = 0;
        for (let i = 0; i < APPLY_ARGS; ++i) {
            args[i] = (i + r) | 0;
            expected += args[i];
        }
        const got = sum64.apply(null, args);
        if (got !== expected)
            throw new Error("seed " + seed + " round " + r + ": apply sum " + got + " != " + expected);
        check = (check + got) | 0;

        // GC lane: churn so collections (and shard walks) happen while
        // sibling threads are mid-registration; a few threads force them.
        if ((r & 7) === 0) {
            let junk = [];
            for (let i = 0; i < 500; ++i)
                junk.push({ x: i, y: "s" + (i & 15) });
            check = (check + junk.length) | 0;
        }
        if (HAVE_GC && (seed & 3) === 0 && (r & 31) === 16)
            gc();
    }
    return check;
}

// Deterministic per-seed expectation: the worker's checksum is a pure
// function of its seed, so run the same seed twice (storm + reference) and
// compare — catches cross-thread value corruption without a hand-computed
// constant.
if (HAVE_THREADS) {
    const threads = spawnN(W, worker);
    const mainCheck = worker(W); // main thread spills concurrently too
    const results = joinAll(threads);
    // Reference pass, single-threaded, after the storm has quiesced.
    for (let i = 0; i < W; ++i) {
        const expected = worker(i);
        if (results[i] !== expected)
            throw new Error("thread " + i + ": checksum " + results[i] + " != reference " + expected);
    }
    if (mainCheck !== worker(W))
        throw new Error("main thread checksum mismatch");
} else {
    // Flag-off: same lanes single-threaded; determinism check only.
    if (worker(0) !== worker(0))
        throw new Error("flag-off determinism failure");
}

print("PASS");
