//@ requireOptions("--useJSThreads=1")
// SPEC-jit Task 1b / I19: 3-thread per-thread butterfly TID-tag exercise.
//
// JS cannot read g_jscButterflyTIDTag directly; what this test pins down is
// the OBSERVABLE contract around it:
//   - the main thread is TID 0 (zero-init of the tag is correct only there),
//   - three concurrently-live spawned threads each get a distinct nonzero
//     TID (Thread.current.id is the same ButterflyTID the tag is built from,
//     vmstate section 6.7 / api section 5.2),
//   - per-thread butterfly work (out-of-line property transitions on objects
//     allocated by that thread) stays correct on every thread,
//   - threads spawned AFTER earlier ones detach still see coherent ids and
//     butterfly behavior (exercises initialize/clear + the CS3 hook path; the
//     C++-side I19 RELEASE_ASSERT and the App. R5 TLS-offset constancy
//     RELEASE_ASSERT fire under the hood on every attach).
//
// Under the phase-1 GIL this is a semantic oracle; flag-on post-GIL the same
// file doubles as a true-concurrency smoke for the R5 tag loads.

load("../resources/assert.js", "caller relative");

// Main thread is the TID-0 owner.
shouldBe(Thread.current.id, 0);

function butterflyWorkout(label) {
    // Force out-of-line (butterfly) properties: more properties than any
    // inline capacity, added dynamically so they transition the structure.
    const objects = [];
    for (let i = 0; i < 64; ++i) {
        const o = {};
        for (let j = 0; j < 40; ++j)
            o["p" + j] = label + ":" + i + ":" + j;
        // Indexed (array) butterfly side too.
        const a = [];
        for (let j = 0; j < 40; ++j)
            a[j] = j + i;
        objects.push({ named: o, indexed: a });
    }
    // Verify everything we wrote on this thread reads back exactly.
    for (let i = 0; i < 64; ++i) {
        const { named, indexed } = objects[i];
        for (let j = 0; j < 40; ++j) {
            shouldBe(named["p" + j], label + ":" + i + ":" + j);
            shouldBe(indexed[j], j + i);
        }
    }
    return objects.length;
}

// Wave 1: three concurrently-live threads, each doing owner-thread butterfly
// transitions, reporting its TID.
const wave1 = spawnN(3, (index) => {
    const tid = Thread.current.id;
    if (tid === 0)
        throw new Error("spawned thread " + index + " must not have TID 0");
    if ((tid >>> 0) !== tid || tid > 0x7fff)
        throw new Error("TID out of the 15-bit ButterflyTID space: " + tid);
    butterflyWorkout("w1t" + index);
    return tid;
});
const wave1Ids = joinAll(wave1);
shouldBe(new Set(wave1Ids).size, 3, "3 concurrently-live threads must have 3 distinct TIDs");
for (let i = 0; i < 3; ++i)
    shouldBe(wave1Ids[i], wave1[i].id, "join result must agree with Thread.id");

// Main thread's TID is unchanged by other threads attaching/detaching.
shouldBe(Thread.current.id, 0);
butterflyWorkout("main");

// Wave 2: after wave-1 threads detached, fresh threads attach (re-running P5
// init on new OS threads; on ELF this re-checks the TLS-offset constancy
// RELEASE_ASSERT, on Darwin the TSD-slot sync). Their TIDs must be nonzero,
// distinct from each other, and they must do correct butterfly work.
const wave2 = spawnN(3, (index) => {
    const tid = Thread.current.id;
    if (tid === 0)
        throw new Error("wave-2 thread " + index + " must not have TID 0");
    butterflyWorkout("w2t" + index);
    return tid;
});
const wave2Ids = joinAll(wave2);
shouldBe(new Set(wave2Ids).size, 3, "wave-2 TIDs must be distinct among live threads");

// Cross-thread writes to a shared, main-thread-allocated object: the writes
// land (GIL oracle today; flag-on this is the SW-bit path the tag compare
// guards, SPEC-jit section 5.5 predicate (4)).
const shared = { hits: 0 };
const writers = spawnN(3, (index) => {
    shared["fromThread" + Thread.current.id] = index;
    for (let i = 0; i < 100; ++i)
        Atomics.add(shared, "hits", 1);
});
joinAll(writers);
shouldBe(shared.hits, 300);
let foreignProperties = 0;
for (const key in shared) {
    if (key.startsWith("fromThread"))
        ++foreignProperties;
}
shouldBe(foreignProperties, 3, "all 3 foreign-thread property additions must be visible");

// Task-8 (SCALEBENCH §43 residual #2): JIT-hot owner-thread JSArray
// inline-allocate + push growth. With JSArray iso-TLC enabled GIL-off,
// the DFG/FTL inline-allocate fast path stores the butterfly word; this
// loop drives both tiers (per-tier forcing covered by run-jit-tests.sh)
// so the §4.2 ensureLength dispatch sees a TID-tagged owner word and
// stays on the contiguous fast path. Functional oracle here is exact
// element correctness across growth; the convertToSegmentedButterfly==0
// invariant is the external uprobe gate.
function jitHotArrayBuildAndGrow(seed) {
    const a = [];
    for (let j = 0; j < 48; ++j)
        a.push(seed + j);
    // Literal form (NewArray / NewArrayBuffer fast path).
    const b = [seed, seed + 1, seed + 2, seed + 3];
    for (let j = 4; j < 48; ++j)
        b[j] = seed + j;
    // NewArrayWithSize fast path.
    const c = new Array(8);
    for (let j = 0; j < 48; ++j)
        c[j] = seed + j;
    let sum = 0;
    for (let j = 0; j < 48; ++j) {
        if (a[j] !== seed + j)
            throw new Error("Task-8: a[" + j + "] = " + a[j] + " want " + (seed + j));
        if (b[j] !== seed + j)
            throw new Error("Task-8: b[" + j + "] = " + b[j] + " want " + (seed + j));
        if (c[j] !== seed + j)
            throw new Error("Task-8: c[" + j + "] = " + c[j] + " want " + (seed + j));
        sum += a[j] + b[j] + c[j];
    }
    return sum;
}
noInline(jitHotArrayBuildAndGrow);

function task8Workout(label) {
    let acc = 0;
    for (let i = 0; i < 20000; ++i)
        acc += jitHotArrayBuildAndGrow(i);
    // Closed form: sum over i of 3 * sum_{j=0}^{47}(i+j) = 3 * (48*i + 1128).
    let expect = 0;
    for (let i = 0; i < 20000; ++i)
        expect += 3 * (48 * i + 1128);
    shouldBe(acc, expect, "Task-8 " + label + " checksum");
}

// Main thread (TID 0): the §43 baseline already had this on the lazy slow
// path; with Task-8 the inline path fires and must stay owner.
task8Workout("main");

// Spawned threads: each has a distinct nonzero TID, so the inline-installed
// butterfly word must carry THAT thread's tag (not 0, not stale).
const task8 = spawnN(3, (index) => {
    task8Workout("t" + index);
    return Thread.current.id;
});
const task8Ids = joinAll(task8);
shouldBe(new Set(task8Ids).size, 3);

print("tid-tag-3-threads: PASS");
