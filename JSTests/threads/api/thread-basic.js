//@ requireOptions("--useJSThreads=1")
// API-I2: for any value v (objects, NaN, -0), new Thread(() => v).join() is
//         SameValue-equal to v (objects: the same reference).
// API-I4: all join/asyncJoin calls agree (any thread, any count, any time);
//         none hangs after completion.
// API-I5: the spawned thread's Thread.current is reference-equal to the
//         parent's new Thread(...) result, and stable.
//
// Conventions (annex T2): self-checking, failure = throw; every spawned
// thread is joined or awaited; no preemptive-GIL reliance (every spawned fn
// runs to completion on its own; main only yields via join/await).
load("../harness.js", "caller relative");

asyncTestStart(1);

// ---- I2: result identity / SameValue across the join ----
const obj = { marker: 1 };
shouldBe(new Thread(() => obj).join(), obj); // same reference
shouldBe(new Thread(() => 42).join(), 42);
shouldBe(new Thread(() => "str").join(), "str");
shouldBe(new Thread(() => null).join(), null);
shouldBe(new Thread(() => undefined).join(), undefined);
shouldBe(new Thread(() => true).join(), true);
shouldBe(new Thread(() => -0).join(), -0); // shouldBe distinguishes -0
{
    const nan = new Thread(() => NaN).join();
    shouldBeTrue(nan !== nan, "NaN must round-trip");
}
{
    const sym = Symbol("payload");
    shouldBe(new Thread(() => sym).join(), sym);
}
{
    const big = 2n ** 64n;
    shouldBe(new Thread(() => big).join(), big);
}

// fn is called with this-argument undefined (SPEC-api 4.1
// "this===undefined"): a strict fn observes undefined; a sloppy fn boxes
// it to globalThis per ordinary [[Call]] semantics, which the engine must
// not override.
shouldBe(new Thread(function (a, b) {
    "use strict";
    if (this !== undefined)
        throw new Error("thread fn must run with this === undefined");
    return a + b;
}, 40, 2).join(), 42);

// Sloppy-mode companion: ordinary [[Call]] boxing means a non-strict thread
// fn observes this === globalThis (its own thread's global). Pins the
// OrdinaryCallBindThis semantics the strict assertion above relies on.
shouldBe(new Thread(function () {
    return this === globalThis;
}).join(), true);

// Argument identity is preserved too (5.10 argSlots root, no copy).
{
    const arg = { deep: true };
    shouldBe(new Thread(a => a, arg).join(), arg);
}

// ---- I5: Thread.current inside the spawned fn ----
{
    const t = new Thread(() => {
        const first = Thread.current;
        const second = Thread.current;
        if (first !== second)
            throw new Error("Thread.current must be stable within the thread");
        return first;
    });
    shouldBe(t.join(), t, "spawned Thread.current must be the parent's Thread object");
}

// Main-thread Thread.current: stable, id 0 (5.1 lazy main ThreadState).
shouldBe(Thread.current, Thread.current);
shouldBe(Thread.current.id, 0);

// ---- I4: joins agree — repeated, cross-thread, before/after completion ----
{
    const target = new Thread(() => ({ value: 7 }));
    const v1 = target.join();
    const v2 = target.join(); // join after completion: no hang, same answer
    shouldBe(v2, v1);
    shouldBe(v1.value, 7);

    // join from another thread agrees by identity.
    const joiner = new Thread(() => target.join());
    shouldBe(joiner.join(), v1);

    // asyncJoin (registered post-completion) agrees and settles.
    target.asyncJoin().then(v => {
        shouldBe(v, v1);
        asyncTestPassed();
    });
}

// Spawned thread ids are >= 1 and distinct from main's 0 (I17's bounds file
// covers the full range; here we only need "not main").
{
    const t = new Thread(() => Thread.current.id);
    const idInside = t.join();
    shouldBe(idInside, t.id);
    shouldBeTrue(t.id >= 1);
}
