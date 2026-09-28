import sys
# What is written in C leaves some of this to the copyreg module. This stands in for it, here and in CPython alike.
stub = type(sys)("copyreg")
def __newobj__(cls, *args): return cls.__new__(cls, *args)
def __newobj_ex__(cls, args, kwargs): return cls.__new__(cls, *args, **kwargs)
def _reconstructor(cls, base, state): return object.__new__(cls) if base is object else base.__new__(cls, state)
def _reduce_ex(self, proto):
    return ("_reduce_ex was asked", type(self).__name__, proto)
def _slotnames(cls):
    names = []
    for c in cls.__mro__:
        s = c.__dict__.get("__slots__", ())
        for n in ((s,) if isinstance(s, str) else s):
            if n not in ("__dict__", "__weakref__"): names.append(n)
    try: cls.__slotnames__ = names
    except Exception: pass
    return names
for f in (__newobj__, __newobj_ex__, _reconstructor, _reduce_ex, _slotnames): setattr(stub, f.__name__, f)
sys.modules["copyreg"] = stub

def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
def tidy(r):
    return tuple(getattr(x, "__name__", None) or (list(x) if hasattr(x, "__next__") else x) for x in r)

class C:
    def __init__(s): s.a = 1
show("plain", lambda: tidy(C().__reduce_ex__(2)))
show("every protocol", lambda: [tidy(C().__reduce_ex__(p))[:3] for p in (0, 1, 2, 3, 4, 5, 100, -1)])
show("reduce", lambda: C().__reduce__())
show("object", lambda: tidy(object().__reduce_ex__(2)))
class Empty: pass
show("nothing in it", lambda: tidy(Empty().__reduce_ex__(2)))
show("getstate", lambda: (C().__getstate__(), Empty().__getstate__(), object().__getstate__(), (1).__getstate__(), "s".__getstate__(), [].__getstate__()))
show("the state is the dict", lambda: (lambda c: c.__getstate__() is c.__dict__)(C()))
class S:
    __slots__ = ("x", "y")
def slotted():
    s = S(); s.x = 1; return s
show("slots", lambda: (slotted().__getstate__(), S().__getstate__(), tidy(slotted().__reduce_ex__(2)), S.__slotnames__))
class SD(S): pass
def both():
    s = SD(); s.x = 1; s.z = 2; return s
show("slots and a dict", lambda: (both().__getstate__(), tidy(both().__reduce_ex__(2))))
class L(list): pass
class D(dict): pass
def filled(o, *a):
    o.tag = 1; return o
show("a list", lambda: tidy(filled(L([1, 2])).__reduce_ex__(2)))
show("a dict", lambda: tidy(filled(D(a=1)).__reduce_ex__(2)))
show("built-in list and dict", lambda: (tidy([1].__reduce_ex__(2)), tidy({"a": 1}.__reduce_ex__(2))))
class I(int): pass
class T(tuple): pass
class St(str): pass
show("derived from int", lambda: tidy(I(5).__reduce_ex__(2)))
show("derived from tuple", lambda: tidy(T((1, 2)).__reduce_ex__(2)))
show("derived from str", lambda: tidy(St("s").__reduce_ex__(2)))
show("built-in values", lambda: [attempt(lambda: tidy(v.__reduce_ex__(2))) for v in (1, 1.5, "s", (1,), None, True, b"b", 1j, frozenset({1}), {1}, range(2), slice(1), ..., NotImplemented)])
def attempt(f):
    try: return f()
    except Exception as e: return type(e).__name__ + ": " + str(e)
show("built-in values", lambda: [attempt(lambda: tidy(v.__reduce_ex__(2))) for v in (1, 1.5, "s", (1,), None, True, b"b", 1j, frozenset({1}), {1}, range(2), slice(1), ..., NotImplemented)])
show("cannot be pickled", lambda: [attempt(lambda: v.__reduce_ex__(2)) for v in (lambda: 0, (i for i in ()), sys, type(sys), iter([]).__class__, memoryview(b""), {}.keys(), len)])
show("a method", lambda: (lambda c: (lambda r: (r[0].__name__, r[1][0] is c, r[1][1]))(c.__init__.__reduce__()))(C()))
class NA:
    def __getnewargs__(s): return (1, 2)
