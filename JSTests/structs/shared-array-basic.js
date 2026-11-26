function assert(c, m) { if (!c) throw new Error('assertion failed: ' + m); }
function deadlocks(fn) { try { fn(); } catch (e) { return e instanceof TypeError; } return false; }
function throws(f, type, m) {
    try { f(); } catch (e) {
        assert(e instanceof type, m + ': wrong error ' + e);
        return;
    }
    assert(false, m + ': did not throw');
}

// The global.
assert(typeof SharedArray === "function", "SharedArray is a function");
const global = Object.getOwnPropertyDescriptor(globalThis, "SharedArray");
assert(global.writable && global.configurable && !global.enumerable, "global property attributes");
assert(SharedArray.prototype === undefined, "SharedArray.prototype is absent");
assert(!Object.prototype.hasOwnProperty.call(SharedArray, "prototype"), "no own prototype property");
assert(SharedArray.name === "SharedArray" && SharedArray.length === 0, "name and length");
throws(() => SharedArray(3), TypeError, "called without new");

// Length.
assert(new SharedArray().length === 0, "no argument is length 0");
assert(new SharedArray(2).length === 2, "one argument is the length");
throws(() => new SharedArray("2"), TypeError, "the length must be a Number");
throws(() => new SharedArray(1.5), TypeError, "the length must be integral");
throws(() => new SharedArray(NaN), TypeError, "NaN is not integral");
throws(() => new SharedArray(Infinity), TypeError, "Infinity is not integral");
throws(() => new SharedArray(-1), RangeError, "negative length");
throws(() => new SharedArray(2 ** 32), RangeError, "huge length");

// Two or more arguments are the elements.
const filled = new SharedArray(1, "x", 2n);
assert(filled.length === 3 && filled[0] === 1 && filled[1] === "x" && filled[2] === 2n, "values become the elements");
assert(new SharedArray(1, 2).length === 2 && new SharedArray(5, 6)[0] === 5, "two numbers are elements, not a length");
throws(() => new SharedArray(1, {}), TypeError, "an element must be shareable");

// Shape of an instance.
const a = new SharedArray(3);
assert(Object.getPrototypeOf(a) === null, "null prototype");
assert(!Object.isExtensible(a), "not extensible");
assert(a.length === 3, "length");
const length = Object.getOwnPropertyDescriptor(a, "length");
assert(length.value === 3 && !length.writable && !length.enumerable && !length.configurable, "length descriptor");
assert(a[0] === undefined && a[1] === undefined && a[2] === undefined, "elements start undefined");
const element = Object.getOwnPropertyDescriptor(a, 1);
assert(element.value === undefined && element.writable && element.enumerable && !element.configurable, "element descriptor");
assert(Object.prototype.toString.call(a) === "[object Object]", "toString tag");
assert(!Array.isArray(a), "not an array");
assert(typeof a === "object", "typeof");

// Elements hold primitives.
a[0] = 1.5;
a[1] = "two";
a[2] = 3n;
assert(a[0] === 1.5 && a[1] === "two" && a[2] === 3n, "stored primitives");
a[0] = null; assert(a[0] === null, "null");
a[0] = true; assert(a[0] === true, "boolean");
a[0] = NaN; assert(Number.isNaN(a[0]), "NaN");
a[0] = -0; assert(Object.is(a[0], -0), "negative zero");
a[1] = "\ud800"; assert(a[1] === "\ud800", "a lone surrogate survives");
a["0"] = 7; assert(a[0] === 7, "string index");

// Out of range reads are undefined, writes fail without adding anything.
assert(a[3] === undefined && a[-1] === undefined && a.foo === undefined && a[1e10] === undefined, "out of range reads");
a[5] = 1;
a.foo = 1;
a.length = 10;
a[3] = 1;
assert(a[5] === undefined && a.foo === undefined && a[3] === undefined && a.length === 3, "sloppy writes fail silently");
throws(() => { "use strict"; a[5] = 1; }, TypeError, "strict write past the end");
throws(() => { "use strict"; a[3] = 1; }, TypeError, "strict write at the length");
throws(() => { "use strict"; a.foo = 1; }, TypeError, "strict write of a new key");
throws(() => { "use strict"; a.length = 10; }, TypeError, "strict write of length");
assert(a.length === 3, "length unchanged");
assert(delete a[7] === false && delete a.foo === false, "[[Delete]] is false for what is absent too");
assert(delete a[0] === false && delete a.length === false, "sloppy delete of what exists fails");
throws(() => { "use strict"; delete a[7]; }, TypeError, "strict delete of an absent element");
throws(() => { "use strict"; delete a[0]; }, TypeError, "strict delete of an element");
throws(() => { "use strict"; delete a.length; }, TypeError, "strict delete of length");
assert(a[0] === 7, "still there");

