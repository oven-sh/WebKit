//@ requireOptions("--useJSThreads=1")
// MC-LOCK S5 (docs/threads/cve/map-MC-LOCK.md): safepoint state-machine
// convergence vs native park sites — the AB-17B regression (the in-tree
// instance of the ERTS allocator-carrier deadlock shape). The conductor's
// stop predicate is access-based ("parked implies access-released",
// UNGIL-HANDOUT §A.3.2); a waiter parked in a native wait that holds heap
// access and never polls the stop word wedges every stop request until the
// 30s watchdog fail-stop (JSThreadsSafepoint.cpp watchdogAssertStopProgress).
// FIX-2 closed it with parkSitePollAndParkForStopTheWorld on every D9
// quantum. This test holds threads parked in the property Atomics.wait path
// AND in cell-lock contention while another thread drives a storm of
// per-event F2 stops (foreign deletes on fresh-shaped objects, each a
// §10.6 STW while the TTL sets are valid).
//
// Oracle: the test COMPLETES — every stop converges while waiters are
// parked, the waiters wake on notify, and the watchdog RELEASE_ASSERT never
// fires. A hang-then-abort at ~30s with the JSThreadsSafepoint watchdog
// message is the regression signature.
//
// EXECUTED POST-UNGIL ONLY (under the phase-1 GIL stops trivially converge).
// Deterministic in its setup; the stop/park overlap itself is timing-driven,
// so run count is sized to make the overlap near-certain.
load("../harness.js", "caller relative");

const WAITERS = 3;
const STOP_ROUNDS = 400;
const gate = { go: 0, started: 0, stop: 0, req: 0, ack: 0 };
const channel = { obj: null };
const contended = { v: 0 };

// --- Parked-in-native-wait threads: the FIX-2 D9 quantum surface. ---
const waiters = spawnN(WAITERS, () => {
    Atomics.add(gate, "started", 1);
    Atomics.notify(gate, "started");
    let wakeups = 0;
    while (Atomics.load(gate, "stop") === 0) {
        // Long-timeout park; must release heap access / poll the stop word
        // per D9 quantum or every F2 stop below wedges on this thread.
        Atomics.wait(gate, "go", 0, 10000);
        wakeups++;
    }
    return wakeups;
});

// --- Cell-lock contention thread: parks in the JSCellLock slow path while
// stops are requested (O2 guarantees holders drain; this exercises the
// waiter-side interaction with the stop machinery). Dictionary-mode adds and
// deletes serialize on the cell lock (SPEC-objectmodel §6 L3/L4). ---
const shared = {};
for (let i = 0; i < 80; ++i)
    shared["d" + i] = i; // push toward dictionary / out-of-line storage
const locker = new Thread(() => {
    let ops = 0;
    while (Atomics.load(gate, "stop") === 0) {
        shared["k" + (ops & 63)] = ops;
        delete shared["k" + (ops & 63)];
        ops++;
    }
    return ops;
});

// --- Stop-storm thread: foreign deletes => F2 fires => per-event STW. ---
const stopper = new Thread(() => {
    let fired = 0;
    let seen = 0;
    while (Atomics.load(gate, "stop") === 0) {
        const r = Atomics.load(gate, "req");
        if (r === seen) {
            Atomics.wait(gate, "req", seen, 1);
            continue;
        }
        seen = r;
        const o = channel.obj;
        // Foreign delete is a transition: with the (fresh) structure's TTL
        // sets valid this keys F2 and requests a stop-the-world
        // (deletePropertyNamedConcurrent F2 step 0) — while the waiters above
        // are parked.
        delete o.p;
        if (o.p !== undefined)
            throw new Error("delete failed at round " + seen);
        fired++;
        Atomics.store(gate, "ack", seen);
        Atomics.notify(gate, "ack");
    }
    return fired;
});

waitUntil(() => Atomics.load(gate, "started") === WAITERS);

for (let r = 1; r <= STOP_ROUNDS; ++r) {
    // Fresh shape every round so the TTL sets are valid and each foreign
    // delete genuinely fires (monotone sets never re-arm, F4 chain-fires).
    const o = { p: r };
    o["u" + r] = 1;
    channel.obj = o;
    Atomics.store(gate, "req", r);
    Atomics.notify(gate, "req");
    while (Atomics.load(gate, "ack") !== r)
        Atomics.wait(gate, "ack", Atomics.load(gate, "ack"), 1);
}

Atomics.store(gate, "stop", 1);
Atomics.store(gate, "req", STOP_ROUNDS + 1);
Atomics.notify(gate, "req");
Atomics.notify(gate, "go");

shouldBe(stopper.join(), STOP_ROUNDS);
shouldBeTrue(locker.join() > 0, "locker made progress through the stop storm");
const counts = joinAll(waiters);
for (const c of counts)
    shouldBeTrue(c >= 1, "waiter woke instead of wedging a stop");
