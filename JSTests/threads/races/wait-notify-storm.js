//@ requireOptions("--useJSThreads=1")
// API-I10 under contention (GI): W waiter threads race R publication rounds
// of one property slot against store+notify storms from main. No wakeup may
// be lost — every waiter must clear every round — and the exact done-count
// proves no waiter skipped or double-counted a round. The two-step notify
// (one, then the rest) stresses FIFO dequeue and partial-wake handover on
// one PropertyWaiterList.
//
// Amplifier target (Tools/threads/amplify.sh). Annex T2: every wait is
// bounded (50ms quanta + monotonic watermark), every thread is joined.
load("../harness.js", "caller relative");

const W = 6;
const R = 200;
const o = { slot: 0 };
const gate = { started: 0 };
const done = { n: 0 };

const waiters = spawnN(W, () => {
    Atomics.add(gate, "started", 1);
    for (let round = 0; round < R; ++round) {
        // wait until the watermark passes `round`; a missed notify costs at
        // most one 50ms quantum, never correctness
        while (Atomics.load(o, "slot") <= round)
            Atomics.wait(o, "slot", round, 50);
        Atomics.add(done, "n", 1);
    }
    return "done";
});

waitUntil(() => Atomics.load(gate, "started") === W);

for (let round = 1; round <= R; ++round) {
    Atomics.store(o, "slot", round);
    Atomics.notify(o, "slot", 1);        // wake one...
    Atomics.notify(o, "slot", Infinity); // ...then the rest (storm shape)
    if (round % 8 === 0)
        sleepMs(1); // let waiters re-park across rounds (storm vs re-enqueue race)
}

shouldBe(joinAll(waiters).join(","), "done,done,done,done,done,done");
shouldBe(Atomics.load(done, "n"), W * R, "every waiter cleared every round exactly once");
shouldBe(o.slot, R);
// nobody left parked on the slot
shouldBe(Atomics.notify(o, "slot"), 0);
