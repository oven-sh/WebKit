function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }
function deadlocks(fn) { try { fn(); } catch (e) { return e instanceof TypeError; } return false; }
function throws(fn, type, m) {
    try { fn(); } catch (e) { assert(e instanceof type, m + " threw " + e); return; }
    throw new Error("did not throw: " + m);
}

shared struct Point {
    x;
    y;
    constructor(x, y) { this.x = x; this.y = y; }
    static origin() { return new Point(0, 0); }
}

const p = new Point(3, 4);
assert(p.x === 3 && p.y === 4, "fields");
assert(Object.getPrototypeOf(p) === null, "null prototype");
assert(!Object.isExtensible(p), "not extensible");
assert(Object.isSealed(p), "sealed");
assert(Object.keys(p).join() === "x,y", "keys in declaration order");
assert("x" in p && !("z" in p), "in operator");
assert(Point.prototype === null, "the prototype of a shared struct constructor is null");
throws(() => p instanceof Point, TypeError, "instanceof throws without a prototype");
assert(Point.origin().y === 0, "static method");
assert(JSON.stringify(p) === '{"x":3,"y":4}', "json");

const descriptor = Object.getOwnPropertyDescriptor(p, "y");
assert(descriptor.value === 4 && descriptor.writable && descriptor.enumerable && !descriptor.configurable, "descriptor");

throws(() => { "use strict"; p.z = 1; }, TypeError, "add a property");
throws(() => { "use strict"; delete p.x; }, TypeError, "delete a field");
throws(() => Object.defineProperty(p, "z", { value: 1 }), TypeError, "define a new property");
assert(Reflect.defineProperty(p, "x", { get() { return 1; } }) === true && p.x === 3, "an accessor descriptor changes nothing");
throws(() => Object.defineProperty(p, "x", { writable: false }), TypeError, "read-only");
throws(() => Object.defineProperty(p, "x", { enumerable: false }), TypeError, "non-enumerable");
throws(() => Object.defineProperty(p, "x", { configurable: true }), TypeError, "configurable");
throws(() => Object.freeze(p), TypeError, "freeze");
throws(() => Object.setPrototypeOf(p, {}), TypeError, "set the prototype");
Object.defineProperty(p, "x", { value: 9 });
assert(p.x === 9, "defineProperty can store a value");
p.x = 10;
assert(p.x === 10, "writable field");

// Defaults and an empty struct.
shared struct Empty { }
assert(Object.keys(new Empty()).length === 0 && !Object.isExtensible(new Empty()), "empty struct");
shared struct Defaults { a; b; }
const d = new Defaults();
assert(d.a === undefined && d.b === undefined && "a" in d && "b" in d, "fields start as undefined");

// Extending: the fields of the chain append, the constructors run, this is the shared struct.
shared struct Base { a; constructor(a) { this.a = a; } }
shared struct Derived extends Base { b; constructor(a, b) { super(a); this.b = b; } }
shared struct Implicit extends Base { c; }
const derived = new Derived(1, 2);
assert(derived.a === 1 && derived.b === 2, "derived values");
assert(Object.keys(derived).join() === "a,b", "derived keys");
assert(Object.getPrototypeOf(derived) === null, "derived null prototype");
const implicit = new Implicit(5);
assert(implicit.a === undefined && implicit.c === undefined && Object.keys(implicit).join() === "a,c", "no constructor: the constructor of the parent is not run");
{
    // The constructor body sees this as the shared struct, also from an arrow function.
    shared struct Self { me; constructor() { const set = () => { this.me = this; }; set(); } }
    const s = new Self();
    assert(s.me === s, "this is the shared struct");
}

// A shared struct only extends a shared struct.
struct Unshared { u; }
class Plain { }
throws(() => { shared struct A1 extends Unshared { } }, TypeError, "extends an unshared struct");
throws(() => { shared struct A2 extends Plain { } }, TypeError, "extends a class");
throws(() => { struct A3 extends Base { } }, TypeError, "an unshared struct extends a shared one");

// No instance methods or accessors.
for (const source of ["shared struct M1 { x; m() { } }", "shared struct M2 { x; get g() { return 1; } }", "shared struct M3 { x; [Symbol.iterator]() { } }"])
    throws(() => (0, eval)("if (false) { " + source + " }"), SyntaxError, "an instance method is an early error: " + source);
shared struct Statics { x; static f() { return 7; } }
assert(Statics.f() === 7, "static members are fine");

// Reflect.canBeShared.
assert(Reflect.canBeShared(1) && Reflect.canBeShared("s") && Reflect.canBeShared(null) && Reflect.canBeShared(undefined)
    && Reflect.canBeShared(true) && Reflect.canBeShared(1n) && Reflect.canBeShared(NaN), "primitives can be shared");
assert(Reflect.canBeShared(p) && Reflect.canBeShared(new Atomics.Mutex()) && Reflect.canBeShared(new Atomics.Condition()), "shared things");
assert(!Reflect.canBeShared({}) && !Reflect.canBeShared([]) && !Reflect.canBeShared(() => 1), "ordinary objects and functions cannot");
assert(Reflect.canBeShared(Symbol()) && Reflect.canBeShared(Symbol.for("k")) && Reflect.canBeShared(Symbol.iterator), "symbols can");

