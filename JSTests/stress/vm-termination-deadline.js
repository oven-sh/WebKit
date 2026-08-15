//@ runDefault("--useDollarVM=1")

// VM::requestTerminationAt() / VM::cancelTerminationRequest(), through $vm.callWithTimeLimit(fn, ms):
// an embedder's scoped, wall-clock time limit (node:vm's `timeout`).

function spin(ms) {
    const start = preciseTime();
    while ((preciseTime() - start) * 1000 < ms) { }
}

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${expected}, got ${actual}`);
}

function timesOut(fn, ms) {
    try {
        $vm.callWithTimeLimit(fn, ms);
    } catch (e) {
        if (e instanceof RangeError && e.message == "timed out")
            return true;
        throw e;
    }
    return false;
}

// A limit that passes while its call is running cuts the call short...
shouldBe(timesOut(() => { for (;;) { } }, 20), true, "endless loop under a 20ms limit");
// ...and nothing of it is left behind: this runs to completion.
spin(50);

// It is wall-clock: time spent blocked counts.
shouldBe(timesOut(() => { $vm.sleepSeconds && $vm.sleepSeconds(1); for (;;) { } }, 20), true, "blocked then looping");

// A call that finishes within its limit returns its value, and the limit expiring afterwards terminates nothing.
shouldBe($vm.callWithTimeLimit(() => 42, 20), 42, "quick call");
spin(50);
let ran = 0;
for (let i = 0; i < 20; i++) {
    // Whether or not a 1–3 ms limit is met (a slow build may not), the code after each call is not cut short.
    timesOut(() => i, 1 + (i % 3));
    spin(5);
    ran++;
}
shouldBe(ran, 20, "twenty short-limit calls followed by ordinary code");

// Anything else the call throws is unaffected.
try {
    $vm.callWithTimeLimit(() => { throw new TypeError("mine"); }, 1000);
    throw new Error("should have thrown");
} catch (e) {
    shouldBe(e instanceof TypeError && e.message, "mine", "ordinary throw under a limit");
}

// Nested limits each keep their own deadline:
// - the inner one fires; the outer call catches that (catchable) error and carries on within its budget;
shouldBe($vm.callWithTimeLimit(() => { const r = timesOut(() => { for (;;) { } }, 20); spin(30); return r; }, 5000), true, "inner limit inside a generous outer one");
// - the outer one fires while the inner call (with a generous limit) is running: it is the outer call that times out;
shouldBe(timesOut(() => $vm.callWithTimeLimit(() => { for (;;) { } }, 5000), 30), true, "outer limit around a generous inner one");
// - both pass before the inner call ends: the inner error is caught by the outer function, which must still be
//   stopped by its own, already-passed limit rather than loop forever.
shouldBe(timesOut(() => { try { $vm.callWithTimeLimit(() => { for (;;) { } }, 60); } catch { } for (;;) { } }, 30), true, "both limits passed");
spin(50);

// cancelTerminationRequest() with nothing requested withdraws nothing.
shouldBe($vm.cancelTerminationRequest(), false, "nothing to cancel");
