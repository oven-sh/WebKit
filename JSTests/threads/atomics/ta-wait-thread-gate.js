//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// API-I21: under the GIL, the 4.5-1a carve-out makes sync Atomics.wait on a
// typed-array view from a spawned Thread throw TypeError ("Atomics.wait
// cannot be called from the current thread.") BEFORE the body runs: no park,
// no side effects, even for a value mismatch or zero timeout. Main-thread TA
// waits, TA waitAsync and TA notify from any thread are unchanged (I1).
// Property waits from spawned threads are NOT gated (only G11 gates their
// block). GIL-off the gate is lifted (ta-sync-wait-spawned-gil-off.js covers
// that shape), so this test only checks the gate when the GIL is on.
load("../harness.js", "caller relative");

const gilOn = typeof $vm === "undefined" || $vm.useThreadGIL();

asyncTestStart(1);

const i32 = new Int32Array(new SharedArrayBuffer(16));

// ---- main thread: today's behavior, untouched ----
shouldBe(Atomics.wait(i32, 0, 1), "not-equal");
shouldBe(Atomics.wait(i32, 0, 0, 1), "timed-out");
shouldBe(Atomics.notify(i32, 0), 0);

const t = new Thread(() => {
    const gateMessage = "Atomics.wait cannot be called from the current thread.";

    // The gate fires before the body: even calls that would never block
    // (mismatch, zero timeout) throw, and even invalid-argument calls that
    // today's body would reject differently are pre-empted by the gate.
    if (gilOn) {
        shouldThrow(TypeError, () => Atomics.wait(i32, 0, 1), gateMessage);
        shouldThrow(TypeError, () => Atomics.wait(i32, 0, 0, 0), gateMessage);
        shouldThrow(TypeError, () => Atomics.wait(i32, 0, 0), gateMessage);
        shouldBe(i32[0], 0, "no side effects");
    }

    // TA waitAsync from a spawned thread: unchanged.
    const ne = Atomics.waitAsync(i32, 0, 1);
    shouldBe(ne.async, false);
    shouldBe(ne.value, "not-equal");
    // Lane 1 (lane 0 stays waiter-free so the notify check below sees 0).
    const w = Atomics.waitAsync(i32, 1, 0, 50);
    shouldBe(w.async, true);

    // TA notify from a spawned thread: unchanged (no waiters on lane 0 -> 0).
    shouldBe(Atomics.notify(i32, 0), 0);

    // PROPERTY wait is not subject to 4.5-1a: the non-blocking forms work
    // from a spawned thread (the blocking form is G11-gated, allowed here).
    const o = { k: 0 };
    shouldBe(Atomics.wait(o, "k", 1), "not-equal");
    shouldBe(Atomics.wait(o, "k", 0, 0), "timed-out");

    return w.value; // settles "timed-out" via today's WLM timer
});

t.asyncJoin().then(p => p).then(v => {
    shouldBe(v, "timed-out");
    asyncTestPassed();
});
