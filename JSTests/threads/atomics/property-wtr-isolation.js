//@ requireOptions("--useJSThreads=1")
// API-I11: waiters on (o,"k") are unaffected by notify on a typed array, on
// (o,"j"), or on another object's "k" — waiter identity is (cell, uid)
// (Dev 3), and property/TA waiters are never cross-woken. notify's return
// count makes this assertable without timing: every wrong-target notify
// must report 0 woken while the right one reports 1.
load("../harness.js", "caller relative");

const o = { k: 0, j: 0 };
const other = { k: 0 };
const i32 = new Int32Array(new SharedArrayBuffer(8));

function parkWaiterOn(target, key) {
    // Spawns a waiter and returns once it is parked (cooperative GIL: the
    // ready-park sequencing guarantees it; see property-wait-notify.js).
    //
    // GIL-OFF (closeout review): that guarantee is GONE — the main thread
    // resumes as soon as `ready` is stored, while the waiter is still en
    // route from the ready store to Atomics.wait, so a right-target notify
    // can legitimately report 0 woken (observed as a ~30-50% flake at the
    // expect-1 asserts). Wrong-target notifies stay timing-independent
    // (they can never wake this waiter, parked or not), so only the
    // expect-1 sites changed: they spin via notifyOne() below until the
    // waiter is actually parked. GIL-on the spin runs exactly once.
    const sync = { ready: 0 };
    const t = new Thread(() => {
        Atomics.store(sync, "ready", 1);
        Atomics.notify(sync, "ready");
        return Atomics.wait(target, key, 0);
    });
    if (Atomics.load(sync, "ready") === 0)
        Atomics.wait(sync, "ready", 0);
    return t;
}

function notifyOne(target, key) {
    // Spin until the single parked waiter is woken; returns the woken count
    // of the successful notify (always 1 — a wrong-target notify can never
    // satisfy the loop, so the isolation property is still what terminates
    // it). A waiter that never parks turns this into the harness timeout.
    for (;;) {
        const woken = Atomics.notify(target, key, 1);
        if (woken !== 0)
            return woken;
    }
}

// ---- waiter on (o,"k"); notifies on every wrong target wake nothing ----
{
    const t = parkWaiterOn(o, "k");

    shouldBe(Atomics.notify(o, "j"), 0, "same object, different key");
    shouldBe(Atomics.notify(other, "k"), 0, "different object, same key name");
    shouldBe(Atomics.notify(i32, 0), 0, "typed-array waiters are a different domain");
    shouldBe(Atomics.notify(o, "K"), 0, "keys are case-sensitive uids");
    shouldBe(Atomics.notify(o, "absent"), 0, "0 woken is valid even if o lacks the property");

    // Only the true (cell, uid) wakes it.
    shouldBe(notifyOne(o, "k"), 1);
    shouldBe(t.join(), "ok");
}

// ---- symmetric check: waiter on (other,"k") untouched by (o,"k") ----
{
    const t = parkWaiterOn(other, "k");
    shouldBe(Atomics.notify(o, "k"), 0, "the dead list for (o,'k') must not alias (other,'k')");
    shouldBe(notifyOne(other, "k"), 1);
    shouldBe(t.join(), "ok");
}

// ---- string key vs canonical index key on the same object are the same
// uid ("1" and 1 canonicalize identically), while distinct names differ ----
{
    const arr = { 1: 0 };
    const t = parkWaiterOn(arr, "1");
    shouldBe(Atomics.notify(arr, "01"), 0, "'01' is a different uid from '1'");
    shouldBe(notifyOne(arr, 1), 1, "numeric 1 canonicalizes to uid '1'");
    shouldBe(t.join(), "ok");
}

// ---- count semantics on the real target ----
{
    const t = parkWaiterOn(o, "k");
    shouldBe(Atomics.notify(o, "k", 0), 0, "explicit count 0 wakes none");
    shouldBe(notifyOne(o, "k"), 1);
    shouldBe(t.join(), "ok");
}
