//@ requireOptions("--useJSThreads=1")
// API-I21 (GPO; deleted by the post-GIL re-freeze, Dev 12): the 4.5-1a
// carve-out — sync Atomics.wait on a typed-array view from a spawned Thread
// throws TypeError ("Atomics.wait cannot be called from the current
// thread.") BEFORE today's body runs: no park, no side effects, even for a
// value mismatch or zero timeout. Main-thread TA waits, TA waitAsync and TA
// notify from any thread are unchanged (I1). Property waits from spawned
// threads are NOT gated (only G11 gates their block).
load("../harness.js", "caller relative");

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
    shouldThrow(TypeError, () => Atomics.wait(i32, 0, 1), gateMessage);
    shouldThrow(TypeError, () => Atomics.wait(i32, 0, 0, 0), gateMessage);
    shouldThrow(TypeError, () => Atomics.wait(i32, 0, 0), gateMessage);
    shouldBe(i32[0], 0, "no side effects");

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
