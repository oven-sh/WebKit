//@ requireOptions("--useJSThreads=1", "--verifyConcurrentButterfly=1")
// MC-DF S8 + S10b (docs/threads/cve/map-MC-DF.md): the CVE-2014-0456
// System.arraycopy shape — type/layout checked on fetch 1, raw bytes
// copied on fetch 2 — at the two §10.7 sites the round-4 single-snapshot
// sweep did NOT reach:
//
//   S10b: setFromArrayLike (JSGenericTypedArrayViewInlines.h:481) gates on
//         !mayBeSegmentedButterfly() then copyFromInt32ShapeArray re-loads
//         array->butterfly() FRESH at :417/:421/:425/:429.
//   S8:   sortCompact (ArrayPrototype.cpp:830) gates on
//         !mayBeSegmentedButterfly() then *thisObject->butterfly() at :834.
//
// Round-4 (JSArray.cpp:1752-1762) established that once a shape family's
// TTL sets are fired, a foreign §4.2 flat→segmented conversion needs only
// the cell lock + DCAS — NO stop — so it can land between the §10.7 check
// and the butterfly() re-load. The flat-only decode then reads the
// ButterflySpine* payload as a Butterfly*; copyElements / the compact loop
// then reads spine innards as the source span.
//
// Detector: --verifyConcurrentButterfly=1 turns the JSObject.h:920
// RELEASE_ASSERT(!isSegmentedButterfly(word)) inside butterfly() into the
// crisp oracle — if the TOCTOU lands, the process aborts with that assert.
// Without the verify flag, the sentinel-set oracle below still applies
// (any TA element ∉ {SENTINEL, 0} is spine-as-flat OOB evidence).
//
// EXECUTED POST-UNGIL ONLY. Amplifier-ready: the §4.2 conversion is
// one-shot per object, so each round publishes a FRESH Int32 JSArray and
// the writer drives it through SW=1-flat → §4.2-segmented while main
// races the two consumers. Trivially green under the phase-1 GIL.
load("../harness.js", "caller relative");

const LEN = 64;
const ROUNDS = 4000;
const SENTINEL = 0x2bad0000 | 0; // distinctive, survives int32 truncation

const dst = new Int32Array(LEN);
const slot = { arr: null, go: 0, done: 0, stop: 0 };

const writer = spawnN(1, () => {
    let conversions = 0;
    while (Atomics.load(slot, "stop") === 0) {
        // Spin until main publishes the round's fresh array.
        while (Atomics.load(slot, "go") === 0) {
            if (Atomics.load(slot, "stop") !== 0)
                return conversions;
        }
        const a = slot.arr;
        // Foreign first write: SW=0→SW=1 (F1 fire, STW the first time per
        // shape family; subsequent rounds: TTL sets already fired).
        a[0] = SENTINEL;
        // Drive §4.2: push past the flat butterfly's vectorLength so the
        // foreign-write grow takes the T2 spine-replacement / segmentation
        // route. After round 0 the family's TTL sets are fired and THIS
        // conversion is cell-lock-only — exactly the window under test.
        for (let i = LEN; i < LEN + 48; ++i)
            a[i] = SENTINEL;
        conversions++;
        Atomics.store(slot, "go", 0);
        Atomics.store(slot, "done", 1);
    }
    return conversions;
});

for (let r = 0; r < ROUNDS; ++r) {
    // Fresh Int32-shape JSArray every round (the §4.2 conversion is one-shot).
    const a = [];
    for (let i = 0; i < LEN; ++i)
        a[i] = SENTINEL;
    slot.arr = a;
    Atomics.store(slot, "done", 0);
    Atomics.store(slot, "go", 1);

    // Race the two §10.7 consumers against the writer's §4.2 window. Both
    // call mayBeSegmentedButterfly() (check) then butterfly() (act) on `a`.
    // A few back-to-back attempts per round widen the hit window without
    // waiting on the writer.
    for (let k = 0; k < 8; ++k) {
        dst.set(a, 0);                         // S10b: setFromArrayLike fast path
        for (let i = 0; i < LEN; ++i) {
            const v = dst[i];
            if (v !== SENTINEL && v !== 0)     // 0 = hole-as-undefined → toNative 0
                throw new Error("S10b OOB evidence: dst[" + i + "] = 0x" + (v >>> 0).toString(16)
                    + " ∉ {SENTINEL, 0} after ta.set(sharedArray) (round " + r + ")");
        }
        a.sort();                              // S8: sortCompact fast path
        // No value oracle for sort (writer also stores SENTINEL); the
        // verifyConcurrentButterfly RELEASE_ASSERT is the detector here.
    }

    while (Atomics.load(slot, "done") === 0) { /* spin */ }
}

Atomics.store(slot, "stop", 1);
const [conversions] = joinAll(writer);
shouldBeTrue(conversions > 0, "writer drove §4.2 conversions");
