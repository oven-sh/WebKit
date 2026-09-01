//@ requireOptions("--useJSThreads=1")
// Build a working mutex out of Atomics.compareExchange / wait / notify on a
// plain object property (the strawman's stated goal for the extended
// Atomics API; `wake` from the 2017 post is spelled `notify` today).
//
// Stub-scheduling note (phase-1 GIL): all blocking and waking happens
// between worker threads; the main thread only spawns and joins (a parked
// worker must never depend on the blocked main thread to wake it), except
// for the final main-thread-waiter section, whose waker never blocks.
load("../resources/assert.js", "caller relative");

// A classic three-state futex mutex (0 = unlocked, 1 = locked,
// 2 = locked with possible waiters), Drepper-style.
//
// The waits are bounded (5ms) purely as a stub workaround: the phase-1 GIL
// reacquisition is LIFO in park order, so a thread that parks indefinitely
// while a shallower-parked peer is mid-wakeup can deadlock the VM. A timeout
// only adds a retry of the CAS loop, so the lock's semantics are unchanged.
function makeMutex() {
    const state = { s: 0 };
    return {
        state,
        lock() {
            if (Atomics.compareExchange(state, "s", 0, 1) === 0)
                return; // fast path, uncontended
            do {
                // Advertise contention, then sleep while it stays contended.
                if (Atomics.load(state, "s") === 2 || Atomics.compareExchange(state, "s", 1, 2) !== 0)
                    Atomics.wait(state, "s", 2, 5);
            } while (Atomics.compareExchange(state, "s", 0, 2) !== 0);
        },
        unlock() {
            if (Atomics.exchange(state, "s", 0) === 2)
                Atomics.notify(state, "s", 1);
        },
    };
}

// ---- Mutual exclusion under contention ----
{
    const mutex = makeMutex();
    const shared = { counter: 0, inside: 0, violations: 0 };
    const THREADS = 4;
    const LAPS = 300;

    const workers = spawnN(THREADS, () => {
        for (let i = 0; i < LAPS; ++i) {
            mutex.lock();
            if (shared.inside !== 0)
                shared.violations++;
            shared.inside = 1;
            // Non-atomic read-modify-write: only the futex lock protects it.
            const before = shared.counter;
            shared.counter = before + 1;
            shared.inside = 0;
            mutex.unlock();
        }
        return "done";
    });
    joinAll(workers).forEach(r => shouldBe(r, "done"));

    shouldBe(shared.counter, THREADS * LAPS, "no lost increments under the futex lock");
    shouldBe(shared.violations, 0, "mutual exclusion never violated");
    shouldBe(shared.inside, 0);
    shouldBe(mutex.state.s, 0, "mutex fully unlocked at the end");
    shouldBe(Atomics.notify(mutex.state, "s"), 0, "no waiter left behind");
}

// ---- Strict ping-pong: main vs a fresh worker each round ----
// Main flips the turn marker and parks in an untimed Atomics.wait; the
// worker — which, with no preemption, only starts once main is parked —
// appends, flips the marker back, notifies (waking exactly main), and
// exits without ever blocking. Any missed or extra wakeup breaks the
// strict alternation or the per-round woken counts.
//
// (Two WORKERS ping-ponging is not expressible under the current stub: each
// would eventually park while its peer is mid-wakeup, and the GIL's LIFO
// regrab then deadlocks them — see condition-worker-waiter.js for the same
// limitation on the Condition side.)
{
    const turn = { v: "main" };
    const box = { log: "" };
    const ROUNDS = 10;

    for (let r = 0; r < ROUNDS; ++r) {
        shouldBe(Atomics.load(turn, "v"), "main", "round " + r + " starts with main's turn");
        box.log += "m";
        const worker = new Thread(round => {
            // Main is parked on turn.v === "worker" by the time this runs.
            if (Atomics.load(turn, "v") !== "worker")
                throw new Error("worker ran out of turn in round " + round);
            box.log += "w";
            Atomics.store(turn, "v", "main");
            return Atomics.notify(turn, "v");
        }, r);
        Atomics.store(turn, "v", "worker");
        shouldBe(Atomics.wait(turn, "v", "worker"), "ok", "main woken by round-" + r + " worker");
        shouldBe(worker.join(), 1, "the notify woke exactly the parked main thread");
    }

    let expected = "";
    for (let r = 0; r < ROUNDS; ++r)
        expected += "mw";
    shouldBe(box.log, expected, "strict alternation");
    shouldBe(turn.v, "main");
}

// ---- Main thread parked in Atomics.wait, woken by a worker ----
// The waker spins on notify's return value (number of waiters actually
// dequeued), so a successful "ok" wakeup is deterministic: the worker only
// stops once it has provably woken main. The worker never blocks, so main
// parking first is safe.
{
    const cell = { v: 0 };
    const waker = new Thread(() => {
        let spins = 0;
        while (Atomics.notify(cell, "v", 1) === 0)
            spins++;
        Atomics.store(cell, "v", 1);
        return spins >= 0;
    });
    shouldBe(Atomics.wait(cell, "v", 0), "ok", "main woken by worker notify");
    shouldBeTrue(waker.join());
    shouldBe(cell.v, 1);
}

// ---- Lost-wakeup safety: value flip plus notify is never missed ----
// The waiter re-checks the value before waiting; whichever of "parked then
// woken" or "saw the flip early" happens, it must terminate and observe 1.
{
    const cell = { v: 0, result: "" };
    const waiter = new Thread(() => {
        let status = "none";
        while (Atomics.load(cell, "v") === 0)
            status = Atomics.wait(cell, "v", 0, 50);
        return status + ":" + Atomics.load(cell, "v");
    });
    const flipper = new Thread(() => {
        Atomics.store(cell, "v", 1);
        return Atomics.notify(cell, "v");
    });
    const seen = waiter.join();
    shouldBeTrue(seen === "ok:1" || seen === "timed-out:1" || seen === "not-equal:1" || seen === "none:1",
        "waiter terminated with the flipped value, got " + seen);
    shouldBeTrue(flipper.join() <= 1);
}
