//@ requireOptions("--useJSThreads=1")
// SPEC-objectmodel invariant: no lost property adds (I21).
//
// N threads concurrently add DISJOINT sets of named properties to one shared
// object. Every add is a transition (out-of-line growth included), so under
// real concurrency this exercises the transition protocols (N3 install,
// flat->segmented conversion, segmented growth). The invariant is
// interleaving-independent: after all threads join, every property added by
// every thread must be present with exactly the value its writer stored.
//
// Deterministic under the GIL stub; reusable unchanged under real threads.
load("../resources/assert.js", "caller relative");

const THREADS = 4;
const PROPS = 32; // well past inline capacity: forces out-of-line transitions
const ROUNDS = 8;

for (let round = 0; round < ROUNDS; ++round) {
    const o = {};
    const workers = spawnN(THREADS, (t) => {
        for (let i = 0; i < PROPS; ++i)
            o["t" + t + "_p" + i] = t * 1000 + i;
    });
    joinAll(workers);

    for (let t = 0; t < THREADS; ++t) {
        for (let i = 0; i < PROPS; ++i) {
            const name = "t" + t + "_p" + i;
            shouldBeTrue(name in o, "round " + round + ": lost property " + name);
            shouldBe(o[name], t * 1000 + i, "round " + round + ": wrong value for " + name);
        }
    }
    shouldBe(Object.keys(o).length, THREADS * PROPS,
        "round " + round + ": property count mismatch");
}

// Same invariant when the object starts with inline properties allocated by
// the main thread (a constructor-shaped object), and foreign threads then
// extend it: the pre-existing inline slots must survive untouched.
for (let round = 0; round < ROUNDS; ++round) {
    function Point() {
        this.x = 1;
        this.y = 2;
        this.z = 3;
    }
    const p = new Point();
    const workers = spawnN(THREADS, (t) => {
        for (let i = 0; i < PROPS; ++i)
            p["ext_t" + t + "_" + i] = -(t * 1000 + i);
    });
    joinAll(workers);

    shouldBe(p.x, 1, "inline x clobbered");
    shouldBe(p.y, 2, "inline y clobbered");
    shouldBe(p.z, 3, "inline z clobbered");
    for (let t = 0; t < THREADS; ++t) {
        for (let i = 0; i < PROPS; ++i)
            shouldBe(p["ext_t" + t + "_" + i], -(t * 1000 + i),
                "round " + round + ": lost extension property");
    }
    shouldBe(Object.keys(p).length, 3 + THREADS * PROPS);
}