class NAE:
    def __getnewargs_ex__(s): return ((1,), {"k": 2})
class NAE0:
    def __getnewargs_ex__(s): return ((1,), {})
show("getnewargs", lambda: (tidy(NA().__reduce_ex__(2)), tidy(NAE().__reduce_ex__(2)), tidy(NAE0().__reduce_ex__(2))))
for label, cls in [("not a tuple", type("X", (), {"__getnewargs__": lambda s: [1]})), ("ex not a tuple", type("X", (), {"__getnewargs_ex__": lambda s: [1]})), ("ex of three", type("X", (), {"__getnewargs_ex__": lambda s: (1, 2, 3)})),
        ("ex first", type("X", (), {"__getnewargs_ex__": lambda s: ([], {})})), ("ex second", type("X", (), {"__getnewargs_ex__": lambda s: ((), [])})), ("raises", type("X", (), {"__getnewargs__": lambda s: 1 / 0}))]:
    show("getnewargs " + label, lambda: cls().__reduce_ex__(2))
class GS:
    def __getstate__(s): return "own state"
show("own getstate", lambda: tidy(GS().__reduce_ex__(2)))
class OR:
    def __reduce__(s): return "own reduce"
show("own reduce", lambda: (OR().__reduce_ex__(2), OR().__reduce_ex__(0), OR().__reduce__()))
class ORI: pass
def instance_reduce():
    o = ORI(); o.__reduce__ = lambda: "the instance's"; return o.__reduce_ex__(2)
show("the instance's reduce is not asked", lambda: tidy(instance_reduce())[:2])
class ORE:
    def __reduce_ex__(s, p): return ("own reduce_ex", p)
show("own reduce_ex", lambda: (ORE().__reduce_ex__(3), tidy(object.__reduce_ex__(ORE(), 2))[:2]))
show("protocol wrong", lambda: C().__reduce_ex__("2"))
show("protocol missing", lambda: C().__reduce_ex__())
show("protocol too big", lambda: C().__reduce_ex__(1 << 40))
show("protocol index", lambda: tidy(C().__reduce_ex__(True))[:2])
class BadNames: __slotnames__ = 5
show("slotnames wrong", lambda: BadNames().__getstate__())
class NoneNames: __slotnames__ = None
show("slotnames None", lambda: NoneNames().__getstate__())
class W:
    __slots__ = ("__weakref__", "__dict__")
show("weakref and dict slots", lambda: tidy(W().__reduce_ex__(2)))
class E(Exception): pass
show("an exception", lambda: (E(1, 2).__reduce_ex__(2), E().__reduce__()))
show("a class", lambda: attempt(lambda: C.__reduce_ex__(2)))
show("sizes", lambda: (object().__sizeof__(), C().__sizeof__(), S().__sizeof__(), ().__sizeof__(), (1, 2).__sizeof__(), T((1, 2, 3)).__sizeof__(), b"".__sizeof__(), b"abc".__sizeof__(), None.__sizeof__(), (1.5).__sizeof__(), (1j).__sizeof__(), range(1).__sizeof__(), slice(1).__sizeof__(), show.__sizeof__(), E().__sizeof__()))
show("sizeof wrong", lambda: object().__sizeof__(1))
show("where they are", lambda: [n in vars(object) for n in ("__reduce__", "__reduce_ex__", "__getstate__", "__sizeof__")])
show("kinds", lambda: [type(vars(object)[n]).__name__ for n in ("__reduce__", "__reduce_ex__", "__getstate__", "__sizeof__")])
show("in dir", lambda: [n for n in dir(object()) if n in ("__reduce__", "__reduce_ex__", "__getstate__", "__sizeof__")])
del sys.modules["copyreg"]
