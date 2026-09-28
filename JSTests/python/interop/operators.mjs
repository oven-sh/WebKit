import m from "./operators.py";
function show(label, f) { try { print(label, "=>", String(f())); } catch (e) { print(label, "!!", e.name + ":", e.message); } }
const a = m.Vec(1, 2), b = m.Vec(3, 4);
show("binary", () => [a + b, a - b, a * 3, a / 2, b % 2, a ** 2, a << 2, b >> 1, a & b, a | b, a ^ b].join(" "));
show("reflected", () => [10 + a, 10 - a, 3 * a].join(" "));
show("unary", () => [-a, ~a].join(" "));
show("compare", () => [a == m.Vec(1, 2), a != m.Vec(1, 2), a == b, a != b, a < b, a <= b, a > b, a >= b, a == 1, 1 == a].join(" "));
show("identity", () => [a === m.Vec(1, 2), a === a, a !== b, a == null, a != undefined, Object.is(a, a)].join(" "));
show("assignment", () => { let v = a; v += b; v *= 2; v -= a; let w = a; w++; let u = a; u--; return [v, w, u, a].join(" "); });
// It is asked, as for anything else. What comes of adding a number to a string in Python is its own affair. See strings-and-operators.mjs.
show("with a string on the right", () => a + "!");
show("with a string on the left", () => "<" + a);
show("in a template", () => `${a}`);
show("in place", () => { let p = m.InPlace(); const same = p; p += 1; return [p === same, p.log].join(" "); });
show("path", () => [m.Path("a") / "b" / "c", "root" / m.Path("x")].join(" "));
show("nothing defined", () => m.Plain() + 1);
show("nothing defined, compare", () => m.Plain() < m.Plain());
show("nothing defined, equal", () => { const p = m.Plain(); return [p == p, p == m.Plain(), p != m.Plain()].join(" "); });
show("nothing defined, negate", () => -m.Plain());
show("built in", () => [m.a_set & m.b_set, m.a_set | m.b_set, m.a_set - m.b_set, m.a_set ^ m.b_set, m.a_set < m.b_set, m.a_set == m.a_set.copy()].join(" "));
show("tuples", () => [m.a_tuple == m.b_tuple, m.a_tuple === m.b_tuple, m.a_tuple < m.c_tuple, m.a_tuple + m.c_tuple, m.a_tuple * 2].join(" "));
show("dicts", () => [m.a_dict == m.b_dict, m.a_dict == m.c_dict, m.a_dict | m.c_dict].join(" "));
show("complex", () => [m.z + 1, m.z * m.z, -m.z, m.z == m.z, 2 * m.z, m.z / 2].join(" "));
show("ranges and classes", () => [m.r == m.range3, m.Vec == m.Vec, m.Vec == m.Plain].join(" "));
show("derived from list", () => [m.VList([1]) + m.VList([2]), m.VList() == 5, [1] + [2]].join(" "));
show("derived from bytes", () => m.VBytes() + 1);
show("what is not a boolean", () => [m.Odd() < 1, m.Odd() > 1].join(" "));
show("raises", () => m.Odd() == 1);

// The same code with numbers, strings, objects of JavaScript's and of Python's, until it is compiled and after.
function add(x, y) { return x + y; }
function mul(x, y) { return x * y; }
function eq(x, y) { return x == y; }
function lt(x, y) { return x < y; }
function neg(x) { return -x; }
noInline(add); noInline(mul); noInline(eq); noInline(lt); noInline(neg);
const o1 = {}, o2 = {}, e1 = new Error("e"), e2 = new Error("e");
show("numbers first", () => { let t = 0; for (let i = 0; i < 100000; i++) { t += add(i, 1) + mul(i, 2) + (eq(i, i) ? 1 : 0) + (lt(i, 5) ? 1 : 0) + neg(i); } return t; });
show("then Python", () => [add(a, b), mul(a, 2), eq(a, m.Vec(1, 2)), lt(a, b), neg(a)].join(" "));
show("objects first", () => { let t = 0; for (let i = 0; i < 100000; i++) t += (eq(o1, o1) ? 1 : 0) + (eq(o1, o2) ? 1 : 0) + (eq(e1, e2) ? 1 : 0) + (eq(e1, e1) ? 1 : 0); return t; });
show("then Python again", () => [eq(a, m.Vec(1, 2)), eq(a, b), eq(a, o1), eq(o1, a), eq(e1, a)].join(" "));
show("Python hot", () => { let v = m.Vec(0, 0), n = 0; for (let i = 0; i < 30000; i++) { v = add(v, a); if (eq(v, v)) n++; if (lt(a, v)) n++; } return [v, n].join(" "); });
show("and numbers still", () => [add(1, 2), mul(3, 4), eq(1, 1), lt(1, 2), neg(5), add("a", "b"), eq(o1, o1), eq(o1, o2)].join(" "));
