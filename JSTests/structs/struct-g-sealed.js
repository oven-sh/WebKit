function assert(c, m) { if (!c) throw new Error('assertion failed: ' + m); }
struct P { x; }
let p = new P();
let threw = false;
try { (function() { "use strict"; p.z = 1; })(); } catch (e) { threw = e instanceof TypeError; }
assert(threw, "add property throws");
threw = false;
try { (function() { "use strict"; delete p.x; })(); } catch (e) { threw = e instanceof TypeError; }
assert(threw, "delete throws");
p.x = 3;
assert(p.x === 3, "writable");
