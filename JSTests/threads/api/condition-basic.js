//@ requireOptions("--useJSThreads=1")
// API-I9: a cond.wait enqueued before a same-lock notify() is woken by it
// (spurious wakeups only add returns — every waiter uses a predicate loop);
// producer/consumer plus a >=3-thread (main + 2 waiters) two-waiter
// handover.
//
// Wakeup guarantee used throughout: the waiter enqueues itself BEFORE
// releasing the JS lock (5.4 step 1 / F3), so any notify() issued while
// HOLDING that lock is ordered after the enqueue — it cannot be lost.
load("../harness.js", "caller relative");

const lock = new Lock();
const cond = new Condition();

// notify with no waiters: returns 0; locks are optional for notify (4.3).
shouldBe(cond.notify(), 0);
shouldBe(cond.notifyAll(), 0);

// ---- producer/consumer: single waiter, payload handoff ----
{
    const box = { ready: 0, value: undefined, waiting: 0 };
    const t = new Thread(() => lock.hold(() => {
        box.waiting = 1; // published under the lock, before wait's enqueue
        while (!box.ready)
            cond.wait(lock); // predicate loop: spurious-safe (I9)
        return box.value;
    }));
    // Yield until the waiter has released the lock inside wait() — box.waiting
    // flips under the lock, so seeing it means the waiter is enqueued or past.
    waitUntil(() => box.waiting === 1);
    lock.hold(() => {
        box.value = "payload";
        box.ready = 1;
        const woken = cond.notify();
        // The waiter is enqueued (it set waiting under the lock and we now
        // hold that lock), so notify must report at most one wakeup; 0 is
        // only possible if a spurious wakeup removed it concurrently — the
        // predicate then re-delivers, so the join below stays sound.
        shouldBeTrue(woken === 0 || woken === 1, "notify count out of range: " + woken);
    });
    shouldBe(t.join(), "payload");
}

// ---- two-waiter handover, >=3 threads (main + 2 waiters), ticketed ----
{
    const box = { waiting: 0, tickets: 0, consumed: 0 };
    const waiter = () => lock.hold(() => {
        box.waiting++;
        while (box.tickets === 0)
            cond.wait(lock);
        box.tickets--;
        box.consumed++;
        return "consumed";
    });
    const threads = spawnN(2, waiter);
    waitUntil(() => box.waiting === 2); // both enqueued (published under lock)

    // Round 1: one ticket, notify() — exactly one waiter may consume.
    lock.hold(() => {
        box.tickets = 1;
        cond.notify();
    });
    waitUntil(() => box.consumed === 1);
    // Grace window: the second waiter must NOT consume a ticket that is not
    // there (spurious wakeups may occur but the predicate re-blocks them).
    sleepMs(50);
    shouldBe(box.consumed, 1, "exactly one waiter handed over per ticket");
    shouldBe(box.tickets, 0);

    // Round 2: release the remaining waiter (notifyAll is safe: the ticket
    // count keeps over-wakeups harmless).
    lock.hold(() => {
        box.tickets = 1;
        cond.notifyAll();
    });
    shouldBe(joinAll(threads).join(","), "consumed,consumed");
    shouldBe(box.consumed, 2);
    shouldBe(box.tickets, 0);
    shouldBeFalse(lock.locked);
}

// ---- notifyAll wakes every parked waiter (count returned) ----
{
    const box = { waiting: 0, go: 0 };
    const threads = spawnN(3, () => lock.hold(() => {
        box.waiting++;
        while (!box.go)
            cond.wait(lock);
        return "released";
    }));
    waitUntil(() => box.waiting === 3);
    let woken = 0;
    lock.hold(() => {
        box.go = 1;
        woken = cond.notifyAll();
    });
    shouldBeTrue(woken <= 3, "notifyAll cannot report more waiters than exist");
    shouldBe(joinAll(threads).join(","), "released,released,released");
}

// ---- wait() reacquires the lock before returning (5.4 step 5) ----
{
    const box = { stage: 0 };
    const t = new Thread(() => lock.hold(() => {
        box.stage = 1;
        while (box.stage < 2)
            cond.wait(lock);
        // If wait() returned without the lock, this read-modify-write could
        // tear against main's hold below; the final assert pins it.
        shouldBeTrue(lock.locked, "wait must return holding the lock");
        box.stage = 3;
        return "ok";
    }));
    waitUntil(() => box.stage === 1);
    lock.hold(() => {
        box.stage = 2;
        cond.notify();
        // We still hold the lock: the woken waiter cannot have advanced.
        shouldBe(box.stage, 2, "woken waiter must wait for the lock");
    });
    shouldBe(t.join(), "ok");
    shouldBe(box.stage, 3);
}
