import sys
def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)

# ---- property
class C:
    def __init__(s): s._x = 0
    @property
    def x(s):
        "the doc"
        return s._x
    @x.setter
    def x(s, v): s._x = v
    @x.deleter
    def x(s): del s._x
    ro = property(lambda s: 1)
    wo = property(None, lambda s, v: None)
    nothing = property()
    renamed = property(lambda s: 2, doc="given")
c = C()
show("get set delete", lambda: (c.x, setattr(c, "x", 5), c.x, delattr(c, "x"), hasattr(c, "_x")))
show("attributes", lambda: (C.x.__doc__, C.x.__name__, C.x.fget.__name__, C.x.fset.__name__, C.x.fdel.__name__, C.ro.fset, C.ro.fdel, C.ro.__doc__, C.renamed.__doc__))
show("names", lambda: (C.ro.__name__, C.wo.__name__, C.nothing.__name__, C.renamed.__name__))
show("no name", lambda: property().__name__)
show("name from getter", lambda: property(len).__name__)
show("getter without a name", lambda: property(C()).__name__)
show("set name", lambda: (lambda p: (setattr(p, "__name__", "n"), p.__name__, delattr(p, "__name__"), p.__name__))(property(len)))
show("no setter", lambda: setattr(c, "ro", 1))
show("no deleter", lambda: delattr(c, "ro"))
show("no getter", lambda: c.wo)
show("nothing get", lambda: c.nothing)
show("nothing set", lambda: setattr(c, "nothing", 1))
show("nothing delete", lambda: delattr(c, "nothing"))
show("unnamed get", lambda: property().__get__(c))
show("unnamed set", lambda: property().__set__(c, 1))
show("unnamed delete", lambda: property().__delete__(c))
show("unnamed with a getter", lambda: property(len).__set__(c, 1))
show("from the class", lambda: (C.x is C.__dict__["x"], C.x.__get__(None, C) is C.x, C.x.__get__(c) if hasattr(c, "_x") else "gone"))
show("__get__ of nothing", lambda: C.x.__get__(None))
show("__get__ of nothing at all", lambda: C.x.__get__(None, None))
show("__get__ wrong", lambda: C.x.__get__())
show("__set__ wrong", lambda: C.x.__set__(c))
show("set_name", lambda: (lambda p: (p.__set_name__(C, "given"), p.__name__)[1])(property()))
show("set_name wrong", lambda: property().__set_name__(C))
show("set_name wrong 2", lambda: property().__set_name__(C, "a", "b"))
show("copies", lambda: (C.ro.getter(len).fget, C.ro.setter(len).fset, C.ro.deleter(len).fdel, C.ro.setter(len).fget is C.ro.fget, C.ro.setter(len) is C.ro, C.ro.setter(None).fset, C.x.getter(None).fget is C.x.fget))
show("copies keep the name", lambda: (C.ro.setter(len).__name__, C.x.setter(len).__name__))
def g1(s): "doc one"
def g2(s): "doc two"
def g3(s): pass
show("doc follows the getter", lambda: (property(g1).__doc__, property(g1).getter(g2).__doc__, property(g1).getter(g3).__doc__, property(g1, doc="own").getter(g2).__doc__, property(g3).getter(g1).__doc__, property(g1).setter(g2).__doc__))
show("set doc", lambda: (lambda p: (setattr(p, "__doc__", "new"), p.__doc__, delattr(p, "__doc__"), p.__doc__))(property(g1)))
show("read only", lambda: setattr(property(), "fget", len))
show("no attributes", lambda: setattr(property(), "other", 1))
show("init again", lambda: (lambda p: (p.__init__(g2), p.fget.__name__, p.__doc__, p.fset))(property(g1, g1)))
show("keywords", lambda: (lambda p: (p.fget, p.fset, p.fdel, p.__doc__))(property(fdel=len, doc="d", fset=abs)))
show("too many", lambda: property(1, 2, 3, 4, 5))
show("bad keyword", lambda: property(nope=1))
class P(property):
    "class doc"
class Q(property):
    __slots__ = ()
show("derived", lambda: (P(g1).__doc__, P().__doc__, P(g3).__doc__, P(g1, doc="own").__doc__, type(P(g1).setter(g2)).__name__, P(g1).__dict__, P.__doc__))
show("derived with no room", lambda: Q(g3).__doc__)
show("derived with no room for the getter's", lambda: Q(g1))
class D:
    p = P(lambda s: "from P")
show("derived in use", lambda: (D().p, D.p.__name__))
show("derived no setter", lambda: setattr(D(), "p", 1))
show("abstract", lambda: (property().__isabstractmethod__, property(g1).__isabstractmethod__))
def ab(s): pass
ab.__isabstractmethod__ = True
show("abstract functions", lambda: (property(ab).__isabstractmethod__, property(None, ab).__isabstractmethod__, property(None, None, ab).__isabstractmethod__, staticmethod(ab).__isabstractmethod__, classmethod(ab).__isabstractmethod__, staticmethod(g1).__isabstractmethod__))
show("abstract read only", lambda: setattr(property(), "__isabstractmethod__", True))
show("own attributes", lambda: sorted(vars(property)))
show("raises", lambda: type("R", (), {"p": property(lambda s: 1 / 0)})().p)
show("attribute error falls to getattr", lambda: type("R", (), {"p": property(lambda s: s.missing), "__getattr__": lambda s, n: "got " + n})().p)
show("nested class name", lambda: setattr(type("Outer.Inner", (), {"p": property(len), "__qualname__": "Outer.Inner"})(), "p", 1))

