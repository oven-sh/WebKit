# _weakref. When a thing is found to be no more is not the same everywhere, so it is always asked for here before anything is made of it.
import sys
import _weakref
from _weakref import ref, proxy, getweakrefcount, getweakrefs, _remove_dead_weakref, ReferenceType, ProxyType, CallableProxyType

try:
    import js
    collect_everything = js.fullGC
except ImportError:
    import gc
    collect_everything = gc.collect


def collect():
    collect_everything()


def nowhere(text):
    out, i = "", 0
    while True:
        j = text.find("0x", i)
        if j < 0:
            return out + text[i:]
        k = j + 2
        while k < len(text) and text[k] in "0123456789abcdef":
            k += 1
        out += text[i:j] + "0x"
        i = k


def t(label, f):
    try:
        r = f()
    except BaseException as e:
        r = type(e).__name__ + ": " + str(e)
    print(label, "=>", nowhere(repr(r)) if not isinstance(r, str) else nowhere(r))


class C:
    def __init__(self, n=0): self.n = n
    def __repr__(self): return "C(%r)" % self.n
    def __eq__(self, other): return isinstance(other, C) and self.n == other.n
    def __hash__(self): return hash(self.n)
    def method(self): return self.n


class Slots:
    __slots__ = ("a",)


class SlotsWeak:
    __slots__ = ("a", "__weakref__")


def function(): pass


print("---- what there can be one to")
for label, make in (("an instance", lambda: C()), ("a class", lambda: C), ("a function", lambda: function), ("a lambda", lambda: lambda: 1), ("a method", lambda: C().method), ("a built-in function", lambda: len),
                    ("a set", lambda: {1}), ("a frozenset", lambda: frozenset({1})), ("a generator", lambda: (x for x in ())), ("a module", lambda: sys), ("a type", lambda: int), ("object()", lambda: object()),
                    ("an int", lambda: 1), ("a large int", lambda: 2 ** 70), ("a float", lambda: 1.5), ("a str", lambda: "a"), ("bytes", lambda: b"a"), ("a bytearray", lambda: bytearray()), ("a tuple", lambda: (1,)),
                    ("a list", lambda: [1]), ("a dict", lambda: {}), ("None", lambda: None), ("True", lambda: True), ("...", lambda: ...), ("a range", lambda: range(1)), ("a slice", lambda: slice(1)),
                    ("a complex", lambda: 1j), ("a memoryview", lambda: memoryview(b"")), ("__slots__", lambda: Slots()), ("__slots__ with __weakref__", lambda: SlotsWeak()), ("an exception", lambda: ValueError()),
                    ("a code object", lambda: function.__code__), ("a frame", lambda: sys._getframe()), ("a property", lambda: property()), ("a staticmethod", lambda: staticmethod(len)), ("a ref", lambda: ref(C)),
                    ("a class derived from list", lambda: type("L", (list,), {})()), ("from int", lambda: type("I", (int,), {})()), ("from tuple", lambda: type("T", (tuple,), {})()), ("from str", lambda: type("S", (str,), {})()),
                    ("a coroutine", lambda: KEEP.append(coroutine()) or KEEP[-1]), ("a cell", lambda: (lambda x: lambda: x)(1).__closure__[0]), ("a mappingproxy", lambda: C.__dict__), ("an iterator", lambda: iter([]))):
    KEEP = []

    async def coroutine(): pass

    def attempt():
        o = make()
        r = ref(o)
        return type(r).__name__, r() is o
    t(label, attempt)
    for c in KEEP:
        c.close()

print("---- one is like another")
o = C(1)
t("ref(o) is ref(o)", lambda: (ref(o) is ref(o), ref(o, None) is ref(o), collect(), getweakrefcount(o)))
callback = lambda r: None
kept = [ref(o), ref(o, callback), ref(o, callback), proxy(o), proxy(o), proxy(o, callback)]
t("with a callback it is another", lambda: (kept[1] is kept[2], kept[1] == kept[2], kept[3] is kept[4], kept[3] is kept[5], getweakrefcount(o)))
t("the order that they are in", lambda: [kept.index(r) for r in getweakrefs(o)])
t("__weakref__", lambda: (o.__weakref__ is kept[0], C(2).__weakref__, SlotsWeak().__weakref__))
t("__callback__", lambda: (kept[0].__callback__, kept[1].__callback__ is callback))
t("__callback__ cannot be set", lambda: setattr(kept[1], "__callback__", None))


class Derived(ref):
    def __init__(self, o, callback=None, extra=None):
        super().__init__(o, callback)
        self.extra = extra


