//@ requireOptions("--useJSThreads=1", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1")
// MC-GC S6 (docs/threads/cve/map-MC-GC.md): FinalizationRegistry reference
// processing when the collection is CONDUCTED BY A SPAWNED THREAD — the
// CVE-2023-21954 "reference enqueue during GC" analog. In shared mode,
// JSFinalizationRegistry::finalizeUnconditionally runs on the conductor
// inside the stop window and calls DeferredWorkTimer::addPendingWork +
// scheduleWorkSoon (JSFinalizationRegistry.cpp:154-160). When the conductor
// is a spawned JS thread, that addPendingWork takes the gilOff
// internal-arm route (DeferredWorkTimer.cpp, UNGIL §E.7) — the cleanup task
// must still be delivered exactly once, to the registering realm, on a
// carrier drain, regardless of which thread conducted the GC and regardless
// of that thread exiting before the task runs.
//
// Susceptibility oracles:
//   1. No duplicate cleanup for one registration (a holdings value seen
//      twice = the registry re-enqueued a dead cell across two conducted
//      stops — reference-processing protocol confusion).
//   2. No cleanup for a holdings value that was never registered, and no
//      cleanup whose holdings was already unregistered.
//   3. At least one cleanup is eventually delivered (zero deliveries after
//      spawned-conductor full GCs = the enqueue was routed into a dead
//      thread's queue and lost; the shell then exits non-zero through the
//      unfulfilled asyncTestStart).
//   4. Cleanup runs with the registry's realm intact (globalThis identity).
//
// Note: conservative scanning may keep SOME targets alive spuriously; the
// test therefore never requires all N cleanups, only exactly-once semantics
// for those delivered, plus at-least-one delivery.
//
// EXECUTED POST-UNGIL ONLY (do not run against the mid-bring-up tree).
load("../harness.js", "caller relative");

asyncTestStart(1);

const TARGETS = 128;
const CONDUCTORS = 3;
const GCS_PER_CONDUCTOR = 6;

const mainGlobal = globalThis;
const seen = new Set();
let passed = false;

const registry = new FinalizationRegistry(holdings => {
    if (globalThis !== mainGlobal)
        throw new Error("cleanup ran against a foreign realm");
    if (typeof holdings !== "number" || !Number.isInteger(holdings))
        throw new Error("cleanup holdings corrupted: " + String(holdings));
    if (holdings < 0 || holdings >= TARGETS)
        throw new Error("cleanup for never-registered holdings " + holdings);
    if ((holdings % 16) === 7)
        throw new Error("cleanup for an UNREGISTERED holdings " + holdings);
    if (seen.has(holdings))
        throw new Error("duplicate cleanup for holdings " + holdings);
    seen.add(holdings);
    if (!passed) {
        passed = true;
        asyncTestPassed();
    }
});

// Register targets in a callee frame so the references die at return. A
// sixteenth of them are unregistered again immediately (oracle 2: their
// holdings must never be delivered).
const tokens = [];
(function makeGarbage() {
    for (let i = 0; i < TARGETS; ++i) {
        const target = { payload: i, s: "t" + i };
        if ((i % 16) === 7) {
            const token = { t: i };
            tokens.push(token);
            registry.register(target, i, token);
        } else
            registry.register(target, i);
    }
    for (const token of tokens)
        registry.unregister(token);
})();

// Spawned conductors: each forces full synchronous collections from its own
// thread, so finalizeUnconditionally + the DWT enqueue run on a SPAWNED
// conductor inside the shared stop. Allocation churn between rounds keeps
// the conducted cycles doing real sweeping work, and the staggered start
// makes different threads win the §10.2 election across rounds.
const conductors = spawnN(CONDUCTORS, which => {
    for (let r = 0; r < GCS_PER_CONDUCTOR; ++r) {
        let churn = [];
        for (let i = 0; i < 2000; ++i)
            churn.push({ w: which, r, i });
        churn = null;
        gc();
    }
    return which;
});
joinAll(conductors);

// One more main-thread full GC: any target the spawned-conductor cycles
// missed (e.g. pinned by a conductor's own conservative roots) is collected
// here; the cleanup tasks then drain on the shell's run loop after the
// script ends (asyncTestStart keeps the process alive until oracle 3).
gc();
gc();
