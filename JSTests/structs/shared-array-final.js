// SharedArray is final in the proposal: a class cannot extend it, and nothing can construct one with another
// new.target. Other `class extends` forms are not affected.
function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }
function throws(fn, type, m) {
    try { fn(); } catch (e) { assert(e instanceof type, m + " threw " + e); return e; }
    throw new Error("did not throw: " + m);
}

// The error is thrown where the class is defined: the heritage is evaluated and rejected before the body runs.
let ran = false;
let defined = false;
let error;
try {
    class Subclass extends SharedArray {
        static marker = (ran = true);
        constructor() { super(2); }
    }
    defined = true;
} catch (e) {
    error = e;
}
assert(!defined && !ran, "the class is not defined and its body did not run");
assert(error instanceof TypeError, "a TypeError at class definition: " + error);
print("# class extends SharedArray: " + error.message);
throws(() => { const Expression = class extends SharedArray { }; }, TypeError, "a class expression");
throws(() => { class Plain extends SharedArray { } }, TypeError, "a class without a constructor");

// The constructor does not look at new.target, so Reflect.construct with another one makes a plain SharedArray.
function Other() { }
assert(Object.getPrototypeOf(Reflect.construct(SharedArray, [1], Other)) === null, "Reflect.construct with another new.target");
assert(Reflect.construct(SharedArray, [1], class { }).length === 1, "Reflect.construct with a class");
assert(Reflect.construct(SharedArray, [3], SharedArray).length === 3, "the same new.target is fine");
throws(() => SharedArray(1), TypeError, "a call without new");
assert(new SharedArray(4).length === 4 && Object.getPrototypeOf(new SharedArray(1)) === null, "a plain SharedArray still works");

// Everything else that can be extended still can.
class Base { constructor() { this.base = true; } }
class Derived extends Base { }
assert(new Derived().base === true && new Derived() instanceof Base, "a class extends a class");
class List extends Array { }
assert(new List(3).length === 3 && new List() instanceof Array, "a class extends Array");
class Dictionary extends Map { }
assert(new Dictionary([[1, 2]]).get(1) === 2, "a class extends Map");
function Legacy() { this.legacy = true; }
class Modern extends Legacy { }
assert(new Modern().legacy === true, "a class extends a function");
class Nothing extends null { }
throws(() => new Nothing(), TypeError, "extends null still cannot be constructed");
throws(() => { class Bad extends 5 { } }, TypeError, "extends a number still throws");
struct Plain1 { x; }
shared struct Shared1 { y; }
assert(new Plain1() instanceof Plain1 && Object.getPrototypeOf(new Shared1()) === null, "structs are unaffected");
