function assert(c, m) { if (!c) throw new Error('assertion failed: ' + m); }
struct A { a; }
struct B extends A { b; }
let b = new B();
assert(Object.keys(b).join() === "a,b", "keys");
assert(b instanceof A, "instanceof");
