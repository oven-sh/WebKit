//@ requireOptions("--useJSThreads=1")
// Shared-object semantics: prototype chains across threads. Prototypes are
// shared objects like any other; mutations to a prototype made on one thread
// must be visible through instances on every thread, [[Prototype]] itself can
// be swapped cross-thread, and method dispatch/instanceof must agree.
load("../resources/assert.js", "caller relative");

// --- A prototype property added on a foreign thread is visible through an
// instance on the main thread.
{
    const proto = { base: "base" };
    const obj = Object.create(proto);
    new Thread(p => { p.addedToProto = "proto-add"; }, proto).join();
    shouldBe(obj.addedToProto, "proto-add");
    shouldBeFalse(Object.prototype.hasOwnProperty.call(obj, "addedToProto"));
}

// --- A method installed on a constructor's prototype by a foreign thread is
// callable on existing instances, with correct `this`.
{
    function Point(x, y) { this.x = x; this.y = y; }
    const p = new Point(3, 4);
    new Thread(ctor => {
        ctor.prototype.norm = function() { return Math.sqrt(this.x * this.x + this.y * this.y); };
    }, Point).join();
    shouldBe(p.norm(), 5);
    // And a fresh instance made on another thread sees it too.
    shouldBe(new Thread(ctor => new ctor(6, 8).norm(), Point).join(), 10);
}

// --- Deep chain: writes at each level from different threads; lookup walks
// the chain correctly afterwards.
{
    const level2 = { fromL2: "l2" };
    const level1 = Object.create(level2);
    const level0 = Object.create(level1);
    new Thread(o => { o.fromL1 = "l1"; }, level1).join();
    new Thread(o => { o.fromL0 = "l0"; }, level0).join();
    shouldBe(level0.fromL0, "l0");
    shouldBe(level0.fromL1, "l1");
    shouldBe(level0.fromL2, "l2");
    shouldBe(Object.getPrototypeOf(Object.getPrototypeOf(level0)), level2);
}

// --- Shadowing from a foreign thread: instance write must shadow, and the
// shared prototype must be untouched (other instances unaffected).
{
    const proto = { v: "proto-v" };
    const a = Object.create(proto);
    const b = Object.create(proto);
    new Thread(o => { o.v = "a-own"; }, a).join();
    shouldBe(a.v, "a-own");
    shouldBe(b.v, "proto-v");
    shouldBe(proto.v, "proto-v");
}

// --- Prototype *value* mutation propagates to all threads' lookups.
{
    const proto = { setting: 1 };
    const obj = Object.create(proto);
    const seen = new Thread(o => {
        const before = o.setting;
        Object.getPrototypeOf(o).setting = 2;
        return before;
    }, obj).join();
    shouldBe(seen, 1);
    shouldBe(obj.setting, 2);
    shouldBe(proto.setting, 2);
}

// --- setPrototypeOf from a foreign thread: chain swap observed by main.
{
    const protoA = { which: "A", onlyA: true };
    const protoB = { which: "B", onlyB: true };
    const obj = Object.create(protoA);
    shouldBe(obj.which, "A");
    new Thread((o, p) => { Object.setPrototypeOf(o, p); }, obj, protoB).join();
    shouldBe(Object.getPrototypeOf(obj), protoB);
    shouldBe(obj.which, "B");
    shouldBe(obj.onlyA, undefined);
    shouldBeTrue(obj.onlyB);
}

// --- setPrototypeOf to null from a foreign thread.
{
    const obj = Object.create({ inherited: 1 });
    new Thread(o => { Object.setPrototypeOf(o, null); }, obj).join();
    shouldBe(Object.getPrototypeOf(obj), null);
    shouldBe(obj.inherited, undefined);
    shouldBe(obj.hasOwnProperty, undefined); // Object.prototype is gone too
}

// --- instanceof agrees across threads, including after a foreign-thread
// subclass is constructed.
{
    class Animal {}
    class Dog extends Animal {}
    const dog = new Thread(D => new D(), Dog).join();
    shouldBeTrue(dog instanceof Dog);
    shouldBeTrue(dog instanceof Animal);
    shouldBe(new Thread((d, A) => d instanceof A, dog, Animal).join(), true);
}

// --- A class defined inside a thread, returned to main: its methods and
// prototype chain work on the main thread.
{
    const instance = new Thread(() => {
        class Counter {
            constructor() { this.n = 0; }
            bump() { return ++this.n; }
        }
        return new Counter();
    }).join();
    shouldBe(instance.bump(), 1);
    shouldBe(instance.bump(), 2);
    shouldBe(instance.n, 2);
}

// --- super dispatch through a shared chain, invoked from a foreign thread.
{
    class Base {
        describe() { return "base"; }
    }
    class Derived extends Base {
        describe() { return "derived+" + super.describe(); }
    }
    const d = new Derived();
    shouldBe(new Thread(o => o.describe(), d).join(), "derived+base");
}

// --- Deleting a prototype's property on one thread exposes the next level
// of the chain to lookups everywhere.
{
    const grandproto = { p: "grand" };
    const proto = Object.create(grandproto);
    proto.p = "middle";
    const obj = Object.create(proto);
    shouldBe(obj.p, "middle");
    new Thread(mid => { delete mid.p; }, proto).join();
    shouldBe(obj.p, "grand");
}

// --- Object.prototype itself is shared: a property added there from a
// foreign thread is visible on plain objects on the main thread. Clean up
// afterwards so we don't poison the rest of the test.
{
    new Thread(() => { Object.prototype.__sharedObjectsTestTemp = 123; }).join();
    shouldBe({}.__sharedObjectsTestTemp, 123);
    const cleaned = new Thread(() => delete Object.prototype.__sharedObjectsTestTemp).join();
    shouldBeTrue(cleaned);
    shouldBe({}.__sharedObjectsTestTemp, undefined);
}

// --- Cyclic prototype chains must still be rejected when the cycle would be
// created cross-thread.
{
    const a = {};
    const b = Object.create(a);
    const threw = new Thread((x, y) => {
        try {
            Object.setPrototypeOf(x, y); // would make a -> b -> a
            return false;
        } catch (e) {
            return e instanceof TypeError;
        }
    }, a, b).join();
    shouldBeTrue(threw);
    shouldBe(Object.getPrototypeOf(a), Object.prototype);
}
