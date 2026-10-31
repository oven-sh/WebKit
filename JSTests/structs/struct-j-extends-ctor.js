function assert(c, m) { if (!c) throw new Error('assertion failed: ' + m); }
struct A { a; constructor(a) { this.a = a; } }
struct B extends A { b; constructor(a, b) { super(a); this.b = b; } }
let b = new B(1, 2);
assert(b.a === 1 && b.b === 2, "values");
