//@ requireOptions("--useJSThreads=1")
// Annex §15.T12 suite: shift/unshift AS-COPY vs stale reader snapshot (I31).
//
// Flag-on, AS innards are NEVER relaid out in place: shift/unshift and any
// vector move / indexBias change allocate a FRESH ArrayStorage butterfly
// under the cell lock and casButterfly it (AS-COPY). Superseded storage is
// never written again, so a stale reader's snapshot stays internally frozen
// (conservative scan keeps it alive, I7). Observable: while one thread
// shift/unshifts, readers see values only from the set ever stored — a
// stale snapshot yields a CONSISTENT old element or undefined, never a
// torn/aliased value — and the final array is exact.
load("../harness.js", "caller relative");

const ROUNDS = 6;
const LEN = 64;
const OPS = 120;
const SAMPLES = 500;

for (let round = 0; round < ROUNDS; ++round) {
    // Force ArrayStorage shape first (sparse anchor far out, then deleted
    // territory stays AS), then lay down a dense window.
    const a = [];
    a[100000] = "force-AS";
    delete a[100000];
    a.length = 0;
    for (let i = 0; i < LEN; ++i)
        a[i] = "val" + i;
    const gate = { go: 0 };

    const reader = new Thread(() => {
        while (Atomics.load(gate, "go") === 0)
            sleepMs(1);
        for (let s = 0; s < SAMPLES; ++s) {
            const i = (s * 17) % LEN;
            const v = a[i];
            // Every value ever stored is "val<k>" or "uns<k>"; a stale
            // snapshot may serve any of them (or a hole), but nothing else.
            if (v !== undefined && !/^(val|uns)\d+$/.test(String(v)))
                throw new Error("round " + round + ": torn/aliased AS read a[" + i + "] = " + describe(v) + " (I31)");
            const len = a.length;
            if (len > LEN + OPS)
                throw new Error("round " + round + ": impossible length " + len);
        }
        return true;
    });

    Atomics.store(gate, "go", 1);
    // Owner: alternating shift (drops the head, bumps indexBias) and
    // unshift (fresh head, vector move) — each op is an AS-COPY flag-on.
    let shifted = 0;
    let unshifted = 0;
    for (let op = 0; op < OPS; ++op) {
        if (op & 1) {
            a.unshift("uns" + op);
            ++unshifted;
        } else {
            const head = a.shift();
            if (head === undefined || !/^(val|uns)\d+$/.test(String(head)))
                throw new Error("round " + round + ": shift returned corrupt head " + describe(head));
            ++shifted;
        }
        if (!(op & 7))
            sleepMs(1);
    }
    shouldBeTrue(reader.join());

    shouldBe(a.length, LEN - shifted + unshifted, "round " + round + ": final length");
    for (let i = 0; i < a.length; ++i) {
        const v = a[i];
        if (!/^(val|uns)\d+$/.test(String(v)))
            throw new Error("round " + round + ": final AS content corrupt at " + i + ": " + describe(v));
    }
    // The tail of the original window survives every copy: the last element
    // is always the original val63 (shifts eat the head; unshifts prepend).
    shouldBe(a[a.length - 1], "val" + (LEN - 1), "round " + round + ": tail lost in AS-COPY");
}