t("a class derived from ref", lambda: (Derived(o) is Derived(o), Derived(o, None, 5).extra, Derived(o, extra=6).extra, Derived(o)() is o, isinstance(Derived(o), ref), Derived(o) == ref(o)))
t("the classes", lambda: (ref is ReferenceType, ref.__name__, ref.__module__, ref.__qualname__, ProxyType.__name__, CallableProxyType.__name__, type(proxy(o)).__name__, type(proxy(function)).__name__, ref[int]))
t("cannot be derived from", lambda: type("P", (ProxyType,), {}))

print("---- what is said")
t("repr", lambda: (repr(ref(o)), repr(ref(function)), repr(ref(C)), repr(proxy(o)), repr(ref(sys))))
t("no arguments", lambda: ref())
t("three", lambda: ref(o, None, 1))
t("by name", lambda: ref(o, callback=None))
t("called with something", lambda: ref(o)(1))
t("called by name", lambda: ref(o)(a=1))
t("__init__ again", lambda: (ref(o).__init__(o), ref(o).__init__(C(9)), ref(o)() is o))
t("__init__ with nothing", lambda: ref(o).__init__())
t("proxy()", lambda: proxy())
t("proxy(1)", lambda: proxy(1))
t("getweakrefcount(1)", lambda: (getweakrefcount(1), getweakrefs(1), getweakrefcount(C(3)), getweakrefs(C(3))))
t("ProxyType()", lambda: ProxyType(o))

print("---- equal, and hash")
a, b, c = C(1), C(1), C(2)
t("as what they refer to", lambda: (ref(a) == ref(b), ref(a) != ref(b), ref(a) == ref(c), ref(a) == a, hash(ref(a)) == hash(a), ref(a) is ref(b)))
t("no order", lambda: ref(a) < ref(b))
t("in a dict", lambda: {ref(a): 1}[ref(b)])
a_set = {1}
t("what cannot be hashed", lambda: hash(ref(a_set)))
t("a proxy cannot", lambda: hash(proxy(a)))


def dead(callback=None, hashed=False, kind=ref):
    o = C(5)
    r = kind(o, callback)
    if hashed:
        hash(r)
    return r


print("---- when it is no more")
r1, r2, r3, p1 = dead(), dead(), dead(hashed=True), dead(kind=proxy)
collect()
t("called", lambda: (r1(), r2(), r3()))
t("repr", lambda: (repr(r1), repr(p1)))
t("equal only to itself", lambda: (r1 == r1, r1 == r2, r1 != r2, r1 != r1, r1 == ref(C(5))))
t("hash", lambda: hash(r1))
t("hash, if it had been asked for", lambda: hash(r3) == hash(C(5)))
for label, f in (("attribute", lambda: p1.n), ("set", lambda: setattr(p1, "n", 1)), ("del", lambda: delattr(p1, "n")), ("str", lambda: str(p1)), ("bool", lambda: bool(p1)), ("len", lambda: len(p1)), ("+", lambda: p1 + 1),
                 ("reflected", lambda: 1 + p1), ("==", lambda: p1 == 1), ("in", lambda: 1 in p1), ("[]", lambda: p1[0]), ("iter", lambda: iter(p1)), ("next", lambda: next(p1)), ("-", lambda: -p1), ("int", lambda: int(p1)),
                 ("type", lambda: type(p1).__name__), ("isinstance", lambda: isinstance(p1, C)), ("__class__", lambda: p1.__class__)):
    t("a proxy: " + label, f)

print("---- callbacks")
called = []
refs = [dead(lambda r, i=i: called.append((i, r(), r.__callback__))) for i in range(3)]
collect()
t("each is called, with the reference", lambda: sorted(called))


def several():
    o = C(6)
    return [ref(o, lambda r: called.append("first")), ref(o, lambda r: called.append("second")), proxy(o, lambda r: called.append("third")), ref(o)]


del called[:]
refs = several()
collect()
t("the last made first", lambda: called)


def dropped():
    o = C(7)
    ref(o, lambda r: called.append("dropped"))
    kept = ref(o, lambda r: called.append("kept"))
    return kept


del called[:]
refs = dropped()
collect()
t("not that of a reference that has gone too", lambda: called)


def raises(r):
    raise ValueError("in a callback")


hooked = []
hook, sys.unraisablehook = sys.unraisablehook, lambda u: hooked.append((type(u.exc_value).__name__, str(u.exc_value), nowhere(u.err_msg), u.object))
refs = dead(raises)
collect()
t("what one raises", lambda: hooked)
sys.unraisablehook = hook


def cycle():
    a, b = C(8), C(9)
    a.other, b.other = b, a
    return ref(a, lambda r: called.append("a")), ref(b, lambda r: called.append("b"))


del called[:]
refs = cycle()
collect()
t("two that refer to each other", lambda: (sorted(called), refs[0](), refs[1]()))

print("---- while it is there")
o = C(10)
r = ref(o, lambda r: called.append("gone"))
del called[:]
collect()
t("it stays", lambda: (r() is o, called, getweakrefcount(o)))


def drop_references():
    for i in range(100):
        ref(o, callback)