// Values that cannot be shared.
throws(() => { a[0] = {}; }, TypeError, "plain object");
throws(() => { a[0] = []; }, TypeError, "array");
throws(() => { a[0] = () => 1; }, TypeError, "function");
{ const before = a[0], symbol = Symbol("s"); a[0] = symbol; assert(a[0] === symbol, "a symbol can be shared"); a[0] = before; }
throws(() => { a[0] = new Map(); }, TypeError, "map");
throws(() => { a[0] = new SharedArrayBuffer(8); }, TypeError, "SharedArrayBuffer");
throws(() => { "use strict"; a[0] = {}; }, TypeError, "plain object in strict code");
throws(() => Object.defineProperty(a, 0, { value: {} }), TypeError, "defineProperty with an object");
assert(a[0] === 7, "failed writes change nothing");

// Shareable objects: SharedArrays, shared structs, Mutex, Condition.
const inner = new SharedArray(2);
a[0] = inner;
assert(a[0] === inner, "nested identity");
assert(a[0] === a[0], "repeated reads are the same object");
a[1] = a;
assert(a[1] === a, "an array can hold itself");
shared struct Point { x; y; }
const point = new Point();
point.x = 4;
a[2] = point;
assert(a[2] === point && a[2].x === 4, "a shared struct element");
const mutex = new Atomics.Mutex();
a[0] = mutex;
assert(Object.getPrototypeOf(a[0]) === null && a[0] === a[0], "a mutex element");
{
    using token = Atomics.Mutex.lock(a[0]);
    assert(deadlocks(() => Atomics.Mutex.lockIfAvailable(mutex, 0)), "the element is the mutex that was stored: this agent holds it");
}
const condition = new Atomics.Condition();
a[0] = condition;
assert(Object.getPrototypeOf(a[0]) === null && a[0] === a[0], "a condition element");
a[0] = inner;
point.x = inner;
assert(point.x === inner, "a shared struct field holds a SharedArray");
inner[0] = point;
assert(inner[0] === point && inner[0].x === inner, "and back");

// Reflect.canBeShared.
assert(Reflect.canBeShared(a), "an array can be shared");
assert(!Reflect.canBeShared({}) && !Reflect.canBeShared([]) && !Reflect.canBeShared(() => 1), "ordinary objects cannot");
assert(Reflect.canBeShared(1) && Reflect.canBeShared("s") && Reflect.canBeShared(null), "primitives can");

// Keys.
const keys = new SharedArray(3);
assert(Object.keys(keys).join() === "0,1,2", "Object.keys order");
assert(Object.getOwnPropertyNames(keys).join() === "0,1,2,length", "own property names");
assert(Reflect.ownKeys(keys).join() === "0,1,2,length", "ownKeys");
assert(Object.getOwnPropertySymbols(keys).length === 0, "no symbols");
assert(Object.entries(keys).length === 3 && Object.values(keys).length === 3, "entries and values");
const seen = [];
for (const key in keys)
    seen.push(key);
assert(seen.join() === "0,1,2", "for-in order");
assert(new SharedArray(0).length === 0 && Object.keys(new SharedArray(0)).length === 0, "empty array");
keys[0] = 1; keys[1] = 2; keys[2] = 3;
assert(JSON.stringify(keys) === '{"0":1,"1":2,"2":3}', "JSON.stringify");
const copy = { ...keys };
assert(copy[0] === 1 && copy[2] === 3 && !("length" in copy), "spread copies the elements");
assert(Array.prototype.slice.call(keys).join() === "1,2,3", "array-like to array");
assert(Array.from(keys).join() === "1,2,3", "Array.from over the array-like");

// in and hasOwnProperty.
assert(0 in keys && 2 in keys && !(3 in keys) && "length" in keys && !("foo" in keys), "in");
assert(Object.prototype.hasOwnProperty.call(keys, 0) && Object.prototype.hasOwnProperty.call(keys, "length") && !Object.prototype.hasOwnProperty.call(keys, 3), "hasOwnProperty");

