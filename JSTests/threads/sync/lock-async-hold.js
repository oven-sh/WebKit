//@ requireOptions("--useJSThreads=1")
// Lock.prototype.asyncHold: promise-based acquisition, release functions,
// FIFO granting, the callback variant, and interaction with sync hold.
//
// Async lock grants settle on run-loop turns (DeferredWorkTimer), which the
// jsc shell pumps after the main script finishes. All assertions on granted
// state therefore live in promise reactions, guarded by asyncTestStart.
load("../resources/assert.js", "caller relative");

asyncTestStart(1);

const lock = new Lock();

// ---- Synchronous error surface ----

// Receiver checks.
shouldThrow(TypeError, () => lock.asyncHold.call({}, () => {}));
// A provided argument must be callable.
shouldThrow(TypeError, () => lock.asyncHold(42));
shouldThrow(TypeError, () => lock.asyncHold("nope"));
// Not recursive: asyncHold while the same thread sync-holds throws.
lock.hold(() => {
    const e = shouldThrow(Error, () => lock.asyncHold());
    shouldBe(e.message, "Lock is not recursive");
});
shouldBeFalse(lock.locked);

// ---- Async acquisition chain ----

const order = [];
let settledEarly = false;

// p1 acquires immediately (uncontended) but the promise must still settle
// asynchronously, on a run-loop turn — not synchronously, not on the
// microtask queue.
const p1 = lock.asyncHold();
shouldBeTrue(p1 instanceof Promise);
shouldBeTrue(lock.locked, "asyncHold acquires the lock synchronously when free");
p1.then(() => { settledEarly = true; });

// p2/p3 queue behind p1 and must be granted in FIFO order.
const p2 = lock.asyncHold();
const p3 = lock.asyncHold(function callbackVariant() {
    order.push("p3-callback");
    shouldBeTrue(lock.locked, "callback variant runs with the lock held");
    return "fnResult";
});

const chain = p1.then(release => {
    order.push("p1");
    shouldBe(typeof release, "function");
    shouldBeTrue(lock.locked);
    release();
    // Double release throws.
    const e = shouldThrow(Error, () => release());
    shouldBe(e.message, "Lock release function called more than once");
    return p2;
}).then(release2 => {
    order.push("p2");
    shouldBe(typeof release2, "function");
    shouldBeTrue(lock.locked, "p2 grant holds the lock");
    release2();
    return p3;
}).then(result => {
    order.push("p3");
    // The callback variant resolves with the callback's return value and
    // auto-releases after the callback.
    shouldBe(result, "fnResult");
    shouldBeFalse(lock.locked, "callback variant auto-releases");

    // Rejection path: a throwing callback rejects the promise and still
    // releases the lock.
    const boom = new Error("boom");
    return lock.asyncHold(() => { throw boom; }).then(
        () => { throw new Error("expected rejection"); },
        caught => {
            shouldBe(caught, boom);
            shouldBeFalse(lock.locked, "lock released after callback throw");
        });
}).then(() => {
    shouldBe(order.join(","), "p1,p2,p3-callback,p3", "FIFO grant order");

    // The lock remains a perfectly good sync lock afterwards, including from
    // another thread. asyncJoin keeps the run loop pumping (a sync join here
    // could starve pending async work).
    const worker = new Thread(() => lock.hold(() => "worker-held"));
    return worker.asyncJoin();
}).then(workerResult => {
    shouldBe(workerResult, "worker-held");
    shouldBeFalse(lock.locked);
    asyncTestPassed();
});
chain.catch(e => { print("FAIL: " + (e && e.stack ? e.stack : e)); });

// Still inside the synchronous part of the script: nothing may have settled
// yet, even after draining microtasks, because grants settle on run-loop
// turns.
drainMicrotasks();
shouldBeFalse(settledEarly, "asyncHold must not settle before a run-loop turn");
shouldBeTrue(lock.locked, "p1's grant is still outstanding");
