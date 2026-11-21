// Symbols as shared values, [[Delete]], accessor descriptors, the receiver of [[Set]], the checks of Atomics on a shared
// struct field or a SharedArray element, and the limits of the SharedArray constructor.
function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }
function throws(fn, type, m) {
    try { fn(); } catch (e) { assert(e instanceof type, m + " threw " + e); return; }
    throw new Error("did not throw: " + m);
}

shared struct Cell { v; w; }
const cell = new Cell();
const array = new SharedArray(4);

// Symbols can be shared: unique, registered and well-known ones keep their identity and their description.
for (const holder of [{ get: () => cell.v, set: x => { cell.v = x; } }, { get: () => array[1], set: x => { array[1] = x; } }]) {
    const unique = Symbol("described");
    holder.set(unique);
    assert(holder.get() === unique && typeof holder.get() === "symbol", "a unique symbol keeps its identity");
    assert(holder.get().description === "described", "its description");
    const bare = Symbol();
    holder.set(bare);
    assert(holder.get() === bare && holder.get().description === undefined, "a symbol without a description");
    const empty = Symbol("");
    holder.set(empty);
    assert(holder.get() === empty && holder.get().description === "", "a symbol with an empty description");
    holder.set(Symbol.for("registered"));
    assert(holder.get() === Symbol.for("registered") && Symbol.keyFor(holder.get()) === "registered", "a registered symbol");
    holder.set(Symbol.iterator);
    assert(holder.get() === Symbol.iterator, "a well-known symbol");
    holder.set(Symbol.asyncDispose);
    assert(holder.get() === Symbol.asyncDispose, "another well-known symbol");
    holder.set(undefined);
}
{
    const a = Symbol("x"), b = Symbol("x");
    cell.v = a;
    assert(cell.v !== b && cell.v === a, "two symbols with one description are two symbols");
    assert(Atomics.compareExchange(cell, "v", b, null) === a && cell.v === a, "compareExchange compares symbols by identity: miss");
    assert(Atomics.compareExchange(cell, "v", a, b) === a && cell.v === b, "compareExchange compares symbols by identity: hit");
    assert(Atomics.store(cell, "w", a) === a && Atomics.load(cell, "w") === a, "Atomics.store and load of a symbol");
    assert(Atomics.exchange(cell, "w", Symbol.for("q")) === a && cell.w === Symbol.for("q"), "Atomics.exchange of a symbol");
    shared struct Holder { s; }
    const holder = new Holder();
    holder.s = a;
    cell.v = holder;
    assert(cell.v.s === a, "a symbol held by a nested struct");
    cell.v = undefined;
    cell.w = undefined;
}

// [[Delete]] is false for every key, strict code throws, and accessor descriptors change nothing.
for (const target of [cell, array]) {
    const key = target === cell ? "v" : 0;
    assert(Reflect.deleteProperty(target, key) === false, "Reflect.deleteProperty of a key that exists");
    assert(Reflect.deleteProperty(target, "nope") === false && Reflect.deleteProperty(target, 99) === false, "Reflect.deleteProperty of absent keys");
    throws(() => { "use strict"; delete target.nope; }, TypeError, "strict delete of an absent key");
    throws(() => { "use strict"; delete target[99]; }, TypeError, "strict delete of an absent index");
    assert(Reflect.defineProperty(target, key, { get() { return 1; } }) === true, "an accessor descriptor is accepted");
    assert(Reflect.defineProperty(target, key, { set(v) { } }) === true, "a setter descriptor is accepted");
    assert(target[key] === undefined || target[key] === null, "and changes nothing");
    assert(Reflect.defineProperty(target, key, { configurable: true }) === false, "configurable is refused");
    assert(Reflect.defineProperty(target, key, { enumerable: false }) === false, "non-enumerable is refused");
    assert(Reflect.defineProperty(target, key, { writable: false }) === false, "read-only is refused");
}