// Sealed.
assert(Object.isSealed(keys), "sealed");
assert(!Object.isFrozen(keys), "not frozen: the elements are writable");
assert(Object.isFrozen(new SharedArray(0)), "an empty one is frozen");
throws(() => Object.freeze(keys), TypeError, "freeze would make an element read-only");
throws(() => Object.setPrototypeOf(keys, {}), TypeError, "immutable prototype");
assert(Object.setPrototypeOf(keys, null) === keys, "same prototype is fine");
throws(() => Object.defineProperty(keys, 5, { value: 1 }), TypeError, "defineProperty past the end");
throws(() => Object.defineProperty(keys, "foo", { value: 1 }), TypeError, "defineProperty of a new key");
assert(Reflect.defineProperty(keys, 5, { value: 1 }) === false, "Reflect.defineProperty past the end");
Object.defineProperty(keys, 0, { value: 9 });
assert(keys[0] === 9, "defineProperty can store a value");
Object.defineProperty(keys, 0, { value: 10, writable: true, enumerable: true, configurable: false });
assert(keys[0] === 10, "with the same attributes");
throws(() => Object.defineProperty(keys, 0, { configurable: true }), TypeError, "configurable element");
throws(() => Object.defineProperty(keys, 0, { enumerable: false }), TypeError, "non-enumerable element");
throws(() => Object.defineProperty(keys, 0, { writable: false }), TypeError, "read-only element");
{ const before = keys[0]; assert(Reflect.defineProperty(keys, 0, { get() { return 1; } }) === true && keys[0] === before, "an accessor descriptor changes nothing"); }
Object.defineProperty(keys, "length", { value: 3 });
Object.defineProperty(keys, "length", { writable: false });
throws(() => Object.defineProperty(keys, "length", { value: 4 }), TypeError, "change the length");
throws(() => Object.defineProperty(keys, "length", { writable: true }), TypeError, "writable length");
throws(() => Object.defineProperty(keys, "length", { configurable: true }), TypeError, "configurable length");
assert(keys.length === 3, "length is still 3");

// Atomics.
const t = new SharedArray(3);
assert(Atomics.store(t, "0", 7) === 7, "store returns the value");
assert(Atomics.load(t, "0") === 7, "load");
assert(Atomics.load(t, "0") === 7, "the property key of an element");
throws(() => Atomics.load(t, 0), TypeError, "a Number is not a property key");
throws(() => Atomics.load(t, 0.9), TypeError, "a Number that is not an index is not a property key");
assert(Atomics.exchange(t, "0", "x") === 7 && t[0] === "x", "exchange returns the old value");
assert(Atomics.compareExchange(t, "0", "x", "y") === "x" && t[0] === "y", "compareExchange that matches");
assert(Atomics.compareExchange(t, "0", "x", "z") === "y" && t[0] === "y", "compareExchange that does not match");
assert(Atomics.compareExchange(t, "0", {}, "z") === "y", "an expected value that cannot match is not an error");
assert(Atomics.compareExchange(t, "1", undefined, 5) === undefined && t[1] === 5, "compareExchange on undefined");
assert(Atomics.compareExchange(t, "1", 5n, 6) === 5 && t[1] === 5, "5n is not 5");
Atomics.store(t, "2", inner);
assert(Atomics.load(t, "2") === inner, "an array through Atomics");
assert(Atomics.compareExchange(t, "2", inner, null) === inner && t[2] === null, "compareExchange on an array identity");
throws(() => Atomics.load(t, "3"), RangeError, "load past the end");
throws(() => Atomics.store(t, -1, 1), TypeError, "store at -1");
throws(() => Atomics.exchange(t, Infinity, 1), TypeError, "exchange at Infinity");
throws(() => Atomics.compareExchange(t, "3", 1, 2), RangeError, "compareExchange past the end");
throws(() => Atomics.store(t, "0", {}), TypeError, "store of an object");
throws(() => Atomics.exchange(t, "0", () => 1), TypeError, "exchange of a function");
throws(() => Atomics.compareExchange(t, 0, 1, {}), TypeError, "compareExchange of an object");
throws(() => Atomics.add(t, 0, 1), TypeError, "add is not extended");
assert(t[0] === "y", "failed Atomics calls change nothing");
