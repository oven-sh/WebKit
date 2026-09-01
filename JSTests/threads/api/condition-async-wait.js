//@ requireOptions("--useJSThreads=1")
// API-I12: asyncWait promises settle on a run-loop turn, never synchronously
// inside the registering call (and notify() never settles them inline).
// Covers both 4.3 consumption modes:
//   (a) sync-held: asyncWait inside hold(fn) consumes the hold and releases
//       the lock now; the hold epilogue must skip its release (no
//       double-unlock).
//   (b) async-held: asyncWait consumes the live asyncHold ticket; the
//       outstanding release function then throws the 4.2 Error.
load("../harness.js", "caller relative");

asyncTestStart(5);

const lock = new Lock();
const cond = new Condition();

// ---- (a) sync-held consumption ----
{
    let p;
    lock.hold(() => {
        p = cond.asyncWait(lock);
        shouldBeTrue(p instanceof Promise);
        shouldBeFalse(lock.locked, "asyncWait releases the lock immediately (4.3)");
    });
    shouldBeFalse(lock.locked, "hold epilogue must skip the consumed release (no double-unlock)");

    let settled = false;
    p.then(() => { settled = true; });
    shouldBe(cond.notify(), 1, "the async waiter is enqueued and countable");
    shouldBeFalse(settled, "I12: notify must not settle the promise synchronously");
    // After notify the ticket re-queues for the lock (5.5a A): the GRANT may
    // be immediate (lock free), but resolution still waits for a RL turn.
    p.then(release => {
        shouldBe(typeof release, "function", "resolves with a fresh release fn (no-fn contract)");
        shouldBeTrue(lock.locked, "lock reacquired before resolution");
        release();
        shouldBeFalse(lock.locked);
        asyncTestPassed();
    });
}

// ---- (b) async-held consumption + consumed-release Error ----
(async () => {
    const release = await lock.asyncHold();
    shouldBeTrue(lock.locked);
    const w = cond.asyncWait(lock); // consumes the async hold (4.3(b), unvalidated)
    shouldBeFalse(lock.locked, "async-held lock released by asyncWait");
    shouldThrow(Error, () => release(),
        "Lock release function called more than once");

    let settled = false;
    w.then(() => { settled = true; });
    shouldBe(cond.notify(), 1);
    shouldBeFalse(settled, "I12 again on the (b) path");

    const release2 = await w;
    shouldBeTrue(lock.locked);
    release2();
    shouldBeFalse(lock.locked);
    asyncTestPassed();
})();

// ---- (b) tightening: a granted-but-UNDELIVERED asyncHold is not "held"
// (4.3(b) means a DELIVERED grant — see ConditionObject.cpp / D6 in
// docs/threads/INTEGRATE-api.md). Consuming the pending grant would unlock
// the lock under the not-yet-run held fn (mutual-exclusion hole). ----
{
    const l = new Lock();
    const c = new Condition();
    let fnRan = false;
    // Immediate grant: the lock's async holder is installed synchronously,
    // but the settle task that RUNS fn only executes on a later RL turn.
    const p = l.asyncHold(() => {
        fnRan = true;
        shouldBeTrue(l.locked, "held fn must run with the lock genuinely held");
        return "fn-done";
    });
    shouldBeTrue(l.locked, "immediate grant holds the lock");
    shouldBeFalse(fnRan, "I12: grant not yet delivered");
    shouldThrow(TypeError, () => c.asyncWait(l),
        "Condition.prototype.asyncWait requires the lock to be held");
    shouldBeTrue(l.locked, "rejected asyncWait must not release the lock");
    p.then(v => {
        shouldBe(v, "fn-done");
        shouldBeTrue(fnRan, "fn still ran exactly as granted");
        shouldBeFalse(l.locked, "implicit post-fn release (E) intact");
        asyncTestPassed();
    });
}

// ---- (b) tightening, round 4 (D12): a live, DELIVERED with-fn grant is
// "held" only for the thread running fn. A FOREIGN thread's asyncWait during
// fn (here: while fn is parked in the harness's property-Atomics.wait, which
// releases the GIL) must throw the 4.3 TypeError and must NOT consume the
// grant — consuming it would unlock the lock mid-critical-section (I6). The
// same-thread (b) consumption from inside fn (I23) stays legal and is
// covered by lock-async-hold.js test 8. ----
{
    const l = new Lock();
    const c = new Condition();
    const box = { foreignDone: 0, foreignResult: "" };
    let t = null;
    const p = l.asyncHold(() => {
        // fn is now live: the grant is delivered, this thread is the runner.
        t = new Thread(() => {
            let r;
            try {
                c.asyncWait(l);
                r = "did not throw";
            } catch (e) {
                if (!(e instanceof TypeError))
                    r = "wrong error: " + e;
                else if (!l.locked)
                    r = "TypeError thrown but the lock was released";
                else
                    r = "ok";
            }
            box.foreignResult = r;
            box.foreignDone = 1;
        });
        // Park INSIDE fn so the foreign thread runs while the grant is live.
        waitUntil(() => box.foreignDone === 1);
        shouldBeTrue(l.locked, "lock still held by the live with-fn grant after the foreign asyncWait attempt");
        return "fn-end";
    });
    p.then(v => {
        shouldBe(v, "fn-end");
        t.join();
        shouldBe(box.foreignResult, "ok",
            "foreign cond.asyncWait during a live with-fn grant must TypeError without consuming the hold; got: " + box.foreignResult);
        shouldBeFalse(l.locked, "implicit post-fn release (E) intact after the rejected foreign consumption");
        asyncTestPassed();
    });
}

// ---- asyncWait waiters and sync waiters share one FIFO notify domain
// (4.3: notify wakes sync+async uniformly) ----
{
    const box = { waiting: 0, go: 0 };
    const t = new Thread(() => lock.hold(() => {
        box.waiting = 1;
        while (!box.go)
            cond.wait(lock);
        return "sync-woken";
    }));
    waitUntil(() => box.waiting === 1);

    let asyncWaitPromise;
    lock.hold(() => { asyncWaitPromise = cond.asyncWait(lock); });

    // Two waiters (one sync, one async): notifyAll reports both.
    let woken = 0;
    lock.hold(() => {
        box.go = 1;
        woken = cond.notifyAll();
    });
    // The async waiter can never wake spuriously, so it is always counted;
    // the sync waiter is counted unless a spurious wakeup transiently
    // dequeued it (it re-blocks on the predicate and is re-delivered when it
    // re-enqueues — delivery is asserted by the join below).
    shouldBeTrue(woken === 1 || woken === 2, "notifyAll counts sync and async waiters uniformly, got " + woken);
    shouldBe(t.join(), "sync-woken");
    asyncWaitPromise.then(release => {
        release();
        shouldBeFalse(lock.locked);
        asyncTestPassed();
    });
}
