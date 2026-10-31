function assert(c, m) { if (!c) throw new Error('assertion failed: ' + m); }
struct D1 { x; }
struct D2 extends D1 { x; }
assert(Object.keys(new D2()).join() === "x", "a field of the parent that is declared again is one field");
struct E1 { x; x; y; x; }
assert(Object.keys(new E1()).join() === "x,y", "a field that is declared twice is one field, at its first position");
