//@ requireOptions("--useJSThreads=1")
// Annex §15.T12 suite: N3 racing first installs (I21).
//
// The FIRST out-of-line allocation installs the butterfly via the N3
// CAS-from-0 (word 0 -> encodeButterfly(b, tid, 0)); a lost race must
// re-dispatch on the winner's tag, never blind-store. Setup: the shared
// object's inline capacity is exactly filled by its literal, so EVERY
// thread's first add is an out-of-line transition racing the first install.
// Invariant (I21): no add is lost, no value tears, regardless of which
// thread's install wins.
load("../harness.js", "caller relative");

const THREADS = 4;
const ROUNDS = 16;

for (let round = 0; round < ROUNDS; ++round) {
    // 6 literal properties = a full default inline capacity; the next add
    // from ANY thread needs out-of-line storage (the racing first install).
    const o = { i0: 0, i1: 1, i2: 2, i3: 3, i4: 4, i5: 5 };

    const workers = spawnN(THREADS, (t) => {
        for (let k = 0; k < 8; ++k)
            o["ool_t" + t + "_" + k] = t * 100 + k;
    });
    joinAll(workers);

    for (let i = 0; i < 6; ++i)
        shouldBe(o["i" + i], i, "round " + round + ": inline i" + i + " clobbered by install race");
    for (let t = 0; t < THREADS; ++t) {
        for (let k = 0; k < 8; ++k)
            shouldBe(o["ool_t" + t + "_" + k], t * 100 + k,
                "round " + round + ": lost first-install add t" + t + "/" + k);
    }
    shouldBe(Object.keys(o).length, 6 + THREADS * 8, "round " + round);
}

// Same race for the INDEXED first install: a literal-free array value (fresh
// `new Array()` has no butterfly until the first element) raced by N threads
// each storing one disjoint dense index.
for (let round = 0; round < ROUNDS; ++round) {
    const a = new Array();
    const workers = spawnN(THREADS, (t) => {
        a[t] = "elem" + t; // each first store may be the installing CAS
    });
    joinAll(workers);

    shouldBe(a.length, THREADS, "round " + round + ": length after racing installs");
    for (let t = 0; t < THREADS; ++t)
        shouldBe(a[t], "elem" + t, "round " + round + ": lost racing element install");
}
