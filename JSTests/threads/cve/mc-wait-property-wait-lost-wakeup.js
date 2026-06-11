//@ requireOptions("--useJSThreads=1", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1")
// MC-WAIT susceptibility test (docs/threads/cve/map-MC-WAIT.md, surface S3a).
// DO NOT RUN during bring-up; executes post-ungil via thread-cve-audit.
//
// The cross-engine "Atomics.wait not-equal ordering" exemplar, on our
// property lane: atomicsWaitOnProperty reads the property ONCE in step 1
// ("no re-read below", ThreadAtomics.cpp) and enqueues its waiter under
// listLock later, with no value re-validation. The I10 lost-wakeup closure
// argument is "JSLock held from the read through the enqueue" — a GIL-ON
// argument. GIL-off the JSLock is a token, not mutual exclusion, so a
// foreign store+notify landing between the waiter's read and its enqueue is
// LOST: the notify finds an empty list, the waiter then parks on a value
// that already mismatches. SPEC-ungil §C.3 (annex C3, BINDING) mandates the
// fix — SVZ re-validation UNDER listLock at enqueue, mismatch => dequeue
// "not-equal" — and INTEGRATE-ungil.md records it as OPEN (owned by U-T11)
// at the time this test was written. atomicsWaitAsyncOnProperty has the
// identical read-then-enqueue shape; the sync arm below exercises the
// shared window (a waitAsync arm would need shell run-loop pumping and adds
// no window coverage).
//
// Probe: per round, the spawned waiter publishes "armed" and immediately
// calls Atomics.wait(box, k_i, 0, WAIT_MS) on a fresh pre-created key; the
// main thread, on seeing "armed", applies a varying busy-jitter (to scan
// window offsets) then stores 1 and notifies. Because the store+notify is
// guaranteed to precede the wait deadline, every legal interleaving ends
// "ok" (notify found the enqueued waiter) or "not-equal" (waiter's read —
// or the §C.3 under-listLock re-validation — saw the store). "timed-out"
// is unambiguous susceptibility: a lost wakeup. GIL-on (or if option
// validation forces the GIL back on) the window does not exist and the
// test passes trivially — post-fix it doubles as the §C.3 regression test.
// Amplifier-ready: a window hit is probabilistic, but any single hit fails
// loudly and deterministically.
load("../harness.js", "caller relative");

const ROUNDS = 200;
const WAIT_MS = 5000; // generous: covers ASAN/TSAN/CI scheduling latency

// Pre-create every per-round key (property wait requires an own data
// property) and the control words. One shared object graph; all cross-
// thread accesses below go through property Atomics (seq_cst).
const box = {};
for (let i = 0; i < ROUNDS; ++i)
    box["k" + i] = 0;
const ctl = { armed: 0, done: 0, result: 0, failedRound: -1 };

// Result codes the waiter publishes per round.
const OK = 1, NOT_EQUAL = 2, TIMED_OUT = 3, OTHER = 4;

const waiter = new Thread(() => {
    for (let i = 0; i < ROUNDS; ++i) {
        const key = "k" + i;
        // Publish "armed" as close as possible to the wait call: the
        // susceptibility window opens at the wait's internal step-1 read.
        Atomics.store(ctl, "armed", i + 1);
        Atomics.notify(ctl, "armed");
        const r = Atomics.wait(box, key, 0, WAIT_MS);
        let code;
        if (r === "ok")
            code = OK;
        else if (r === "not-equal")
            code = NOT_EQUAL;
        else if (r === "timed-out")
            code = TIMED_OUT;
        else
            code = OTHER;
        if (code === TIMED_OUT || code === OTHER)
            Atomics.store(ctl, "failedRound", i);
        Atomics.store(ctl, "result", code);
        Atomics.store(ctl, "done", i + 1);
        Atomics.notify(ctl, "done");
        if (code === TIMED_OUT || code === OTHER)
            return r; // first hit ends the run; main reports it
    }
    return "clean";
});

// Main = notifier. Spin briefly (cheap GIL-off, where main runs in
// parallel), falling back to a GIL-dropping sleep so the test also makes
// progress under a cooperative GIL.
function awaitWord(key, value, what) {
    const deadline = Date.now() + WAIT_MS + 30000;
    let spins = 0;
    while (Atomics.load(ctl, key) < value) {
        if (++spins % 4096 === 0) {
            if (Date.now() > deadline)
                throw new Error("rendezvous stuck: " + what + " (round " + value + ")");
            sleepMs(1);
        }
    }
}

for (let i = 0; i < ROUNDS; ++i) {
    const key = "k" + i;
    awaitWord("armed", i + 1, "waiter never armed");
    // Varying jitter scans store+notify placements across the waiter's
    // read -> enqueue window. Volatile-ish accumulator defeats DCE.
    let sink = 0;
    for (let j = (i % 50) * 20; j > 0; --j)
        sink += j;
    if (sink === -1)
        throw new Error("unreachable");
    Atomics.store(box, key, 1);
    Atomics.notify(box, key);
    awaitWord("done", i + 1, "waiter never reported (lost wakeup would park it for WAIT_MS first)");
    const code = Atomics.load(ctl, "result");
    if (code === TIMED_OUT) {
        waiter.join();
        throw new Error("MC-WAIT S3a HIT: round " + Atomics.load(ctl, "failedRound")
            + " returned 'timed-out' although a store+notify pair was issued well before "
            + "the deadline — the pair landed in the read->enqueue window and was lost "
            + "(missing SPEC-ungil §C.3 under-listLock SVZ re-validation).");
    }
    if (code !== OK && code !== NOT_EQUAL) {
        waiter.join();
        throw new Error("MC-WAIT S3a: round " + i + " produced an impossible wait result (code " + code + ")");
    }
}

shouldBe(waiter.join(), "clean");
