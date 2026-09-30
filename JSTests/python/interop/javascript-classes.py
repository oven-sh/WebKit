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

# What stands for one where a string or a number is wanted. Each language would leave it to the other, if neither had anything to say.
convert = js.eval("""x => ['String(x)', 'x + 1', '`${x}`', 'x.toString()', '+x', 'x == 1', 'x.valueOf() === x', 'Object.prototype.toString.call(x)'].map(source => {
    try { return source + ': ' + String(new Function('x', 'return ' + source)(x)); } catch (e) { return source + ': threw ' + String(e).slice(0, 50); }
}).join(' | ')""")
class PlainMap(js.Map): pass
class PlainSet(js.Set): pass
class PlainRegExp(js.RegExp): pass
class PlainError(js.Error): pass
class PlainTypeError(js.TypeError): pass
class PlainObject(js.Object): pass
class PlainArray(js.Array): pass
class Below(PlainMap): pass
for made in (PlainMap, PlainSet, lambda: PlainRegExp("a+", "g"), lambda: PlainError("message"), lambda: PlainTypeError("message"), PlainObject, lambda: PlainArray(1, 2), Below):
    show("with nothing to say", lambda: (lambda x: (type(x).__name__, str(x), repr(x), f"{x}", "%s" % (x,), convert(x)))(made()))
# It has a way of its own, which goes by what is wanted.
class PlainDate(js.Date): pass
show("a Date", lambda: (lambda x: (js.eval("x => [+x, x - 1, typeof (x + 1), (x + 1).endsWith('1'), String(x) === x.toString(), `${x}` === x.toString(), x == x.toString(), x.valueOf()].join()")(x), str(x) == x.toString(), repr(x)))(PlainDate(5)))
class WithStr(js.Map):
    def __str__(self): return "what __str__() says"
class WithRepr(js.Map):
    def __repr__(self): return "what __repr__() says"
class WithFloat(js.Map):
    def __float__(self): return 2.5
class WithIndex(js.Map):
    def __index__(self): return 7
class WithBoth(js.Date):
    def __str__(self): return "a date of Python's"
    def __float__(self): return 1.5
class StrRaises(js.Map):
    def __str__(self): raise KeyError("from __str__")
class ErrorWithStr(js.Error):
    def __str__(self): return "an error of Python's"
for C in (WithStr, WithRepr, WithFloat, WithIndex, ErrorWithStr):
    show(C.__name__, lambda: (lambda x: (str(x), repr(x), convert(x)))(C()))
show("WithBoth", lambda: (lambda x: (str(x), js.eval("x => [String(x), `${x}`, +x, x + 1, x - 1, x.valueOf(), x.getTime()].join(' | ')")(x)))(WithBoth(5)))
show("what it raises", lambda: convert(StrRaises()))
show("and in Python", lambda: str(StrRaises()))
# A class of JavaScript's derived from one of Python's derived from one of JavaScript's, and one of Python's from that.
js.eval("(function (P) { globalThis.JSBelow = class JSBelow extends P {}; globalThis.JSBelowStr = class JSBelowStr extends P { toString() { return 'what toString() says'; } }; })")(PlainMap)
class PyBelow(js.JSBelow): pass
class PyBelowStr(js.JSBelowStr): pass
for C in (js.JSBelow, PyBelow, js.JSBelowStr, PyBelowStr):
    show("in turn", lambda: (lambda x: (names(type(x)), str(x), convert(x)))(C()))
js.eval("(function (P) { globalThis.JSDate = class JSDate extends P {}; })")(PlainDate)
class PyDate(js.JSDate): pass
for C in (PlainDate, js.JSDate, PyDate):
    show("a Date, in turn", lambda: (lambda x: (names(type(x)), js.eval("x => [+x, x - 1, typeof (x + 1), x + 1 === x.toString() + '1', `${x}` === x.toString()].join()")(x), str(x) == x.toString()))(C(5)))

# An Error that JavaScript makes for a class of Python's is as anything else that it makes for one.
e = PlainError("message")
show("an Error", lambda: (isinstance(e, js.Error), isinstance(e, BaseException), e.message, e.name, type(e.stack).__name__, hasattr(e, "nothing"), getattr(e, "nothing", "no such thing"), js.Error.isError(e)))
show("to JavaScript", lambda: js.eval("e => [e instanceof Error, e.message, e.name, typeof e.stack, e.nothing, e.constructor.name, 'message' in e, 'nothing' in e, Object.keys(e).join()].join(' | ')")(e))
def attributes():
    e.mine = 5
    return e.mine, vars(e).get("mine"), js.eval("e => e.mine")(e), js.eval("e => { e.yours = 6; return e.yours; }")(e), e.yours
