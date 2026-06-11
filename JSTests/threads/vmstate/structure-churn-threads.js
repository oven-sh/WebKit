//@ requireOptions("--useJSThreads=1")
// SPEC-vmstate W2 stress (I8/I9): Structure cell allocation + ID-creating
// transitions from many threads sharing one logical VM.
//
// Every property name below is unique per (thread, round, property), so
// every add is an ID-creating transition allocating a fresh Structure —
// under useStructureAllocationLock (implied by useJSThreads, R2) each one
// runs inside the §5.2 StructureAllocationLocker, whose I8 in-region counter
// RELEASE_ASSERTs single occupancy (any two threads simultaneously inside
// the region fail-stop, even under the GIL if a safepoint ever lands there).
// I9: a misdecoded StructureID (decode = base + offset VA arithmetic) shows
// up as wrong/missing property values or a crash — the read-back checks and
// shape snapshots below are the observable form.
//
// The unique names also churn the shared atom table (W1) from every thread.
load("../resources/assert.js", "caller relative");

const THREADS = 4;
const ROUNDS = 200;
const PROPS = 8;

const threads = spawnN(THREADS, t => {
    let digest = 0;
    for (let r = 0; r < ROUNDS; ++r) {
        const o = {};
        for (let p = 0; p < PROPS; ++p)
            o["t" + t + "r" + r + "p" + p] = t * 1000 + r + p;

        // Read back through the fresh structure chain.
        for (let p = 0; p < PROPS; ++p) {
            const v = o["t" + t + "r" + r + "p" + p];
            if (v !== t * 1000 + r + p)
                throw new Error("I9 violation: t" + t + " r" + r + " p" + p + " read " + v);
            digest += v;
        }

        // Shape snapshot: exactly PROPS own properties, in insertion order.
        const names = Object.keys(o);
        if (names.length !== PROPS)
            throw new Error("I9 violation: shape has " + names.length + " properties");
        if (names[0] !== "t" + t + "r" + r + "p0")
            throw new Error("I9 violation: property order broke");
    }
    return digest;
});

const results = joinAll(threads);
for (let t = 0; t < THREADS; ++t) {
    let expected = 0;
    for (let r = 0; r < ROUNDS; ++r) {
        for (let p = 0; p < PROPS; ++p)
            expected += t * 1000 + r + p;
    }
    shouldBe(results[t], expected, "thread " + t + " digest");
}
