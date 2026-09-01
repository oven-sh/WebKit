//@ requireOptions("--useJSThreads=1")
// API-I3: a value e thrown by the thread fn is rethrown by join() with
// identity (the same value, not a copy or wrapper), every join agrees, and
// asyncJoin() rejects with the same value.
load("../harness.js", "caller relative");

asyncTestStart(3);

// ---- Error object: identity through join, repeated joins, asyncJoin ----
{
    const boom = new Error("boom");
    boom.tagged = { side: "channel" };
    const t = new Thread(() => { throw boom; });

    const caught = shouldThrow(Error, () => t.join());
    shouldBe(caught, boom, "join must rethrow the same exception object");
    shouldBe(caught.tagged, boom.tagged);

    // join after the first rethrow: agrees, no hang (I4 flavor of I3).
    shouldBe(shouldThrow(Error, () => t.join()), boom);

    t.asyncJoin().then(
        () => { throw new Error("asyncJoin must reject"); },
        e => { shouldBe(e, boom); asyncTestPassed(); });
}

// ---- Non-Error object thrown value ----
{
    const payload = { code: 1 };
    const t = new Thread(() => { throw payload; });
    let threw = false;
    try {
        t.join();
    } catch (e) {
        threw = true;
        shouldBe(e, payload);
    }
    shouldBeTrue(threw);
    t.asyncJoin().then(
        () => { throw new Error("asyncJoin must reject"); },
        e => { shouldBe(e, payload); asyncTestPassed(); });
}

// ---- Primitive thrown values keep SameValue identity ----
{
    const t = new Thread(() => { throw "plain string"; });
    let caught = null, threw = false;
    try { t.join(); } catch (e) { threw = true; caught = e; }
    shouldBeTrue(threw);
    shouldBe(caught, "plain string");
}
{
    const t = new Thread(() => { throw 0 / 0; }); // NaN
    let caught = 0, threw = false;
    try { t.join(); } catch (e) { threw = true; caught = e; }
    shouldBeTrue(threw);
    shouldBeTrue(caught !== caught, "thrown NaN must arrive as NaN");
}

// ---- exception thrown from a joiner thread propagates to ITS joiner ----
{
    const inner = new Error("inner");
    const failing = new Thread(() => { throw inner; });
    const relay = new Thread(() => {
        try {
            failing.join();
            return "no-throw";
        } catch (e) {
            return e; // relay the identity outward
        }
    });
    shouldBe(relay.join(), inner);
}

// ---- a rejected-promise RESULT is a result, not an exception ----
{
    const t = new Thread(() => Promise.reject("rejected-result"));
    const p = t.join(); // join returns the promise; it does not await it
    shouldBeTrue(p instanceof Promise);
    p.then(
        () => { throw new Error("must stay rejected"); },
        v => { shouldBe(v, "rejected-result"); asyncTestPassed(); });
}
