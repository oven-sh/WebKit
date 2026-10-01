// Locking an object changes nothing for the objects that inherit from it: property attributes stay as they are, so an
// assignment on an inheritor that finds the name on the locked prototype creates an own property, as it always did.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + String(expected) + " but got " + String(actual));
}

function shouldThrow(func, errorType) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof errorType))
        throw new Error("expected " + errorType.name + " but got " + String(error));
}

// Object.create() of a locked prototype.
{
    let prototype = $vm.lockProperties({ name: "base", count: 0, greet() { return "hi " + this.name; } });
    let instance = Object.create(prototype);
    shouldBe($vm.didLockProperties(instance), false);
    shouldBe(Object.isExtensible(instance), true);
    (function () { "use strict"; instance.name = "mine"; instance.count++; instance.fresh = 1; })();
    shouldBe(instance.name, "mine");
    shouldBe(instance.count, 1);
    shouldBe(instance.fresh, 1);
    shouldBe(Object.hasOwn(instance, "name"), true);
    shouldBe(instance.greet(), "hi mine");
    shouldBe(prototype.name, "base");
    shouldBe(prototype.count, 0);
    shouldBe(delete instance.name, true);
    shouldBe(instance.name, "base");
}

// Classes: a locked constructor and prototype can still be instantiated and extended.
{
    class Base {
        constructor() { this.name = "Base"; this.kind = "instance"; }
        describe() { return this.name + ":" + this.kind; }
        static create() { return new this(); }
    }
    Base.prototype.name = "on prototype";
    Base.prototype.kind = "prototype";
    $vm.lockProperties(Base);
    $vm.lockProperties(Base.prototype);

    let base = new Base();
    shouldBe(base.describe(), "Base:instance");

    class Derived extends Base {
        constructor() { super(); this.name = "Derived"; }
        describe() { return "<" + super.describe() + ">"; }
    }
    Derived.label = "static on the subclass";
    let derived = Derived.create();
    shouldBe(derived.describe(), "<Derived:instance>");
    shouldBe(derived instanceof Base, true);
    shouldBe(Derived.label, "static on the subclass");
    shouldBe(Object.hasOwn(Base, "label"), false);

    shouldThrow(() => { "use strict"; Base.prototype.describe = function () { }; }, TypeError);
    shouldThrow(() => { "use strict"; Base.extra = 1; }, TypeError);
    shouldBe(Base.prototype.name, "on prototype");
}

// Error subclasses whose constructor assigns `name`, with Error.prototype locked.
{
    $vm.lockProperties(Error.prototype);
    class MyError extends Error {
        constructor(message) { super(message); this.name = "MyError"; }
    }
    let error = new MyError("m");
    shouldBe(error.name, "MyError");
    shouldBe(String(error), "MyError: m");
    shouldBe(Error.prototype.name, "Error");
    function OldStyle() { this.toString = function () { return "own toString"; }; }
    $vm.lockProperties(Object.prototype);
    shouldBe(String(new OldStyle()), "own toString");
    shouldBe(Object.prototype.toString.call(1), "[object Number]");
}

// An accessor on a locked prototype: the setter runs for an inheritor (the receiver is not locked), and does not run for a
// locked receiver.
{
    let calls = 0;
    let prototype = $vm.lockProperties({ set value(v) { calls++; this.stored = v; }, get value() { return this.stored; } });
    let instance = Object.create(prototype);
    instance.value = 5;
    shouldBe(calls, 1);
    shouldBe(instance.stored, 5);
    shouldBe(instance.value, 5);

    let lockedInstance = $vm.lockProperties(Object.create(prototype));
    shouldThrow(() => { "use strict"; lockedInstance.value = 6; }, TypeError);
    shouldBe(calls, 1);
    shouldBe(Reflect.set(lockedInstance, "value", 6), false);
    shouldBe(calls, 1);
}

// Reflect.set() with a separate receiver: what matters is the receiver, not the object the lookup starts from.
{
    let locked = $vm.lockProperties({ a: 1 });
    let receiver = {};
    shouldBe(Reflect.set(locked, "a", 2, receiver), true);
    shouldBe(receiver.a, 2);
    shouldBe(locked.a, 1);

    let holder = { a: 1 };
    shouldBe(Reflect.set(holder, "a", 2, locked), false);
    shouldBe(Reflect.set(holder, "fresh", 2, locked), false);
    shouldBe(locked.a, 1);
    shouldBe("fresh" in locked, false);
}

// A locked object becomes a prototype afterwards.
{
    let locked = $vm.lockProperties({ a: 1 });
    let child = Object.create(locked);
    let grandchild = Object.create(child);
    grandchild.a = 3;
    shouldBe(grandchild.a, 3);
    shouldBe(child.a, 1);
    shouldBe($vm.didLockProperties(locked), true);
    shouldThrow(() => { "use strict"; locked.a = 2; }, TypeError);
}
