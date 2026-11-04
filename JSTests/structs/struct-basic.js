function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }
function throws(fn, type, m) {
    try { fn(); } catch (e) { assert(e instanceof type, m + " threw " + e); return; }
    throw new Error("did not throw: " + m);
}

struct Point {
    x;
    y;
    constructor(x, y) { this.x = x; this.y = y; }
    sum() { return this.x + this.y; }
    get len() { return Math.hypot(this.x, this.y); }
    static origin() { return new Point(0, 0); }
}

let p = new Point(3, 4);
assert(p.x === 3 && p.y === 4, "fields");
assert(p.sum() === 7, "method");
assert(p.len === 5, "getter");
assert(Point.origin().x === 0, "static method");
assert(Object.keys(p).join() === "x,y", "keys in declaration order");
assert(!Object.isExtensible(p), "instance not extensible");
throws(() => { "use strict"; p.z = 1; }, TypeError, "add property");
throws(() => { "use strict"; delete p.x; }, TypeError, "delete property");
p.x = 10;
assert(p.x === 10, "writable field");

struct Empty { }
let e = new Empty();
assert(Object.keys(e).length === 0 && !Object.isExtensible(e), "empty struct");

struct Defaults { a; b; }
let d = new Defaults();
assert(d.a === undefined && d.b === undefined && "a" in d && "b" in d, "undefined defaults");

// The prototype is sealed. The constructor of an unshared struct is not.
assert(Object.isExtensible(Point), "constructor not locked");
assert(!Object.isExtensible(Point.prototype) && Object.isSealed(Point.prototype) && !Object.isFrozen(Point.prototype), "prototype sealed");
throws(() => { "use strict"; Point.prototype.extra = 1; }, TypeError, "prototype add");
throws(() => { "use strict"; delete Point.prototype.sum; }, TypeError, "method delete");
assert(Object.getOwnPropertyDescriptor(Point.prototype, "sum").writable === true, "a method stays writable");
throws(() => Point(), TypeError, "call without new");

// `struct` and `shared` remain ordinary identifiers.
var struct = 1, shared = 2;
assert(struct + shared === 3, "contextual keywords");
