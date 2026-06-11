//@ requireOptions("--useJSThreads=1", "--useSharedArrayBuffer=0")
// MC-SPEC S1/S2 capability WITNESS (docs/threads/cve/map-MC-SPEC.md).
//
// This is not a failure detector for a bug — MC-SPEC is structural. It is
// the audit's witness that --useJSThreads is itself a timing-capability
// grant, independent of --useSharedArrayBuffer:
//
//   (S2, deterministic) With useJSThreads=1 and useSharedArrayBuffer=0 the
//   Thread API is present and the SharedArrayBuffer constructor is ABSENT
//   (OptionsList.h:683 vs :703 are independent gates; JSGlobalObject.cpp:
//   2004 vs 2007). Note the jsc shell force-enables SAB in its defaults
//   (jsc.cpp:4147); the explicit =0 above must win — if SAB shows up here,
//   the gate split regressed or the shell default leaked past runtime flags.
//
//   (S1, witness) Even with SAB absent, a spawned Thread spinning
//   Atomics.add on a plain shared-heap object is a no-permission
//   high-resolution clock: we assert the counter advances between two
//   back-to-back property-atomic loads on the observer thread (i.e. the
//   clock ticks faster than one observer loop iteration), and we REPORT
//   observed ticks per Date.now() millisecond. If a future change coarsens,
//   throttles, or gates the property-atomics fast path, this assertion or
//   the gating shape fails and forces the SPEC-api §4.5 conversation.
//
// WRITTEN DURING BRING-UP: do not execute until the GIL-off ladder is up.
// Under the phase-1 cooperative GIL the spinner may starve the observer;
// post-ungil both run in parallel and the witness is robust.
load("../harness.js", "caller relative");

// --- S2: gating shape (deterministic) ---------------------------------------
shouldBe(typeof Thread, "function");
shouldBe(typeof Lock, "function");
shouldBe(typeof SharedArrayBuffer, "undefined");
shouldBe(typeof Atomics, "object"); // Property-atomics path needs no SAB.

// --- S1: counter-thread clock witness ---------------------------------------
const lane = { c: 0, stop: 0 };

const spinner = new Thread(() => {
    // Free-running counter: the canonical SAB-era timer, rebuilt on plain
    // shared objects. Bounded by the stop flag only.
    while (Atomics.load(lane, "stop") === 0) {
        // Batch increments between stop polls so the clock rate is dominated
        // by the RMW itself, not the poll.
        for (let i = 0; i < 64; ++i)
            Atomics.add(lane, "c", 1);
    }
});

// Wait for the spinner to actually run.
withTimeout(30000, () => {
    while (Atomics.load(lane, "c") === 0) { /* spin */ }
});

// Witness 1: the clock ticks between two back-to-back atomic loads at least
// once in SAMPLES attempts. On any real machine with the spinner running in
// parallel this happens almost every sample; requiring 1/100000 makes the
// assertion robust to scheduling noise while still proving sub-iteration
// resolution.
const SAMPLES = 100000;
let advancingPairs = 0;
for (let i = 0; i < SAMPLES; ++i) {
    const a = Atomics.load(lane, "c");
    const b = Atomics.load(lane, "c");
    if (b !== a)
        ++advancingPairs;
}
shouldBeTrue(advancingPairs > 0);

// Witness 2 (report only — machine-dependent, never asserted): resolution
// relative to Date.now(). This is the number the map file cites: ticks/ms is
// the granularity advantage handed to in-process code by --useJSThreads.
const t0 = Date.now();
const c0 = Atomics.load(lane, "c");
while (Date.now() - t0 < 50) { /* spin */ }
const elapsedMs = Date.now() - t0;
const ticks = Atomics.load(lane, "c") - c0;
print(`MC-SPEC witness: ${ticks} counter ticks in ${elapsedMs}ms ` +
    `(~${Math.round(ticks / elapsedMs)} ticks/ms); ` +
    `${advancingPairs}/${SAMPLES} back-to-back load pairs advanced`);

// The clock must actually have been running across the report window too.
shouldBeTrue(ticks > 0);

Atomics.store(lane, "stop", 1);
spinner.join();
