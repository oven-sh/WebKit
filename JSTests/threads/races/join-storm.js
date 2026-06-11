//@ requireOptions("--useJSThreads=1")
// API-I4 at scale (GI): one target thread, a storm of concurrent joiners —
// sync joins from J spawned threads racing the target's completion sequence
// (F5: publish, notifyAll, ticket settles), asyncJoins from main, plus
// post-completion waves. All joins must agree on the RESULT IDENTITY and
// none may hang after completion (the joinLock/notifyAll protocol has no
// lost-wakeup window: store + re-check both run under joinLock).
//
// Amplifier target (Tools/threads/amplify.sh). Annex T2: bounded — every
// fn terminates unconditionally and every spawned thread is joined.
load("../harness.js", "caller relative");

asyncTestStart(8);

const result = { unique: true };
const exc = new Error("storm-exc");

// ---- resolution storm ----
{
    const target = new Thread(() => result);

    const J = 8;
    const joiners = spawnN(J, () => target.join());

    for (let i = 0; i < 8; ++i) {
        target.asyncJoin().then(v => {
            shouldBe(v, result);
            asyncTestPassed();
        });
    }

    shouldBe(target.join(), result, "main races the storm");
    for (const v of joinAll(joiners))
        shouldBe(v, result, "every concurrent joiner agrees by identity");

    // post-completion joins never hang and still agree
    shouldBe(target.join(), result);
    const late = spawnN(4, () => target.join());
    for (const v of joinAll(late))
        shouldBe(v, result);
}

// ---- rejection storm: all joiners see the SAME exception object ----
{
    const failing = new Thread(() => { throw exc; });
    const catchers = spawnN(6, () => {
        try {
            failing.join();
            return "no-throw";
        } catch (e) {
            return e;
        }
    });
    for (const v of joinAll(catchers))
        shouldBe(v, exc, "every joiner rethrows the same exception object");
    let threw = false;
    try { failing.join(); } catch (e) { threw = true; shouldBe(e, exc); }
    shouldBeTrue(threw);
}

// ---- nested join chains race completion too ----
{
    const a = new Thread(() => 1);
    const b = new Thread(() => a.join() + 1);
    const c = new Thread(() => b.join() + 1);
    shouldBe(c.join(), 3);
    shouldBe(b.join(), 2);
    shouldBe(a.join(), 1);
}
