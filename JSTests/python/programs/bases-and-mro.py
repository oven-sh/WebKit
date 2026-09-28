def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
def names(t): return [c.__name__ for c in t]
class A:
    def f(s): return "A.f"
    def only_a(s): return "only A"
class B:
    def f(s): return "B.f"
    def only_b(s): return "only B"
class C(A): pass
class D(C):
    def f(s): return "D.f then " + super().f()
c = C(); d = D()
show("before", lambda: (c.f(), d.f(), names(C.__mro__), names(D.__mro__), names(A.__subclasses__()), names(B.__subclasses__())))
C.__bases__ = (B,)
show("after", lambda: (c.f(), d.f(), names(C.__mro__), names(D.__mro__), names(A.__subclasses__()), names(B.__subclasses__()), C.__base__.__name__, names(C.__bases__)))
show("what A had is gone", lambda: c.only_a())
show("what B has is there", lambda: (c.only_b(), d.only_b(), isinstance(c, B), isinstance(c, A), issubclass(D, B), issubclass(D, A)))
C.__bases__ = (A, B)
show("two", lambda: (c.f(), names(C.__mro__), names(D.__mro__), c.only_a(), c.only_b(), names(A.__subclasses__()), names(B.__subclasses__())))
C.__bases__ = (B, A)
show("the other way", lambda: (c.f(), names(C.__mro__), C.__base__.__name__))
show("not a tuple", lambda: setattr(C, "__bases__", [A]))
show("empty", lambda: setattr(C, "__bases__", ()))
show("not classes", lambda: setattr(C, "__bases__", (A, 1)))
show("itself", lambda: setattr(C, "__bases__", (C,)))
show("a cycle", lambda: setattr(C, "__bases__", (D,)))
show("delete", lambda: delattr(C, "__bases__"))
show("twice the same", lambda: setattr(C, "__bases__", (A, A)))
show("object", lambda: setattr(C, "__bases__", (object,)))
show("another layout", lambda: setattr(C, "__bases__", (int,)))
show("another layout 2", lambda: setattr(C, "__bases__", (list,)))
show("an exception", lambda: setattr(C, "__bases__", (Exception,)))
show("built in", lambda: setattr(int, "__bases__", (object,)))
show("unchanged by all that", lambda: (names(C.__bases__), names(C.__mro__), names(D.__mro__)))
class S1:
    __slots__ = ("a",)
class S2:
    __slots__ = ("a",)
class S3:
    __slots__ = ("b",)
class S4:
    __slots__ = ("a", "b")
class US(S1): pass
show("same slots", lambda: (setattr(US, "__bases__", (S2,)), names(US.__mro__))[1])
show("other slots", lambda: setattr(US, "__bases__", (S3,)))
show("more slots", lambda: setattr(US, "__bases__", (S4,)))
show("no slots", lambda: setattr(US, "__bases__", (A,)))
class X: pass
class Y(X): pass
class Z(X, ): pass
class W(Y, Z): pass
show("a failure further down", lambda: setattr(Y, "__bases__", (Z, X)) or names(W.__mro__))
class P: pass
class Q(P): pass
class R(P, ): pass
class T(Q, R): pass
show("undone", lambda: (attempt(lambda: setattr(R, "__bases__", (Q,))), names(R.__bases__), names(R.__mro__), names(T.__mro__), names(P.__subclasses__()), names(Q.__subclasses__())))
def attempt(f):
    try: return f()
    except TypeError as e: return str(e)
show("undone", lambda: (attempt(lambda: setattr(Q, "__bases__", (R,))), names(Q.__bases__), names(Q.__mro__), names(T.__mro__), names(P.__subclasses__()), names(R.__subclasses__())))
class G1:
    def __getattr__(s, n): return "G1 got " + n
class G2: pass
class UG(G2): pass
ug = UG()
show("hooks before", lambda: attempt2(lambda: ug.missing))
def attempt2(f):
    try: return f()
    except AttributeError as e: return "AttributeError"
show("hooks before", lambda: attempt2(lambda: ug.missing))
UG.__bases__ = (G1,)
show("hooks after", lambda: ug.missing)
UG.__bases__ = (G2,)
show("hooks after that", lambda: attempt2(lambda: ug.missing))
class DD:
    x = property(lambda s: "from the property")
class UD(G2): pass
ud = UD(); ud.__dict__["x"] = "from the instance"
show("data descriptor before", lambda: ud.x)
UD.__bases__ = (DD,)
show("data descriptor after", lambda: ud.x)
class L1(list): pass
class L2(list):
    def first(s): return s[0]
class UL(L1): pass
show("derived from list", lambda: (setattr(UL, "__bases__", (L2,)), UL([7]).first(), names(UL.__mro__))[1:])
class E1(Exception): pass
class E2(ValueError): pass
class UE(E1): pass
show("exceptions", lambda: (setattr(UE, "__bases__", (E2,)), names(UE.__mro__), isinstance(UE(), ValueError))[1:])
def catches():
    try: raise UE("x")
    except ValueError as e: return "caught as ValueError"
show("caught", catches)

# ---- a metaclass with an mro() of its own
log = []
class M(type):
    def mro(cls):
        log.append(("mro", cls.__name__, cls.__dict__.get("marker")))
        return type.mro(cls)
class MA(metaclass=M): marker = 1
class MB(MA): marker = 2
show("called", lambda: (list(log), names(MB.__mro__)))
class Rev(type):
    def mro(cls):
        o = type.mro(cls)
        return [o[0]] + o[1:-1][::-1] + [o[-1]]
class RA:
    def f(s): return "RA"
class RB:
    def f(s): return "RB"
class RC(RA, RB, metaclass=Rev): pass
show("reversed", lambda: (names(RC.__mro__), RC().f(), names(type.mro(RC))))
log.clear()
class MC: pass
MB.__bases__ = (MA, MC)
show("called again", lambda: (list(log), names(MB.__mro__)))
for label, result in [("empty", []), ("not classes", [1]), ("not iterable", 5), ("unsuitable", [int]), ("without itself", [object]), ("a generator", (c for c in (object,)))]:
    show("mro() returns " + label, lambda: names(type("K", (type,), {"mro": lambda cls: result})("k", (), {}).__mro__))
show("mro() raises", lambda: type("K", (type,), {"mro": lambda cls: 1 / 0})("k", (), {}))
show("mro is not callable", lambda: type("K", (type,), {"mro": 5})("k", (), {}))
show("type.mro", lambda: (names(type.mro(int)), names(int.mro()), names(bool.mro()), type(int.mro()).__name__, int.mro() is int.mro()))
show("__mro__ read only", lambda: setattr(C, "__mro__", ()))
