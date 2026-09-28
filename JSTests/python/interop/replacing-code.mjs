// f.__code__ = ... changes what a call from JavaScript runs, in code that every compiler there is has compiled with the function known.
import * as m from "./replacing-code.py";
const show = (label, f) => { try { print(label, "=>", String(f())); } catch (e) { print(label, "!!", String(e)); } };
const f = m.f;
const direct = x => f(x);                 // The function is a constant.
const viaModule = x => m.f(x);
const viaCall = x => f.call(null, x);
const viaApply = x => f.apply(null, [x]);
const bound = f.bind(null);
const viaBound = x => bound(x);
const method = x => m.c.m(x);
const all = [direct, viaModule, viaCall, viaApply, viaBound];
const sum = g => { let t = 0; for (let i = 0; i < 200000; i++) t += g(1); return t; };

show("before", () => all.map(sum));
m.swap(m.f, m.g);
show("after", () => all.map(sum));
m.swap(m.f, m.h);
show("and again", () => all.map(sum));
show("a method before", () => sum(method));
m.swap(m.C.m, m.other_method);
show("a method after", () => sum(method));

// While the loop is running
show("in the middle", () => { let t = 0; for (let i = 0; i < 300000; i++) { if (i === 150000) m.swap(m.f, m.g); t += direct(0); } return t; });

// Functions made from one code object, each with cells of its own
const made = [m.build(m.closure_code, 1), m.build(m.closure_code, 10), m.make(100)];
show("made from code", () => made.map(k => sum(k)));
show("polymorphic", () => { let t = 0; for (let i = 0; i < 300000; i++) t += made[i % 3](0); return t; });
show("from JavaScript", () => { made[0].__code__ = made[1].__code__; return [made[0](0), made[0].__closure__[0].cell_contents]; });
show("wrong from JavaScript", () => { "use strict"; m.f.__code__ = 5; });
