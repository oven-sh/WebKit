//@ runDefault("--useDollarVM=1")

// VM::addTerminationDeadline() / VM::cancelTermination() / VM::hasPendingTermination(), through
// $vm.callWithTimeLimit(fn, ms): an embedder's wall-clock time limit on one call (node:vm's `timeout`).

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

function nothingLeftBehind(what) {
    shouldBe($vm.hasPendingTermination(), false, `${what}: something is still pending`);
    spin(30); // ...and ordinary code right after runs to completion.
}

// A limit that passes while its call is running cuts the call short, and nothing of it is left behind.
shouldBe(timesOut(() => { for (;;) { } }, 20), true, "endless loop under a 20ms limit");
nothingLeftBehind("after a fired limit");

// A limit that passes while its call is blocked in native code takes effect once it is back running script.
shouldBe(timesOut(() => { sleepSeconds(0.1); for (;;) { } }, 40), true, "blocked past a 40ms limit, then looping");
nothingLeftBehind("after a limit that passed while blocked");

// A limit that passes after its call has returned to native code, with the request not yet acted on (no trap
// check happens between sleepSeconds() returning and the arrow function returning): the call counts as having
// completed, and the unhandled request is withdrawn rather than left for the next loop to run into.
shouldBe(timesOut(() => sleepSeconds(0.05), 10), false, "limit passes with the request never acted on");
nothingLeftBehind("after an unhandled request was withdrawn");

// A call that finishes within its limit returns its value; the limit expiring afterwards terminates nothing.
shouldBe($vm.callWithTimeLimit(() => 42, 20), 42, "quick call");
spin(50);
let ran = 0;
for (let i = 0; i < 20; i++) {
    // Whether or not a 1–3 ms limit is met (a slow build may not), the code after each call is not cut short.
    timesOut(() => i, 1 + (i % 3));
    spin(5);
    ran++;
}
shouldBe(ran, 20, "twenty short-limit calls interleaved with ordinary code");
nothingLeftBehind("after twenty short limits");

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
// - the outer one fires while the inner call (with a generous limit) is running: it is the outer call that times
//   out — the inner call is cut short by a request that is not its own and lets it through;
shouldBe(timesOut(() => $vm.callWithTimeLimit(() => { for (;;) { } }, 5000), 30), true, "outer limit around a generous inner one");
// - both pass before the inner call ends (it is blocked past both): the inner call's error is caught by the outer
//   function, which must nevertheless be stopped by its own, already-passed limit rather than loop forever.
shouldBe(timesOut(() => { try { $vm.callWithTimeLimit(() => { sleepSeconds(0.1); for (;;) { } }, 20); } catch { } for (;;) { } }, 40), true, "both limits passed");
nothingLeftBehind("after nested limits");

// With nothing requested there is nothing to withdraw.
shouldBe($vm.cancelTermination(), false, "nothing to cancel");
