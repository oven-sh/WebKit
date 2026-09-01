//@ requireOptions("--useJSThreads=1")
// Annex §15.T12 suite: delete -> re-add quarantine reuse across GC
// (I18/I30/I34).
//
// §6: EVERY deleted out-of-line slot goes Quarantined, stamped with the
// owning heap's epoch; Reusable is fed SOLELY by promotion of stamps older
// than the heap's current epoch (bumped by the per-heap safepoint adapter in
// every collection). takeDeletedOffset draws ONLY from Reusable. So:
//   - BEFORE a GC, a re-add must NOT reuse the deleted slot (a stale reader
//     holding the old offset would alias the new property — the I18 kill);
//   - AFTER a GC postdating the deletion, reuse is legal;
//   - D1/I30: the delete release-stores jsUndefined() first, so a tardy
//     reader sees old-value-or-undefined, never EMPTY (crash) and never the
//     re-added neighbor's value.
// JS cannot observe slot numbers, so the test observes the CONSEQUENCE:
// racing stale readers across delete -> filler adds -> GC -> more adds ->
// same-name re-add can only ever see {old, undefined, new}.
load("../harness.js", "caller relative");

const ROUNDS = 6;
const PAD = 40;        // out-of-line storage
const PRE_GC_ADDS = 24;  // pre-epoch adds: must NOT land in the quarantined slot
const POST_GC_ADDS = 24; // post-epoch adds: MAY reuse it
const SAMPLES = 500;

for (let round = 0; round < ROUNDS; ++round) {
    const o = {};
    for (let i = 0; i < PAD; ++i)
        o["p" + i] = "old" + i;
    const gate = { phase: 0 };

    const reader = new Thread(() => {
        let samples = 0;
        while (Atomics.load(gate, "phase") < 3 || samples < SAMPLES) {
            ++samples;
            const v = o.p9; // the victim
            if (v !== "old9" && v !== undefined && v !== "new9")
                throw new Error("round " + round + ": quarantined slot aliased: o.p9 = "
                    + describe(v) + " (I18/I30)");
            const n = o.p10; // neighbor: must never waver
            if (n !== "old10")
                throw new Error("round " + round + ": neighbor corrupted: " + describe(n));
            if (samples > SAMPLES * 50)
                throw new Error("round " + round + ": reader starved");
            if (!(samples & 63))
                sleepMs(1);
        }
        return samples;
    });

    const mutator = new Thread(() => {
        // Phase 1: delete + pre-GC adds (quarantine must hold these OUT of
        // the victim's slot — no epoch bump yet).
        delete o.p9;
        for (let i = 0; i < PRE_GC_ADDS; ++i)
            o["pre" + i] = "preval" + i;
        Atomics.store(gate, "phase", 1);
        sleepMs(2);
        return true;
    });
    shouldBeTrue(mutator.join());

    // Phase 2 (main): epoch bump via full collections, then post-GC adds —
    // these MAY legally reuse the promoted slot.
    gc();
    gc();
    Atomics.store(gate, "phase", 2);
    const mutator2 = new Thread(() => {
        for (let i = 0; i < POST_GC_ADDS; ++i)
            o["post" + i] = "postval" + i;
        o.p9 = "new9"; // same-name re-add, possibly into the recycled slot
        Atomics.store(gate, "phase", 3);
        return true;
    });
    shouldBeTrue(mutator2.join());
    shouldBeTrue(reader.join() > 0);

    // Deterministic final state.
    shouldBe(o.p9, "new9", "round " + round + ": re-add lost");
    for (let i = 0; i < PAD; ++i) {
        if (i !== 9)
            shouldBe(o["p" + i], "old" + i, "round " + round + ": survivor p" + i);
    }
    for (let i = 0; i < PRE_GC_ADDS; ++i)
        shouldBe(o["pre" + i], "preval" + i, "round " + round + ": pre-GC add lost");
    for (let i = 0; i < POST_GC_ADDS; ++i)
        shouldBe(o["post" + i], "postval" + i, "round " + round + ": post-GC add lost");
    shouldBe(Object.keys(o).length, PAD + PRE_GC_ADDS + POST_GC_ADDS);
}

// Dictionary-mode flavor: a delete STORM drives the structure to dictionary
// (L3: storage access under the cell lock; table edits also m_lock), then
// re-adds across a GC. Same alias-freedom invariant.
for (let round = 0; round < ROUNDS; ++round) {
    const o = {};
    for (let i = 0; i < 64; ++i)
        o["d" + i] = i;

    const deleter = new Thread(() => {
        for (let i = 0; i < 64; i += 2)
            delete o["d" + i];
        return true;
    });
    const reader = new Thread(() => {
        for (let s = 0; s < SAMPLES; ++s) {
            const i = 2 * ((s * 13) % 32);
            const v = o["d" + i];
            if (v !== i && v !== undefined)
                throw new Error("round " + round + ": dictionary delete aliased d" + i + " = " + describe(v));
        }
        return true;
    });
    shouldBeTrue(deleter.join());
    shouldBeTrue(reader.join());

    gc();
    for (let i = 0; i < 64; i += 2)
        o["d" + i] = i + 1000; // re-adds after the epoch bump
    for (let i = 0; i < 64; ++i)
        shouldBe(o["d" + i], (i % 2) ? i : i + 1000, "round " + round + ": dictionary re-add");
    shouldBe(Object.keys(o).length, 64);
}
