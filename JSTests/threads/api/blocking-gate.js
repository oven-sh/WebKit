//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
//@ runDefault("--can-block-is-false")
// API-I18: under --can-block-is-false (G34; per-VM, so under the GIL EVERY
// thread of the shared VM is G11-false) the blocking primitives throw
// TypeError: join(), CONTENDED hold(), cond.wait(), property Atomics.wait.
// Async variants and uncontended hold() succeed — async paths never consult
// G11. GIL-off, a spawned Thread may always block (SPEC-ungil G.1;
// spawned-thread-may-block-gil-off.js), so the spawned-thread section below
// asserts the gate only while the GIL is on.
//
// The runner appends --can-block-is-false (annex T2): Tools/threads/
// run-tests.sh does it for this file, and the //@ runDefault line above
// makes parseRunCommands do it under threads.yaml (9.2-7). This test
// refuses to pass vacuously if the flag is missing.
//
// NOTE: each section uses its own Lock — a no-fn asyncHold GRANTS at
// registration (5.5a A), so its lock stays async-held until the run-loop
// settles the promise; sharing it with later synchronous sections would
// make their uncontended holds contended.
load("../harness.js", "caller relative");

const gilOn = typeof $vm === "undefined" || $vm.useThreadGIL();

// Probe: with --can-block-is-false, typed-array Atomics.wait throws before
// even looking at the value; without it, the mismatched expected value
// returns "not-equal" with no blocking.
{
    let canBlock = true;
    try {
        Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 1);
    } catch {
        canBlock = false;
    }
    if (canBlock)
        throw new Error("blocking-gate.js requires --can-block-is-false (run via Tools/threads/run-tests.sh, which appends it)");
}

asyncTestStart(3);

// ---- uncontended hold: always allowed (4.2 tryLock first) ----
{
    const lock = new Lock();
    shouldBe(lock.hold(() => "held-uncontended"), "held-uncontended");
    shouldBeFalse(lock.locked);
}

// ---- contended hold: TypeError (G11), exact message. The lock is made
// contended without another thread: a no-fn asyncHold grants at
// registration (tryLock success), so m_lock is held when the sync hold
// tries. asyncHold itself is an async path: allowed. ----
{
    const lockA = new Lock();
    (async () => {
        const release = await lockA.asyncHold();
        shouldThrow(TypeError, () => lockA.hold(() => 0),
            "Lock.prototype.hold cannot block the current thread");
        release();
        shouldBeFalse(lockA.locked);
        asyncTestPassed();
    })();
    // Synchronously after registration the lock is already granted-held:
    // the sync hold is ALREADY gated here too.
    shouldBeTrue(lockA.locked);
    shouldThrow(TypeError, () => lockA.hold(() => 0),
        "Lock.prototype.hold cannot block the current thread");
}

// ---- cond.wait: TypeError even while properly holding the lock; the
// failed wait must leave no waiter behind and the hold must still release ----
{
    const lockB = new Lock();
    const condB = new Condition();
    lockB.hold(() => {
        shouldThrow(TypeError, () => condB.wait(lockB),
            "Condition.prototype.wait cannot block the current thread");
    });
    shouldBeFalse(lockB.locked, "hold epilogue still releases after the gated wait");
    shouldBe(condB.notify(), 0, "the gated wait must not have enqueued a waiter");

    // cond.asyncWait succeeds (async paths never consult G11).
    let p;
    lockB.hold(() => { p = condB.asyncWait(lockB); });
    shouldBeFalse(lockB.locked);
    shouldBe(condB.notify(), 1);
    p.then(release => {
        release();
        shouldBeFalse(lockB.locked);
        asyncTestPassed();
    });
}

// ---- property Atomics.wait: TypeError; non-blocking forms + waitAsync OK ----
{
    const o = { k: 0 };
    shouldThrow(TypeError, () => Atomics.wait(o, "k", 0),
        "Atomics.wait cannot be called from the current thread.");
    // The gate guards the BLOCK, not the call: a non-equal value still
    // short-circuits to "not-equal" (4.5 wait semantics, like uncontended
    // hold).
    shouldBe(Atomics.wait(o, "k", 999), "not-equal");
    const r = Atomics.waitAsync(o, "k", 0, 0); // zero timeout: no blocking
    shouldBe(r.async, false);
    shouldBe(r.value, "timed-out");
}

// ---- join(): TypeError on a Running thread; asyncJoin succeeds; the
// spawned thread is G11-false too (per-VM gate) ----
{
    // GIL-off, t could otherwise finish before the main thread's join() check
    // below (join of a finished thread does not block, so it would not throw);
    // it starts its work only once that check is done. Under the GIL t cannot
    // run before the main thread yields, and the flag is set before that.
    const tMayStart = { v: 0 };
    const t = new Thread(() => {
        while (Atomics.load(tMayStart, "v") === 0) { }
        const inner = new Thread(() => 5);
        if (!gilOn) {
            // GIL-off a spawned thread may block: the gate does not apply.
            shouldBe(Atomics.wait({ k: 0 }, "k", 0, 0), "timed-out");
            shouldBe(inner.join(), 5);
            return inner.asyncJoin();
        }
        // Under the GIL, spawned threads share the VM's gate: same TypeErrors here.
        shouldThrow(TypeError, () => Atomics.wait({ k: 0 }, "k", 0),
            "Atomics.wait cannot be called from the current thread.");
        // inner has not run (we hold the GIL): Running => join would block
        // => gated.
        shouldThrow(TypeError, () => inner.join(),
            "Thread.prototype.join cannot block the current thread");
        return inner.asyncJoin(); // await it instead (4.6.3 convention)
    });
    // t has not finished (it waits for tMayStart): Running => gated.
    shouldThrow(TypeError, () => t.join(),
        "Thread.prototype.join cannot block the current thread");
    Atomics.store(tMayStart, "v", 1);
    t.asyncJoin().then(innerPromise => innerPromise).then(v => {
        shouldBe(v, 5);
        // join of a FINISHED thread never blocks: allowed even when
        // G11-false (F5 fast path reads the result without parking).
        shouldBeTrue(t.join() instanceof Promise);
        asyncTestPassed();
    });
}
