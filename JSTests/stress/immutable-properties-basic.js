// $vm.makePropertiesImmutable(object) calls JSObject::makePropertiesImmutable(): the object becomes non-extensible, and a put with it as the receiver, a define or delete that
// would change one of its own properties, and a change of its prototype all fail. Property attributes do not change.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + String(expected) + " but got " + String(actual));
}

function shouldThrow(func, errorType, message) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof errorType))
        throw new Error((message ? message + ": " : "") + "expected " + errorType.name + " but got " + String(error));
}

function snapshot(object) {
    return JSON.stringify(Reflect.ownKeys(object).map(key => {
        let d = Object.getOwnPropertyDescriptor(object, key);
        return [String(key), "value" in d ? String(d.value) : "accessor", d.writable, d.enumerable, d.configurable, typeof d.get, typeof d.set];
    })) + " proto:" + (Object.getPrototypeOf(object) === Object.prototype) + " extensible:" + Object.isExtensible(object);
}

let symbol = Symbol("s");
function make() {
    return { a: 1, b: "two", [symbol]: 3, get g() { return 4; }, set g(v) { throw new Error("setter must not run"); } };
}

// It returns the object, is reported by hasImmutableProperties(), and is idempotent.
{
    let object = make();
    shouldBe($vm.hasImmutableProperties(object), false);
    shouldBe($vm.makePropertiesImmutable(object), object);
    shouldBe($vm.hasImmutableProperties(object), true);
    shouldBe($vm.makePropertiesImmutable(object), object);
    shouldBe($vm.hasImmutableProperties(1), false);
    shouldThrow(() => $vm.makePropertiesImmutable(1), TypeError);
}

// Attributes are untouched; the object is non-extensible; it is not thereby sealed or frozen.
{
    let object = make();
    let before = snapshot(object).replace("extensible:true", "extensible:false");
    $vm.makePropertiesImmutable(object);
    shouldBe(snapshot(object), before);
    shouldBe(Object.getOwnPropertyDescriptor(object, "a").writable, true);
    shouldBe(Object.getOwnPropertyDescriptor(object, "a").configurable, true);
    shouldBe(Object.isExtensible(object), false);
    shouldBe(Reflect.isExtensible(object), false);
    shouldBe(Object.isSealed(object), false);
    shouldBe(Object.isFrozen(object), false);
}

// [[Set]] with the object as receiver.
{
    let object = $vm.makePropertiesImmutable(make());
    let before = snapshot(object);
    (function () { object.a = 9; object.fresh = 9; object[symbol] = 9; object[0] = 9; object.g = 9; })(); // sloppy: ignored
    shouldBe(snapshot(object), before);
    shouldThrow(() => { "use strict"; object.a = 9; }, TypeError);
    shouldThrow(() => { "use strict"; object.fresh = 9; }, TypeError);
    shouldThrow(() => { "use strict"; object[symbol] = 9; }, TypeError);
    shouldThrow(() => { "use strict"; object[0] = 9; }, TypeError);
    shouldThrow(() => { "use strict"; object.g = 9; }, TypeError); // the setter does not run
    shouldBe(Reflect.set(object, "a", 9), false);
    shouldBe(Reflect.set(object, "fresh", 9), false);
    shouldBe(Reflect.set(object, "g", 9), false);
    shouldBe(snapshot(object), before);
    shouldBe(object.a, 1);
    shouldBe(object.g, 4);
}

