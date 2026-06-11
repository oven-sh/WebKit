//@ requireOptions("--useJSThreads=1")
// API-I22: property Atomics.waitAsync with a finite timeout on a spawned
// thread, never notified, settles "timed-out" via the 5.6 run-loop timer
// (armed with vm.runLoop().dispatchAfter, G28 — never the calling thread's
// current run loop); the parent awaits the result. The ticket keeps the
// shell alive until the timer settles it (I20 liveness).
load("../harness.js", "caller relative");

asyncTestStart(3);

const o = { k1: 0, k2: 0, k3: 0, k4: 0 }; // one key per section: waiters must not cross-interleave (I11)

// ---- spawned thread arms a 100ms waitAsync, finishes immediately; the
// timer must still settle the dead registrant's ticket (4.6.2) ----
{
    const t = new Thread(() => {
        const r = Atomics.waitAsync(o, "k1", 0, 100);
        if (r.async !== true)
            throw new Error("expected async:true, got " + r.async);
        if (!(r.value instanceof Promise))
            throw new Error("expected a Promise value");
        return r.value;
    });
    t.asyncJoin().then(p => p).then(v => {
        shouldBe(v, "timed-out");
        asyncTestPassed();
    });
}

// ---- immediate (non-blocking) forms on a spawned thread: TA result shape ----
{
    const t2 = new Thread(() => {
        const ne = Atomics.waitAsync(o, "k2", 999);       // value mismatch
        const zt = Atomics.waitAsync(o, "k2", 0, 0);      // zero timeout
        const neg = Atomics.waitAsync(o, "k2", 0, -5);    // negative clamps to 0
        return [ne.async, ne.value, zt.async, zt.value, neg.async, neg.value].join("|");
    });
    shouldBe(t2.join(), "false|not-equal|false|timed-out|false|timed-out");
}

// ---- main-thread waitAsync with finite timeout also times out (the timer
// is armed on the registering VM's run loop, which the shell drains) ----
{
    const m = Atomics.waitAsync(o, "k3", 0, 50);
    shouldBe(m.async, true);
    let settled = false;
    m.value.then(() => { settled = true; });
    shouldBeFalse(settled, "I12 discipline: never settles synchronously");
    m.value.then(v => {
        shouldBe(v, "timed-out");
        asyncTestPassed();
    });
}

// ---- a notified waitAsync settles "ok", not "timed-out", even with a
// generous timeout racing the notify ----
{
    const w = Atomics.waitAsync(o, "k4", 0, 60000);
    shouldBe(w.async, true);
    shouldBe(Atomics.notify(o, "k4"), 1, "async property waiters are countable");
    w.value.then(v => {
        shouldBe(v, "ok");
        asyncTestPassed();
    });
}
