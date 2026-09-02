//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=50")
//@ runDefault()
//@ runDefault("--useFTLJIT=0")
//@ threadsRequireGILOff
// An optimized a[i] on an ArrayStorage array calls an operation for an index
// past the vector, and that operation looks the index up in the sparse map.
// The thread that owns the array adds and deletes sparse entries, so the map
// rehashes, while this thread reads. (Once another thread has written an
// ArrayStorage array, the optimized read exits instead, so the owner must be
// the writer.) The lookup must hold the map's lock, as every other sparse map
// read does with threads on, or it walks a freed table. The reader runs until
// the writer is done, with a cap.

load("../harness.js", "caller relative");

// With the GIL on, the other thread runs only while this one blocks, so the
// race cannot happen, and the reader's loops stop early.
const gilOn = $vm.useThreadGIL();

const base = 1000000;
const count = 64;

function get(a, i) { return a[i]; }
noInline(get);

function makeSparse() {
    const a = [1, 2, 3];
    a[base] = 0;
    a[base + 1] = 1;
    return a;
}

// Tier the read up on an array another thread owns.
const warm = new Thread(makeSparse).join();
for (let n = 0; n < 2000; ++n) {
    const v = get(warm, base + (n & 3));
    if (v !== ((n & 3) < 2 ? (n & 3) : undefined))
        throw new Error("warm-up read " + v);
}

const rounds = 300;
const state = { array: null, done: false };
const other = new Thread(() => {
    const shared = makeSparse();
    Atomics.store(state, "array", shared);
    for (let r = 0; r < rounds; ++r) {
        for (let k = 2; k < count; ++k)
            shared[base + k] = k;
        for (let k = 2; k < count; ++k)
            delete shared[base + k];
    }
    Atomics.store(state, "done", true);
    return shared;
});

let shared = null;
for (let n = 0; !Atomics.load(state, "done") && n < (gilOn ? 100 : 200000); ++n) {
    if (!shared) {
        shared = Atomics.load(state, "array");
        continue;
    }
    const k = n % count;
    const v = get(shared, base + k);
    if (v !== undefined && v !== k)
        throw new Error("read " + v + " at " + (base + k));
}
shared = other.join();

shouldBe(get(shared, base), 0);
shouldBe(get(shared, base + 1), 1);
shouldBe(get(shared, base + 2), undefined);
