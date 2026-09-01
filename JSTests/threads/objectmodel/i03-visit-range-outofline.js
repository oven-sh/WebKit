//@ requireOptions("--useJSThreads=1")
// Annex §15.T12 suite: §4.5 visit-range cross-check, outOfLineSize 1..9
// (I25/I8).
//
// The segmented GC visit marks ALL fragments j < outOfLineFragmentCount but
// value-visits ONLY j < ceil(outOfLineSize/4), and within the last live
// fragment visits the HIGH end: liveCount = min(4, outOfLineSize - 4j),
// appendValuesHidden(slots + (4 - liveCount), liveCount). The §4.1 equations
// (I8) put out-of-line offset k in fragment k/4 slot 3-(k%4), so every
// boundary k in {1..9} crosses a distinct edge case: partial first fragment
// (1..3), exactly-full (4, 8), one-into-next (5, 9). A visit-range bug
// either under-visits (live property collected => corrupt read below) or
// over-visits uninitialized slots (crash). Each property value is a heap
// object reachable ONLY through its slot, GC'd twice with churn in between.
load("../harness.js", "caller relative");

const ROUNDS = 4;

function makeSegmentedWithOutOfLine(k, round) {
    // Literal fills inline capacity exactly (6); the next k adds are
    // out-of-line, so outOfLineSize == k.
    const o = { i0: 0, i1: 1, i2: 2, i3: 3, i4: 4, i5: 5 };
    for (let j = 0; j < k - 1; ++j)
        o["ool" + j] = { k: k, j: j, tag: "r" + round };
    // The LAST add is foreign => §4.2 conversion; the spine's out-of-line
    // fragments alias the flat slices holding ool0..ool(k-2).
    new Thread(() => {
        o["ool" + (k - 1)] = { k: k, j: k - 1, tag: "r" + round };
    }).join();
    return o;
}

for (let round = 0; round < ROUNDS; ++round) {
    const objects = [];
    for (let k = 1; k <= 9; ++k)
        objects.push(makeSegmentedWithOutOfLine(k, round));

    gc();
    // Churn: recycle anything wrongly unmarked before the re-read.
    for (let j = 0; j < 2000; ++j) {
        const junk = [j, "churn" + j, { f: j }];
        if (junk.length !== 3)
            throw 0;
    }
    gc();

    for (let k = 1; k <= 9; ++k) {
        const o = objects[k - 1];
        for (let i = 0; i < 6; ++i)
            shouldBe(o["i" + i], i, "k=" + k + ": inline slot");
        for (let j = 0; j < k; ++j) {
            const v = o["ool" + j];
            shouldBe(typeof v, "object", "k=" + k + " j=" + j + ": out-of-line slot collected (I25 under-visit)");
            shouldBe(v.k, k, "k=" + k + " j=" + j + ": visit-range identity (I8 mapping)");
            shouldBe(v.j, j, "k=" + k + " j=" + j + ": slot aliased another offset (I8)");
            shouldBe(v.tag, "r" + round);
        }
        shouldBe(Object.keys(o).length, 6 + k, "k=" + k + ": shape after GC");
    }
}

// Indexed counterpart at fragment boundaries: vector lengths 1..9 on a
// converted array (element i lives at fragment (i+1)/4 slot (i+1)%4 — the
// +1 skips the frozen IndexingHeader slot), GC'd with churn.
for (let round = 0; round < ROUNDS; ++round) {
    const arrays = [];
    for (let n = 1; n <= 9; ++n) {
        const a = [];
        for (let i = 0; i < n; ++i)
            a[i] = { n: n, i: i };
        new Thread(() => { a.conv = n; }).join(); // convert
        arrays.push(a);
    }

    gc();
    for (let j = 0; j < 2000; ++j) {
        const junk = { churn: j };
        if (junk.churn !== j)
            throw 0;
    }
    gc();

    for (let n = 1; n <= 9; ++n) {
        const a = arrays[n - 1];
        shouldBe(a.length, n, "n=" + n + ": length after GC");
        for (let i = 0; i < n; ++i) {
            shouldBe(typeof a[i], "object", "n=" + n + " i=" + i + ": element collected (I25)");
            shouldBe(a[i].i, i, "n=" + n + " i=" + i + ": element aliased (I8 indexed mapping)");
            shouldBe(a[i].n, n);
        }
        shouldBe(a.conv, n);
    }
}
