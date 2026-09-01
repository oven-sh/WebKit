//@ requireOptions("--useJSThreads=1")
// SPEC-objectmodel invariants I21/I37: racing adds of the SAME property names.
//
// Part 1: N threads all add the same set of names to one shared object.
// The program race means the final value of each name is nondeterministic,
// but the engine invariant is exact: every name must exist, and its value
// must be a value that SOME thread wrote FOR THAT NAME (never another
// property's value, never a torn/garbage value, never missing).
//
// Part 2 (I37 same-shape add-storm): every thread builds many objects through
// the identical transition chain. No lost/duplicated transitions: each thread
// validates its own objects fully.
load("../resources/assert.js", "caller relative");

const THREADS = 4;
const ROUNDS = 8;

const NAMES = [];
for (let i = 0; i < 24; ++i)
    NAMES.push("shared" + i);

for (let round = 0; round < ROUNDS; ++round) {
    const o = {};
    const workers = spawnN(THREADS, (t) => {
        for (const name of NAMES)
            o[name] = "t" + t + ":" + name;
    });
    joinAll(workers);

    for (const name of NAMES) {
        shouldBeTrue(name in o, "round " + round + ": lost racing property " + name);
        const v = o[name];
        let valid = false;
        for (let t = 0; t < THREADS; ++t) {
            if (v === "t" + t + ":" + name)
                valid = true;
        }
        if (!valid)
            throw new Error("round " + round + ": property " + name
                + " holds a value no thread wrote for it: " + describe(v));
    }
    shouldBe(Object.keys(o).length, NAMES.length,
        "round " + round + ": duplicated or lost properties in key snapshot");
}

// Part 2: same-shape add storm. Each thread privately creates objects through
// the same transition chain (shared Structures under the hood, I37). Each
// thread checks its own objects, so this is race-free at the JS level: any
// failure is an engine-level lost/duplicated transition or torn table read.
const PER_THREAD = 64;
const results = spawnN(THREADS, (t) => {
    const mine = [];
    for (let k = 0; k < PER_THREAD; ++k) {
        const obj = {};
        for (let i = 0; i < 12; ++i)
            obj["s" + i] = t * 100000 + k * 100 + i;
        mine.push(obj);
    }
    // Validate within the thread.
    for (let k = 0; k < PER_THREAD; ++k) {
        for (let i = 0; i < 12; ++i) {
            if (mine[k]["s" + i] !== t * 100000 + k * 100 + i)
                throw new Error("thread " + t + ": add-storm object " + k
                    + " lost or corrupted s" + i);
        }
        if (Object.keys(mine[k]).length !== 12)
            throw new Error("thread " + t + ": add-storm object " + k
                + " has wrong key count");
    }
    return mine;
});
// Cross-thread re-validation from the main thread after join.
const all = joinAll(results);
for (let t = 0; t < THREADS; ++t) {
    shouldBe(all[t].length, PER_THREAD);
    for (let k = 0; k < PER_THREAD; ++k)
        for (let i = 0; i < 12; ++i)
            shouldBe(all[t][k]["s" + i], t * 100000 + k * 100 + i,
                "post-join validation of add-storm object");
}