// Round trips.
shared struct Cell { v; }
const cell = new Cell();
function roundTrip(value, same) {
    cell.v = value;
    const got = cell.v;
    assert(same ? same(got) : Object.is(got, value), "round trip of " + String(typeof value) + " " + String(value));
}
roundTrip(1.5);
roundTrip(-0);
roundTrip(0);
roundTrip(NaN);
roundTrip(Infinity);
roundTrip(2 ** 53);
roundTrip(true);
roundTrip(false);
roundTrip(null);
roundTrip(undefined);
roundTrip("");
roundTrip("hello");
roundTrip("café \u{1f600}");
roundTrip("\ud800lone");
roundTrip("x".repeat(100000));
roundTrip(7n);
roundTrip(-123456789012345678901234567890n);
roundTrip(2n ** 200n);
const rope = "ab".concat("cd", "ef");
roundTrip(rope);
cell.v = rope + "gh";
assert(cell.v === "abcdefgh", "a rope is flattened");
throws(() => { cell.v = {}; }, TypeError, "an object is not shareable");
throws(() => { cell.v = () => 1; }, TypeError, "a function is not shareable");
throws(() => { cell.v = [1]; }, TypeError, "an array is not shareable");
throws(() => new Point({}, 1), TypeError, "a constructor storing an object throws");
assert(cell.v === "abcdefgh", "a failed write leaves the field alone");

// Nested shared structs keep their identity.
shared struct Node { next; value; }
const first = new Node(), second = new Node();
first.next = second;
second.value = "tail";
assert(first.next === first.next, "the same wrapper each time");
assert(first.next === second, "the wrapper of the original");
assert(first.next.value === "tail", "nested read");
first.next.value = "changed";
assert(second.value === "changed", "a nested write is visible through the original");
second.next = first;
assert(first.next.next === first, "a cycle keeps its identity");
first.next = null;
assert(first.next === null, "overwritten");

// Mutexes and conditions can be stored.
shared struct Locked { mutex; condition; n; }
const locked = new Locked();
locked.mutex = new Atomics.Mutex();
locked.condition = new Atomics.Condition();
assert(locked.mutex === locked.mutex && locked.condition === locked.condition, "same wrapper for a mutex");
assert(Object.getPrototypeOf(locked.mutex) === null && Object.getPrototypeOf(locked.condition) === null, "kinds");
{
    using token = Atomics.Mutex.lock(locked.mutex);
    assert(deadlocks(() => Atomics.Mutex.lockIfAvailable(locked.mutex, 0)), "the stored mutex is the one that was locked");
}
assert(Atomics.Mutex.lockIfAvailable(locked.mutex, 0) !== null, "released");

// Atomics on fields.
shared struct Counter { n; s; o; }
const c = new Counter();
assert(Atomics.store(c, "n", 5) === 5 && Atomics.load(c, "n") === 5, "store and load");
assert(Atomics.exchange(c, "n", 6) === 5 && c.n === 6, "exchange");
assert(Atomics.compareExchange(c, "n", 6, 7) === 6 && c.n === 7, "compareExchange hit");
assert(Atomics.compareExchange(c, "n", 6, 8) === 7 && c.n === 7, "compareExchange miss");
assert(Atomics.compareExchange(c, "n", {}, 8) === 7 && c.n === 7, "an unshareable expected value never matches");
assert(Atomics.compareExchange(c, "n", 7, null) === 7 && c.n === null, "compareExchange to null");
assert(Atomics.compareExchange(c, "n", undefined, 1) === null && c.n === null, "null is not undefined");
Atomics.store(c, "s", "one" + "two");
assert(Atomics.compareExchange(c, "s", "o" + "netwx", "x") === "onetwo" && c.s === "onetwo", "strings compare by content: miss");
assert(Atomics.compareExchange(c, "s", "one" + "two", "x") === "onetwo" && c.s === "x", "strings compare by content: hit");
assert(Atomics.exchange(c, "s", 1n) === "x" && c.s === 1n, "exchange to a BigInt");
assert(Atomics.compareExchange(c, "s", 1n, 2n) === 1n && c.s === 2n, "BigInts compare by content");
const target = new Cell();
Atomics.store(c, "o", target);
assert(Atomics.load(c, "o") === target, "load returns the same wrapper");
assert(Atomics.compareExchange(c, "o", target, null) === target && c.o === null, "structs compare by identity");
throws(() => Atomics.store(c, "n", {}), TypeError, "store of an object");
throws(() => Atomics.exchange(c, "n", () => 1), TypeError, "exchange of a function");
throws(() => Atomics.compareExchange(c, "n", 1, {}), TypeError, "compareExchange to an object");
throws(() => Atomics.load(c, "nope"), RangeError, "unknown field");
throws(() => Atomics.load(c, Symbol()), RangeError, "a symbol is a property key, and not a field");

// Typed arrays are unchanged.
const ta = new Int32Array(new SharedArrayBuffer(8));
assert(Atomics.add(ta, 0, 2) === 0 && Atomics.load(ta, 0) === 2 && Atomics.compareExchange(ta, 0, 2, 3) === 2, "typed arrays");
throws(() => Atomics.load({}, "x"), TypeError, "a plain object is not accepted");
