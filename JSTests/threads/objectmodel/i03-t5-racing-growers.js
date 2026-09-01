//@ requireOptions("--useJSThreads=1")
// Annex §15.T12 suite: T5 racing growers + T5-vs-conversion (I21).
//
// T5 is the cell-locked, owner-only in-place vectorLength growth
// (ensureLengthSlow's in-place branch): lock; re-check tag == (currentTID,0);
// clear [oldVL,newVL); fenced setVectorLength; unlock. Foreign growth is T2
// (fresh spine via §4.2/§4.3-1 + 64-bit CAS). This suite races:
//   (a) N threads growing ONE shared array with disjoint dense stripes —
//       every interleaving of T5 / T1 / T2 / conversion must keep every
//       stripe element (I21: no lost element stores);
//   (b) a grower vs a CONVERTER (foreign out-of-line property add => §4.2
//       flat->segmented conversion re-reads vectorLength under the SAME cell
//       lock — the §16.1/T5 interleaving proof) — both must land.
load("../harness.js", "caller relative");

const THREADS = 4;
const N = 512; // dense range, striped across threads
const ROUNDS = 8;

// (a) Striped dense growth: thread t writes every index i with i % THREADS == t,
// in ascending order, so the array grows densely from all sides at once.
for (let round = 0; round < ROUNDS; ++round) {
    const a = [];
    a[0] = -1; // owner-installed butterfly; growth from here races
    const workers = spawnN(THREADS, (t) => {
        for (let i = t; i < N; i += THREADS) {
            a[i] = i * 7 + 1;
            if (!(i & 127))
                sleepMs(1);
        }
    });
    joinAll(workers);

    shouldBe(a.length, N, "round " + round + ": length after striped racing growth");
    for (let i = 0; i < N; ++i)
        shouldBe(a[i], i * 7 + 1, "round " + round + ": lost striped element a[" + i + "] (I21)");
}

// (b) T5-vs-conversion: the owner grows the array's vector in place while a
// foreign thread adds out-of-line NAMED properties to the same array object
// (forcing the §4.2 conversion, which re-reads VL/publicLength under the
// cell lock). Both the elements and the named properties must survive.
const PROPS = 24;
for (let round = 0; round < ROUNDS; ++round) {
    const a = [0];
    const gate = { go: 0 };

    const converter = new Thread(() => {
        while (Atomics.load(gate, "go") === 0)
            sleepMs(1);
        for (let p = 0; p < PROPS; ++p) {
            a["prop" + p] = "v" + p; // foreign transition: conversion + segmented growth
            if (!(p & 3))
                sleepMs(1);
        }
        return true;
    });

    Atomics.store(gate, "go", 1);
    for (let i = 1; i < N; ++i) {
        a[i] = i; // owner growth: T5/T1 until the conversion, T2 after
        if (!(i & 63))
            sleepMs(1);
    }
    shouldBeTrue(converter.join());

    shouldBe(a.length, N, "round " + round + ": length after T5-vs-conversion");
    for (let i = 0; i < N; ++i)
        shouldBe(a[i], i === 0 ? 0 : i, "round " + round + ": element lost across conversion (I21)");
    for (let p = 0; p < PROPS; ++p)
        shouldBe(a["prop" + p], "v" + p, "round " + round + ": named property lost across growth (I21)");
}
