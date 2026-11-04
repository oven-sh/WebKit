function assert(c, m) { if (!c) throw new Error('assertion failed: ' + m); }
struct P { x; sum() { return 1; } }
let threw = false;
try { (function() { "use strict"; P.prototype.extra = 1; })(); } catch (e) { threw = e instanceof TypeError; }
assert(threw, "add to prototype");
threw = false;
try { (function() { "use strict"; delete P.prototype.sum; })(); } catch (e) { threw = e instanceof TypeError; }
assert(threw, "delete method");
