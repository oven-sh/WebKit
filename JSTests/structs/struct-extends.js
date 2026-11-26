function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }
function throws(fn, type, m) {
    try { fn(); } catch (e) { assert(e instanceof type, m + " threw " + e); return; }
    throw new Error("did not throw: " + m);
}

struct A {
    a;
    constructor(a) { this.a = a; }
    who() { return "A" + this.a; }
}
struct B extends A {
    b;
    constructor(a, b) { super(a); this.b = b; }
    who() { return "B" + this.a + this.b; }
}
struct C extends B {
    c;
}

let b = new B(1, 2);
assert(b.a === 1 && b.b === 2, "inherited and own fields");
assert(Object.keys(b).join() === "a,b", "parent fields first");
assert(b.who() === "B12", "override");
assert(b instanceof A && b instanceof B, "instanceof");
assert(!Object.isExtensible(b), "sealed");

let c = new C(1, 2);
assert(Object.keys(c).join() === "a,b,c", "three levels");
c.c = 9;
assert(c.c === 9 && c.a === undefined && c.b === undefined, "a struct without a constructor runs no constructor of its parents");

// All fields exist before the parent constructor body runs.
struct Probe {
    seen;
    constructor() { this.seen = Object.keys(this).join(); }
}
struct Probe2 extends Probe {
    extra;
    constructor() { super(); }
}
assert(new Probe2().seen === "seen,extra", "subclass fields visible in the parent constructor");

throws(() => { struct Bad extends Object { } }, TypeError, "extends a class");
throws(() => { class K {} struct Bad2 extends K { } }, TypeError, "extends a non-struct class");
{ struct D1 { x; } struct D2 extends D1 { x; } assert(Object.keys(new D2()).join() === "x", "a redeclared field is one field"); }
throws(() => { struct Q { constructor() { this.y = 1; } } new Q(); }, TypeError, "unknown field in the constructor");
