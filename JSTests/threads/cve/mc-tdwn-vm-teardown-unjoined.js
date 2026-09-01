//@ requireOptions("--useJSThreads=1")
// MC-TDWN S1 (docs/threads/cve/map-MC-TDWN.md): VM/shell teardown vs
// in-flight spawned-thread exit tails (the CVE-2020-12387 shape: host
// shutdown races a worker's own shutdown sequence).
//
// Main spawns a wave of UNJOINED threads at staggered points in their
// lifecycle — some still parked, some mid-fn, some already inside the T5
// teardown tail (access release -> TEARDOWN mark -> client destroy ->
// unregister) — then ends the script immediately. Shell/VM teardown must:
//   - park at the EXIT1.9 fence until every spawned lite's server-touching
//     tail completed (no server-Heap UAF from a mid-tail `delete client`),
//   - survive the residual tail (lite free / M12 queue removal) via the
//     spawn-time Ref<VM>,
//   - tolerate the last VM deref landing on whichever thread finishes
//     last (the S1 "suspected" placement: if the spawned thread's lambda
//     Ref is the final reference, ~VM runs there).
//
// PASS = clean exit, exit code 0, no assert/crash/TSAN report. There is
// deliberately no join and no end-of-script synchronization: the race IS
// the test. Amplifier-ready: the EXIT1.8 stall points
// (post-release / post-mark / pre-destroy / post-destroy) widen every
// window; WAVE is the knob.
load("../harness.js", "caller relative");

const WAVE = 16;

const shared = { exits: 0, never: 0 };
const lock = new Lock();

for (let i = 0; i < WAVE; ++i) {
    new Thread((sh, lk, delay, mode) => {
        if (mode === 0) {
            // Exit instantly: tail likely concurrent with main's exit.
        } else if (mode === 1) {
            // Exit after a short park: tail lands while teardown is
            // already fencing.
            Atomics.wait(sh, "never", 0, delay);
        } else if (mode === 2) {
            // Leave a pending async registration behind (close residue /
            // main-fallback routing during teardown).
            lk.asyncHold(() => {});
            Atomics.waitAsync(sh, "never", 0, 10 + delay);
        } else {
            // Allocation burst right up to exit: the freshest possible
            // per-thread GC-client state for the teardown tail to detach.
            let junk = [];
            for (let j = 0; j < 1000; ++j)
                junk.push({ j, s: "x" + j });
        }
        Atomics.add(sh, "exits", 1);
    }, shared, lock, i, i & 3);
}

// A couple of threads get a head start so the wave spans the whole
// lifecycle spectrum at script end; the rest race teardown cold.
sleepMs(2);

// No join, no waitUntil: fall off the end while threads are running,
// parked, registering async work, and mid-exit.