// [[Set]] ignores the receiver: it writes the element.
{
    const other = {};
    assert(Reflect.set(array, 0, 5, other) === true && array[0] === 5 && !("0" in other), "Reflect.set with another receiver on an array");
    assert(Reflect.set(cell, "v", 6, other) === true && cell.v === 6 && !("v" in other), "Reflect.set with another receiver on a struct");
    assert(Reflect.set(cell, "nope", 6, other) === false && !("nope" in other), "Reflect.set of a field that is not there");
    throws(() => Reflect.set(cell, "v", {}, other), TypeError, "Reflect.set of a value that cannot be shared");
    cell.v = undefined;
    array[0] = undefined;
}

// Atomics on a shared struct: the field must be a String or a Symbol (a TypeError otherwise, and no conversion), a
// value that cannot be shared is a TypeError before a missing field is a RangeError.
{
    let converted = false;
    const key = { toString() { converted = true; return "v"; } };
    throws(() => Atomics.load(cell, key), TypeError, "an object is not a property key");
    throws(() => Atomics.store(cell, key, 1), TypeError, "an object as the key of a store");
    assert(!converted, "the key is not converted");
    throws(() => Atomics.load(cell, 0), TypeError, "a Number is not a property key of a struct");
    throws(() => Atomics.load(cell, undefined), TypeError, "undefined");
    throws(() => Atomics.load(cell, "nope"), RangeError, "a missing field is a RangeError");
    throws(() => Atomics.store(cell, "nope", 1), RangeError, "a missing field of a store");
    throws(() => Atomics.exchange(cell, "nope", 1), RangeError, "a missing field of an exchange");
    throws(() => Atomics.compareExchange(cell, "nope", 1, 2), RangeError, "a missing field of a compareExchange");
    throws(() => Atomics.store(cell, "nope", {}), TypeError, "an unshareable value comes first: store");
    throws(() => Atomics.exchange(cell, "nope", {}), TypeError, "an unshareable value comes first: exchange");
    throws(() => Atomics.compareExchange(cell, "nope", 1, {}), TypeError, "an unshareable value comes first: compareExchange");
    assert(Atomics.compareExchange(cell, "nope".slice(0, 0) + "v", {}, 3) === undefined, "an unshareable expected value is not an error");
    cell.v = undefined;
}

// Atomics on a SharedArray: the length and the elements are its own properties, nothing else is.
{
    const sa = new SharedArray(3);
    assert(Atomics.load(sa, "length") === 3, "load of length");
    throws(() => Atomics.load(sa, "foo"), RangeError, "a key that is not an element");
    throws(() => Atomics.load(sa, "foo") , RangeError, "foo does not alias element 0");
    throws(() => Atomics.load(sa, 3), TypeError, "a Number is not a property key");
    throws(() => Atomics.load(sa, 0), TypeError, "not even the index of an element");
    throws(() => Atomics.load(sa, "3"), RangeError, "past the end as a string");
    throws(() => Atomics.load(sa, Symbol()), RangeError, "a symbol");
    throws(() => Atomics.load(sa, 0.5), TypeError, "a fractional Number");
    throws(() => Atomics.load(sa, -1), TypeError, "a negative Number");
    throws(() => Atomics.load(sa, {}), TypeError, "an object");
    throws(() => Atomics.load(sa, null), TypeError, "null");
    throws(() => Atomics.store(sa, "length", 1), TypeError, "the length is read-only");
    throws(() => Atomics.store(sa, "foo", {}), TypeError, "an unshareable value comes first");
    throws(() => Atomics.store(sa, "foo", 1), RangeError, "a missing property of a store");
    assert(Atomics.store(sa, "1", "a") === "a" && Atomics.load(sa, "1") === "a" && sa[1] === "a", "an element by its property key");
    assert(Atomics.load(sa, "0") === undefined, "element 0 is untouched");
}

// The constructor accepts lengths up to 2**32 - 1 and does not look at new.target.
throws(() => new SharedArray(2 ** 32), RangeError, "2**32 is too long");
assert(Object.getPrototypeOf(Reflect.construct(SharedArray, [2], function () { })) === null, "a foreign new.target");
