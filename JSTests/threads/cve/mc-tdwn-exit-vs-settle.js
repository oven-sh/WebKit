//@ requireOptions("--useJSThreads=1")
// MC-TDWN S2/S3/S9 (docs/threads/cve/map-MC-TDWN.md): registrant teardown
// vs in-flight settlement. A spawned thread registers async work
// (lock.asyncHold with-fn, finite-timeout property Atomics.waitAsync,
// asyncJoin) and exits IMMEDIATELY, so its E2A close (GIL-off) races the
// cross-thread settle targeting its inbox:
//   - settle observes inbox open  => ThreadTask enqueued into a queue the
//     close block is about to harvest (residue must route to main, §E.4
//     dead=>main; nothing lost, nothing run twice);
//   - settle observes inbox closed => main fallback directly.
// Under the phase-1 GIL (and pre-U-T9-INT1, where keepalive is never
// armed) every settle takes the landed DWT path; the observables below
// must hold IDENTICALLY in both regimes:
//   1. no crash / no assert (UAF of freed per-thread state),
//   2. each asyncHold with-fn runs EXACTLY once (lock mutual exclusion
//      survives grant-to-dead-registrant) and the lock ends unlocked,
//   3. each waitAsync settles exactly once with "ok" or "timed-out"
//      (notify racing registrant exit racing the local timeout),
//   4. asyncJoin promises settle with the dead thread's result.
// Amplifier-ready: the EXIT1.8 / E2A stall points widen the
// open-vs-closed window; iteration count is the knob.
load("../harness.js", "caller relative");

const ITER = 24;

asyncTestStart(ITER * 2 + 1); // waitAsync settle + asyncJoin settle per iteration, + the final grant/lock check

const shared = { grants: 0 };
for (let i = 0; i < ITER; ++i)
    shared["k" + i] = 0; // pre-created wait lanes (one per iteration, no cross-interleaving, I11)
const lock = new Lock();

let grantPromises = [];

for (let i = 0; i < ITER; ++i) {
    // Main holds the lock so the dying thread's asyncHold is PENDING at
    // registration; main releases right after the spawn, racing the
    // thread's exit. The grant must be delivered (with-fn runs, then
    // auto-release) no matter which side of the close it lands on.
    let spawned;
    lock.hold(() => {
        spawned = new Thread((lk, sh, key) => {
            // Pending lock grant: with-fn must run exactly once, on
            // whichever thread drains the (re-routed) settle.
            const grantP = lk.asyncHold(() => { Atomics.add(sh, "grants", 1); });
            // Finite-timeout property waitAsync racing main's notify AND
            // this thread's exit (close harvest settles "timed-out" if
            // neither won; exactly one value either way).
            const w = Atomics.waitAsync(sh, key, 0, 50);
            if (w.async !== true)
                throw new Error("expected async waitAsync, got " + w.async);
            // Return both promises to the joiner; exit immediately — the
            // inbox close races every settle registered above.
            return [grantP, w.value];
        }, lock, shared, "k" + i);
        // Stay inside hold() a beat so registration vs release interleaves
        // differently across iterations (cooperative GIL: the spawned
        // thread runs while we park).
        if (i & 1)
            sleepMs(1);
    });
    // Racing edges, alternating order across iterations:
    if (i & 2)
        Atomics.notify(shared, "k" + i);

    spawned.asyncJoin().then(pair => {
        shouldBeTrue(pair[0] instanceof Promise, "grant promise crossed join");
        shouldBeTrue(pair[1] instanceof Promise, "wait promise crossed join");
        grantPromises.push(pair[0]);
        pair[1].then(v => {
            shouldBeTrue(v === "ok" || v === "timed-out",
                "waitAsync settled exactly once with a real value, got " + describe(v));
            asyncTestPassed();
        });
        asyncTestPassed();
    });

    if (!(i & 2))
        Atomics.notify(shared, "k" + i);
}

// All grants must eventually be delivered exactly once and the lock must
// end free: with-fn auto-releases, so a lost/duplicated grant shows up as
// grants !== ITER or a forever-locked lock.
//
// Grant settles run on run-loop turns, which the GIL-phase shell pumps
// only AFTER the main script ends — so the final check must itself live
// on run-loop turns (a main-script waitUntil would deadlock). Re-arm via
// short finite-timeout waitAsync ticks, bounded.
let finalTicks = 0;
function finalCheck() {
    const grants = Atomics.load(shared, "grants");
    if (grants === ITER && !lock.locked) {
        // Exactly once each, lock free, and mutual exclusion still intact.
        shouldBeTrue(grants <= ITER, "no duplicated grant");
        let reacquired = false;
        lock.hold(() => { reacquired = true; });
        shouldBeTrue(reacquired, "lock reusable after the dead-registrant storm");
        asyncTestPassed();
        return;
    }
    if (++finalTicks > 1200) // ~30s of 25ms ticks
        throw new Error("grants=" + grants + "/" + ITER + " locked=" + lock.locked
            + ": lost or stuck dead-registrant settlement");
    Atomics.waitAsync(shared, "finalLane", 0, 25).value.then(finalCheck);
}
shared.finalLane = 0;
finalCheck();
