//@ requireOptions("--useJSThreads=1")
// Annex §15.T12 suite: owner sparse-insert loop vs foreign hole-reads,
// locked (I31).
//
// ArrayStorage objects NEVER segment; flag-on, EVERY runtime/interpreter
// access — reads included, any SW state — takes the cell lock (§4.6/L5).
// The sparse map is a rehashing HashMap: an unlocked foreign hole-read
// walking the bucket array while the owner's insert rehashes it is a UAF.
// Scenario: the owner inserts into the sparse map in a loop (forcing
// repeated rehashes) while foreign threads read holes and present sparse
// entries. Invariants: reads return undefined or the inserted value, never
// garbage; no crash; the final map is exact.
load("../harness.js", "caller relative");

const ROUNDS = 6;
const INSERTS = 160;       // enough sparse entries for several rehashes
const SAMPLES = 500;
const SPARSE_BASE = 1e7;   // far beyond any dense vector

for (let round = 0; round < ROUNDS; ++round) {
    const a = [];
    a[SPARSE_BASE - 1] = "anchor"; // ArrayStorage + sparse map from the start
    // (below every raced key, so readers never observe it mid-loop)
    const gate = { go: 0 };

    const holeReader = new Thread(() => {
        while (Atomics.load(gate, "go") === 0)
            sleepMs(1);
        for (let s = 0; s < SAMPLES; ++s) {
            // Read holes BETWEEN the owner's insert keys (never written).
            const hole = a[SPARSE_BASE + 2 * ((s * 11) % INSERTS) + 1];
            if (hole !== undefined)
                throw new Error("round " + round + ": hole read non-undefined " + describe(hole) + " (I31)");
            // Read a key the owner may or may not have inserted yet.
            const k = 2 * ((s * 7) % INSERTS);
            const v = a[SPARSE_BASE + k];
            if (v !== undefined && v !== k)
                throw new Error("round " + round + ": sparse read tore/aliased: a[base+" + k + "] = " + describe(v));
        }
        return true;
    });
    const presenceReader = new Thread(() => {
        while (Atomics.load(gate, "go") === 0)
            sleepMs(1);
        for (let s = 0; s < SAMPLES; ++s) {
            const k = 2 * ((s * 5) % INSERTS);
            const present = (SPARSE_BASE + k) in a;
            if (present && a[SPARSE_BASE + k] === undefined && (SPARSE_BASE + k) in a)
                throw new Error("round " + round + ": present sparse key read undefined twice (I31)");
        }
        return true;
    });

    Atomics.store(gate, "go", 1);
    // Owner sparse-insert loop (even keys only; odd keys stay holes).
    for (let k = 0; k < INSERTS; ++k) {
        a[SPARSE_BASE + 2 * k] = 2 * k;
        if (!(k & 15))
            sleepMs(1); // let the readers interleave with rehash windows
    }

    shouldBeTrue(holeReader.join());
    shouldBeTrue(presenceReader.join());

    shouldBe(a[SPARSE_BASE - 1], "anchor", "round " + round);
    shouldBe(a[SPARSE_BASE], 0, "round " + round);
    for (let k = 0; k < INSERTS; ++k) {
        shouldBe(a[SPARSE_BASE + 2 * k], 2 * k, "round " + round + ": sparse insert lost (I31)");
        shouldBe(a[SPARSE_BASE + 2 * k + 1], undefined, "round " + round + ": hole materialized");
    }
    shouldBe(a.length, SPARSE_BASE + 2 * (INSERTS - 1) + 1, "round " + round + ": sparse length");
}
