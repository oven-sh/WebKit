function assert(c, m) { if (!c) throw new Error('assertion failed: ' + m); }
struct P { x; constructor(x) { this.x = x; } sum() { return this.x + 1; } }
assert(new P(1).sum() === 2, "method");
