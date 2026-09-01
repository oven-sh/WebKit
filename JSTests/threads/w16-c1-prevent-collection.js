//@ requireOptions("--useJSThreads=1")
// w16-c1-prevent-collection.js — W16 residual C1 regression (legs (a)+(b)):
// preventCollection() semantics with N mutators once isSharedServer().
//
// Mechanism under test (leg (b), Heap::preventCollection): the legacy
// postcondition "wait until served == granted, then no collection can start
// unless this thread starts it" silently relied on the single-mutator
// protocol — only the caller and the CIND timer could request, and the timer
// is excluded by m_collectContinuouslyLock. Once shared, OTHER mutators can
// ticket (requestCollectionShared) and elect at any time, so served ==
// granted is neither achievable under churn nor sufficient after return.
// The landed fix raises m_sharedGCPreventCount (conduct-tenure gate) under
// *m_threadLock and waits only for the in-flight cycle to drain
// (!m_gcConductorActive && phase == NotRunning); BOTH shared
// collection-start sites — the §10.2 election winner arm and
// tryConductSharedCollectionForPoll() — refuse while the gate is up, and
// tickets sit granted-unserved until allowCollection() drops the gate and
// notifies the GC election condition.
//
// Leg (a) rides the same caller: the PreventCollectionScope holder reaches
// waitForCollector(), whose ISS branch is the keep-waiting + rate-limited
// dump policy (SINFAC poll each iteration + <=1ms timed GEC waits). A wedge
// there is hang-shaped; this test bounds it by doing a fixed number of
// snapshots and joining all threads — a stuck waiter fails via the
// harness/runner timeout instead of passing silently.
//
// JS-reachable PreventCollectionScope: the heap snapshot builders
// (HeapSnapshotBuilder.cpp:73 / BunV8HeapSnapshotBuilder.cpp), exposed in
// the jsc shell as generateHeapSnapshot()/generateHeapSnapshotForGCDebugging().
//
// Storm shape: W churn threads allocate heavily and a subset force gc() —
// a continuous stream of shared tickets and conduct attempts — while the
// main thread loops snapshot generation. Pre-fix, a sibling's ticket+elect
// could start a collection inside the prevent window: Debug trips
// RELEASE_ASSERT(!m_collectionScope) at the end of preventCollection() (or
// the I5 stopped-world assert in runBeginPhase); Release walks a mutating
// heap mid-snapshot. Each snapshot is JSON.parsed as the self-check so
// structural corruption — not just the assert — fails the test. Flag-on
// GIL'd and flag-off single-thread arms must also pass (the gate code is
// isSharedServer()-only; legacy behavior is byte-identical).

load("./harness.js", "caller relative");

const HAVE_THREADS = typeof Thread === "function";
const HAVE_GC = typeof gc === "function";
const HAVE_SNAPSHOT = typeof generateHeapSnapshot === "function";
const HAVE_SNAPSHOT_GCDEBUG = typeof generateHeapSnapshotForGCDebugging === "function";

const W = HAVE_THREADS ? 8 : 1;
const ROUNDS = 200;   // churn rounds per worker
const SNAPS = 12;     // prevent-collection windows opened by the main thread

function churnWorker(seed) {
    let check = 0;
    for (let r = 0; r < ROUNDS; ++r) {
        // Allocation lane: enough garbage per round that shared tickets keep
        // flowing (RCAC thresholds) while the main thread holds the gate.
        let junk = [];
        for (let i = 0; i < 800; ++i)
            junk.push({ x: (i ^ seed) | 0, y: "s" + (i & 31), z: [i, r, seed] });
        check = (check + junk.length + junk[((seed + r) % 800) | 0].x) | 0;

        // Election lane: a subset of threads force synchronous collections —
        // these are the §10.2 elections that must back off to the follower
        // wait while m_sharedGCPreventCount is raised, then win later.
        if (HAVE_GC && (seed & 1) === 0 && (r & 15) === 8)
            gc();
    }
    return check;
}

function takeSnapshots() {
    for (let s = 0; s < SNAPS; ++s) {
        if (HAVE_SNAPSHOT) {
            const snap = generateHeapSnapshot();
            // Self-check: the builder walked a non-mutating heap. A
            // collection started inside the prevent window shows up as an
            // assert (Debug) or a malformed/torn snapshot (Release).
            if (typeof snap === "string") {
                const parsed = JSON.parse(snap);
                if (!parsed || typeof parsed !== "object")
                    throw new Error("snapshot " + s + ": parsed to non-object");
            } else if (!snap || typeof snap !== "object")
                throw new Error("snapshot " + s + ": unexpected result type " + typeof snap);
        }
        // The GC-debugging variant takes the same PreventCollectionScope but
        // keeps cell metadata; alternate so both builder paths cross the
        // prevent window under churn.
        if (HAVE_SNAPSHOT_GCDEBUG && (s & 3) === 1)
            generateHeapSnapshotForGCDebugging();
        if (!HAVE_SNAPSHOT && !HAVE_SNAPSHOT_GCDEBUG)
            return false; // Shell without snapshot hooks: nothing to exercise.
    }
    return true;
}

if (HAVE_THREADS) {
    const threads = spawnN(W, churnWorker);
    // Main thread opens prevent windows while the storm runs. Even if some
    // workers finish early the first windows overlap the storm; SNAPS and
    // ROUNDS are sized so several windows open under full churn.
    const exercised = takeSnapshots();
    const results = joinAll(threads);
    // Determinism reference (same shape as dw2): the worker checksum is a
    // pure function of its seed; a post-quiesce rerun must match, catching
    // cross-thread heap corruption from a collection that ran inside a
    // prevent window.
    for (let i = 0; i < W; ++i) {
        const expected = churnWorker(i);
        if (results[i] !== expected)
            throw new Error("thread " + i + ": checksum " + results[i] + " != reference " + expected);
    }
    if (!exercised)
        print("PASS (snapshot hooks unavailable; churn-only)");
    else
        print("PASS");
} else {
    // Flag-off arm: same lanes single-threaded; the gate code is dormant
    // (isSharedServer() is false) and legacy preventCollection semantics
    // must be unchanged.
    const c0 = churnWorker(0);
    takeSnapshots();
    if (churnWorker(0) !== c0)
        throw new Error("flag-off determinism failure");
    print("PASS");
}
