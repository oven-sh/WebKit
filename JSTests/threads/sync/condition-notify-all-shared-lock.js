//@ requireOptions("--useJSThreads=1")
// The canonical Condition usage: N waiters share ONE Lock and a single
// notifyAll releases them all.
//
// This deadlocked under the original phase-1 GIL stub wake path; it now
// runs because:
//   - conditionProtoFuncWait no longer reacquires the Lock's internal
//     m_lock inside the JSLock::DropAllLocks scope (holding it across
//     grabAllLocks' strict-LIFO unwind spin deadlocked against the other
//     woken waiters of the same Lock); the Lock is reacquired GIL-first
//     with a tryLock + depth-free GIL handoff loop.
//   - notifyAll performs an unconditional depth-free GIL handoff
//     (jsThreadGILHandoffYield) so a notifier looping in JS (the
//     coordinator's done-counter sweep below) cannot starve the waiters
//     it just woke.
//   - Park sites save/restore vm.topCallFrame/vm.topEntryFrame
//     (GILParkSavedExecutionState), which non-LIFO GIL handoffs require.
load("../resources/assert.js", "caller relative");

const WAITERS = 4;

const lock = new Lock();
const cond = new Condition();
const box = { go: 0, woken: 0, done: 0 };

const waiters = spawnN(WAITERS, index => {
    lock.hold(() => {
        while (!box.go)
            cond.wait(lock);
        // The lock is reacquired: this increment is protected.
        box.woken++;
    });
    Atomics.add(box, "done", 1);
    return index;
});

const coordinator = new Thread(() => {
    // Flip the predicate first so any waiter that has not parked yet skips
    // the wait entirely; then sweep until every waiter has finished.
    lock.hold(() => { box.go = 1; });
    let total = 0;
    while (Atomics.load(box, "done") < WAITERS)
        total += cond.notifyAll();
    return total;
});

const results = joinAll(waiters);
for (let i = 0; i < WAITERS; ++i)
    shouldBe(results[i], i);
shouldBeTrue(coordinator.join() <= WAITERS);
shouldBe(box.woken, WAITERS, "every waiter ran its post-wait code");
shouldBeFalse(lock.locked);
shouldBe(cond.notifyAll(), 0, "no waiters remain");
