//@ requireOptions("--immutableGlobals=*,-globalThis,-Map,-Date,-Math,-Object,-print,-noSuchGlobal,-undefined")

// ,-name after the * of --immutableGlobals leaves the global of that name as it is and, if it is a function, its "prototype". A name
// that is not there, or whose value is not an object, is ignored.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + String(expected) + " but got " + String(actual));
}

const isImmutable = $vm.hasImmutableProperties;

// Date and Math are created on first use.
for (let object of [globalThis, Map, Map.prototype, Map.prototype.get, Map.groupBy, Date, Date.prototype, Date.prototype.getTime, Date.now, Math, Math.max,
    Object, Object.prototype, Object.keys, Object.prototype.toString, print])
    shouldBe(isImmutable(object), false);

Map.prototype.get = () => "patched";
Date.now = () => 0;
Math.random = () => 0.5;
Object.prototype.extra = 1;
globalThis.Set = 1;
shouldBe(new Map().get() + Date.now() + Math.random() + [].extra + Set, "patched00.511");

// A class that is created later, and whose prototype inherits from Object.prototype, does not make Object.prototype immutable.
new Intl.Segmenter().segment("a").containing(0);
new Intl.DateTimeFormat().formatToParts();
new WeakRef({ });
shouldBe(isImmutable(Object.prototype), false);

// Everything else is covered: the unnamed prototype of Map's iterators, objects that inherit from an opted-out one, the other globals.
for (let object of [Object.getPrototypeOf(new Map().entries()), WeakMap, WeakMap.prototype, Array.prototype, Function.prototype, JSON, Intl, Intl.Segmenter.prototype,
    parseInt, debug, performance, Reflect, Reflect.get])
    shouldBe(isImmutable(object), true);

// Each new global object looks the names up for itself.
{
    let other = $vm.createGlobalObject();
    for (let object of [other, other.Map.prototype, other.Math])
        shouldBe(isImmutable(object), false);
    shouldBe(isImmutable(other.WeakMap.prototype), true);
    shouldBe("print" in other, false);
}
