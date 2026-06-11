//@ requireOptions("--useJSThreads=1")
// Shared-object semantics: accessors across threads. Loading the accessor is
// atomic; *calling* it runs arbitrary code on the calling thread, with the
// calling thread's identity, against shared state. Covers definition,
// invocation, receivers, redefinition, and exceptions crossing join().
load("../resources/assert.js", "caller relative");

// --- A getter defined on the main thread runs when invoked from a foreign
// thread, and sees shared backing state.
{
    const backing = { raw: 10 };
    const obj = {
        get doubled() { return backing.raw * 2; }
    };
    shouldBe(new Thread(o => o.doubled, obj).join(), 20);
    backing.raw = 50;
    shouldBe(new Thread(o => o.doubled, obj).join(), 100);
}

// --- A setter defined on the main thread, driven from many threads under a
// lock, accumulates into shared state with no lost updates.
{
    const log = { total: 0, calls: 0 };
    const obj = {
        set feed(v) { log.total += v; log.calls++; }
    };
    const lock = new Lock();
    joinAll(spawnN(4, i => {
        for (let j = 0; j < 50; ++j)
            lock.hold(() => { obj.feed = i + 1; });
    }));
    shouldBe(log.calls, 200);
    shouldBe(log.total, 50 * (1 + 2 + 3 + 4));
}

// --- The getter executes on the *calling* thread: observe Thread.current.
{
    const obj = {
        get tid() { return Thread.current.id; }
    };
    shouldBe(obj.tid, 0);
    const foreignTid = new Thread(o => o.tid, obj).join();
    shouldBeTrue(foreignTid !== 0);
}

// --- defineProperty of an accessor from a foreign thread; main sees a real
// accessor (descriptor has get/set, no value/writable).
{
    const obj = {};
    new Thread(o => {
        let hidden = "initial";
        Object.defineProperty(o, "acc", {
            get() { return hidden; },
            set(v) { hidden = v; },
            enumerable: true,
            configurable: true,
        });
    }, obj).join();
    const desc = Object.getOwnPropertyDescriptor(obj, "acc");
    shouldBe(typeof desc.get, "function");
    shouldBe(typeof desc.set, "function");
    shouldBe(desc.value, undefined);
    shouldBe(obj.acc, "initial");
    obj.acc = "set-by-main"; // the closure state lives on, thread is gone
    shouldBe(obj.acc, "set-by-main");
}

// --- Accessor on a shared prototype: correct receiver (`this`) when invoked
// through different instances on different threads.
{
    const proto = {
        get label() { return "label:" + this.name; },
        set label(v) { this.name = v.toUpperCase(); }
    };
    const a = Object.create(proto); a.name = "a";
    const b = Object.create(proto); b.name = "b";
    shouldBe(new Thread(o => o.label, a).join(), "label:a");
    shouldBe(new Thread(o => o.label, b).join(), "label:b");
    new Thread(o => { o.label = "renamed"; }, a).join();
    shouldBe(a.name, "RENAMED"); // setter wrote through receiver a
    shouldBe(b.name, "b");       // b untouched
    shouldBeFalse(Object.prototype.hasOwnProperty.call(proto, "name"));
}

// --- A getter that throws: the exception propagates out of the thread and
// is rethrown by join() with identity preserved.
{
    const err = new Error("getter-boom");
    const obj = { get trap() { throw err; } };
    const t = new Thread(o => o.trap, obj);
    shouldBe(shouldThrow(() => t.join()), err);
}

// --- Redefining data -> accessor from a foreign thread, then accessor ->
// data from another.
{
    const obj = { p: 1 };
    new Thread(o => {
        Object.defineProperty(o, "p", { get() { return 2; }, configurable: true });
    }, obj).join();
    shouldBe(obj.p, 2);
    shouldBe(typeof Object.getOwnPropertyDescriptor(obj, "p").get, "function");
    new Thread(o => {
        Object.defineProperty(o, "p", { value: 3, writable: true });
    }, obj).join();
    shouldBe(obj.p, 3);
    shouldBe(Object.getOwnPropertyDescriptor(obj, "p").get, undefined);
}

// --- Getter-only property: foreign strict write throws TypeError, sloppy
// write is a silent no-op.
{
    const obj = { get fixed() { return "ro"; } };
    const sloppy = new Thread(o => { o.fixed = "ignored"; return o.fixed; }, obj).join();
    shouldBe(sloppy, "ro");
    const strictName = new Thread(o => {
        "use strict";
        try { o.fixed = "ignored"; return null; } catch (e) { return e.constructor.name; }
    }, obj).join();
    shouldBe(strictName, "TypeError");
}

// --- A getter created inside a thread (closure over thread-created state)
// keeps working from the main thread after the thread has exited.
{
    const obj = new Thread(() => {
        let count = 0;
        return { get next() { return ++count; } };
    }).join();
    shouldBe(obj.next, 1);
    shouldBe(obj.next, 2);
    shouldBe(new Thread(o => o.next, obj).join(), 3);
    shouldBe(obj.next, 4);
}

// --- Accessors interleaved with ThreadLocal: same getter, per-thread result.
{
    const tls = new ThreadLocal();
    tls.value = "main-flavor";
    const obj = { get flavor() { return tls.value; } };
    shouldBe(obj.flavor, "main-flavor");
    const fromThread = new Thread(o => {
        tls.value = "thread-flavor";
        return o.flavor;
    }, obj).join();
    shouldBe(fromThread, "thread-flavor");
    shouldBe(obj.flavor, "main-flavor"); // main's slot untouched
}

// --- __defineGetter__-style legacy definition cross-thread.
{
    const obj = {};
    new Thread(o => { o.__defineGetter__("legacy", function() { return 77; }); }, obj).join();
    shouldBe(obj.legacy, 77);
}
