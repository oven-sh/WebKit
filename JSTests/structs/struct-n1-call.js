function assert(c, m) { if (!c) throw new Error('assertion failed: ' + m); }
struct P { x; }
let threw = false;
try { P(); } catch (e) { threw = e instanceof TypeError; }
assert(threw, "call without new");
