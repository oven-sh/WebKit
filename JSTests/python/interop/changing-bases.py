class A:
    def f(self): return "A.f"
    def only_a(self): return "only A"
    tag = "A"
class B:
    def f(self): return "B.f"
    def only_b(self): return "only B"
    tag = "B"
class C(A): pass
class D(C): pass
def rebase(cls, *bases): cls.__bases__ = bases
def order(cls): return [c.__name__ for c in cls.__mro__]
class Abstract:
    def f(self): pass
Abstract.__abstractmethods__ = frozenset({"f"})
class P:
    def __init__(self): self._x = 1
    @property
    def x(self): return self._x
    @x.setter
    def x(self, v): self._x = v
    ro = property(lambda self: "read only")
class Slots:
    __slots__ = ("a",)
