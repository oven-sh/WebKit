# Several small things about getting and setting attributes, each of which CPython's own tests found.
import copyreg
import types


def attempt(label, f):
    try:
        print("   ", label, "=>", f())
    except BaseException as e:
        print("   ", label, "=>", type(e).__name__, e)


print("---- __set__() and no __get__()")


class OnlySet:
    def __init__(self, name): self.name = name
    def __set__(self, obj, value): obj.__dict__[self.name] = value
    def __repr__(self): return "OnlySet"


class OnlyDelete:
    def __delete__(self, obj): print("    __delete__")
    def __repr__(self): return "OnlyDelete"


class Both:
    def __get__(self, obj, cls): return "Both.__get__"
    def __set__(self, obj, value): obj.__dict__["b"] = value


class X:
    a = OnlySet("a")
    b = Both()
    d = OnlyDelete()


x = X()
print("   ", x.a, x.b, x.d)
x.a = 42
x.b = 43
x.__dict__["d"] = 44
print("   ", x.a, x.b, x.d, x.__dict__)
for turn in range(3000):
    assert x.a == 42 and x.b == "Both.__get__" and x.d == 44
del x.d
attempt("set what can only be deleted", lambda: setattr(x, "d", 1))
attempt("delete what can only be set", lambda: delattr(x, "a"))


class Meta(type):
    pass


class Y(metaclass=Meta):
    pass


Y.a = 42
Meta.a = OnlySet("a")
print("   ", Y.a)

print("---- what a method is called")


class Callable:
    def __call__(self, arg): pass


func = Callable()


class Instance:
    def __repr__(self): return "instance"


method = types.MethodType(func, Instance())
print("   ", method)
func.__name__ = "name"
print("   ", method)
func.__qualname__ = "qualname"
print("   ", method)
func.__qualname__ = 5
print("   ", method)
del func.__qualname__
func.__name__ = None
print("   ", method)
print("   ", types.MethodType(len, Instance()), types.MethodType(Instance, Instance()), types.MethodType(Instance().__repr__, Instance()))

print("---- __dict__, found by way of an ordinary class")


class Base:
    pass


class Meta1(type, Base): pass
class Meta2(Base, type): pass
class Module1(types.ModuleType, Base): pass
class Module2(Base, types.ModuleType): pass
class D(metaclass=Meta1): pass
class E(metaclass=Meta2): pass


descriptor = Base.__dict__["__dict__"]
for thing in (D, E, Module1("m"), Module2("m"), Base()):
    label = type(thing).__name__
    attempt(label + " has", lambda: type(thing.__dict__).__name__)
    attempt(label + " by the descriptor", lambda: type(descriptor.__get__(thing)).__name__)
    attempt(label + " set", lambda: setattr(thing, "__dict__", {}))
    attempt(label + " set by the descriptor", lambda: descriptor.__set__(thing, {}))
    attempt(label + " deleted", lambda: delattr(thing, "__dict__"))
attempt("of what has none", lambda: descriptor.__get__(5))

print("---- with looks for each once")
record = []


class Recorded:
    def __init__(self, name, f): self.name, self.f = name, f
    def __get__(self, obj, cls):
        record.append(self.name)
        return self.f.__get__(obj, cls)


class Manager:
    __enter__ = Recorded("__enter__", lambda self: record.append("entered"))
    __exit__ = Recorded("__exit__", lambda self, *a: record.append("exited"))


with Manager():
    record.append("body")
print("   ", record)


class NoEnter:
    __exit__ = Recorded("__exit__", lambda self, *a: None)


class NoExit:
    __enter__ = Recorded("__enter__", lambda self: None)


def use(manager):
    with manager:
        pass


for cls in (NoEnter, NoExit, object):
    record.clear()
    attempt(cls.__name__, lambda: use(cls()))
    print("   ", record)

print("---- the name of an attribute may be of a class derived from str")


class S(str):
    pass


class A:
    __slotnames__ = [S("spam")]

    def __getattr__(self, attr):
        if attr == "spam":
            A.__slotnames__[:] = [S("spam")]
            return 42
        raise AttributeError


attempt("__reduce_ex__", lambda: A().__reduce_ex__(2) == (copyreg.__newobj__, (A,), (None, {"spam": 42}), None, None))
p = Base()
attempt("setattr, getattr, hasattr, delattr", lambda: (setattr(p, S("x"), 1), getattr(p, S("x")), hasattr(p, S("x")), p.x, list(p.__dict__), delattr(p, S("x")), hasattr(p, "x")))
attempt("object's own", lambda: (object.__setattr__(p, S("y"), 2), object.__getattribute__(p, S("y")), object.__delattr__(p, S("y"))))
attempt("what is no str", lambda: getattr(p, b"x"))
