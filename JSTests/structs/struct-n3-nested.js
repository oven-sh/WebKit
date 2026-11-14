function assert(c, m) { if (!c) throw new Error('assertion failed: ' + m); }
function f() { struct Inner { a; } return new Inner(); }
assert(Object.keys(f()).join() === "a", "struct in a function");
let g = () => { struct Inner2 { b; } return new Inner2(); };
assert(Object.keys(g()).join() === "b", "struct in an arrow function");
