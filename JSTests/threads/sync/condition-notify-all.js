//@ requireOptions("--useJSThreads=1")
// Condition.prototype.notifyAll / notify: wake-count semantics that are
// testable under the phase-1 GIL stub's cooperative scheduler.
//
// The stub has no preemption and unwinds concurrent GIL drops strictly
// LIFO, so scenarios with SEVERAL simultaneously-parked cond.wait callers
// deadlock inside the stub itself. Those canonical scenarios are kept as
// skipped tests with FIXMEs:
//   - condition-notify-all-shared-lock.js   (N waiters, one shared Lock)
//   - condition-notify-all-multi-waiter.js  (N waiters, one Lock each)
// This file covers what is sound today: empty-queue behavior, and
// notify/notifyAll against a single parked waiter (the main thread), where
// wake counts are exact and deterministic.
load("../resources/assert.js", "caller relative");

const cond = new Condition();
const lock = new Lock();

// ---- Empty queue: notifications wake nobody and are not buffered ----
shouldBe(cond.notify(), 0);
shouldBe(cond.notifyAll(), 0);
for (let i = 0; i < 10; ++i)
    cond.notifyAll();
// Despite 10+ prior notifyAll calls, a subsequent waiter still parks: the
// worker below observes main parked (it can only run once main blocks) and
// its own notifyAll reports exactly one dequeued waiter.
{
    const box = { ready: 0, woken: -1 };
    const w = new Thread(() => {
        lock.hold(() => { box.ready = 1; });
        box.woken = cond.notifyAll();
        return "ok";
    });
    lock.hold(() => {
        while (!box.ready)
            cond.wait(lock);
    });
    shouldBe(w.join(), "ok");
    shouldBe(box.woken, 1, "earlier notifications were not buffered; main really parked");
}

// ---- notify vs notifyAll with exactly one parked waiter ----
// Both must report 1, and a second call 0 — sequentially, several times,
// to check the queue fully resets.
for (const useAll of [false, true]) {
    for (let lap = 0; lap < 3; ++lap) {
        const box = { ready: 0, first: -1, second: -1 };
        const w = new Thread(() => {
            lock.hold(() => { box.ready = 1; });
            box.first = useAll ? cond.notifyAll() : cond.notify();
            box.second = useAll ? cond.notifyAll() : cond.notify();
            return "ok";
        });
        lock.hold(() => {
            while (!box.ready)
                cond.wait(lock);
        });
        shouldBe(w.join(), "ok");
        shouldBe(box.first, 1, (useAll ? "notifyAll" : "notify") + " woke the parked waiter (lap " + lap + ")");
        shouldBe(box.second, 0, "queue empty after the wake (lap " + lap + ")");
    }
}

// ---- Wake counts are per-condition, not global ----
{
    const condB = new Condition();
    const box = { ready: 0, onB: -1, onA: -1 };
    const w = new Thread(() => {
        lock.hold(() => { box.ready = 1; });
        box.onB = condB.notifyAll(); // nobody waits on condB
        box.onA = cond.notifyAll();
        return "ok";
    });
    lock.hold(() => {
        while (!box.ready)
            cond.wait(lock);
    });
    shouldBe(w.join(), "ok");
    shouldBe(box.onB, 0);
    shouldBe(box.onA, 1);
}