show("attributes", attributes)
class ErrorWithMore(js.RangeError):
    kind = "of the class"
    def describe(self): return "%s: %s (%s)" % (type(self).__name__, self.message, self.kind)
    @property
    def shout(self): return self.message.upper()
show("methods", lambda: (lambda x: (x.describe(), x.shout, js.eval("x => [x.describe(), x.shout, x.kind, x instanceof RangeError, String(x)].join(' | ')")(x)))(ErrorWithMore("too far")))
show("thrown and caught by JavaScript", lambda: js.eval("f => { try { f(); } catch (x) { return [x.constructor.name, x.describe()].join(); } }")(lambda: js.eval("x => { throw x; }")(ErrorWithMore("back"))))

def attempt(f, *a):
    try: return f(*a)
    except BaseException as e: return e

# What JavaScript gave it is its own, though it is hidden, or worked out when it is asked for, or got by a getter.
def own():
    a, r, u = PlainArray(1, 2, 3), PlainRegExp("a", "g"), type("Bytes", (js.Uint8Array,), {})(4)
    r.exec("banana")
    return a.length, len(a), r.lastIndex, r.source, r.flags, u.length, u.byteLength, hasattr(a, "nothing"), sorted(vars(a)), sorted(vars(r))
show("of its own", own)
def set_them():
    a, r, x = PlainArray(1, 2, 3), PlainRegExp("a", "g"), PlainError("message")
    a.length = 1
    r.lastIndex = 3
    x.message = "another"
    return list(a), a.length, r.lastIndex, js.eval("r => r.lastIndex")(r), x.message, str(x), js.eval("x => [x.message, Object.keys(x).join(), Object.getOwnPropertyDescriptor(x, 'message').enumerable].join(' | ')")(x)
show("set", set_them)
def defined():
    o = PlainObject()
    js.eval("""o => { Object.defineProperty(o, 'hidden', { value: 1, writable: true, configurable: true, enumerable: false }); Object.defineProperty(o, 'fixed', { value: 2 });
        Object.defineProperty(o, 'got', { get() { return 'by a getter of ' + this.constructor.name; }, set(v) { this.was_set = v; }, configurable: true, enumerable: true }); Object.defineProperty(o, 'only_got', { get() { return 3; } }); o.plain = 4; }""")(o)
    return o
show("what is defined on it", lambda: (lambda o: (o.hidden, o.fixed, o.got, o.only_got, o.plain, sorted(vars(o))))(defined()))
def set_defined():
    o = defined()
    o.hidden, o.got, o.plain = 10, 20, 40
    return o.hidden, o.got, o.was_set, o.plain, js.eval("o => [o.hidden, o.was_set, o.plain, Object.keys(o).join()].join(' | ')")(o)
show("set", set_defined)
show("what cannot be set", lambda: (lambda o: [type(e).__name__ + ": " + str(e) for e in [attempt(setattr, o, n, 0) for n in ("fixed", "only_got")]] + [o.fixed, o.only_got])(defined()))
def deleted():
    o = defined()
    del o.hidden, o.got, o.plain
    return [hasattr(o, n) for n in ("hidden", "got", "plain", "fixed")], [type(e).__name__ + ": " + str(e) for e in [attempt(delattr, o, n) for n in ("fixed", "only_got", "nothing", "hidden")]]
show("deleted", deleted)
class Shadows(js.Array):
    length = "of the class"
    @property
    def size(self): return "a property of the class"
show("what the class has by the same name comes after, unless it is a property", lambda: (lambda s: (s.length, s.size, js.eval("s => [s.length, s.size].join(' | ')")(s)))(Shadows(1, 2)))

# Going through one, and how many there are. Here too each would leave it to the other.
spread = js.eval("x => { try { return JSON.stringify([...x]); } catch (e) { return 'threw ' + String(e).slice(0, 40); } }")
for made in (lambda: PlainArray(1, 2, 3), lambda: PlainMap([[1, 2]]), lambda: PlainSet([1, 2]), lambda: type("Bytes", (js.Uint8Array,), {})(2), lambda: type("Text", (js.String,), {})("ab"), lambda: Below([[3, 4]]), lambda: PyBelow([[5, 6]])):
    show("gone through", lambda: (lambda x: (type(x).__name__, [list(v) if isinstance(v, list) else v for v in x], len(x), [*x] == list(x), bool(x), spread(x), js.eval("x => [x.length, x.size].join()")(x)))(made()))
