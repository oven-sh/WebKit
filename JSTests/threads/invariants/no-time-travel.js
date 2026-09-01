//@ requireOptions("--useJSThreads=1")
// SPEC-objectmodel invariant: no property-value time-travel (I21).
//
// A broken transition protocol (copy butterfly, then swing the pointer)
// makes a concurrent write to an existing slot land in the OLD copy and
// vanish when the new butterfly is published — observers see A, then B,
// then A again. The invariant: a single-writer monotonically-increasing
// property must never be observed to decrease, by any thread, ever —
// including across transitions that reallocate the storage.
load("../resources/assert.js", "caller relative");

const ROUNDS = 6;
const WRITES = 2000;
const TRANSITIONS = 64;
const SAMPLES = 1500;

for (let round = 0; round < ROUNDS; ++round) {
    const o = { v: 0 };
    // Pre-populate so v lives out-of-line (past inline capacity) in flat
    // layouts, maximizing exposure to butterfly reallocation.
    for (let i = 0; i < 24; ++i)
        o["pad" + i] = i;

    const writer = new Thread(() => {
        for (let i = 1; i <= WRITES; ++i)
            o.v = i;
    });
    // Transitioner forces storage growth/reshape while the writer is storing
    // to an existing slot.
    const transitioner = new Thread(() => {
        for (let i = 0; i < TRANSITIONS; ++i)
            o["grow" + round + "_" + i] = i;
    });
    // Reader: o.v must be non-decreasing in program order of its samples.
    const reader = new Thread(() => {
        let last = 0;
        for (let s = 0; s < SAMPLES; ++s) {
            const v = o.v;
            if (typeof v !== "number")
                throw new Error("torn/corrupt read of o.v: " + describe(v));
            if (v < last)
                throw new Error("time travel: o.v went from " + last
                    + " back to " + v + " (round " + round + ")");
            last = v;
        }
        return last;
    });

    const lastSeen = reader.join();
    writer.join();
    transitioner.join();

    shouldBe(o.v, WRITES, "final write lost (round " + round + ")");
    shouldBeTrue(lastSeen <= WRITES);
    for (let i = 0; i < TRANSITIONS; ++i)
        shouldBe(o["grow" + round + "_" + i], i, "transitioner add lost");
    for (let i = 0; i < 24; ++i)
        shouldBe(o["pad" + i], i, "pre-existing slot corrupted");
}

// Indexed variant: element a[0] is monotone while another thread grows the
// array (butterfly-pointer resizes, §4.4). Neither a[0] nor a.length may be
// observed to regress.
for (let round = 0; round < ROUNDS; ++round) {
    const a = [0];
    const writer = new Thread(() => {
        for (let i = 1; i <= WRITES; ++i)
            a[0] = i;
    });
    const grower = new Thread(() => {
        for (let i = 1; i <= 512; ++i)
            a[i] = -i; // out-of-bounds store: forces vector growth
    });
    const reader = new Thread(() => {
        let lastV = 0, lastLen = 1;
        for (let s = 0; s < SAMPLES; ++s) {
            const len = a.length;
            const v = a[0];
            if (typeof v !== "number")
                throw new Error("torn read of a[0]: " + describe(v));
            if (v < lastV)
                throw new Error("time travel on a[0]: " + lastV + " -> " + v);
            if (len < lastLen)
                throw new Error("time travel on a.length: " + lastLen + " -> " + len);
            lastV = v;
            lastLen = len;
        }
        return true;
    });
    shouldBeTrue(reader.join());
    writer.join();
    grower.join();
    shouldBe(a[0], WRITES, "final a[0] write lost");
    shouldBe(a.length, 513);
    for (let i = 1; i <= 512; ++i) {
        if (a[i] !== -i)
            throw new Error("lost element write a[" + i + "]: " + describe(a[i]));
    }
}
