//@ requireOptions("--useJSThreads=1")
// Shared-object semantics: frozen, sealed, and non-extensible objects across
// threads. Integrity levels are object state, so they must be visible to and
// enforced on every thread: foreign writes/adds/deletes obey the same rules
// (silent failure in sloppy mode, TypeError in strict mode) as local ones.
load("../resources/assert.js", "caller relative");

// --- Frozen on main: a foreign thread cannot write, add, or delete; sloppy
// failures are silent, the object is bit-for-bit unchanged.
{
    const obj = Object.freeze({ a: 1, b: "two" });
    const report = new Thread(o => {
        const frozen = Object.isFrozen(o);
        o.a = 100;            // sloppy: silent no-op
        o.added = true;       // sloppy: silent no-op
        const delResult = delete o.a; // sloppy: returns false
        return frozen + ":" + o.a + ":" + ("added" in o) + ":" + delResult;
    }, obj).join();
    shouldBe(report, "true:1:false:false");
    shouldBe(obj.a, 1);
    shouldBe(obj.b, "two");
    shouldBe(Object.keys(obj).join(","), "a,b");
}

// --- Frozen on main: strict-mode foreign accesses throw TypeError.
{
    const obj = Object.freeze({ x: 7 });
    const names = new Thread(o => {
        "use strict";
        const caught = [];
        try { o.x = 8; caught.push("no-throw"); } catch (e) { caught.push(e.constructor.name); }
        try { o.fresh = 1; caught.push("no-throw"); } catch (e) { caught.push(e.constructor.name); }
        try { delete o.x; caught.push("no-throw"); } catch (e) { caught.push(e.constructor.name); }
        return caught.join(",");
    }, obj).join();
    shouldBe(names, "TypeError,TypeError,TypeError");
    shouldBe(obj.x, 7);
}

// --- Freeze performed *by* a spawned thread is in force on the main thread.
{
    const obj = { v: 1 };
    new Thread(o => { Object.freeze(o); }, obj).join();
    shouldBeTrue(Object.isFrozen(obj));
    obj.v = 2; // sloppy silent no-op on main now
    shouldBe(obj.v, 1);
    shouldThrow(TypeError, () => { "use strict"; obj.v = 3; });
}

// --- Mutate-then-freeze sequence across three turns: a first thread writes,
// main freezes, a second thread's write must bounce off. (Serialized by
// join(); a true in-flight handshake needs worker-side cond.wait, which the
// GIL stub cannot support — see risks.)
{
    const target = { n: 0 };
    new Thread(() => { target.n = 1; }).join(); // pre-freeze write succeeds
    Object.freeze(target);
    const after = new Thread(() => {
        target.n = 99; // post-freeze sloppy write: no-op
        return target.n;
    }).join();
    shouldBe(after, 1);
    shouldBe(target.n, 1);
}

// --- Sealed: foreign threads may write existing properties but not add or
// delete.
{
    const obj = Object.seal({ open: 1, slot: "s" });
    const report = new Thread(o => {
        const sealed = Object.isSealed(o) + ":" + Object.isFrozen(o);
        o.open = 2;          // allowed
        o.intruder = true;   // blocked, silent
        const del = delete o.slot; // blocked, false
        return sealed + ":" + del;
    }, obj).join();
    shouldBe(report, "true:false:false");
    shouldBe(obj.open, 2);
    shouldBeFalse("intruder" in obj);
    shouldBe(obj.slot, "s");
}

// --- Sealed by one thread, written by another, verified by main.
{
    const obj = { counter: 0 };
    new Thread(o => { Object.seal(o); }, obj).join();
    new Thread(o => { o.counter = 5; o.nope = 1; }, obj).join();
    shouldBe(obj.counter, 5);
    shouldBeFalse("nope" in obj);
    shouldBeTrue(Object.isSealed(obj));
}

// --- preventExtensions: foreign adds fail, but writes and deletes work.
{
    const obj = { keep: 1, removable: 2 };
    Object.preventExtensions(obj);
    const report = new Thread(o => {
        const ext = Object.isExtensible(o);
        o.added = 1;             // blocked
        o.keep = 10;             // allowed
        const del = delete o.removable; // allowed
        return ext + ":" + del;
    }, obj).join();
    shouldBe(report, "false:true");
    shouldBe(obj.keep, 10);
    shouldBeFalse("added" in obj);
    shouldBeFalse("removable" in obj);
    // Re-adding a deleted property is still an add: blocked even though the
    // name used to exist.
    new Thread(o => { o.removable = 3; }, obj).join();
    shouldBeFalse("removable" in obj);
}

// --- Integrity-level queries agree on every thread.
{
    const frozen = Object.freeze({});
    const sealed = Object.seal({ s: 1 });
    const plain = { p: 1 };
    const report = new Thread((f, s, p) => [
        Object.isFrozen(f), Object.isSealed(f), Object.isExtensible(f),
        Object.isFrozen(s), Object.isSealed(s), Object.isExtensible(s),
        Object.isFrozen(p), Object.isSealed(p), Object.isExtensible(p),
    ].join(","), frozen, sealed, plain).join();
    shouldBe(report, "true,true,false,false,true,false,false,false,true");
}

// --- Freezing does not deep-freeze: a shared nested object stays mutable
// from any thread.
{
    const inner = { mutable: 0 };
    const outer = Object.freeze({ inner });
    new Thread(o => { o.inner.mutable = 42; }, outer).join();
    shouldBe(inner.mutable, 42);
    shouldBe(outer.inner, inner);
}

// --- Frozen object as a prototype: foreign instance writes to a name owned
// (read-only) by the frozen prototype must fail; unrelated names still work.
{
    const proto = Object.freeze({ locked: "proto" });
    const obj = Object.create(proto);
    const report = new Thread(o => {
        o.locked = "shadow-attempt"; // blocked by non-writable proto prop (sloppy: silent)
        o.free = "fine";
        return Object.prototype.hasOwnProperty.call(o, "locked") + ":" + o.locked + ":" + o.free;
    }, obj).join();
    shouldBe(report, "false:proto:fine");
    shouldBe(obj.free, "fine");
}

// --- Frozen object with an accessor: the getter still runs cross-thread and
// the setter (frozen means non-configurable, but accessors keep operating)
// still fires.
{
    const sink = { last: null };
    const obj = {};
    Object.defineProperty(obj, "acc", {
        get() { return "got"; },
        set(v) { sink.last = v; },
        enumerable: true,
        configurable: true,
    });
    Object.freeze(obj);
    const got = new Thread(o => { o.acc = "from-thread"; return o.acc; }, obj).join();
    shouldBe(got, "got");
    shouldBe(sink.last, "from-thread"); // setters are not disabled by freeze
}

// --- Sealed dictionary-ish object (delete first, then seal) across threads.
{
    const obj = { a: 1, b: 2, c: 3 };
    delete obj.b; // dictionary transition before sealing
    Object.seal(obj);
    const report = new Thread(o => {
        o.a = 11;
        o.b = 22; // add: blocked
        return delete o.c;
    }, obj).join();
    shouldBe(report, false);
    shouldBe(obj.a, 11);
    shouldBeFalse("b" in obj);
    shouldBe(obj.c, 3);
}
