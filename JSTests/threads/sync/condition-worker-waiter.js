//@ skip
//@ requireOptions("--useJSThreads=1")
// FIXME: a WORKER thread parked in cond.wait, woken by another worker — the
// canonical producer/consumer mailbox. Deadlocks/starves under the phase-1
// GIL stub; unskip when the stub gains preemption (fair GIL handoff) or a
// non-LIFO JSLock::grabAllLocks.
//
// Why no ordering of this test is sound today:
// - The producer cannot busy-wait for the consumer to park: with no
//   preemption, if the producer wins the GIL first its spin starves the
//   consumer forever (the consumer never begins executing).
// - The producer cannot poll-sleep in a timed Atomics.wait either: if the
//   consumer's cond.wait parks while the producer is sleeping, the producer
//   wakes as a non-deepest GIL dropper and spins in JSLock::grabAllLocks
//   (LIFO unwind, JSLock.cpp:371) until the consumer unwinds — but the
//   consumer can only be woken by this producer. Deadlock.
// - The producer cannot just hold the lock and notify blindly: if its hold
//   contends with the consumer's pre-park hold, the producer's GIL drop
//   lands shallower than the consumer's cond.wait drop, and its wakeup
//   (when the consumer releases the lock inside wait) hits the same LIFO
//   spin with the same circular dependency.
//
// Main-thread-waiter variants of all these semantics run live in
// condition-wait-notify.js.
load("../resources/assert.js", "caller relative");

const lk = new Lock();
const cv = new Condition();
const box = { payload: null, registered: 0 };

const consumer = new Thread(() => {
    let got;
    lk.hold(() => {
        Atomics.store(box, "registered", 1);
        while (box.payload === null)
            cv.wait(lk);
        shouldBeTrue(lk.locked, "wait reacquires the lock before returning");
        got = box.payload;
    });
    return got;
});

const producer = new Thread(() => {
    // The consumer always parks: the payload only appears afterwards.
    while (Atomics.load(box, "registered") === 0 || lk.locked) { }
    lk.hold(() => { box.payload = "package"; });
    return cv.notify();
});

shouldBe(consumer.join(), "package");
shouldBe(producer.join(), 1, "the parked consumer was woken by this notify");
shouldBeFalse(lk.locked);
