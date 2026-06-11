//@ requireOptions("--useJSThreads=1")
// SPEC-objectmodel §6: quarantine + dictionary mode (I18, I19, L3).
//
// Heavy delete/add churn drives an object into (uncacheable) dictionary
// mode, where storage access is cell-locked under the real implementation.
// Invariants checked here:
//  - deleted-name reads only ever yield old-value-or-undefined while a
//    foreign thread churns the table (quarantine: no aliasing into slots
//    handed to new properties),
//  - survivors keep exact values through arbitrary churn,
//  - final table state is exact (no lost adds/deletes through reuse).
load("../resources/assert.js", "caller relative");

const ROUNDS = 4;
const PAD = 48;
const CHURN = 200;
const SAMPLES = 600;

for (let round = 0; round < ROUNDS; ++round) {
    const o = {};
    for (let i = 0; i < PAD; ++i)
        o["base" + i] = "stable" + i;
    o.watched = "first";

    // Churner: repeated delete/add cycles with fresh names — classic
    // dictionary-mode trigger and maximal pressure on deleted-offset reuse.
    const churner = new Thread(() => {
        for (let c = 0; c < CHURN; ++c) {
            o["tmp" + c] = c;
            delete o["tmp" + c];
        }
        delete o.watched;
        for (let c = 0; c < 24; ++c)
            o["fresh" + c] = "fresh" + c; // candidates for the watched slot
    });
    const reader = new Thread(() => {
        for (let s = 0; s < SAMPLES; ++s) {
            const w = o.watched;
            if (w !== "first" && w !== undefined)
                throw new Error("round " + round + ": deleted 'watched' aliased "
                    + describe(w));
            // Stable survivors must never waver.
            const idx = s % PAD;
            const v = o["base" + idx];
            if (v !== "stable" + idx)
                throw new Error("round " + round + ": survivor base" + idx
                    + " corrupted: " + describe(v));
        }
        return true;
    });

    shouldBeTrue(reader.join());
    churner.join();

    shouldBeFalse("watched" in o);
    shouldBe(o.watched, undefined);
    for (let i = 0; i < PAD; ++i)
        shouldBe(o["base" + i], "stable" + i);
    for (let c = 0; c < CHURN; ++c)
        shouldBeFalse(("tmp" + c) in o, "round " + round + ": tmp" + c + " resurrected");
    for (let c = 0; c < 24; ++c)
        shouldBe(o["fresh" + c], "fresh" + c);
    shouldBe(Object.keys(o).length, PAD + 24);
}

// Concurrent deleters on disjoint names: every targeted name must end
// deleted, every untargeted name must survive intact — no lost deletes and
// no collateral damage through shared table edits.
for (let round = 0; round < ROUNDS; ++round) {
    const THREADS = 4;
    const PER = 16;
    const o = {};
    for (let t = 0; t < THREADS; ++t)
        for (let i = 0; i < PER; ++i)
            o["del_t" + t + "_" + i] = t * 100 + i;
    for (let i = 0; i < PER; ++i)
        o["keep" + i] = "k" + i;

    const deleters = spawnN(THREADS, (t) => {
        for (let i = 0; i < PER; ++i) {
            if (!delete o["del_t" + t + "_" + i])
                throw new Error("delete returned false for own property");
        }
    });
    joinAll(deleters);

    for (let t = 0; t < THREADS; ++t)
        for (let i = 0; i < PER; ++i)
            shouldBeFalse(("del_t" + t + "_" + i) in o,
                "round " + round + ": lost delete del_t" + t + "_" + i);
    for (let i = 0; i < PER; ++i)
        shouldBe(o["keep" + i], "k" + i, "round " + round + ": keep" + i + " damaged");
    shouldBe(Object.keys(o).length, PER);
}
