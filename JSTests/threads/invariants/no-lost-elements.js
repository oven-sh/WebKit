//@ requireOptions("--useJSThreads=1")
// SPEC-objectmodel invariant I21 for indexed properties (§4.4 array CAS):
// no lost element writes across racing element-storage resizes.
//
// Part 1: N threads write DISJOINT index ranges into one shared array.
// Every out-of-bounds store can trigger a vector resize racing the others;
// a broken resize (copy storage, swing pointer) silently drops concurrent
// writes. Invariant: after join, every element holds its writer's value
// and length is exact.
//
// Part 2: program-level-synchronized push (push is a compound op, so the
// PROGRAM uses a Lock); the engine must keep the element multiset exact.
load("../resources/assert.js", "caller relative");

const THREADS = 4;
const PER = 128;
const ROUNDS = 6;

for (let round = 0; round < ROUNDS; ++round) {
    const a = [];
    const writers = spawnN(THREADS, (t) => {
        // Interleave ranges so every thread keeps appending near the
        // current end, maximizing resize contention: thread t owns indices
        // i*THREADS + t.
        for (let i = 0; i < PER; ++i)
            a[i * THREADS + t] = t * 1000000 + i;
    });
    joinAll(writers);

    shouldBe(a.length, THREADS * PER, "round " + round + ": length wrong");
    for (let t = 0; t < THREADS; ++t) {
        for (let i = 0; i < PER; ++i) {
            const idx = i * THREADS + t;
            if (a[idx] !== t * 1000000 + i)
                throw new Error("round " + round + ": lost element write a["
                    + idx + "]: " + describe(a[idx]));
        }
    }
}

// Part 2: lock-protected pushes — exact multiset afterwards.
for (let round = 0; round < ROUNDS; ++round) {
    const a = [];
    const lock = new Lock();
    const pushers = spawnN(THREADS, (t) => {
        for (let i = 0; i < PER; ++i)
            lock.hold(() => { a.push(t * 1000000 + i); });
    });
    joinAll(pushers);

    shouldBe(a.length, THREADS * PER);
    const counts = new Map();
    for (const v of a) {
        if (typeof v !== "number")
            throw new Error("torn element: " + describe(v));
        counts.set(v, (counts.get(v) || 0) + 1);
    }
    for (let t = 0; t < THREADS; ++t) {
        for (let i = 0; i < PER; ++i) {
            const v = t * 1000000 + i;
            shouldBe(counts.get(v), 1, "round " + round + ": element " + v
                + " lost or duplicated");
        }
    }
}

// Part 3: named + indexed on the same object (the costly combined case in
// the spec): disjoint named adds racing disjoint element writes; both
// families must be complete and exact.
for (let round = 0; round < ROUNDS; ++round) {
    const o = [];
    const namers = spawnN(2, (t) => {
        for (let i = 0; i < 32; ++i)
            o["n" + t + "_" + i] = "N" + t + "_" + i;
    });
    const indexers = spawnN(2, (t) => {
        for (let i = 0; i < PER; ++i)
            o[i * 2 + t] = t * 1000 + i;
    });
    joinAll(namers);
    joinAll(indexers);

    shouldBe(o.length, 2 * PER);
    for (let t = 0; t < 2; ++t) {
        for (let i = 0; i < 32; ++i)
            shouldBe(o["n" + t + "_" + i], "N" + t + "_" + i,
                "round " + round + ": named property lost on array");
        for (let i = 0; i < PER; ++i)
            shouldBe(o[i * 2 + t], t * 1000 + i,
                "round " + round + ": element lost on named-property array");
    }
}
