function assert(c, m) { if (!c) throw new Error('assertion failed: ' + m); }
class K {}
let threw = false;
try { struct Bad2 extends K { } } catch (e) { threw = e instanceof TypeError; }
assert(threw, "extends a non-struct class");