show("what cannot be", lambda: [(type(attempt(iter, x)).__name__, type(attempt(len, x)).__name__, spread(x)) for x in (PlainObject(), PlainDate(0), PlainError("m"), PlainRegExp("a"))])
class Backwards(js.Array):
    def __iter__(self): return iter([self[i] for i in range(self.length - 1, -1, -1)])
    def __len__(self): return 100
show("with a way of its own", lambda: (lambda x: (list(x), len(x), spread(x), js.eval("x => [x.length, Array.from(x).join(), x.join(), x.map(v => v).join()].join(' | ')")(x)))(Backwards(1, 2, 3)))
class Counts(js.Object):
    def __init__(self): self.n = 0
    def __next__(self):
        self.n += 1
        if self.n > 3: raise StopIteration
        return self.n
    def __iter__(self): return self
show("that is gone through once", lambda: (list(Counts()), spread(Counts()), js.eval("x => JSON.stringify([x.next(), x.next(), x.next(), x.next()])")(Counts())))
class Managed(js.Map):
    def __enter__(self): return self
    def __exit__(self, *a): self.set("closed", True)
show("using", lambda: js.eval("x => { { using y = x; } return x.get('closed'); }")(Managed()))

# What is set on a class of JavaScript's is as what is written in one is, and does not show when an instance is gone through by its keys. The library sets such things where no one is looking.
keys_of = js.eval("x => { const keys = []; for (const key in x) keys.push(key); return keys.join(); }")
def set_on_a_class():
    js.eval("globalThis.Shape = class Shape { area() { return 1; } }")
    js.Shape.perimeter = lambda self: 2
    js.Shape.sides = 4
    s = js.Shape.new()
    return s.perimeter(), s.sides, keys_of(s), js.eval("['area', 'perimeter', 'sides'].map(n => JSON.stringify(Object.getOwnPropertyDescriptor(Shape.prototype, n)).replace(/\"/g, '')).join(' ')")
show("set on a class", set_on_a_class)
def set_again():
    js.eval("Object.defineProperty(Shape.prototype, 'shown', { value: 1, writable: true, enumerable: true, configurable: true })")
    js.Shape.shown, js.Shape.sides = 2, 5
    return keys_of(js.Shape.new()), js.Shape.new().shown, js.Shape.new().sides
show("what is there already stays as it was", set_again)
import copy
show("copy.copy()", lambda: (type(attempt(copy.copy, js.Map.new())).__name__, type(attempt(copy.copy, js.Shape.new())).__name__, keys_of(js.Map.new()), keys_of(js.Shape.new()), keys_of(js.Object.new())))

# An Error that JavaScript made is an instance of a class of Python's, and has never been through its __init__().
import pickle
for source in ("new Error('m')", "new TypeError('m')", "new RangeError('m')", "new Error()", "new (class Mine extends Error {})('m')", "new AggregateError([new Error('a')], 'm')", "Object.assign(new Error('m'), { extra: 1 })"):
    show(source, lambda: (lambda x: (type(x).__name__, x.args, x.__reduce__(), (lambda c: (type(c).__name__, c.args, c is not x))(copy.copy(x)), (lambda c: (type(c).__name__, c.args))(attempt(lambda: pickle.loads(pickle.dumps(x))))))(js.eval(source)))

# Object and Promise are there from the start, and are derived from as any other is.
class Record(js.Object):
    def __init__(self, *args): self.made = True
    def keys(self): return sorted(vars(self))
show("derived from Object", lambda: (lambda r: (type(r).__name__, names(Record), r.made, r.keys(), isinstance(r, js.Object), js.eval("r => [r instanceof Object, r.made, JSON.stringify(r), Object.getPrototypeOf(r) === r.constructor].join(' | ')")(r)))(Record()))
class Later(js.Promise):
    def label(self): return "a promise of Python's"
settled = []
def promises():
    p = Later(lambda resolve, reject: resolve(5))
    q = p.then(lambda v: settled.append(("then", v, )) or v + 1)
    q.then(lambda v: settled.append(("and then", v, type(q).__name__)))
    return type(p).__name__, names(Later), p.label(), isinstance(p, js.Promise), type(q).__name__, js.eval("p => [p instanceof Promise, p.label(), Object.prototype.toString.call(p)].join(' | ')")(p)
show("derived from Promise", promises)
async def waits():
    settled.append(("awaited", await Later(lambda resolve, reject: resolve("value"))))
    try:
        await Later(lambda resolve, reject: reject(js.Error.new("refused")))
    except Exception as error:
        settled.append(("refused", type(error).__name__, str(error)))
js.Promise.resolve(waits()).then(lambda _: print("what came of them =>", settled))
show("nothing can be derived from Function", lambda: type("F", (js.Function,), {}))
