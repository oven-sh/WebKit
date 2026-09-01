//@ requireOptions("--useJSThreads=1")
// AB18-S3 regression: named adds racing INDEXED first installs on the SAME
// shared object. The indexed first install
// (createInitialIndexedStorageConcurrent) must never publish lock-free into
// the cell-locked named-transition protocols' check->CAS window
// (tryStructureOnlyTransition / trySegmentedTransition FirstInstall fail-stop
// on lane divergence), and the E4 owner leg must revalidate the TTL sets
// after its allocation poll. i03 does NOT cover this shape: its named and
// indexed halves use disjoint objects.
load("../harness.js", "caller relative");

const THREADS = 4;
const ROUNDS = 24;

// Half the threads add named properties (inline first, then out-of-line),
// the other half race dense-index first installs and indexed stores — all on
// ONE shared object per round. Invariants: no add is lost, no abort, no
// value tears.
for (let round = 0; round < ROUNDS; ++round) {
    const o = {};
    const workers = spawnN(THREADS, (t) => {
        if (t & 1) {
            for (let k = 0; k < 8; ++k)
                o["n_t" + t + "_" + k] = t * 100 + k;
        } else {
            for (let k = 0; k < 8; ++k)
                o[t * 8 + k] = "e" + t + "_" + k;
        }
    });
    joinAll(workers);

    for (let t = 0; t < THREADS; ++t) {
        if (t & 1) {
            for (let k = 0; k < 8; ++k)
                shouldBe(o["n_t" + t + "_" + k], t * 100 + k,
                    "round " + round + ": lost named add t" + t + "/" + k + " racing indexed first install");
        } else {
            for (let k = 0; k < 8; ++k)
                shouldBe(o[t * 8 + k], "e" + t + "_" + k,
                    "round " + round + ": lost indexed install t" + t + "/" + k + " racing named adds");
        }
    }
}

// Same shape with the owner thread doing the indexed first install while
// foreign threads add named properties (exercises the E4 owner fast path's
// post-poll set revalidation: the foreign adds' step-0 set fire can land at
// the owner's allocation poll). The owner also pre-populates a flat butterfly
// so the install runs the word!=0 E4 leg, not only N3.
for (let round = 0; round < ROUNDS; ++round) {
    const o = { i0: 0, i1: 1, i2: 2, i3: 3, i4: 4, i5: 5, ool0: 100 }; // ool0 forces out-of-line storage (word != 0).
    const workers = spawnN(THREADS, (t) => {
        for (let k = 0; k < 8; ++k)
            o["f_t" + t + "_" + k] = t * 1000 + k;
    });
    // Owner: indexed first installs while the foreign named adds run.
    for (let k = 0; k < 16; ++k)
        o[k] = k * 3;
    joinAll(workers);

    shouldBe(o.ool0, 100, "round " + round + ": out-of-line literal clobbered");
    for (let i = 0; i < 6; ++i)
        shouldBe(o["i" + i], i, "round " + round + ": inline i" + i + " clobbered");
    for (let k = 0; k < 16; ++k)
        shouldBe(o[k], k * 3, "round " + round + ": lost owner indexed install " + k);
    for (let t = 0; t < THREADS; ++t) {
        for (let k = 0; k < 8; ++k)
            shouldBe(o["f_t" + t + "_" + k], t * 1000 + k,
                "round " + round + ": lost foreign named add t" + t + "/" + k);
    }
}