// [[DefineOwnProperty]]: a definition that changes nothing succeeds, every other one fails.
{
    let object = $vm.makePropertiesImmutable(make());
    let before = snapshot(object);
    let getter = Object.getOwnPropertyDescriptor(object, "g").get;
    shouldBe(Reflect.defineProperty(object, "a", {}), true);
    shouldBe(Reflect.defineProperty(object, "a", { value: 1 }), true);
    shouldBe(Reflect.defineProperty(object, "a", { writable: true, enumerable: true }), true);
    shouldBe(Reflect.defineProperty(object, "a", { value: 1, writable: true, enumerable: true, configurable: true }), true);
    shouldBe(Reflect.defineProperty(object, "g", { get: getter }), true);
    shouldBe(Reflect.defineProperty(object, "g", { enumerable: true, configurable: true }), true);
    shouldBe(Object.defineProperty(object, "a", { value: 1 }), object);

    shouldBe(Reflect.defineProperty(object, "a", { value: 2 }), false);
    shouldBe(Reflect.defineProperty(object, "a", { writable: false }), false);
    shouldBe(Reflect.defineProperty(object, "a", { enumerable: false }), false);
    shouldBe(Reflect.defineProperty(object, "a", { configurable: false }), false);
    shouldBe(Reflect.defineProperty(object, "a", { get() { } }), false);
    shouldBe(Reflect.defineProperty(object, "g", { get() { } }), false);
    shouldBe(Reflect.defineProperty(object, "g", { value: 1 }), false);
    shouldBe(Reflect.defineProperty(object, "fresh", { value: 1 }), false);
    shouldBe(Reflect.defineProperty(object, 0, { value: 1 }), false);
    shouldBe(Reflect.defineProperty(object, Symbol(), { value: 1 }), false);
    shouldThrow(() => Object.defineProperty(object, "a", { value: 2 }), TypeError);
    shouldThrow(() => Object.defineProperty(object, "fresh", { value: 1 }), TypeError);
    shouldThrow(() => Object.defineProperties(object, { a: { value: 2 } }), TypeError);
    shouldThrow(() => object.__defineGetter__("a", function () { }), TypeError);
    shouldThrow(() => object.__defineSetter__("fresh", function () { }), TypeError);
    shouldBe(snapshot(object), before);
}

// [[Delete]]: an existing property stays; a property the object does not have deletes to true.
{
    let object = $vm.makePropertiesImmutable(make());
    let before = snapshot(object);
    shouldBe(delete object.a, false);
    shouldBe(Reflect.deleteProperty(object, "a"), false);
    shouldBe(Reflect.deleteProperty(object, symbol), false);
    shouldBe(Reflect.deleteProperty(object, "g"), false);
    shouldBe(Reflect.deleteProperty(object, "missing"), true);
    shouldBe(Reflect.deleteProperty(object, 0), true);
    shouldBe(delete object.missing, true);
    shouldThrow(() => { "use strict"; delete object.a; }, TypeError);
    shouldBe(snapshot(object), before);
}

// [[SetPrototypeOf]]: only the prototype it already has.
{
    let object = $vm.makePropertiesImmutable(make());
    shouldBe(Reflect.setPrototypeOf(object, Object.prototype), true);
    shouldBe(Object.setPrototypeOf(object, Object.prototype), object);
    shouldBe(Reflect.setPrototypeOf(object, null), false);
    shouldBe(Reflect.setPrototypeOf(object, {}), false);
    shouldThrow(() => Object.setPrototypeOf(object, null), TypeError);
    // `object.__proto__ = value` is a [[Set]] with the object as the receiver: refused before the setter runs.
    (function () { object.__proto__ = {}; })();
    shouldThrow(() => { "use strict"; object.__proto__ = {}; }, TypeError);
    shouldBe(Object.getPrototypeOf(object), Object.prototype);

    let nullProto = $vm.makePropertiesImmutable(Object.create(null));
    shouldBe(Reflect.setPrototypeOf(nullProto, null), true);
    shouldBe(Reflect.setPrototypeOf(nullProto, Object.prototype), false);
}