# ---- abstract classes
class A:
    def f(s): pass
    def g(s): pass
show("not there", lambda: A.__abstractmethods__)
show("on type", lambda: type.__abstractmethods__)
show("on a built-in class", lambda: int.__abstractmethods__)
A.__abstractmethods__ = frozenset({"f"})
show("one", lambda: A())
show("flags", lambda: (bool(A.__flags__ & (1 << 20)), A.__abstractmethods__, "__abstractmethods__" in A.__dict__))
A.__abstractmethods__ = {"g", "f"}
show("two", lambda: A())
A.__abstractmethods__ = ["z", "a", "m"]
show("sorted", lambda: A())
class B(A): pass
show("not inherited", lambda: (type(B()).__name__, bool(B.__flags__ & (1 << 20))))
show("but found", lambda: B.__abstractmethods__)
A.__abstractmethods__ = ()
show("empty", lambda: (type(A()).__name__, bool(A.__flags__ & (1 << 20))))
A.__abstractmethods__ = {"f"}
del A.__abstractmethods__
show("deleted", lambda: (type(A()).__name__, bool(A.__flags__ & (1 << 20))))
show("deleted twice", lambda: delattr(A, "__abstractmethods__"))
A.__abstractmethods__ = [1]
show("not strings", lambda: A())
A.__abstractmethods__ = 5
show("not iterable", lambda: A())
del A.__abstractmethods__
class WithNew:
    def __new__(cls): return "own __new__"
WithNew.__abstractmethods__ = {"f"}
show("own __new__", lambda: WithNew())
class CallsObject:
    def __new__(cls): return object.__new__(cls)
CallsObject.__abstractmethods__ = {"f"}
show("own __new__ that calls object's", lambda: CallsObject())
class L(list): pass
L.__abstractmethods__ = {"f"}
show("derived from list", lambda: L())
class E(Exception): pass
E.__abstractmethods__ = {"f"}
show("derived from Exception", lambda: type(E()).__name__)
show("set on a built-in class", lambda: setattr(int, "__abstractmethods__", ()))

# ---- and the rest
show("bool of NotImplemented", lambda: bool(NotImplemented))
show("not NotImplemented", lambda: not NotImplemented)
show("if NotImplemented", lambda: 1 if NotImplemented else 2)
def f(a): pass
show("duplicate keyword", lambda: f(a=1, **{"a": 2}))
show("duplicate keyword for a method", lambda: C.__init__(s=1, **{"s": 2}))
show("duplicate keyword for a built-in", lambda: dict(a=1, **{"a": 2}))
show("duplicate keyword for a class", lambda: C(a=1, **{"a": 2}))
show("not a mapping", lambda: f(**1))
show("not a mapping for a built-in", lambda: len(**1))
class Repr:
    def __repr__(s): return "<a Repr>"
show("not a mapping for an object", lambda: Repr()(**1))
show("not iterable, after others", lambda: f(1, *1))
show("not iterable, twice", lambda: f(*[], *1))
show("not iterable, in a list", lambda: [*1])
show("not iterable for a built-in", lambda: len(*1))
show("set anything on a built-in class", lambda: [attempt(n) for n in ("__name__", "__doc__", "__module__", "__bases__", "__qualname__", "x", "__annotations__")])
def attempt(n):
    try: setattr(int, n, None)
    except TypeError as e: return str(e)
show("set anything on a built-in class", lambda: [attempt(n) for n in ("__name__", "__doc__", "__module__", "__bases__", "__qualname__", "x", "__annotations__")])
show("delete on a built-in class", lambda: delattr(int, "__doc__"))
show("keywords must be strings", lambda: f(**{1: 2}))
show("not iterable", lambda: f(*1))
class X: pass
class Y(X): pass
show("mro failure", lambda: type("Z", (X, Y), {}))
class M1: pass
class M2: pass
class M3(M1, M2): pass
class M4(M2, M1): pass
show("mro failure of two", lambda: type("Z", (M3, M4), {}))
show("module", lambda: (lambda m: (m.__name__, m.__doc__, sorted(vars(m))))(type(sys)("m", "doc")))
show("module keywords", lambda: (lambda m: (m.__name__, m.__doc__))(type(sys)(name="m", doc="d")))
show("module wrong", lambda: type(sys)())
show("module wrong 2", lambda: type(sys)(1))
show("module wrong 3", lambda: type(sys)("m", "d", "e"))
show("module doc of any type", lambda: type(sys)("m", 5).__doc__)
