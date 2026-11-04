function assert(c, m) { if (!c) throw new Error('assertion failed: ' + m); }
struct Q { constructor() { this.y = 1; } }
let threw = false;
try { new Q(); } catch (e) { threw = e instanceof TypeError; }
assert(threw, "unknown field in the constructor");
