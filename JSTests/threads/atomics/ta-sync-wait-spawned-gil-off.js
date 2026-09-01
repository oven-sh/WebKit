//@ requireOptions("--useJSThreads=1", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1")
// GIL-off, a spawned Thread may call the synchronous typed-array Atomics.wait:
// the SPEC-api 4.5-1a gate ("Atomics.wait cannot be called from the current
// thread.") applies GIL-on only (ta-wait-thread-gate.js). The non-blocking
// forms return their usual results, and a blocking wait parks on its own
// per-wait node with heap access released until a main-thread notify wakes
// it with exactly one counted "ok".
load("../harness.js", "caller relative");

asyncTestStart(1);

// lane 0: the waited-on word; lane 1: 1 once the waiter is about to park,
// 2 if the non-blocking calls threw.
const i32 = new Int32Array(new SharedArrayBuffer(16));

const t = new Thread(() => {
    const results = [];
    try {
        results.push(Atomics.wait(i32, 0, 1)); // value mismatch: no park
        results.push(Atomics.wait(i32, 0, 0, 0)); // zero timeout: no park
    } catch (e) {
        Atomics.store(i32, 1, 2);
        throw e;
    }
    Atomics.store(i32, 1, 1);
    results.push(Atomics.wait(i32, 0, 0)); // parks until main notifies
    return results.join(",");
});

waitUntil(() => Atomics.load(i32, 1) !== 0, 10000);
shouldBe(Atomics.load(i32, 1), 1, "a spawned Thread's sync typed-array wait must not throw GIL-off");
let woken = 0;
waitUntil(() => {
    woken += Atomics.notify(i32, 0);
    return woken !== 0;
}, 10000);
shouldBe(woken, 1, "exactly one counted notify");

t.asyncJoin().then(v => {
    shouldBe(v, "not-equal,timed-out,ok");
    asyncTestPassed();
});
