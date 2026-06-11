//@ requireOptions("--useJSThreads=1")
// Shared-object semantics: adding properties (structure transitions) across
// threads. Foreign-thread transitions are the hard case for the flat/segmented
// butterfly design; under the GIL stub they must already have the right
// observable semantics: no lost properties, correct enumeration order,
// correct attribute defaults.
load("../resources/assert.js", "caller relative");

// --- A foreign thread adds a property; main observes value and presence.
{
    const obj = { existing: 1 };
    new Thread(o => { o.added = "by-thread"; }, obj).join();
    shouldBe(obj.added, "by-thread");
    shouldBeTrue("added" in obj);
    shouldBeTrue(Object.prototype.hasOwnProperty.call(obj, "added"));
    const desc = Object.getOwnPropertyDescriptor(obj, "added");
    shouldBeTrue(desc.writable);
    shouldBeTrue(desc.enumerable);
    shouldBeTrue(desc.configurable);
}

// --- A property added by a thread is visible to the main thread *before*
// the thread exits: the worker adds, then notifies main (main-side cond.wait
// is the one blocking pattern the GIL stub supports; worker-side cond.wait
// livelocks in JSLock::DropAllLocks, see risks).
{
    const lock = new Lock();
    const cond = new Condition();
    const obj = { phase: 0 };
    const writer = new Thread(o => {
        lock.hold(() => {
            o.early = "before-exit";
            o.phase = 1;
            cond.notifyAll();
        });
        // Keep the thread alive a bit doing unrelated work, so main's read
        // below is genuinely concurrent with a live thread.
        let x = 0;
        for (let i = 0; i < 1e5; ++i) x += i;
        return x;
    }, obj);
    let seen;
    lock.hold(() => {
        while (obj.phase !== 1)
            cond.wait(lock);
        seen = obj.early;
    });
    shouldBe(seen, "before-exit");
    writer.join();
    shouldBe(obj.early, "before-exit");
}

// --- Many threads adding distinct properties under a lock: every transition
// must survive (no lost properties), and each value must be the one its
// thread stored.
{
    const obj = {};
    const lock = new Lock();
    const threadCount = 8;
    joinAll(spawnN(threadCount, i => {
        lock.hold(() => { obj["fromThread" + i] = i; });
    }));
    shouldBe(Object.keys(obj).length, threadCount);
    for (let i = 0; i < threadCount; ++i)
        shouldBe(obj["fromThread" + i], i);
}

// --- A foreign thread grows an object far past inline capacity, forcing
// repeated butterfly reallocation; nothing may be lost or reordered.
{
    const obj = { seed: "s" };
    new Thread(o => {
        for (let i = 0; i < 300; ++i)
            o["grow" + i] = i;
    }, obj).join();
    shouldBe(obj.seed, "s");
    for (let i = 0; i < 300; ++i)
        shouldBe(obj["grow" + i], i);
    const keys = Object.keys(obj);
    shouldBe(keys.length, 301);
    // Enumeration order: insertion order for string keys.
    shouldBe(keys[0], "seed");
    shouldBe(keys[1], "grow0");
    shouldBe(keys[300], "grow299");
}

// --- Interleaved adds from two threads (serialized by join) build the
// expected combined shape.
{
    const obj = {};
    new Thread(o => { o.a = 1; }, obj).join();
    obj.b = 2;
    new Thread(o => { o.c = 3; }, obj).join();
    obj.d = 4;
    shouldBe(JSON.stringify(obj), '{"a":1,"b":2,"c":3,"d":4}');
}

// --- Adding the same property name from several threads: last value wins,
// exactly one property exists.
{
    const obj = {};
    const lock = new Lock();
    joinAll(spawnN(6, i => { lock.hold(() => { obj.contended = i; }); }));
    shouldBe(Object.keys(obj).length, 1);
    shouldBeTrue(obj.contended >= 0 && obj.contended <= 5);
}

// --- Symbol-keyed addition from a foreign thread.
{
    const obj = {};
    const key = Symbol("added-cross-thread");
    new Thread((o, k) => { o[k] = 7; }, obj, key).join();
    shouldBe(obj[key], 7);
    shouldBe(Object.getOwnPropertySymbols(obj).length, 1);
    shouldBe(Object.getOwnPropertySymbols(obj)[0], key);
}

// --- Two objects with the same shape: a foreign-thread transition on one
// must not affect the other (structure sharing must not leak properties).
{
    const a = { x: 1 };
    const b = { x: 2 };
    new Thread(o => { o.onlyOnA = true; }, a).join();
    shouldBeTrue("onlyOnA" in a);
    shouldBeFalse("onlyOnA" in b);
    shouldBe(b.onlyOnA, undefined);
}

// --- Property-name snapshot taken inside a thread agrees with the main
// thread's view (joined, so no concurrent mutation).
{
    const obj = { k1: 1, k2: 2 };
    const snapshot = new Thread(o => {
        o.k3 = 3;
        return Object.keys(o).join(",");
    }, obj).join();
    shouldBe(snapshot, "k1,k2,k3");
    shouldBe(Object.keys(obj).join(","), "k1,k2,k3");
}

// --- Adds on a prototype-bearing object: new own property must shadow,
// not overwrite, the prototype's property.
{
    const proto = { p: "proto" };
    const obj = Object.create(proto);
    new Thread(o => { o.p = "own"; }, obj).join();
    shouldBe(obj.p, "own");
    shouldBe(proto.p, "proto");
    shouldBeTrue(Object.prototype.hasOwnProperty.call(obj, "p"));
}

// --- defineProperty (non-default attributes) from a foreign thread.
{
    const obj = {};
    new Thread(o => {
        Object.defineProperty(o, "ro", { value: 5, writable: false, enumerable: false, configurable: false });
    }, obj).join();
    shouldBe(obj.ro, 5);
    const desc = Object.getOwnPropertyDescriptor(obj, "ro");
    shouldBeFalse(desc.writable);
    shouldBeFalse(desc.enumerable);
    shouldBeFalse(desc.configurable);
    shouldThrow(TypeError, () => { "use strict"; obj.ro = 6; });
    shouldBe(obj.ro, 5);
}
