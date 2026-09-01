//@ requireOptions("--useJSThreads=1", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1")
// MC-GC S5 (docs/threads/cve/map-MC-GC.md): the SPEC-api 5.10 addFinalizer
// lambda (registerThreadStateFinalizer, ThreadObject.cpp:117-146) is the
// engine's own native finalizer — the .NET "finalization during method"
// analog surface. It runs when a dead JSThread shell is finalized by a
// conducted shared collection; it clears the ThreadState's Strongs (HandleSet
// mutation under m_strongLock), takes ThreadState::joinLock, and drains
// never-settled asyncJoin tickets. UNGIL-HANDOUT (the §LK HandleSet ruling,
// carve-out (b)) requires these lambdas to run entered-with-access OUTSIDE
// the stop window; the landed tree still runs them inside WeakBlock::sweep
// during the conducted cycle (map S5 records the divergence).
//
// This storm makes dead thread shells + their finalizer lambdas race
// conducted collections from MULTIPLE conductors, while live asyncJoin
// settles race the same ThreadState fields the finalizer touches.
//
// Susceptibility oracles:
//   1. Every asyncJoin of a COMPLETED thread settles with the thread's exact
//      result (the finalizer's Strong clears must never eat a settle that
//      was already swapped out of asyncJoiners — exactly-once handoff).
//   2. No crash/deadlock: the finalizer's joinLock/m_strongLock acquisitions
//      inside (or after) the stop must never deadlock against a parked
//      mutator or a settling carrier (heap I6: parked threads hold no such
//      locks).
//   3. The process exits cleanly with all asyncTestPassed() fired (a lost
//      settle = unfulfilled asyncTestStart = non-zero exit).
//
// EXECUTED POST-UNGIL ONLY (do not run against the mid-bring-up tree).
// Amplifier-ready: the interesting windows are the 5.10-lambda vs settle
// vs conducted-sweep interleavings; RaceAmplifier stall points already sit
// on the detach/exit paths (ThreadManager.cpp EXIT1.8).
load("../harness.js", "caller relative");

const WAVES = 8;
const PER_WAVE = 6;     // threads per wave whose shells are dropped unjoined
const JOINED_PER_WAVE = 4; // threads per wave watched via asyncJoin

asyncTestStart(WAVES * JOINED_PER_WAVE);

for (let wave = 0; wave < WAVES; ++wave) {
    // Abandoned shells: complete quickly, never joined, references dropped
    // at the end of this iteration. Their JSThread cells die at the next
    // conducted full collection -> 5.10 finalizer lambda runs there
    // (clearing jsThread/threadLocals/result Strongs).
    for (let i = 0; i < PER_WAVE; ++i)
        new Thread((w, i) => ({ w, i, junk: "abandoned-" + w + "-" + i }), wave, i);

    // Watched threads: their asyncJoin settles must survive the finalizer
    // storm with exact results (oracle 1).
    for (let j = 0; j < JOINED_PER_WAVE; ++j) {
        const expected = "result-" + wave + "-" + j;
        const t = new Thread((w, j) => {
            // Touch shared state + allocate so completion interleaves with
            // the conducted cycles.
            const local = [];
            for (let k = 0; k < 500; ++k)
                local.push({ k, s: "x" + k });
            return "result-" + w + "-" + j;
        }, wave, j);
        t.asyncJoin().then(value => {
            if (value !== expected)
                throw new Error("asyncJoin settled with wrong/corrupt result: "
                    + String(value) + " (expected " + expected + ")");
            asyncTestPassed();
        }, error => {
            throw new Error("asyncJoin unexpectedly rejected: " + String(error));
        });
    }

    // Conducted full collection from a spawned conductor: finalizes the
    // previous wave's abandoned shells inside a shared stop while this
    // wave's joins/settles are in flight.
    const conductor = new Thread(() => { gc(); return 1; });
    shouldBe(conductor.join(), 1);
}

// Final main-thread collections: finalize the last wave's shells; the
// settle tasks drain on the shell run loop after script end.
gc();
gc();
