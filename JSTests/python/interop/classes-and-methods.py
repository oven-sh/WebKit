import js

def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)

js.eval("""
globalThis.Point = class Point {
    constructor(x, y) { this.x = x; this.y = y; }
    norm() { return Math.hypot(this.x, this.y); }
    get sum() { return this.x + this.y; }
    get handler() { return function () { return this === undefined ? "no this" : typeof this; }; }
    static origin() { return new Point(0, 0); }
    static Inner = class Inner {};
};
globalThis.Point3 = class Point3 extends Point {
    constructor(x, y, z) { super(x, y); this.z = z; }
    norm() { return Math.hypot(super.norm(), this.z); }
};
globalThis.Old = function Old(v) { if (!new.target) return "called"; this.v = v; };
Old.prototype.get = function () { return this.v; };
globalThis.literal = { v: 5, own() { return this === undefined ? "no this" : this.v; } };
globalThis.callIt = f => f();
globalThis.Throws = class Throws { constructor() { throw new RangeError("in constructor"); } };
globalThis.callFromJS = C => { try { return C(); } catch (e) { return e.constructor.name + ": " + e.message; } };
globalThis.viaPython = (py, C) => py(C);
""")

# calling a class makes an instance
show("class", lambda: (p := js.Point(3, 4), p.x, p.y, p.norm(), p.sum, isinstance(p, js.Point))[1:])
show("derived class", lambda: (q := js.Point3(2, 3, 6), q.norm(), isinstance(q, js.Point), isinstance(q, js.Point3))[1:])
show("star arguments", lambda: js.Point(*[6, 8]).norm())
show("static", lambda: (js.Point.origin().norm(), type(js.Point.Inner()).__name__))
show("builtin constructors", lambda: (js.Map().size, js.Set([1, 2, 2]).size, len(js.Uint8Array(3)), js.WeakMap() is not None))
show("promise", lambda: type(js.Promise(lambda resolve, reject: resolve(1))).__name__)
show("callable both ways is called", lambda: (js.Old(1), js.Old.new(1).v, js.Date.new(0).getTime(), type(js.Date()).__name__, len(js.Array(3))))
show("new still works", lambda: js.Point.new(1, 2).sum)
show("through map()", lambda: [p.sum for p in map(js.Point, [1, 2], [3, 4])])
show("as a key function", lambda: [type(x).__name__ for x in map(js.Map, [[]])])
show("constructor raises", lambda: js.Throws())
show("hot", lambda: sum(js.Point(i, 1).sum for i in range(3000)))
show("JavaScript still cannot", lambda: (js.callFromJS(js.Point), js.callFromJS(js.Map)))
show("JS calls Python calls class", lambda: js.viaPython(lambda C: C(1, 2).sum, js.Point))
show("Python calls JS calls class", lambda: js.callFromJS(js.Point3))

# a function that is inherited remembers the object
p = js.Point(3, 4)
show("bound", lambda: (f := p.norm, f(), js.callIt(f), js.callIt(p.norm))[1:])
show("host function", lambda: (m := js.Map(), s := m.set, g := m.get, s("k", 1), g("k"))[-1])
show("old style", lambda: (o := js.Old.new(9), f := o.get, f())[-1])
show("own is as it is", lambda: (js.literal.own(), (lambda f: f())(js.literal.own), js.literal.own is js.literal.own))
show("globals", lambda: (js.Old is js.Old, js.callIt is js.callIt, js.parseInt is js.parseInt, js.globalThis.Old is js.Old))
show("namespaces", lambda: (js.Math.floor is js.Math.floor, (lambda f: f(2.5))(js.Math.floor), js.Array is js.Array, js.Point is js.Point))
show("constructor", lambda: (p.constructor is js.Point, js.Old.new(1).constructor is js.Old, js.Map().constructor is js.Map))
show("what a getter gives", lambda: (lambda f: f())(p.handler))
show("inner class", lambda: js.Point.Inner is js.Point.Inner)
show("equal like methods", lambda: (p.norm == p.norm, p.norm is p.norm, p.norm != js.Point(3, 4).norm, hash(p.norm) == hash(p.norm), {p.norm: 1}[p.norm], p.norm in [p.norm]))
show("__self__ and __func__", lambda: (p.norm.__self__ is p, p.norm.__func__ is js.Point.prototype.norm, p.norm.__name__))
show("not a method", lambda: js.Math.floor.__self__)
show("getattr", lambda: getattr(p, "norm")())
show("super in JS", lambda: (lambda f: f())(js.Point3(2, 3, 6).norm))
show("array methods", lambda: (a := js.eval("[3, 1, 2]"), type(a).__name__)[1:])
