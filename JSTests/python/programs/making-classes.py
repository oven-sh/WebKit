def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
def keys(c): return [(k, type(v).__name__) for k, v in vars(c).items()]

class A: pass
class B(A): pass
class D:
    "doc"
    x = 1
    def m(self): self.a = 1
show("plain", lambda: keys(A))
show("derived", lambda: keys(B))
show("with things", lambda: keys(D))
for base in (list, dict, set, frozenset, tuple, int, float, str, bytes, bytearray, complex, Exception, type, object, type(show) if False else object):
    show("from " + base.__name__, lambda: keys(type("X", (base,), {})))
show("type()", lambda: keys(type("T", (), {"y": 2})))
show("descriptors", lambda: (repr(A.__dict__["__dict__"]), repr(A.__dict__["__weakref__"]), A.__dict__["__dict__"].__name__, A.__dict__["__dict__"].__objclass__, A.__dict__["__dict__"].__doc__, A.__dict__["__weakref__"].__doc__))
show("through the descriptor", lambda: (a := A(), setattr(a, "v", 1), A.__dict__["__dict__"].__get__(a), A.__dict__["__weakref__"].__get__(a), A.__dict__["__dict__"].__get__(B()))[2:])
show("wrong instance", lambda: A.__dict__["__dict__"].__get__(1))
show("object has none", lambda: (hasattr(object, "__dict__"), "__dict__" in vars(object), hasattr(object(), "__dict__"), "__dict__" in dir(object), "__dict__" in dir(int), "__dict__" in dir(A), "__weakref__" in dir(A), "__dict__" in dir(A())))
show("no dict", lambda: object().__dict__)
show("int no dict", lambda: (1).__dict__)
show("qualname", lambda: (A.__qualname__, "__qualname__" in vars(A), type("Q", (), {"__qualname__": "a.b"}).__qualname__, "__qualname__" in vars(type("Q", (), {"__qualname__": "a.b"}))))
show("qualname type", lambda: type("Q", (), {"__qualname__": 1}))

# __slots__
def slots(*s, bases=(), **ns): return type("S", bases, {"__slots__": s if len(s) != 1 or not isinstance(s[0], (str, list, dict)) else s[0], **ns})
show("slots", lambda: keys(slots("b", "a")))
show("slots none", lambda: keys(slots()))
show("slots str", lambda: keys(slots("ab")))
show("slots list", lambda: keys(slots(["x", "y"])))
show("slots dict", lambda: (keys(slots({"x": "doc of x", "y": None})), slots({"x": "doc of x"}).x.__doc__))
show("slots with dict", lambda: keys(slots("a", "__dict__")))
show("slots with weakref", lambda: keys(slots("a", "__weakref__")))
show("slots with both", lambda: keys(slots("__weakref__", "__dict__")))
show("slots dict works", lambda: (o := slots("a", "__dict__")(), setattr(o, "z", 1), setattr(o, "a", 2), o.__dict__, o.a)[3:])
show("slots no dict", lambda: setattr(slots("a")(), "z", 1))
show("slots no __dict__", lambda: slots("a")().__dict__)
show("slots twice dict", lambda: slots("__dict__", "__dict__"))
show("slots twice weakref", lambda: slots("__weakref__", "__weakref__"))
show("slots dict again", lambda: slots("__dict__", bases=(A,)))
show("slots weakref again", lambda: slots("__weakref__", bases=(A,)))
show("slots under dict", lambda: (keys(slots("a", bases=(A,))), hasattr(slots("a", bases=(A,))(), "__dict__")))
show("slots under slots", lambda: keys(slots("b", bases=(slots("a"),))))
show("plain under slots", lambda: keys(type("P", (slots("a"),), {})))
show("not identifier", lambda: slots("a b"))
show("not string", lambda: slots(1))
show("empty name", lambda: slots(""))
show("conflict", lambda: slots("a", a=1))
show("mangled", lambda: keys(type("M", (), {"__slots__": ("__p", "__q__", "_r")})))
show("mangled underscore class", lambda: keys(type("_M", (), {"__slots__": ("__p",)})))
show("duplicate", lambda: keys(slots("a", "a")))
show("int slots", lambda: slots("a", bases=(int,)))
show("int empty slots", lambda: keys(slots(bases=(int,))))
show("tuple slots", lambda: slots("a", bases=(tuple,)))
show("bytes slots", lambda: slots("a", bases=(bytes,)))
show("list slots", lambda: keys(slots("a", bases=(list,))))
show("str slots", lambda: keys(slots("a", bases=(str,))))
show("exception slots", lambda: (keys(slots("a", bases=(Exception,))), hasattr(slots("a", bases=(Exception,))(), "__dict__")))
show("two slotted bases", lambda: type("Z", (slots("a"), slots("b")), {}))
show("slotted and empty", lambda: keys(type("Z", (slots("a"), slots()), {})))
show("__slots__ kept", lambda: (slots("a", "b").__slots__, slots("ab").__slots__, slots(["x"]).__slots__))
show("iterator slots", lambda: keys(type("I", (), {"__slots__": iter(["p", "q"])})))
show("member", lambda: (s := slots("a"), repr(s.a), s.a.__name__, s.a.__objclass__ is s, type(s.a).__name__, s.a.__doc__)[1:])
show("unset", lambda: slots("a")().a)
show("delete unset", lambda: delattr(slots("a")(), "a"))
show("set get delete", lambda: (o := slots("a")(), setattr(o, "a", 5), o.a, delattr(o, "a"), hasattr(o, "a"))[2:])

