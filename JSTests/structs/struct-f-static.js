function assert(c, m) { if (!c) throw new Error('assertion failed: ' + m); }
struct P { x; static make() { return new P(); } }
assert(P.make().x === undefined, "static method");
