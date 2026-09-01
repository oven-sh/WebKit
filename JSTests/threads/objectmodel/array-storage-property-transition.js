//@ requireOptions("--useJSThreads=1")
// FUZZ r3-001 regression: §4.6 (I31) AS out-of-line property transition.
//
// A non-dictionary ArrayStorage-shaped object (object literal with a
// sparse-index integer key) adding an out-of-line named property is excluded
// from BOTH E4 (Structure::mayTransitionLockFreeFromThisStructure rejects AS,
// I31 review-round-3) AND §4.3/§4.2 (trySegmentedTransition /
// convertToSegmentedButterfly entry-assert !AS - AS never segments). The
// routing gap left it falling into trySegmentedTransition's I31
// RELEASE_ASSERT. Fix: tryPutDirectTransitionConcurrent routes AS shapes to
// tryArrayStoragePropertyTransition (cell-locked AS-COPY + nuke-bracketed
// structure publish, T3/I17/M5/M8). Pre-existing bug, NOT a §45 regression -
// the §45 StayFlatShared gate is gilOff-only and sits AFTER the I31 assert.
//
// Minimized from Tools/threads/fuzz/crashes/r3/r3-001-*.js: a literal with a
// sparse integer key (forces NonArrayWithArrayStorage at construction) plus
// enough named properties to spill out of line. Single-threaded,
// deterministic; flag-off semantics are unchanged (I22).
load("../harness.js", "caller relative");

// ---- Single-threaded, owner TID: hits the no-growth (reuse) AND the
// growth (AS-COPY) legs as named properties accumulate.
{
    // {sparse-index: ...} literal: 2 inline slots; p0/p1 inline, p2 spills
    // out of line on a NonArrayWithArrayStorage structure (the original
    // crash point).
    const o = { 3340997507: 99 };
    for (let i = 0; i < 30; ++i)
        o["p" + i] = i;
    for (let i = 0; i < 30; ++i)
        shouldBe(o["p" + i], i, "AS named add p" + i);
    shouldBe(o[3340997507], 99, "AS sparse index survived AS-COPY growths");
    shouldBe(Object.keys(o).length, 31);

    // Replace through an out-of-line offset on the AS shape (I31/L5 locked).
    o.p17 = "replaced";
    shouldBe(o.p17, "replaced");
}

{
    // 6-inline literal; the .g add is the first out-of-line offset on an AS
    // structure (the literal itself is created via op_new_object then
    // putDirect, so the inline adds pre-stamp the structure flag-on too).
    const q = { 3340997507: 1, a: 1, b: 2, c: 3, d: 4, e: 5, f: 6 };
    q.g = 7; q.h = 8; q.i = 9; q.j = 10; q.k = 11; q.l = 12; q.m = 13;
    shouldBe(q.g, 7);
    shouldBe(q.m, 13);
    shouldBe(q[3340997507], 1, "AS sparse index survived");
    shouldBe(q.a, 1, "inline survived");
}

{
    // The fuzzer's original shape: huge integer key + computed Symbol method
    // + computed accessor (a setter under a stringified key), then a named
    // out-of-line add. The accessor add itself is an out-of-line offset on
    // an AS structure too.
    const sym = Symbol.iterator;
    const v97 = {
        3340997507() { return 0; },
        [sym]() { return { next() { return { done: true }; } }; },
        set ["computed-setter-key"](v) { },
    };
    v97.e = v97;
    v97.f = "f"; v97.g = "g"; v97.h = "h";
    shouldBe(v97.e, v97);
    shouldBe(v97.h, "h");
    shouldBe(typeof v97[3340997507], "function");
    shouldBe(typeof v97[sym], "function");
}

// ---- Foreign thread: the §4.6 per-event SW stop + cell-locked AS-COPY
// property transition. A foreign out-of-line named add to an SW=0 AS
// instance first runs ensureSharedWriteBit (per-event STW publishes
// (installerTID, 1) flat), RESTARTs, then the locked AS-COPY transition
// preserves the (installerTID, 1) tag verbatim (I27/T3). Under the
// cooperative GIL the rendezvous still executes the same code paths;
// gilOff just lets the owner reads interleave for real.
{
    const ROUNDS = 4;
    for (let round = 0; round < ROUNDS; ++round) {
        const a = { 3340997507: "sparse" }; // owner-installed AS, SW=0
        a.own0 = 0; a.own1 = 1; // fill inline; foreign adds spill out of line
        const gate = { go: 0 };

        const adder = new Thread(() => {
            while (Atomics.load(gate, "go") === 0)
                sleepMs(1);
            for (let i = 0; i < 24; ++i)
                a["w" + i] = i; // foreign AS out-of-line property transition
            return true;
        });

        Atomics.store(gate, "go", 1);
        // Owner: read named + sparse while the foreign thread transitions.
        for (let s = 0; s < 200; ++s) {
            shouldBe(a.own0, 0, "round " + round + ": owner inline survived foreign AS add");
            const sp = a[3340997507];
            if (sp !== "sparse")
                throw new Error("round " + round + ": sparse read tore/dropped: " + describe(sp));
            const w7 = a.w7;
            if (w7 !== undefined && w7 !== 7)
                throw new Error("round " + round + ": foreign add aliased: a.w7 = " + describe(w7));
        }
        shouldBeTrue(adder.join());

        for (let i = 0; i < 24; ++i)
            shouldBe(a["w" + i], i, "round " + round + ": foreign AS add lost (I31)");
        shouldBe(a[3340997507], "sparse", "round " + round + ": AS-COPY dropped sparse entry");
        shouldBe(a.own0, 0);
        shouldBe(a.own1, 1);
    }
}
