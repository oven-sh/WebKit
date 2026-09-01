//@ requireOptions("--useJSThreads=1")
// Annex §15.T12 suite: convert -> grow x2 -> GC -> read pre-conversion
// property (I7/I25).
//
// The §4.2 conversion ALIASES the existing flat butterfly: the new spine's
// fragments point into 32-byte slices of the old allocation, recorded as
// aliasedAllocationBase/Size on EVERY replacement spine verbatim (I7 — else
// GC UAF). After two further growths (each a fresh spine + fresh fragments,
// old fragments shared), a full GC must still mark: the live spine, the
// aliased flat base, every non-aliased fragment, and every live slot's value
// (I25). The kill shot this suite aims at: a replacement spine that DROPS
// the aliased base lets GC free the original flat allocation while
// pre-conversion properties still live in its slices — the post-GC reads
// below would then see garbage or crash.
load("../harness.js", "caller relative");

const PRECONV = 16;   // pre-conversion out-of-line properties (live in aliased slices)
const GROW1 = 24;
const GROW2 = 48;
const ROUNDS = 6;

for (let round = 0; round < ROUNDS; ++round) {
    // Owner-built flat object: inline full + PRECONV out-of-line properties,
    // each holding a heap object ONLY reachable through that slot (so a
    // missed visit is collectable => detectable corruption).
    const o = { i0: 0, i1: 1, i2: 2, i3: 3, i4: 4, i5: 5 };
    for (let k = 0; k < PRECONV; ++k)
        o["pre" + k] = { round: round, k: k, payload: "alias-slice-" + k };

    // Convert: ONE foreign add (the spine now aliases the flat butterfly).
    new Thread(() => { o.conv = { mark: "converted" }; }).join();

    // Grow x2: two further batches of foreign adds, each crossing fragment
    // counts so the spine is REPLACED (aliased base/size must be copied
    // verbatim both times).
    new Thread(() => {
        for (let k = 0; k < GROW1; ++k)
            o["g1_" + k] = { v: 1000 + k };
    }).join();
    new Thread(() => {
        for (let k = 0; k < GROW2; ++k)
            o["g2_" + k] = { v: 2000 + k };
    }).join();

    // GC pressure: full collections plus garbage churn to recycle anything
    // wrongly unmarked.
    gc();
    for (let j = 0; j < 1000; ++j) {
        const junk = { filler: "x".repeat(32) + j };
        if (junk.filler.length < 0)
            throw 0; // keep the allocation un-elidable
    }
    gc();

    // Read PRE-CONVERSION properties (they live in the aliased slices).
    for (let k = 0; k < PRECONV; ++k) {
        const v = o["pre" + k];
        shouldBe(typeof v, "object", "round " + round + ": pre-conversion slot " + k + " corrupted (I7)");
        shouldBe(v.k, k, "round " + round + ": pre-conversion identity lost (I7)");
        shouldBe(v.payload, "alias-slice-" + k, "round " + round + ": pre-conversion payload lost (I25)");
    }
    // And everything added after (non-aliased fragments, both growth spines).
    shouldBe(o.conv.mark, "converted", "round " + round);
    for (let k = 0; k < GROW1; ++k)
        shouldBe(o["g1_" + k].v, 1000 + k, "round " + round + ": grow-1 slot unvisited (I25)");
    for (let k = 0; k < GROW2; ++k)
        shouldBe(o["g2_" + k].v, 2000 + k, "round " + round + ": grow-2 slot unvisited (I25)");
    for (let i = 0; i < 6; ++i)
        shouldBe(o["i" + i], i, "round " + round + ": inline slot disturbed");
    shouldBe(Object.keys(o).length, 6 + PRECONV + 1 + GROW1 + GROW2);
}

// Indexed flavor: a dense array converted then grown twice; the aliased
// indexed slices (fragment 0 carries the frozen flat IndexingHeader) must
// keep their elements across GC.
for (let round = 0; round < ROUNDS; ++round) {
    const a = [];
    for (let i = 0; i < 20; ++i)
        a[i] = { idx: i };

    new Thread(() => { a.seed = "conv"; }).join(); // convert (aliases the flat butterfly)
    new Thread(() => {
        for (let i = 20; i < 120; ++i)
            a[i] = { idx: i }; // grow 1
    }).join();
    new Thread(() => {
        for (let i = 120; i < 400; ++i)
            a[i] = { idx: i }; // grow 2
    }).join();

    gc();
    gc();

    shouldBe(a.length, 400, "round " + round);
    for (let i = 0; i < 400; ++i)
        shouldBe(a[i].idx, i, "round " + round + ": indexed slot lost after convert+grow+GC (I7/I25)");
    shouldBe(a.seed, "conv");
}
