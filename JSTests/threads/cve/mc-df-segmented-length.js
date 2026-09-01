//@ requireOptions("--useJSThreads=1", "--forceButterflySWBit=1")
// MC-DF S3 (docs/threads/cve/map-MC-DF.md): segmented-butterfly indexed
// bounds. publicLength lives in fragment 0 slot 0 and is SHARED by every
// spine the object ever publishes (SPEC-objectmodel C4), while vectorLength
// is per-spine and immutable. The double-fetch hazard: bounds-check against
// publicLength from one tag-word load, then index fragments of a DIFFERENT
// (older, smaller) spine fetched separately => OOB past that spine's
// fragments / the C2 tail. I33 closes it: every access bounds by
// min(publicLength, the SAME loaded spine's vectorLength), and the bounded
// accessors (ConcurrentButterfly.cpp segmentedIndexedSlot family) make the
// stale-spine case return "re-dispatch", not a dereference.
//
// Susceptibility oracle: a reader indexing [0, len) must see only values the
// writer ever stored at that index (i, after any round: still i) or a hole
// (undefined). Garbage, a torn JSValue, or a crash = I33/C4 violation.
//
// EXECUTED POST-UNGIL ONLY. forceButterflySWBit pushes every write through
// the foreign-write path so growth takes the T2 spine-replacement route
// (maximizes spine churn). Amplifier-ready.
load("../harness.js", "caller relative");

const CAP = 4096;
const ROUNDS = 300;
const READERS = 3;
const gate = { started: 0, stop: 0 };
const a = [];

const readers = spawnN(READERS, () => {
    Atomics.add(gate, "started", 1);
    // Foreign write from this thread forces SW=1 even without the stress
    // flag, so subsequent growth segments (SPEC-objectmodel T2).
    a[0] = 0;
    let checks = 0;
    while (Atomics.load(gate, "stop") === 0) {
        const len = a.length; // fetch 1: publicLength
        for (let i = 0; i < len; i += 7) {
            const v = a[i]; // fetch 2: spine/fragment chain
            if (v !== undefined && v !== i)
                throw new Error("torn/garbage element a[" + i + "] = " + v + " (len " + len + ")");
            checks++;
        }
        Atomics.wait(gate, "stop", 0, 1);
    }
    return checks;
});

waitUntil(() => Atomics.load(gate, "started") === READERS);

// Writer: grow (vectorLength growth + spine replacement), shrink via
// length-truncation, re-grow — every element write is index-valued so the
// reader oracle is exact.
for (let r = 0; r < ROUNDS; ++r) {
    for (let i = a.length; i < CAP; ++i)
        a[i] = i;
    a.length = 16;        // shrink: clears [16, min(publicLength, VL)), then publishes
    for (let i = 16; i < CAP; i += 31)
        a[i] = i;          // sparse re-grow: holes between => readers see undefined
    a.length = 8;
}
Atomics.store(gate, "stop", 1);
Atomics.notify(gate, "stop");

const counts = joinAll(readers);
for (const c of counts)
    shouldBeTrue(c > 0, "reader made progress");