drop_references()
collect()
t("references that are dropped are not kept", lambda: getweakrefcount(o))

print("---- _remove_dead_weakref")
d = {"dead": dead(), "alive": ref(o), "other": 1}
collect()
t("takes out what is dead", lambda: (_remove_dead_weakref(d, "dead"), _remove_dead_weakref(d, "alive"), _remove_dead_weakref(d, "missing"), sorted(d)))
t("what is no reference", lambda: _remove_dead_weakref(d, "other"))
t("of what is no dict", lambda: _remove_dead_weakref([], 0))
t("with what cannot be hashed", lambda: _remove_dead_weakref({}, []))

print("---- a proxy does what it stands for does")


class Number:
    def __init__(self, v): self.v = v
    def __repr__(self): return "Number(%r)" % self.v
    def __str__(self): return "n%r" % self.v
    def __bytes__(self): return b"bytes"
    def __reversed__(self): return "reversed"
    def __call__(self, *a, **k): return ("called", a, k)
    def __len__(self): return 3
    def __getitem__(self, k): return ("item", k)
    def __setitem__(self, k, v): self.set = (k, v)
    def __delitem__(self, k): self.deleted = k
    def __contains__(self, x): return x == self.v
    def __iter__(self): return iter([self.v])
    def __bool__(self): return bool(self.v)
    def __int__(self): return int(self.v)
    def __float__(self): return float(self.v)
    def __index__(self): return int(self.v)
    def __neg__(self): return ("neg", self.v)
    def __pos__(self): return ("pos", self.v)
    def __abs__(self): return ("abs", self.v)
    def __invert__(self): return ("invert", self.v)
    def __eq__(self, other): return ("eq", other)
    def __lt__(self, other): return ("lt", other)
    def __pow__(self, other, mod=None): return ("pow", other, mod)
    def __rpow__(self, other, mod=None): return ("rpow", other, mod)
    def __ipow__(self, other): return ("ipow", other)
    def __divmod__(self, other): return ("divmod", other)
    def __rdivmod__(self, other): return ("rdivmod", other)


for name in ("add", "sub", "mul", "matmul", "truediv", "floordiv", "mod", "lshift", "rshift", "and", "or", "xor"):
    for prefix in ("", "r", "i"):
        setattr(Number, "__%s%s__" % (prefix, name), lambda self, other, n=prefix + name: (n, other))
n = Number(4)
p = proxy(n)
t("its class", lambda: (type(p).__name__, p.__class__.__name__, isinstance(p, Number), callable(p)))
t("attributes", lambda: (p.v, setattr(p, "w", 5), n.w, delattr(p, "w"), hasattr(n, "w"), sorted(k for k in dir(p) if not k.startswith("_"))))
t("str and repr", lambda: (str(p), repr(p), bytes(p), format(p)))
t("reversed, of one that can be called", lambda: reversed(p))


class Plain(Number):
    __call__ = None


plain = Plain(4)
t("and of one that cannot", lambda: (type(proxy(plain)).__name__, reversed(proxy(plain)), bytes(proxy(plain))))
t("call", lambda: p(1, a=2))
t("a container", lambda: (len(p), p[1], p[1:2], p.__setitem__(1, 2), n.set, p.__delitem__(3), n.deleted, 4 in p, 5 in p, list(p), bool(p)))
t("a number", lambda: (int(p), float(p), [0, 1, 2, 3, 4][p], -p, +p, abs(p), ~p, p == 1, p < 1, p ** 2, pow(p, 2, 3), 2 ** p, divmod(p, 2), divmod(2, p)))
import _warnings
t("operators", lambda: [eval("p %s 1" % op) for op in ("+", "-", "*", "@", "/", "//", "%", "<<", ">>", "&", "|", "^")])
t("reflected", lambda: [eval("1 %s p" % op) for op in ("+", "-", "*", "@", "/", "//", "%", "<<", ">>", "&", "|", "^")])


def in_place(op):
    q = proxy(n)
    namespace = {"q": q}
    exec("q %s= 1" % op, namespace)
    return namespace["q"]


t("in place", lambda: [in_place(op) for op in ("+", "-", "*", "@", "/", "//", "%", "<<", ">>", "&", "|", "^", "**")])
t("two proxies", lambda: (p + p, p == p))
t("next, of what is no iterator", lambda: next(p))
it = iter([1, 2])


class Iterator:
    def __iter__(self): return self
    def __next__(self): return next(it)


i = Iterator()
t("next", lambda: (next(proxy(i)), next(proxy(i)), next(proxy(i), "no more")))
t("a proxy of what cannot be called", lambda: proxy(C())())
t("what the classes have", lambda: (sorted(ProxyType.__dict__), sorted(set(CallableProxyType.__dict__) ^ set(ProxyType.__dict__)), sorted(ref.__dict__)))
