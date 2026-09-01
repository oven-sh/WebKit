//@ requireOptions("--useJSThreads=1")
// Annex §15.T12 suite: N2 inline-add races (I9/I21).
//
// Inline (in-cell) property ADDS are structure-only transitions: the
// butterfly is untouched, so the §2.1 N2 path is either E4 (owner lock-free)
// or the cell-locked header CAS that release-stores the inline value BEFORE
// publishing the new structure (no holes, I9). Targets:
//   - I21: no lost inline adds when N threads add disjoint inline-capacity
//     names to one shared object;
//   - I9: a reader that observes a property as present reads its writer's
//     value, never undefined-after-publication and never another slot's
//     value.
//
// Deterministic under the GIL stub; re-run unmodified at M4/CS2.
load("../harness.js", "caller relative");

const THREADS = 3;
const ROUNDS = 16;

// Each thread adds exactly one inline-range property; the literal leaves
// inline capacity to spare (object literals get inlineCapacity >= their
// property count, and fresh {} objects have >= 6 inline slots).
for (let round = 0; round < ROUNDS; ++round) {
    const o = { base: round };
    const workers = spawnN(THREADS, (t) => {
        o["inl" + t] = { writer: t }; // one N2 add per thread (cell value: identity-checkable)
        return o["inl" + t].writer; // immediate read-back through the new structure
    });
    const results = joinAll(workers);

    for (let t = 0; t < THREADS; ++t) {
        shouldBe(results[t], t, "round " + round + ": writer " + t + " read-back");
        shouldBeTrue("inl" + t in o, "round " + round + ": lost inline add inl" + t);
        shouldBe(o["inl" + t].writer, t, "round " + round + ": inline slot aliased");
    }
    shouldBe(o.base, round, "round " + round + ": pre-existing inline slot clobbered");
    shouldBe(Object.keys(o).length, 1 + THREADS);
}

// I9 reader side: while one foreign thread does the N2 add, a racing reader
// must never see (name in o) true with a value other than the release-stored
// one. Pre-publication it may see absent/undefined; never a torn or foreign
// value.
const SAMPLES = 400;
for (let round = 0; round < ROUNDS; ++round) {
    const o = { pad: 0 };
    const sentinel = { mark: "n2-value-" + round };

    const reader = new Thread(() => {
        let sawPresent = false;
        for (let s = 0; s < SAMPLES; ++s) {
            // Single read, then classify (no in-then-read TOCTOU: the add may
            // land between two observations under real threads).
            const v = o.fresh;
            if (v !== undefined && v !== sentinel)
                throw new Error("round " + round + ": torn/foreign N2 value (I9): " + describe(v));
            if (v === sentinel)
                sawPresent = true;
        }
        return sawPresent;
    });
    const adder = new Thread(() => { o.fresh = sentinel; });

    reader.join();
    adder.join();
    shouldBe(o.fresh, sentinel, "round " + round + ": add lost (I21)");
    shouldBe(o.pad, 0);
}
