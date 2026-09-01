//@ requireOptions("--useJSThreads=1")
// asyncJoin(): returns a Promise that resolves with the thread's result or
// rejects with the thread's exception. Works before and after completion,
// and supports multiple concurrent joiners.
load("../resources/assert.js", "caller relative");

// Each asyncTestPassed() below must fire or the shell exits non-zero.
asyncTestStart(7);

// 1. Basic resolution with the thread's return value.
const resolved = new Thread(() => 42).asyncJoin();
shouldBeTrue(resolved instanceof Promise);
resolved.then(value => {
    shouldBe(value, 42);
    asyncTestPassed();
});

// 2. Resolution keeps heap identity.
const payload = { shared: true };
new Thread(p => p, payload).asyncJoin().then(value => {
    shouldBe(value, payload);
    asyncTestPassed();
});

// 3. Rejection carries the thrown value by identity.
const boom = new Error("boom");
new Thread(() => { throw boom; }).asyncJoin().then(
    () => { throw new Error("should not resolve"); },
    error => {
        shouldBe(error, boom);
        asyncTestPassed();
    });

// 4. asyncJoin after the thread already finished (settled synchronously
// observed via join() first) still resolves.
const finished = new Thread(() => "early");
shouldBe(finished.join(), "early");
finished.asyncJoin().then(value => {
    shouldBe(value, "early");
    asyncTestPassed();
});

// 5. Multiple asyncJoin calls on one thread each get their own promise,
// all settling with the same value.
const multi = new Thread(() => ({ once: 1 }));
const pa = multi.asyncJoin();
const pb = multi.asyncJoin();
shouldBeFalse(pa === pb);
Promise.all([pa, pb]).then(([a, b]) => {
    shouldBe(a, b);
    shouldBe(a.once, 1);
    asyncTestPassed();
});

// 6. asyncJoin and blocking join can be mixed on the same thread.
const mixed = new Thread(() => "both");
const mixedPromise = mixed.asyncJoin();
shouldBe(mixed.join(), "both");
mixedPromise.then(value => {
    shouldBe(value, "both");
    asyncTestPassed();
});

// 7. asyncJoin on a failing thread after join() already observed the throw.
const failedFirst = new Thread(() => { throw "plain-string"; });
shouldBe(shouldThrow(() => failedFirst.join()), "plain-string");
failedFirst.asyncJoin().then(
    () => { throw new Error("should not resolve"); },
    error => {
        shouldBe(error, "plain-string");
        asyncTestPassed();
    });
