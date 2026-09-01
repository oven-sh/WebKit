//@ requireOptions("--useJSThreads=1")
// Shared-object semantics: deleting properties across threads. Delete is the
// rare slow path in the design (per-object lock + quarantined slots), but the
// observable semantics must match single-threaded JS exactly: presence,
// enumeration, descriptor queries, and re-addition after delete.
load("../resources/assert.js", "caller relative");

// --- A foreign thread deletes a property; main observes its absence.
{
    const obj = { keep: 1, drop: 2 };
    const deleted = new Thread(o => delete o.drop, obj).join();
    shouldBeTrue(deleted);
    shouldBeFalse("drop" in obj);
    shouldBe(obj.drop, undefined);
    shouldBe(obj.keep, 1);
    shouldBe(Object.keys(obj).join(","), "keep");
    shouldBe(Object.getOwnPropertyDescriptor(obj, "drop"), undefined);
}

// --- Main deletes; a foreign thread observes absence (ordered by spawn).
{
    const obj = { a: 1, b: 2 };
    delete obj.a;
    const result = new Thread(o => ("a" in o) + ":" + ("b" in o), obj).join();
    shouldBe(result, "false:true");
}

// --- Delete then re-add from a different thread: the property must come
// back fresh (new value, default attributes, moved to end of key order).
{
    const obj = { first: 1, second: 2, third: 3 };
    new Thread(o => { delete o.second; }, obj).join();
    new Thread(o => { o.second = "readded"; }, obj).join();
    shouldBe(obj.second, "readded");
    shouldBe(Object.keys(obj).join(","), "first,third,second");
    const desc = Object.getOwnPropertyDescriptor(obj, "second");
    shouldBeTrue(desc.writable && desc.enumerable && desc.configurable);
}

// --- Stale-read guard: after a foreign delete and a foreign re-add of a
// *different* property, reading the deleted name must not alias the new
// property's storage (the "quarantined slot" hazard).
{
    const obj = { victim: "old-value", pad1: 1, pad2: 2 };
    new Thread(o => {
        delete o.victim;
        o.replacement = "new-value";
    }, obj).join();
    shouldBe(obj.victim, undefined);
    shouldBe(obj.replacement, "new-value");
    shouldBeFalse("victim" in obj);
}

// --- Repeated delete/add cycles alternating between threads.
{
    const obj = { cycled: 0 };
    for (let round = 1; round <= 5; ++round) {
        new Thread((o, r) => {
            shouldBeTrue(delete o.cycled);
            shouldBeFalse("cycled" in o);
            o.cycled = r;
        }, obj, round).join();
        shouldBe(obj.cycled, round);
    }
    shouldBe(Object.keys(obj).length, 1);
}

// --- Delete of a missing property returns true and changes nothing.
{
    const obj = { x: 1 };
    shouldBeTrue(new Thread(o => delete o.notThere, obj).join());
    shouldBe(Object.keys(obj).join(","), "x");
}

// --- Delete of a non-configurable property from a foreign thread: returns
// false (sloppy) or throws TypeError (strict); property survives.
{
    const obj = {};
    Object.defineProperty(obj, "pinned", { value: 9, configurable: false, enumerable: true });
    const sloppyResult = new Thread(o => delete o.pinned, obj).join();
    shouldBeFalse(sloppyResult);
    shouldBe(obj.pinned, 9);
    const strictError = new Thread(o => {
        "use strict";
        try {
            delete o.pinned;
            return null;
        } catch (e) {
            return e.constructor.name;
        }
    }, obj).join();
    shouldBe(strictError, "TypeError");
    shouldBe(obj.pinned, 9);
}

// --- Many threads each delete a distinct property under a lock; survivors
// are exactly the undeleted set.
{
    const obj = {};
    const total = 16, deleters = 8;
    for (let i = 0; i < total; ++i)
        obj["k" + i] = i;
    const lock = new Lock();
    joinAll(spawnN(deleters, i => { lock.hold(() => { delete obj["k" + i]; }); }));
    shouldBe(Object.keys(obj).length, total - deleters);
    for (let i = 0; i < deleters; ++i)
        shouldBeFalse(("k" + i) in obj);
    for (let i = deleters; i < total; ++i)
        shouldBe(obj["k" + i], i);
}

// --- Deleting an out-of-line property from a foreign thread leaves its
// neighbors intact.
{
    const big = {};
    for (let i = 0; i < 150; ++i)
        big["q" + i] = i;
    new Thread(o => { delete o.q75; }, big).join();
    shouldBeFalse("q75" in big);
    shouldBe(big.q74, 74);
    shouldBe(big.q76, 76);
    shouldBe(Object.keys(big).length, 149);
}

// --- Deleting an own property exposes the prototype's property of the same
// name, across threads.
{
    const proto = { shadowed: "from-proto" };
    const obj = Object.create(proto);
    obj.shadowed = "own";
    const seen = new Thread(o => {
        delete o.shadowed;
        return o.shadowed; // now a prototype hit
    }, obj).join();
    shouldBe(seen, "from-proto");
    shouldBe(obj.shadowed, "from-proto");
    shouldBeFalse(Object.prototype.hasOwnProperty.call(obj, "shadowed"));
}

// --- Symbol-keyed delete from a foreign thread.
{
    const key = Symbol("doomed");
    const obj = { [key]: 1, stays: 2 };
    shouldBeTrue(new Thread((o, k) => delete o[k], obj, key).join());
    shouldBe(Object.getOwnPropertySymbols(obj).length, 0);
    shouldBe(obj.stays, 2);
}