// [[PreventExtensions]] has nothing left to do. Object.seal() and Object.freeze() would change attributes, so they fail,
// unless there is nothing for them to change.
{
    let object = $vm.makePropertiesImmutable(make());
    let before = snapshot(object);
    shouldBe(Reflect.preventExtensions(object), true);
    shouldBe(Object.preventExtensions(object), object);
    shouldThrow(() => Object.seal(object), TypeError);
    shouldThrow(() => Object.freeze(object), TypeError);
    shouldBe(snapshot(object), before);

    let empty = $vm.makePropertiesImmutable({});
    shouldBe(Object.freeze(empty), empty);
    shouldBe(Object.isFrozen(empty), true);

    let alreadyFrozen = $vm.makePropertiesImmutable(Object.freeze({ a: 1 }));
    shouldBe(Object.freeze(alreadyFrozen), alreadyFrozen);
    shouldBe(Object.seal(alreadyFrozen), alreadyFrozen);
}

// Bulk writers.
{
    let object = $vm.makePropertiesImmutable(make());
    let before = snapshot(object);
    shouldThrow(() => Object.assign(object, { a: 2, z: 3 }), TypeError);
    shouldThrow(() => Object.assign(object, { z: 3 }), TypeError);
    shouldBe(snapshot(object), before);

    // A copy's properties are not immutable.
    let copy = { ...object };
    shouldBe($vm.hasImmutableProperties(copy), false);
    copy.a = 7;
    copy.fresh = 8;
    shouldBe(copy.a, 7);
    shouldBe(copy.fresh, 8);
    let assigned = Object.assign({}, object);
    shouldBe($vm.hasImmutableProperties(assigned), false);
    assigned.a = 7;
    shouldBe(assigned.a, 7);
}

// JSON.parse with a reviver that makes the holder's properties immutable: the reviver's results are not stored into it.
{
    let result = JSON.parse('{"a":1,"b":2}', function (key, value) {
        if (key === "a")
            $vm.makePropertiesImmutable(this);
        return typeof value === "number" ? value * 10 : value;
    });
    shouldBe(result.a, 1);
    shouldBe(result.b, 2);
}

// Reads are unaffected.
{
    let object = $vm.makePropertiesImmutable(make());
    shouldBe(object.a, 1);
    shouldBe(object[symbol], 3);
    shouldBe("a" in object, true);
    shouldBe(Object.keys(object).join(), "a,b,g");
    shouldBe(JSON.stringify(object), '{"a":1,"b":"two","g":4}');
    let keys = [];
    for (let key in object)
        keys.push(key);
    shouldBe(keys.join(), "a,b,g");
}

// Internal slots are not properties: they are not affected.
{
    let map = $vm.makePropertiesImmutable(new Map());
    map.set("k", 1);
    shouldBe(map.get("k"), 1);
    shouldBe(map.size, 1);
    shouldThrow(() => { "use strict"; map.extra = 1; }, TypeError);

    let set = $vm.makePropertiesImmutable(new Set());
    set.add(1);
    shouldBe(set.has(1), true);

    let date = $vm.makePropertiesImmutable(new Date(0));
    date.setTime(5);
    shouldBe(date.getTime(), 5);

    let weakMap = $vm.makePropertiesImmutable(new WeakMap());
    let key = {};
    weakMap.set(key, 1);
    shouldBe(weakMap.get(key), 1);

    let promise = $vm.makePropertiesImmutable(Promise.resolve(1));
    let seen;
    promise.then(value => { seen = value; });
    drainMicrotasks();
    shouldBe(seen, 1);
}

// Classes that do not support it: nothing changes and $vm.makePropertiesImmutable() throws.
{
    let proxy = new Proxy({}, {});
    shouldThrow(() => $vm.makePropertiesImmutable(proxy), TypeError);
    shouldBe($vm.hasImmutableProperties(proxy), false);
    let typedArray = new Uint8Array(4);
    shouldThrow(() => $vm.makePropertiesImmutable(typedArray), TypeError);
    typedArray[0] = 7;
    shouldBe(typedArray[0], 7);
    let mapped = (function (a) { return arguments; })(1);
    shouldThrow(() => $vm.makePropertiesImmutable(mapped), TypeError);
    mapped[0] = 2;
    shouldBe(mapped[0], 2);
}
