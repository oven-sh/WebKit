# Weak references of Python's to what is JavaScript's, and of JavaScript's to what is Python's.
import js
from _weakref import ref, proxy, getweakrefcount, getweakrefs


def t(label, f):
    try:
        r = f()
    except BaseException as e:
        r = type(e).__name__ + ": " + str(e)
    print(label, "=>", r)


def collect():
    js.fullGC()


print("---- what of JavaScript's there can be one to")
for label, source in (("an object", "({a: 1})"), ("a function", "(function f() {})"), ("an arrow function", "(() => 1)"), ("a class", "(class K {})"), ("an instance of one", "new (class K {})"), ("a Map", "new Map"),
                      ("a Promise", "Promise.resolve(1)"), ("one that is frozen", "Object.freeze({a: 1})"), ("one that cannot be extended", "Object.preventExtensions({})"), ("a Proxy", "new Proxy({}, {})"), ("a Date", "new Date(0)"),
                      ("one with no prototype", "Object.create(null)"), ("a RegExp", "/a/"), ("globalThis", "globalThis"), ("a WeakRef", "new WeakRef({})"),
                      # These are what Python has already, and it is as it is with those.
                      ("an array, which is a list", "[1, 2]"), ("a Uint8Array, which is a bytearray", "new Uint8Array(2)"), ("an Error, which is an exception", "new Error('e')"), ("a Symbol", "Symbol('s')"), ("a string", "'s'"), ("a number", "1.5"), ("a BigInt", "10n ** 30n")):
    def attempt():
        o = js.eval(source)
        r = ref(o)
        return r() is o, ref(o) is r, getweakrefcount(o), o.__weakref__ is r
    t(label, attempt)

print("---- JavaScript sees nothing of it")
o = js.eval("({a: 1})")
kept = ref(o)
t("its properties", lambda: (list(js.Object.keys(o)), list(js.Reflect.ownKeys(o)), js.JSON.stringify(o), list(js.Object.getOwnPropertySymbols(o)), sorted(vars(o))))
frozen = js.eval("Object.freeze({a: 1})")
kept_frozen = ref(frozen)
t("it is as frozen as it was", lambda: (js.Object.isFrozen(frozen), list(js.Reflect.ownKeys(frozen))))

print("---- when it is no more")
called = []


def dead(source, callback=None):
    return ref(js.eval(source), callback)


refs = [dead("({a: 1})"), dead("(function () {})"), dead("new Map", lambda r: called.append("a Map")), dead("Promise.resolve(1)", lambda r: called.append("a Promise"))]
collect()
t("they are let go of", lambda: ([r() for r in refs], sorted(called)))
holder = js.eval("({})")
holder.kept = js.eval("({b: 2})")
r = ref(holder.kept, lambda r: called.append("what an object had"))
del called[:]
collect()
t("not while JavaScript has it", lambda: (r().b, called))
js.Reflect.deleteProperty(holder, "kept")
collect()
t("and then", lambda: (r(), called))

print("---- a callback that is JavaScript's")
log = js.eval("globalThis.log = []")
callback = js.eval("(function (r) { log.push(typeof r, r() === undefined); })")


class C:
    pass


def with_javascript_callback():
    return ref(C(), callback)


kept = with_javascript_callback()
collect()
t("is called", lambda: list(log))

print("---- a proxy for what is JavaScript's")
target = js.eval("({a: 1, f() { return this.a + 1; }})")
p = proxy(target)
t("does what it does", lambda: (type(p).__name__, p.a, p.f(), setattr(p, "b", 5), target.b, "a" in p, p["a"], bool(p)))
function = js.eval("(function (x, y) { return x + y; })")
t("and can be called", lambda: (type(proxy(function)).__name__, proxy(function)(1, 2)))

print("---- JavaScript's, to what is Python's")
make = js.eval("(function (o) { return new WeakRef(o); })")
c = C()
w = make(c)
both = ref(c)
t("the one object", lambda: (w.deref() is c, both() is c))
del c
# What a WeakRef has been asked for is kept until whatever asked has run its course.
t("to JavaScript, one of Python's", lambda: js.eval("(function (r) { return [typeof r, String(r).startsWith('<weakref at '), typeof r()]; })")(both))
registry_log = js.eval("globalThis.gone = []")
registry = js.eval("new FinalizationRegistry(function (held) { gone.push(held); })")


def register():
    registry.register(C(), "an instance of C")


register()
t("weakref.ref is a class to JavaScript too", lambda: js.eval("(function (ref, o) { const r = new ref(o); return r() === o; })")(ref, target))
