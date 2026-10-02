// Making an object's properties immutable changes nothing for the objects that inherit from it: property attributes stay as they
// are, so an assignment on an inheritor that finds the name on that prototype creates an own property, as it always did.

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

// Object.create() of a prototype with immutable properties.
{
    let prototype = $vm.makePropertiesImmutable({ name: "base", count: 0, greet() { return "hi " + this.name; } });
    let instance = Object.create(prototype);
    shouldBe($vm.hasImmutableProperties(instance), false);
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

// Classes: a constructor and prototype with immutable properties can still be instantiated and extended.
{
    class Base {
        constructor() { this.name = "Base"; this.kind = "instance"; }
        describe() { return this.name + ":" + this.kind; }
        static create() { return new this(); }
    }
    Base.prototype.name = "on prototype";
    Base.prototype.kind = "prototype";
    $vm.makePropertiesImmutable(Base);
    $vm.makePropertiesImmutable(Base.prototype);

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

// Error subclasses whose constructor assigns `name`, with Error.prototype's properties immutable.
{
    $vm.makePropertiesImmutable(Error.prototype);
    class MyError extends Error {
        constructor(message) { super(message); this.name = "MyError"; }
    }
    let error = new MyError("m");
    shouldBe(error.name, "MyError");
    shouldBe(String(error), "MyError: m");
    shouldBe(Error.prototype.name, "Error");
    function OldStyle() { this.toString = function () { return "own toString"; }; }
    $vm.makePropertiesImmutable(Object.prototype);
    shouldBe(String(new OldStyle()), "own toString");
    shouldBe(Object.prototype.toString.call(1), "[object Number]");
}

// An accessor on such a prototype: the setter runs for an ordinary inheritor, and does not run for a receiver whose own
// properties are immutable.
{
    let calls = 0;
    let prototype = $vm.makePropertiesImmutable({ set value(v) { calls++; this.stored = v; }, get value() { return this.stored; } });
    let instance = Object.create(prototype);
    instance.value = 5;
    shouldBe(calls, 1);
    shouldBe(instance.stored, 5);
    shouldBe(instance.value, 5);

    let immutableInstance = $vm.makePropertiesImmutable(Object.create(prototype));
    shouldThrow(() => { "use strict"; immutableInstance.value = 6; }, TypeError);
    shouldBe(calls, 1);
    shouldBe(Reflect.set(immutableInstance, "value", 6), false);
    shouldBe(calls, 1);
}

// Reflect.set() with a separate receiver: what matters is the receiver, not the object the lookup starts from.
{
    let immutable = $vm.makePropertiesImmutable({ a: 1 });
    let receiver = {};
    shouldBe(Reflect.set(immutable, "a", 2, receiver), true);
    shouldBe(receiver.a, 2);
    shouldBe(immutable.a, 1);

    let holder = { a: 1 };
    shouldBe(Reflect.set(holder, "a", 2, immutable), false);
    shouldBe(Reflect.set(holder, "fresh", 2, immutable), false);
    shouldBe(immutable.a, 1);
    shouldBe("fresh" in immutable, false);
}

// The object becomes a prototype afterwards.
{
    let immutable = $vm.makePropertiesImmutable({ a: 1 });
    let child = Object.create(immutable);
    let grandchild = Object.create(child);
    grandchild.a = 3;
    shouldBe(grandchild.a, 3);
    shouldBe(child.a, 1);
    shouldBe($vm.hasImmutableProperties(immutable), true);
    shouldThrow(() => { "use strict"; immutable.a = 2; }, TypeError);
}

// A put that starts on another object and lands on such a receiver while the receiver still has static properties that are not
// reified: it fails like any other put with that receiver, a native setter of a static property is not called, and nothing changes.
{
    let other = $vm.createGlobalObject();
    $vm.makePropertiesImmutable(other.JSON);
    shouldBe(Reflect.set({}, "parse", 1, other.JSON), false);
    shouldBe(Reflect.set({}, "added", 1, other.JSON), false);
    let holder = { __proto__: {}, assign(value) { "use strict"; super.parse = value; } };
    shouldThrow(() => holder.assign.call(other.JSON, 1), TypeError);
    shouldBe(typeof other.JSON.parse, "function");
    shouldBe("added" in other.JSON, false);

    let withStatics = $vm.makePropertiesImmutable($vm.createStaticCustomValue());
    for (let name of ["testStaticValue", "testStaticValueSetFlag", "testStaticValueNoSetter", "testStaticValueReadOnly", "added"])
        shouldBe(Reflect.set({}, name, 1, withStatics), false, name);
    shouldBe("testStaticValueSetterCalled" in withStatics, false);
    shouldBe(Object.hasOwn(withStatics, "added"), false);
}

// A put that starts on another object goes by the ordinary steps, in which the receiver's [[DefineOwnProperty]] decides: it fails
// unless it changes nothing, and on every route to the receiver alike: an ordinary key, a key the receiver's class treats
// specially, an index, a Proxy as the object it started on, and super. (A frozen receiver fails sooner, on its attribute.)
{
    let object = $vm.makePropertiesImmutable({ a: 1 });
    let array = $vm.makePropertiesImmutable([1, 2, 3]);
    let func = $vm.makePropertiesImmutable(function () { });
    let proxy = new Proxy({}, {});
    let frozen = Object.freeze({ a: 1 });
    for (let i = 0; i < 100; ++i) {
        for (let [target, key, same, different] of [[object, "a", 1, 2], [array, "length", 3, 2], [array, 0, 1, 2], [array, 3, undefined, 2], [func, "prototype", func.prototype, {}]]) {
            for (let start of [{}, proxy]) {
                if (same !== undefined)
                    shouldBe(Reflect.set(start, key, same, target), true, String(key) + " same");
                shouldBe(Reflect.set(start, key, different, target), false, String(key) + " different");
            }
        }
    }
    shouldBe(Reflect.set({}, "a", 1, frozen), false);
    let holder = { __proto__: {}, assign(value) { "use strict"; super.a = value; } };
    holder.assign.call(object, 1);
    shouldThrow(() => holder.assign.call(object, 2), TypeError);
    shouldBe(object.a, 1);
    shouldBe(array.length, 3);
    shouldBe(Object.keys(array).join(), "0,1,2");

    // A setter of another object runs with such a receiver, as it does with any receiver; what it puts on the receiver fails.
    let calls = 0;
    let withSetter = { set a(value) { calls++; shouldBe(Reflect.set(this, "a", value), false); } };
    shouldBe(Reflect.set(withSetter, "a", 5, object), true);
    shouldBe(calls, 1);
    shouldBe(object.a, 1);
}

// The same when the put starts on an object that inherits from the receiver and cannot take the fast path (it has a getter), so
// that the walk up the prototype chain meets the receiver itself, with a static property that is not reified.
{
    let other = $vm.createGlobalObject();
    $vm.makePropertiesImmutable(other.JSON);
    let inheritor = Object.create(other.JSON, { unrelated: { get() { return 1; } } });
    shouldBe(Reflect.set(inheritor, "stringify", 42, other.JSON), false);
    shouldBe(typeof other.JSON.stringify, "function");
    shouldBe(Reflect.set(inheritor, "stringify", 42), true);
    shouldBe(inheritor.stringify, 42);

    let withStatics = $vm.makePropertiesImmutable($vm.createStaticCustomValue());
    let inheritorOfStatics = Object.create(withStatics, { unrelated: { get() { return 1; } } });
    for (let name of ["testStaticValue", "testStaticValueSetFlag", "testStaticValueNoSetter"])
        shouldBe(Reflect.set(inheritorOfStatics, name, 1, withStatics), false, name);
    shouldBe("testStaticValueSetterCalled" in withStatics, false);
}

// An object that inherits from such an array gets an own length when it assigns one, as with an ordinary array: the array has a
// length, so nothing further up the prototype chain is asked, neither a setter nor a frozen Array.prototype.
for (let prepare of [array => array, array => { delete array[1]; return array; }]) {
    class WithLengthSetter extends Array { set length(value) { this.setterCalled = true; } }
    for (let change of [array => array, array => $vm.makePropertiesImmutable(array)]) {
        let child = Object.create(change(prepare(new WithLengthSetter(1, 2, 3))));
        child.length = 0;
        shouldBe(Object.hasOwn(child, "length"), true);
        shouldBe("setterCalled" in child, false);

        let other = $vm.createGlobalObject();
        let array = change(prepare(new other.Array(1, 2, 3)));
        Object.freeze(other.Array.prototype);
        let strictChild = Object.create(array);
        (function () { "use strict"; strictChild.length = 7; })();
        shouldBe(strictChild.length, 7);
        shouldBe(array.length, 3);
    }
}
