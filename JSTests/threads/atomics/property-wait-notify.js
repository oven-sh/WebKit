//@ requireOptions("--useJSThreads=1")
// API-I10: notify(o,k) wakes a parked waiter that observed SVZ(o[k], exp);
// there is no lost store+notify window (F4: the value read and the waiter
// enqueue happen in one JSLock section); ping-pong terminates.
// API-I24 (quantum-wakeup half): the 10ms termination-poll quanta (5.6-4)
// never surface as spurious returns — a timed wait with no notify returns
// "timed-out" and only at/after its deadline; a notified wait returns
// exactly "ok". (The termination half is property-wait-termination.js.)
load("../harness.js", "caller relative");

const o = { flag: 0, ready: 0, done: 0 };

// value mismatch: "not-equal" without blocking (main thread, no park)
shouldBe(Atomics.wait(o, "flag", 12345), "not-equal");
// zero timeout with matching value: immediate "timed-out"
shouldBe(Atomics.wait(o, "flag", 0, 0), "timed-out");

// ---- handshake: waiter observes flag==0, parks; main stores 1 + notifies.
// Cooperative-GIL sequencing (5.2, no preemption assumed): main parks on
// (o,"ready") => GIL drops => the spawned fn runs, publishes ready, then
// parks on (o,"flag") (its own GIL drop is what lets main resume), so when
// main runs again the waiter IS enqueued. The notify loop below makes the
// test also valid post-GIL where that argument no longer holds. ----
{
    const t = new Thread(() => {
        Atomics.store(o, "ready", 1);
        Atomics.notify(o, "ready");
        const r = Atomics.wait(o, "flag", 0); // infinite timeout
        Atomics.store(o, "done", 1);
        return r;
    });
    if (Atomics.load(o, "ready") === 0)
        Atomics.wait(o, "ready", 0); // park until the waiter has started
    Atomics.store(o, "flag", 1);
    let woken = Atomics.notify(o, "flag");
    const deadline = Date.now() + 30000;
    while (woken === 0 && Atomics.load(o, "done") === 0) {
        if (Date.now() > deadline)
            throw new Error("waiter never parked and never finished");
        sleepMs(5);
        woken += Atomics.notify(o, "flag");
    }
    const r = t.join();
    if (woken === 1)
        shouldBe(r, "ok", "a counted notify must produce exactly 'ok'");
    else
        shouldBe(r, "not-equal", "if never parked it must have seen the store");
    shouldBeTrue(woken <= 1);
}

// ---- no lost store+notify window (I10/F4): the waiter re-waits in a
// predicate loop; each main-side store+notify pair must eventually land ----
// ping-pong: strict alternation for ROUNDS rounds; termination is the assert.
{
    const ROUNDS = 50;
    const pp = { turn: 0 };
    const t = new Thread(() => {
        for (let i = 0; i < ROUNDS; ++i) {
            while (Atomics.load(pp, "turn") !== 2 * i + 1)
                Atomics.wait(pp, "turn", 2 * i, 1000); // bounded (annex T2)
            Atomics.store(pp, "turn", 2 * i + 2);
            Atomics.notify(pp, "turn");
        }
        return "pp-done";
    });
    for (let i = 0; i < ROUNDS; ++i) {
        Atomics.store(pp, "turn", 2 * i + 1);
        Atomics.notify(pp, "turn");
        while (Atomics.load(pp, "turn") !== 2 * i + 2)
            Atomics.wait(pp, "turn", 2 * i + 1, 1000);
    }
    shouldBe(t.join(), "pp-done");
    shouldBe(pp.turn, 2 * ROUNDS);
}

// ---- I24 quantum half: a 250ms wait with no notify lives through ~25 poll
// quanta and must return "timed-out", never early, never "ok" ----
{
    o.quiet = 0;
    const t = new Thread(() => {
        const start = Date.now();
        const r = Atomics.wait(o, "quiet", 0, 250);
        return { r, elapsed: Date.now() - start };
    });
    const { r, elapsed } = t.join();
    shouldBe(r, "timed-out");
    shouldBeTrue(elapsed >= 200,
        "quantum wakeups must not surface early (elapsed=" + elapsed + "ms of 250)");
}

// ---- and a notified wait that has already crossed several quanta returns
// exactly "ok" (quantum wakeups never mistranslate a notify) ----
{
    o.slow = 0;
    const t = new Thread(() => Atomics.wait(o, "slow", 0, 10000));
    waitUntil(() => Atomics.notify(o, "slow") === 1, 30000, 25); // park >= a couple quanta
    shouldBe(t.join(), "ok");
}

// notify count semantics: default Infinity, explicit 0 wakes none.
shouldBe(Atomics.notify(o, "flag"), 0); // no waiters left
shouldBe(Atomics.notify(o, "noSuchProp"), 0); // 0 valid even if o lacks k (4.5)
