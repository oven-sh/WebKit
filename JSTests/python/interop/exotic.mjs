import m from "./exotic.py";
function show(label, f) { try { print(label, "=>", f()); } catch (e) { print(label, "!!", e.name + ":", e.message); } }
const s = v => JSON.stringify(v);

const p = m.Plain(1);
show("own", () => [p.x, "x" in p, Object.hasOwn(p, "x"), Object.keys(p)].join(" "));
show("class attribute", () => [p.kind, "kind" in p, Object.hasOwn(p, "kind"), m.Plain.kind].join(" "));
show("bound method", () => { const f = p.method; return s(f()) + " " + (typeof f); });
show("classmethod", () => [m.Plain.make(5).x, p.make(6).x].join(" "));
show("staticmethod", () => [m.Plain.helper(1), p.helper(2)].join(" "));
show("missing", () => [p.nope, "nope" in p].join(" "));
show("set and python sees", () => { p.y = 2; p.x = 10; return s(m.attrs(p)); });
show("delete", () => [delete p.y, delete p.y, s(m.attrs(p))].join(" "));
show("shadow class attribute", () => { p.kind = "mine"; return [p.kind, m.Plain.kind, delete p.kind, p.kind].join(" "); });
show("prototype", () => [m.Plain.prototype === m.Plain, Object.getPrototypeOf(p) === m.Plain, Object.hasOwn(m.Plain, "kind"), Object.hasOwn(m.Late1, "kind")].join(" "));

const t = m.Temperature(20);
show("property get over own", () => [t.celsius, s(m.take_log())].join(" "));
show("property set over own", () => { t.celsius = 25; return [s(m.take_log()), t.fahrenheit].join(" "); });
show("setter raises", () => { t.celsius = -300; });
show("no setter", () => { m.take_log(); t.fahrenheit = 1; });
show("getter says gone", () => [t.gone, "gone" in t].join(" "));
show("own keys", () => Object.keys(t).join());

const fz = m.Frozen(1);
show("frozen set", () => { fz.x = 2; });
show("frozen new", () => { fz.z = 2; });
show("frozen delete", () => delete fz.x);
show("frozen still", () => fz.x);

const sl = m.Slotted();
show("slot", () => { sl.a = 5; return [sl.a, Object.keys(sl).length, "a" in sl].join(" "); });
show("no dict", () => { sl.b = 1; });

show("__getattr__", () => [m.Dynamic().dyn_hello, m.Dynamic().other, "dyn_x" in m.Dynamic()].join(" "));
const spy = m.Spy();
show("__getattribute__", () => [spy.real, s(m.take_log())].join(" "));
show("__getattribute__ asked once", () => [spy.hidden, s(m.take_log())].join(" "));

const d = m.D();
show("mro", () => [d.who(), d.only_c].join(" "));
show("instanceof", () => [d instanceof m.D, d instanceof m.B, d instanceof m.C, d instanceof m.A, p instanceof m.A, 1 instanceof m.A].join(" "));
show("__instancecheck__", () => [42 instanceof m.Odd, 41 instanceof m.Odd, m.Odd() instanceof m.Odd].join(" "));

const ad = m.AttrDict();
show("dict subclass", () => { ad.k = 1; return [ad.k, ad.get("k"), ad.size].join(" "); });

show("class set from JS", () => { m.Plain.added = 7; return [p.added, m.Late1(0).added].join(" "); });
show("JS function in a class", () => { m.Plain.twice = function () { return this.x * 2; }; return m.Plain(4).twice(); });
show("class delete from JS", () => [delete m.Plain.added, p.added].join(" "));
show("builtin class is immutable", () => { Object.getPrototypeOf(m.Plain).extra = 1; });

show("defineProperty data", () => { Object.defineProperty(p, "dp", { value: 3, writable: true, enumerable: true, configurable: true }); return p.dp; });
show("defineProperty accessor", () => Object.defineProperty(p, "acc", { get() { return 1; } }));
show("defineProperty hidden", () => Object.defineProperty(p, "hid", { value: 1 }));
show("freeze", () => Object.freeze(p));
show("isExtensible", () => Object.isExtensible(p));
show("assign and spread", () => s(Object.assign({}, m.Plain(3))) + s({ ...m.Plain(4) }));
show("for in", () => { const out = []; for (const k in m.Plain(1)) out.push(k); return out.join(); });
show("Reflect", () => [Reflect.get(p, "x"), Reflect.set(p, "x", 11), Reflect.has(p, "method"), Reflect.ownKeys(p).join()].join(" "));

// What is found out about a class holds until the class is given something that changes it, however hot the code is.
function readX(o) { return o.x; }
function writeX(o, v) { o.x = v; }
function hot(o) { let last; for (let i = 0; i < 20000; i++) { writeX(o, i); last = readX(o); } return last; }
const l1 = m.Late1(0), l2 = m.Late2(0), l3 = m.Late3(0), ld = m.LateDerived(0);
show("hot", () => [hot(l1), hot(l2), hot(l3), hot(ld)].join(" "));
show("late property", () => { m.add_property(m.Late1, "x"); const r = readX(l1); writeX(l1, 5); return [r, s(m.take_log()), s(m.attrs(l1))].join(" "); });
show("late __setattr__", () => { m.add_setattr(m.Late2); writeX(l2, 5); return [readX(l2), s(m.take_log())].join(" "); });
show("late __getattribute__", () => { m.add_getattribute(m.Late3); return readX(l3); });
show("late property on a base", () => { m.add_property(m.LateBase, "x"); return readX(ld); });
show("others are as they were", () => [readX(p), hot(m.Plain(0))].join(" "));

for (const g of m.make_guards()) {
    show(Object.getPrototypeOf(g).__name__, () => { g.tag = "changed"; g.fresh = 1; return [g.tag, g.fresh, g.prop, s(m.take_log()), Object.keys(g).filter(k => isNaN(k)).join()].join(" "); });
}
const gl = m.guards[0]([]);
show("still an array", () => { gl.push(1, 2); gl.length = 1; return [Array.isArray(gl), gl.length, gl[0], gl instanceof m.guards[0]].join(" "); });
const gb = m.guards[8]();
show("still a Uint8Array", () => [gb instanceof Uint8Array, gb.length, gb.byteLength].join(" "));

const np = m.numbered();
show("index get", () => [np[0], np["0"], 0 in np, 1 in np, np[1], Object.hasOwn(np, 0)].join(" "));
show("index set", () => { np[2] = "two"; np["3"] = "three"; return [m.get(np, "2"), m.get(np, "3"), np[2]].join(" "); });
show("index delete", () => [delete np[0], m.get(np, "0"), np[0], delete np[0]].join(" "));
show("index hot", () => { let last; for (let i = 0; i < 20000; i++) { np[5] = i; last = np[5]; } return [last, m.get(np, "5")].join(" "); });
show("index frozen", () => { fz[0] = 1; });
show("index keys", () => s(m.attrs(np)));
