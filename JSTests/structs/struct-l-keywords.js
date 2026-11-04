function assert(c, m) { if (!c) throw new Error('assertion failed: ' + m); }
var struct = 1, shared = 2;
assert(struct + shared === 3, "contextual");
