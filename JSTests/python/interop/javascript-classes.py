import js
def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
def names(c): return [k.__name__ for k in c.__mro__]

show("type of things", lambda: [type(x).__name__ for x in (js.Map.new(), js.Set.new(), js.Date.new(0), js.Object.new(), js.RegExp.new("a"), js.Promise.resolve(1))])
show("type is the class", lambda: (type(js.Map.new()) is js.Map, type(js.Map) is type, isinstance(js.Map, type), isinstance(js.Map.new(), js.Map), issubclass(js.Map, js.Object), issubclass(js.Map, object)))
show("mro", lambda: (names(js.Map), js.Map.__name__, js.Map.__bases__, js.Map.__base__, repr(js.Map)))
show("arrow is no class", lambda: (type(js.eval("() => 1")).__name__, isinstance(js.eval("() => 1"), type), type(js.Math.floor).__name__))

class Counter(js.Map):
    def __init__(self, *args):
        self.bumps = 0
    def bump(self, key):
        self.bumps += 1
        self.set(key, (self.get(key) or 0) + 1)
        return self
    def get(self, key):
        return super().get(key)
c = None
def make():
    global c
    c = Counter()
    return (type(c).__name__, names(Counter), isinstance(c, js.Map), isinstance(c, Counter))
show("derive from Map", make)
show("use it", lambda: (c.bump("a").bump("a").bump("b").size, c.get("a"), c.bumps, list(c.keys()), vars(c)))
show("JS sees it", lambda: (js.eval("(c => [c instanceof Map, c.size, c.get('a'), c.bumps, Object.prototype.toString.call(c), c.bump('z').get('z')].join(' '))")(c)))
K = type("K", (js.Map,), {})
show("with arguments", lambda: (lambda k: (k.size, k.get(1)))(K([[1, 2]])))

class Mixin:
    def hello(self): return "hello from " + type(self).__name__
class Both(Mixin, js.Set): pass
show("mixin", lambda: (names(Both), Both().hello(), Both([1, 2, 2]).size))
show("conflict", lambda: type("Bad", (js.Map, dict), {}))
show("two of JavaScript's", lambda: names(type("Two", (js.Map, js.Set), {})))

class Stamp(js.Date):
    def year(self): return self.getUTCFullYear()
show("Date", lambda: (Stamp(0).year(), Stamp(0).toISOString(), isinstance(Stamp(0), js.Date)))

show("object.__new__", lambda: object.__new__(js.Map))
show("__new__ directly", lambda: (type(js.Map.__new__(js.Map)).__name__, type(js.Map.__new__(Counter)).__name__))
show("__new__ of the wrong class", lambda: js.Map.__new__(js.Set))
show("subclasses", lambda: [k.__name__ for k in js.Map.__subclasses__()])
show("set attribute of a class", lambda: (setattr(js.Map, "extra", lambda self: "x"), js.eval("typeof Map.prototype.extra"), js.eval("typeof Map.extra"), delattr(js.Map, "extra"), js.eval("typeof Map.prototype.extra"))[1::1])

Old = js.eval("(function Old(x) { this.x = x; })")
js.eval("(function (O) { O.prototype.twice = function () { return this.x * 2; }; })")(Old)
class New(Old):
    def thrice(self): return self.x * 3
show("old style", lambda: (names(New), New(4).twice(), New(4).thrice(), type(Old.new(1)) is Old))

registry = []
class Plugin:
    def __init_subclass__(cls, **kw):
        registry.append(cls.__name__)
js.eval("(function (P) { globalThis.JSPlugin = class JSPlugin extends P {}; })")(Plugin)
show("__init_subclass__", lambda: (registry, type(js.JSPlugin()).__name__, registry))
js.eval("(function () { globalThis.JSPlugin2 = class JSPlugin2 extends JSPlugin {}; globalThis.Unrelated = class Unrelated extends Map {}; })")()
show("and below that", lambda: registry)
class PyPlugin(js.JSPlugin2): pass
show("and Python below that", lambda: (registry, names(PyPlugin)))
class Named:
    def __set_name__(self, owner, name): self.where = (owner.__name__, name)
n = Named()
js.eval("(function (P, n) { globalThis.WithNamed = class WithNamed extends P {}; })")(Plugin, n)
class Refuses:
    def __init_subclass__(cls): raise TypeError("no subclasses of " + cls.__mro__[1].__name__)
show("refusing", lambda: js.eval("(function (P) { return class Sub extends P {}; })")(Refuses))
