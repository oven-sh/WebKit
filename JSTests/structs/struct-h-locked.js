function assert(c, m) { if (!c) throw new Error('assertion failed: ' + m); }
struct P { x; m() {} }
let p = new P();
assert(Object.isExtensible(P), "an unshared struct constructor is not locked");
assert(!Object.isExtensible(P.prototype) && Object.isSealed(P.prototype), "proto sealed");
