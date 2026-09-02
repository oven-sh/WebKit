//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
//@ threadsRequireGILOff
// VM::notifyNeedTermination() raises VM-wide: GIL-off that sets the current
// thread's own trap word as well as the VM word. VM::cancelTermination()
// must withdraw it from both, or the code that runs after a time-limited call
// is terminated by a request that was already withdrawn (node:vm's timeout).

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": expected " + expected + ", got " + actual);
}

function spin(ms) {
    const start = preciseTime();
    while ((preciseTime() - start) * 1000 < ms) { }
}

// The limit passes while the call is blocked in native code, and no trap
// check runs before the call returns: the call completes, and its request is
// withdrawn. If the limit passes before the call starts, the call times out
// instead. Either way nothing may be left pending. If the timer thread does
// not fire at all within the sleep, nothing is checked, and the test passes.
try {
    shouldBe($vm.callWithTimeLimit(() => { sleepSeconds(0.5); return 42; }, 100), 42, "the call completes");
} catch (e) {
    if (!(e instanceof RangeError) || e.message != "timed out")
        throw e;
}
shouldBe($vm.hasPendingTermination(), false, "nothing is pending after the call");
spin(50);
for (let i = 0; i < 100000; ++i) { }
shouldBe($vm.cancelTermination(), false, "nothing left to cancel");
