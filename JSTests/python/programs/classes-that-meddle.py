# Classes that do what they should not with how classes are made and looked in.


def attempt(label, f):
    try:
        print("   ", label, "=>", f())
    except Exception as e:
        print("   ", label, "=>", type(e).__name__, e)


print("---- a metaclass puts the classes in an order of its own")


class Backwards(type):
    def mro(cls):
        order = type.mro(cls)
        order.reverse()
        return order


class A:
    def f(self): return "A"
    def __init__(self, *a): self.given = a


class B(A): pass


class C(A):
    def f(self): return "C"


class D(B, C): pass


class X(D, B, C, A, metaclass=Backwards): pass


print([c.__name__ for c in X.__mro__], X().f(), type(X()).__name__, isinstance(X(), A), issubclass(X, A), [c.__name__ for c in D.__mro__], D().f())


class HasNew:
    def __new__(cls, *a):
        self = object.__new__(cls)
        self.made_by = "HasNew"
        return self


class Y(HasNew, metaclass=Backwards): pass


attempt("object comes first, so its __new__() is the one", lambda: vars(Y()))
attempt("and it takes no arguments", lambda: Y(1))
attempt("without object", lambda: type("M", (type,), {"mro": lambda cls: [cls]})("Z", (), {})().__class__.__name__)
attempt("nothing at all", lambda: type("M", (type,), {"mro": lambda cls: []})("Z", (), {}))
attempt("what is not a class", lambda: type("M", (type,), {"mro": lambda cls: [cls, 5]})("Z", (), {}))
attempt("what is not a list", lambda: type("M", (type,), {"mro": lambda cls: 5})("Z", (), {}))
attempt("a class that has nothing to do with it", lambda: type("M", (type,), {"mro": lambda cls: [cls, int, object]})("Z", (), {}))
attempt("it raises", lambda: type("M", (type,), {"mro": lambda cls: 1 / 0})("Z", (), {}))

print("---- __getattr__() is taken away by __getattribute__()")


class Evil:
    def __getattr__(self, name):
        return "from __getattr__(): " + name

    def __getattribute__(self, name):
        if "__getattr__" in vars(Evil):
            del Evil.__getattr__
        raise AttributeError(name)


attempt("it had been got hold of", lambda: Evil().attr)
attempt("and now there is none", lambda: Evil().attr)
attempt("hasattr()", lambda: hasattr(Evil(), "attr"))


class Evil2:
    def __getattr__(self, name):
        return "from __getattr__(): " + name

    @property
    def p(self):
        del Evil2.__getattr__
        raise AttributeError("p")


attempt("by a property", lambda: Evil2().p)
attempt("and now there is none", lambda: Evil2().q)
Evil2.__getattr__ = lambda self, name: "put back: " + name
attempt("getattr() with something to fall back on", lambda: (getattr(Evil2(), "p", "fallen back on"), getattr(Evil2(), "q", "fallen back on")))

print("---- __bases__ is set while __bases__ is being set")
ready = False


class M(type):
    def mro(cls):
        if ready:
            if cls.__name__ == "B1":
                B2.__bases__ = (B1,)
            if cls.__name__ == "B2":
                B1.__bases__ = (B2,)
        return type.mro(cls)


class Base(metaclass=M): pass
class B1(Base): pass
class B2(Base): pass


ready = True


def set_them():
    B1.__bases__ += ()


attempt("round in a ring", set_them)
ready = False
print([c.__name__ for c in B1.__mro__], [c.__name__ for c in B2.__mro__], B1.__bases__, B2.__bases__)
attempt("itself", lambda: setattr(B1, "__bases__", (B1,)))
attempt("what is derived from it", lambda: setattr(Base, "__bases__", (B1,)))
