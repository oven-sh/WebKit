//@ requireOptions("--useStructureAllocationLock=1")
// SPEC-vmstate I8/I10 single-threaded arm of the flag matrix: with ONLY
// useStructureAllocationLock=1 (no threads, no GIL involvement), every
// Structure-allocating path must take and release the §5.2 locker exactly
// once, non-nested (the I8 RELEASE_ASSERT(!previous) in the locker ctor
// fail-stops on nesting — this file is the cheapest executable form of the
// M7 "never nest" audit), and behavior must be identical to flags-off.
//
// NOTE (flag matrix): the option ships via INTEGRATE-vmstate M_opts; this
// file is part of the integrated-tree matrix.
load("../resources/assert.js", "caller relative");

// Exercise nested-looking allocation shapes: a transition triggered while
// building the value of another property (object literals inside object
// literals), class hierarchies, and transitions from inside getters — the
// shapes most likely to catch an accidentally nested locker.
let digest = 0;
for (let r = 0; r < 100; ++r) {
    const o = {
        ["outer" + r]: {
            ["inner" + r]: { ["leaf" + r]: r },
        },
    };
    digest += o["outer" + r]["inner" + r]["leaf" + r];

    class Base { constructor() { this["b" + r] = 1; } }
    class Derived extends Base { constructor() { super(); this["d" + r] = 2; } }
    const inst = new Derived();
    digest += inst["b" + r] + inst["d" + r];

    const withGetter = {
        get g() {
            // Allocates a fresh shape while the property read is on-stack.
            const inner = {};
            inner["fromGetter" + r] = r;
            return inner["fromGetter" + r];
        },
    };
    digest += withGetter.g;

    // Dictionary + flatten round trip.
    const dict = {};
    for (let p = 0; p < 8; ++p)
        dict["sd" + r + "_" + p] = p;
    delete dict["sd" + r + "_0"];
    digest += Object.keys(dict).length;
}

let expected = 0;
for (let r = 0; r < 100; ++r)
    expected += r + 3 + r + 7;
shouldBe(digest, expected);
