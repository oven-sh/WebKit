import m from "./derived-from-builtin.py";
function show(label, f) { try { print(label, "=>", f()); } catch (e) { print(label, "!!", e.name + ":", e.message); } }
const s = v => JSON.stringify(v);
class MyDict extends m.dict { total() { let t = 0; for (const v of this.values()) t += v; return t; } }
show("dict", () => { const d = new MyDict(); d.__setitem__("a", 1); d.update(m.dict([["b", 2]])); return [m.type_name(d), s(m.names(MyDict)), d.total(), m.len(d), m.repr(d), m.isinstance(d, m.dict), d instanceof m.dict, d.get("b")].join(" | "); });
class MyList extends m.list { last() { return this[this.length - 1]; } }
show("list", () => { const l = new MyList([1, 2, 3]); l.append(4); l.push(5); return [m.type_name(l), Array.isArray(l), l.last(), m.len(l), m.repr(l), m.sum(l), l instanceof Array].join(" | "); });
class MyTuple extends m.tuple { first() { return this[0]; } }
show("tuple", () => { const t = new MyTuple([7, 8]); return [m.type_name(t), t.first(), m.len(t), m.repr(t), m.hash(t) === m.hash(m.tuple([7, 8]))].join(" | "); });
class MySet extends m.set {}
show("set", () => { const x = new MySet([1, 2, 2]); return [m.type_name(x), m.len(x), String(x | m.set([9]))].join(" | "); });
class MyInt extends m.int { double() { return this * 2; } }
show("int", () => { const i = new MyInt(21); return [m.type_name(i), i.double(), i + 1, m.repr(i), m.isinstance(i, m.int)].join(" | "); });
class MyStr extends m.str { shout() { return this.upper() + "!"; } }
show("str", () => { const x = new MyStr("hi"); return [m.type_name(x), x.shout(), m.len(x), m.repr(x)].join(" | "); });
class MyBytes extends m.bytes {}
show("bytes", () => { const b = new MyBytes([65, 66]); return [m.type_name(b), m.repr(b), b instanceof Uint8Array, b.length].join(" | "); });

class NotFound extends m.AppError { constructor(what) { super(what + " not found", 404); this.what = what; } get isClientError() { return true; } }
class Deeper extends NotFound {}
show("exception", () => { const e = new NotFound("x"); return [e instanceof Error, e instanceof m.Exception, e instanceof NotFound, e.message, e.name, e.code, e.what, e.describe(), e.isClientError, typeof e.stack].join(" | "); });
show("thrown by JS, caught by Python", () => s(m.catch(() => { throw new NotFound("page"); }, m.AppError)));
show("by its own class", () => s(m.catch(() => { throw new Deeper("deep"); }, NotFound)));
show("not by another", () => s(m.catch(() => { throw new NotFound("page"); }, m.ValueError)));
show("raised by Python, caught by JS", () => { try { m.raise_it(NotFound, "file"); } catch (e) { return [e instanceof NotFound, e.constructor === NotFound, e.what, e.code].join(" "); } });
class JSValueError extends m.ValueError {}
show("ValueError", () => { const e = new JSValueError("bad"); return [e instanceof RangeError, e instanceof m.ValueError, e.name, s(m.catch(() => { throw e; }, m.ValueError))].join(" | "); });
show("match", () => [m.match_it(new NotFound("x"), NotFound), m.match_it(new NotFound("x"), m.AppError), m.match_it(new NotFound("x"), Deeper), m.match_it(new Map, Map), m.match_it(new Map, Set)].join(" "));
class Wide extends m.Shape { area() { return this.w * this.w; } }
show("slots", () => { const w = new Wide(3); w.extra = 1; return [w.w, w.area(), w.extra, s(Object.keys(w)), s(m.sorted(m.vars(w)))].join(" | "); });

// What is not an instance does not become one.
show("Object.create of its prototype", () => [m.type_name(Object.create(MyDict.prototype)), m.type_name(Object.create(NotFound.prototype)), m.type_name(Object.create(Wide.prototype))].join(" "));
show("a method on a fake", () => m.dict.get(Object.create(MyDict.prototype), 1));
show("Reflect.construct across", () => m.type_name(Reflect.construct(m.dict, [], NotFound)));
show("Reflect.construct Object", () => m.type_name(Reflect.construct(Object, [], MyDict)));
show("prototype is fixed", () => Object.setPrototypeOf(new MyDict(), MySet.prototype));
show("changing the class's prototype chain", () => { class A extends m.dict {} const a = new A(); Object.setPrototypeOf(A.prototype, m.set); return [m.type_name(a), m.len(a)].join(" "); });
show("no super()", () => { class Bad extends m.dict { constructor() { } } return new Bad(); });
show("returns another", () => { class Odd extends m.dict { constructor() { super(); return { other: 1 }; } } return s(new Odd()); });