# what the namespace may hold
import _warnings
_warnings.filters.insert(0, ("ignore", None, RuntimeWarning, None, 0)) # It is warned of.
show("non-string key", lambda: (c := type("N", (), {1: "one", "s": 2}), sorted((k for k in vars(c) if not isinstance(k, str) or k == "s"), key=str), vars(c)[1])[1:])
show("module", lambda: (type("N", (), {}).__module__, type("N", (), {"__module__": "mm"}).__module__, A.__module__))
show("set __dict__", lambda: (a := A(), setattr(a, "__dict__", {"k": 1}), a.k, a.__dict__)[2:])
show("shared __dict__", lambda: (a := A(), b := A(), d := {"k": 1}, setattr(a, "__dict__", d), setattr(b, "__dict__", d), setattr(a, "n", 2), b.n, a.__dict__ is b.__dict__, a.__dict__ is d)[6:])
show("set __dict__ wrong", lambda: setattr(A(), "__dict__", 1))
show("delete __dict__", lambda: (a := A(), setattr(a, "v", 1), delattr(a, "__dict__"), a.__dict__)[3:])
show("class __dict__ read only", lambda: setattr(A, "__dict__", {}))
show("function __dict__", lambda: (keys(type(show))[:0], "__dict__" in vars(type(show)), type(vars(type(show))["__dict__"]).__name__, show.__dict__))
show("exception __dict__", lambda: ("__dict__" in vars(BaseException), "__dict__" in vars(Exception), type(vars(BaseException)["__dict__"]).__name__, ValueError().__dict__))
show("module __dict__", lambda: ("__dict__" in vars(type(__builtins__)), type(vars(type(__builtins__))["__dict__"]).__name__))
show("type __dict__", lambda: (type(vars(type)["__dict__"]).__name__, type(A.__dict__).__name__))

# one dict for many: what is set on any of them is set on all
class Borg:
    _shared = {}
    def __init__(self):
        self.__dict__ = self._shared
show("borg", lambda: (a := Borg(), b := Borg(), c := Borg(), setattr(a, "x", 1), b.x, setattr(c, "y", 2), a.y, sorted(Borg._shared.items()), a.__dict__ is b.__dict__ is Borg._shared, delattr(b, "x"), hasattr(a, "x"), vars(c))[4:])
show("borg parts", lambda: (a := Borg(), b := Borg(), setattr(a, "k", 1), setattr(a, "__dict__", {"own": 1}), a.own, hasattr(a, "k"), b.k, Borg._shared["k"], setattr(b, "z", 3), Borg._shared["z"], hasattr(a, "z"))[4:])
show("static attributes", lambda: (D.__static_attributes__, A.__static_attributes__))
class SA:
    def f(self):
        self.b = 1; self.a = 2; self.a += 1
        other.c = 3
        def inner(): self.d = 4
        lambda: (self.e)
        for self.g in (): pass
        with x as self.h: pass
        del self.i
        self.j: int = 1
        self.k.l = 1
        (self.m, [self.n]) = 1, [2]
    self = None
    class Nested:
        def g(self): self.inner_one = 1
    def h(this): this.z = 1
    @staticmethod
    def s(): self.t = 1
show("static attributes of all kinds", lambda: (SA.__static_attributes__, SA.Nested.__static_attributes__))
show("sizes", lambda: [(t.__name__, t.__basicsize__, t.__itemsize__, t.__dictoffset__, t.__weakrefoffset__) for t in (object, int, list, A, B, slots("a", "b"), type("X", (list,), {}), type("X", (int,), {}), Exception, type("X", (Exception,), {}), type)] if hasattr(type, "__basicsize__") else "none")

# type.__flags__
class S: __slots__ = ("a",)
class S0: __slots__ = ()
class SD: __slots__ = ("__dict__",)
class SW: __slots__ = ("__weakref__",)
class M(type): pass
def bits(n): return [i for i in range(40) if n >> i & 1]
for t in (object, int, list, dict, tuple, str, bytes, type, BaseException, Exception, A, B, S, S0, SD, SW, type("L", (list,), {}), type("I", (int,), {}), type("D", (dict,), {}), type("T", (tuple,), {}), type("St", (str,), {}), type("By", (bytes,), {}), type("E", (Exception,), {}), M, type("F", (float,), {}), type("Se", (set,), {})):
    print(t.__name__, bits(t.__flags__))
