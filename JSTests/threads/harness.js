// JSTests/threads/harness.js — SPEC-api §8 harness for the threads corpus.
//
// The spec-named helpers (shouldBe, shouldThrow(type, fn), spawnN(n, fn),
// withTimeout(ms, fn)) plus shouldBeTrue/shouldBeFalse/shouldNotThrow/joinAll
// live in resources/assert.js; this file is the §8 entry point. Tests load it
// with:
//   load("../harness.js", "caller relative");
load("./resources/assert.js", "caller relative");

// Sleeps the calling thread for about ms milliseconds.
//
// Flag-on (Thread API present): uses the PROPERTY-path Atomics.wait on a
// harness-private plain object. That path parks with the GIL DROPPED
// (GILDroppedSection in ThreadAtomics.cpp's atomicsWaitOnProperty), so
// spawned Threads run while the sleeper is parked — this is what makes
// waitUntil() a working rendezvous under the cooperative phase-1 GIL. The
// typed-array Atomics.wait path must NOT be used here: WaiterListManager's
// sync wait parks while still holding the shared VM's JSLock, so a
// main-thread TA-lane sleep would starve every spawned Thread for the full
// duration (and a waitUntil() built on it would deadlock until its 30s
// deadline). Isolation from property-waiter tests is by construction:
// PropertyWaiterTable waiters are keyed by (cell, uid) and the lane object
// below is harness-private, so harness sleeps can never alias any test's
// waiter list or perturb its notify counts.
//
// Flag-off fallback (no Thread global => no property path): a private
// SharedArrayBuffer lane. There are no spawned Threads to starve in that
// configuration, so holding the GIL while sleeping is harmless.
//
// The jsc shell's main thread may block unless --can-block-is-false;
// blocking-gate.js must not use this.
const __harnessSleepLane = (typeof Thread === "function")
    ? { v: 0 }
    : ((typeof SharedArrayBuffer === "function") ? new Int32Array(new SharedArrayBuffer(4)) : null);

function sleepMs(ms) {
    if (__harnessSleepLane === null)
        throw new Error("sleepMs requires Thread or SharedArrayBuffer");
    // Nothing ever notifies the lane; the wait always returns "timed-out"
    // after ~ms. The property form drops the GIL while parked (see above).
    if (typeof Thread === "function")
        Atomics.wait(__harnessSleepLane, "v", 0, ms);
    else
        Atomics.wait(__harnessSleepLane, 0, 0, ms);
}

// Cooperative-GIL polling rendezvous: sleeps in bounded steps until cond()
// is true. Every step releases the GIL so spawned Threads can run (the
// phase-1 GIL is cooperative-only, SPEC-api 5.2/Dev 9 — a spinning loop
// would never yield; sleepMs's property-path park is the GIL-dropping step).
// Throws after maxMs (annex T2: race tests bound their blocking operations;
// a stuck rendezvous fails loudly instead of hanging).
function waitUntil(cond, maxMs, stepMs) {
    maxMs = maxMs === undefined ? 30000 : maxMs;
    stepMs = stepMs === undefined ? 5 : stepMs;
    const deadline = Date.now() + maxMs;
    while (!cond()) {
        if (Date.now() > deadline)
            throw new Error("waitUntil: condition not reached within " + maxMs + "ms");
        sleepMs(stepMs);
    }
}
