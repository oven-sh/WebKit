//@ skip
//@ requireOptions("--useJSThreads=1")
// FIXME: N waiters parked on one Condition (each with its OWN Lock, to
// sidestep the shared-lock m_lock scramble described in
// condition-notify-all-shared-lock.js), released by a coordinator thread.
// This still cannot run reliably under the phase-1 GIL stub (~1 in 8 runs
// hangs even after the depth-free notify handoff + park-site
// topCallFrame/topEntryFrame save/restore landed and unskipped the
// shared-lock variant): the coordinator below busy-waits in plain JS
// (waitAllParked) without calling notify, so no handoff point is reached.
// Unskip when the stub gets:
//
//   1. Preemption / fair GIL handoff. Today a thread that busy-waits in JS
//      (e.g. the coordinator's waitAllParked loop below) holds the GIL
//      forever; the waiter threads never even start, so the coordinator's
//      condition can never become true (observed: coordinator on-CPU in the
//      locked-getter spin, all four waiters parked in WTF::Lock::lockSlow on
//      the JSLock they need to begin execution).
//   2. Non-LIFO JSLock::grabAllLocks unwinding. The alternative to busy-
//      waiting is sleeping in a timed Atomics.wait, but a thread that wakes
//      from a timed park while a LATER (deeper) dropper is still parked
//      spins in grabAllLocks until that deeper dropper unwinds
//      (JSLock.cpp:371). If the deeper dropper is a cond.wait waiter that
//      only this thread would notify, that is a deadlock.
//
// With both constraints there is no sound way for any thread to (a) learn
// that another thread is parked and then (b) wake it — except the special
// case where the waiter is the main thread (covered, live, in
// condition-notify-all.js and condition-wait-notify.js).
load("../resources/assert.js", "caller relative");

const WAITERS = 4;

function makeParkedWaiters(cond, onResume) {
    const locks = [];
    for (let i = 0; i < WAITERS; ++i)
        locks.push(new Lock());
    const registered = { 0: 0, 1: 0, 2: 0, 3: 0 };
    const threads = spawnN(WAITERS, index => {
        locks[index].hold(() => {
            Atomics.store(registered, index, 1);
            cond.wait(locks[index]);
        });
        if (onResume)
            onResume(index);
        return index;
    });
    return { locks, registered, threads };
}

function waitAllParked(locks, registered) {
    // registered[i] is set while waiter i holds its lock, and cond.wait
    // enqueues before releasing the lock, so registered[i] && !locks[i].locked
    // proves waiter i is in the condition's queue.
    for (let i = 0; i < WAITERS; ++i) {
        while (Atomics.load(registered, i) === 0 || locks[i].locked) { }
    }
}

// ---- Phase 1: a single notifyAll wakes every parked waiter ----
{
    const cond = new Condition();
    const box = { resumed: 0 };
    const { locks, registered, threads } =
        makeParkedWaiters(cond, () => { Atomics.add(box, "resumed", 1); });

    const coordinator = new Thread(() => {
        waitAllParked(locks, registered);
        return cond.notifyAll();
    });

    const results = joinAll(threads);
    for (let i = 0; i < WAITERS; ++i)
        shouldBe(results[i], i);
    shouldBe(coordinator.join(), WAITERS, "one notifyAll woke every parked waiter");
    shouldBe(box.resumed, WAITERS);
    shouldBe(cond.notifyAll(), 0, "no waiters remain");
    for (const lk of locks)
        shouldBeFalse(lk.locked);
}

// ---- Phase 2: notify wakes exactly one of many parked waiters ----
{
    const cond = new Condition();
    const { locks, registered, threads } = makeParkedWaiters(cond, null);

    const coordinator = new Thread(() => {
        waitAllParked(locks, registered);
        const perCall = [];
        for (let k = 0; k < WAITERS; ++k)
            perCall.push(cond.notify());
        perCall.push(cond.notify()); // queue is empty now
        return perCall;
    });

    joinAll(threads);
    const perCall = coordinator.join();
    shouldBe(perCall.length, WAITERS + 1);
    for (let k = 0; k < WAITERS; ++k)
        shouldBe(perCall[k], 1, "notify wakes exactly one parked waiter");
    shouldBe(perCall[WAITERS], 0, "extra notify with an empty queue wakes nobody");
}
