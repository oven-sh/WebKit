function assert(c, m) { if (!c) throw new Error('assertion failed: ' + m); }
struct P { x; constructor(x) { this.x = x; } get dbl() { return this.x * 2; } }
assert(new P(2).dbl === 4, "getter");
