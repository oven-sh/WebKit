// C.__bases__ = ... changes what JavaScript finds too, in code that has been compiled to find what was there before.
import * as m from "./changing-bases.py";
const show = (label, f) => { try { print(label, "=>", String(f())); } catch (e) { print(label, "!!", String(e)); } };
const c = m.C(), d = m.D();
const call = o => o.f();
const tag = o => o.tag;
const isA = o => o instanceof m.A;
const isB = o => o instanceof m.B;
const hasOnlyA = o => "only_a" in o;
const all = o => [call(o), tag(o), isA(o), isB(o), hasOnlyA(o)];
const warm = () => { for (let i = 0; i < 20000; i++) { all(c); all(d); } };

warm();
show("derived from A", () => [all(c), all(d), m.order(m.D)].join(" | "));
m.rebase(m.C, m.B);
show("derived from B", () => [all(c), all(d), m.order(m.D)].join(" | "));
show("its prototype", () => [Object.getPrototypeOf(m.C) === m.B, Object.getPrototypeOf(c) === m.C]);
warm();
show("and still", () => [all(c), all(d)].join(" | "));
m.rebase(m.C, m.A, m.B);
show("derived from both", () => [all(c), all(d), c.only_a(), c.only_b(), m.order(m.D)].join(" | "));
warm();
m.rebase(m.C, m.B, m.A);
show("the other way round", () => [all(c), all(d), c.only_a(), c.only_b(), m.order(m.D)].join(" | "));
warm();
m.rebase(m.C, m.A);
show("back again", () => [all(c), all(d), m.order(m.D)].join(" | "));
show("what B had is gone", () => c.only_b());

// A class of JavaScript's that is derived from one of them
class J extends m.C { g() { return "J.g then " + super.f(); } }
const j = new J();
const viaSuper = o => o.g();
for (let i = 0; i < 20000; i++) viaSuper(j);
show("through super", () => [viaSuper(j), j instanceof m.A, j instanceof m.B]);
m.rebase(m.C, m.B);
show("through super, after", () => [viaSuper(j), j instanceof m.A, j instanceof m.B, m.order(J)]);
show("the bases of a class of JavaScript's", () => m.rebase(J, m.A));
show("from JavaScript", () => { m.C.__bases__ = [m.A]; });
show("the prototype cannot be set", () => Object.setPrototypeOf(m.C, m.A));

// An abstract class is not instantiated by `new` either.
show("abstract", () => m.Abstract());
show("abstract with new", () => new m.Abstract());
class Concrete extends m.Abstract { f() { return 1; } }
show("derived in JavaScript", () => new Concrete().f());

// A property says what it is called.
const p = m.P();
show("property", () => { p.x = 5; return [p.x, p.ro]; });
show("no setter", () => { "use strict"; p.ro = 1; });
show("no deleter", () => { "use strict"; delete p.x; });
show("what is not there", () => { "use strict"; return delete p.nothing; });
show("a slot with nothing in it", () => { "use strict"; return delete m.Slots().a; });
show("a slot with something in it", () => { "use strict"; const s = m.Slots(); s.a = 1; return [delete s.a, s.a]; });
