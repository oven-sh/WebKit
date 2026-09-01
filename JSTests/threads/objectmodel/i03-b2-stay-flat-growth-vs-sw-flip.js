//@ requireOptions("--useJSThreads=1")
// Annex §15.T12 suite: §4.3(b2) stay-flat growth vs SW flip.
//
// A locked OWNER transition that grows the flat out-of-line storage by
// COPYING (stay-flat growth) publishes via the 128-bit DCAS with SW=0
// expected. If a foreign SW-setter's lock-free DCAS lands in between, the
// taxonomy is (b2): the desired payload is a freshly copied flat butterfly,
// so merge-and-retry is FORBIDDEN — the foreign store would be lost in the
// stale copy (I21) — and the transition must RESTART (re-dispatching as a
// shared transition => segmented). Scenario: the owner (main) adds
// out-of-line properties (forcing repeated flat copying growth) while a
// foreign thread keeps overwriting EXISTING out-of-line properties (SW flip
// + plain stores). Invariants:
//   - every owner add lands (I21);
//   - the foreign writer's LAST value per slot is the final value — a
//     republished stale flat copy would resurrect the old value (the exact
//     b2 lost-write);
//   - values never tear or alias other slots.
load("../harness.js", "caller relative");

const ROUNDS = 8;
const OWNER_ADDS = 48;     // crosses several outOfLineCapacity doublings
const FOREIGN_PASSES = 24;

for (let round = 0; round < ROUNDS; ++round) {
    // Fill inline capacity, then give the object a few out-of-line slots for
    // the foreign writer to hammer.
    const o = { i0: 0, i1: 1, i2: 2, i3: 3, i4: 4, i5: 5 };
    o.w0 = "init0";
    o.w1 = "init1";
    o.w2 = "init2";
    const gate = { go: 0 };

    const writer = new Thread(() => {
        while (Atomics.load(gate, "go") === 0)
            sleepMs(1);
        for (let p = 0; p < FOREIGN_PASSES; ++p) {
            o.w0 = "f0_" + p; // existing-slot foreign writes: F1 SW flip, then plain stores
            o.w1 = "f1_" + p;
            o.w2 = "f2_" + p;
            if (!(p & 3))
                sleepMs(1); // interleave with the owner's copying growth
        }
        return true;
    });

    Atomics.store(gate, "go", 1);
    for (let k = 0; k < OWNER_ADDS; ++k) {
        o["own" + k] = k * 11; // owner adds: stay-flat copying growth until the flip, then §4.3/RESTART
        if (!(k & 7))
            sleepMs(1);
    }
    shouldBeTrue(writer.join());

    // I21: every owner add survived every interleaving.
    for (let k = 0; k < OWNER_ADDS; ++k)
        shouldBe(o["own" + k], k * 11, "round " + round + ": owner add lost (b2 stale copy)");
    // b2 witness: the foreign writer is the only post-gate writer of w0..w2,
    // so a final value older than its last pass means a stale flat copy got
    // republished over the foreign store.
    const last = FOREIGN_PASSES - 1;
    shouldBe(o.w0, "f0_" + last, "round " + round + ": foreign w0 lost to stay-flat copy (I21/b2)");
    shouldBe(o.w1, "f1_" + last, "round " + round + ": foreign w1 lost to stay-flat copy (I21/b2)");
    shouldBe(o.w2, "f2_" + last, "round " + round + ": foreign w2 lost to stay-flat copy (I21/b2)");
    for (let i = 0; i < 6; ++i)
        shouldBe(o["i" + i], i, "round " + round + ": inline slot disturbed");
    shouldBe(Object.keys(o).length, 6 + 3 + OWNER_ADDS);
}
