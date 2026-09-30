# How the methods of a class come to know which class they are in: __class__, __classcell__ and super() with no arguments.
import builtins
import sys
import types


def attempt(label, f):
    try:
        print("   ", label, "=>", f())
    except BaseException as e:
        print("   ", label, "=>", type(e).__name__, e)


print("---- the class is known before anything of the program's is called")
seen = []


class Descriptor:
    def __set_name__(self, owner, name):
        seen.append(("__set_name__", name, owner.which().__name__, owner.viaSuper()))


class Base:
    @classmethod
    def viaSuper(cls):
        return "Base"

    def __init_subclass__(cls, **keywords):
        seen.append(("__init_subclass__", cls.which().__name__, cls.viaSuper()))


class Meta(type):
    def mro(cls):
        if "which" in cls.__dict__:
            seen.append(("mro", cls.__dict__["which"].__func__(cls).__name__))
        return super().mro()


class Derived(Base, metaclass=Meta):
    d = Descriptor()

    @classmethod
    def which(cls):
        return __class__

    @classmethod
    def viaSuper(cls):
        return "Derived, then " + super().viaSuper()


for entry in seen:
    print("   ", entry)


class Member:
    "As enum does: the members are made while the class is."
    def __init__(self, value):
        self.value = value

    def __set_name__(self, owner, name):
        setattr(owner, name, owner(self.value))


class Made:
    def __new__(cls, value):
        self = super().__new__(cls)
        self.value = value * 2
        return self
    one = Member(1)


print("   ", type(Made.one).__name__, Made.one.value)

print("---- __classcell__")
snapshots = []


class Snapshot(type):
    def __new__(metaclass, name, bases, namespace):
        snapshots.append({key: type(value).__name__ for key, value in namespace.items() if key.startswith("__class")})
        return super().__new__(metaclass, name, bases, namespace)


class WithoutIt(metaclass=Snapshot):
    def f(self):
        return 1


class WithClass(metaclass=Snapshot):
    def f(self):
        return __class__


class WithSuper(metaclass=Snapshot):
    def f(self):
        return super()


class WithAnnotations[T](metaclass=Snapshot):
    x: T

    def f(self):
        return __class__


for snapshot in snapshots:
    print("   ", snapshot)
print("   ", WithClass().f() is WithClass, hasattr(WithClass, "__classcell__"), "__classcell__" in WithClass.__dict__, "__classdictcell__" in WithAnnotations.__dict__, WithAnnotations.__annotations__)
print("   ", WithClass.f.__closure__[0].cell_contents is WithClass, WithClass.f.__code__.co_freevars, WithSuper.f.__code__.co_freevars)


class Order(dict):
    def __setitem__(self, key, value):
        order.append(key)
        super().__setitem__(key, value)


class Prepared(type):
    @classmethod
    def __prepare__(metaclass, name, bases):
        return Order()

    def __new__(metaclass, name, bases, namespace):
        return super().__new__(metaclass, name, bases, dict(namespace))


order = []


class InOrder[T](metaclass=Prepared):
    a = 1
    x: T

    def f(self):
        self.b = 2
        return __class__


print("   ", order)


def keptBack():
    class Forgetful(type):
        def __new__(metaclass, name, bases, namespace):
            namespace.pop("__classcell__", None)
            return super().__new__(metaclass, name, bases, namespace)

    class A(metaclass=Forgetful):
        pass
    print("    with none to keep back:", A.__name__)

    class B(metaclass=Forgetful):
        def f(self):
            return __class__


attempt("kept back from type()", keptBack)


def overwritten(value):
    class A:
        __classcell__ = value

        def f(self):
            return __class__
    return A().f().__name__


def notACell(value):
    class Meddling(type):
        def __new__(metaclass, name, bases, namespace):
            namespace["__classcell__"] = value
            return super().__new__(metaclass, name, bases, namespace)

    class A(metaclass=Meddling):
        def f(self):
            return __class__


attempt("set in the body, and set again after it", lambda: overwritten(5))
for value in (None, 5, "cell", object):
    attempt("not a cell: %r" % (value,), lambda: notACell(value))
attempt("a cell of the program's", lambda: notACell(types.CellType()))
attempt("with no body to have made it", lambda: (cell := types.CellType(), C := type("C", (), {"__classcell__": cell}), cell.cell_contents is C, hasattr(C, "__classcell__"))[2:])
attempt("__classdictcell__", lambda: (cell := types.CellType(), C := type("C", (), {"__classdictcell__": cell, "a": 1}), type(cell.cell_contents).__name__, cell.cell_contents["a"], hasattr(C, "__classdictcell__"))[2:])
attempt("__classdictcell__ that is none", lambda: type("C", (), {"__classdictcell__": 5}))


def wrongClass():
    class Twice(type):
        def __new__(metaclass, name, bases, namespace):
            first = super().__new__(metaclass, name, bases, namespace)
            return super().__new__(metaclass, "Second", bases, {})

    class A(metaclass=Twice):
        def f(self):
            return __class__


attempt("put in one class, and another handed back", wrongClass)


def notAClass():
    def make(name, bases, namespace):
        return sorted(key for key in namespace if key.startswith("__class"))

    class A(metaclass=make):
        def f(self):
            return __class__
    return A


attempt("what is made is no class", notAClass)

print("---- __build_class__")
original = builtins.__build_class__
returned = []


def building(body, name, *bases, **keywords):
    def wrapped(namespace=None):
        raise AssertionError
    namespace = {}
    returned.append((name, type(body).__name__, body.__name__, body.__qualname__, body.__code__.co_argcount))
    return original(body, name, *bases, **keywords)


builtins.__build_class__ = building
try:
    class Built:
        def f(self):
            return __class__
finally:
    builtins.__build_class__ = original
print("   ", returned, Built().f() is Built)

print("---- super() with no arguments")


def noArguments():
    super()


def deleted(x):
    del x
    super()


def noCell(x):
    super()


class Errors:
    def noArguments():
        super()

    def onlyStarred(*args):
        super()

    def onlyKeyword(*, self):
        super()

    def deleted(self):
        del self
        super()

    def deletedAndCaptured(self):
        def inner():
            return self
        del self
        super()

    def emptied(self):
        nonlocal __class__
        del __class__
        super()

    def isNoClass(self):
        nonlocal __class__
        __class__ = 5
        super()

    def another(self):
        self = 5
        super()

    def none(self):
        self = None
        return repr(super())

    def captured(self):
        def inner():
            return self
        return type(super()).__name__, inner() is self

    def inAGenerator(self):
        yield type(super()).__name__

    def inAComprehension(self):
        return [type(super()).__name__ for i in range(2)]

    def inAGeneratorExpression(self):
        return list(type(super()).__name__ for i in range(2))

    def inALambda(self):
        return (lambda: super())()

    def inALambdaWithOne(self):
        return type((lambda self: super())(self)).__name__

    def inANestedFunction(self):
        def inner(me):
            return type(super()).__name__
        return inner(self)


attempt("no arguments", noArguments)
attempt("deleted, with no class", lambda: deleted(1))
attempt("no class", lambda: noCell(1))
for name in ("noArguments", "onlyStarred"):
    attempt(name, getattr(Errors, name))
attempt("onlyKeyword", lambda: Errors.onlyKeyword(self=1))
for name in ("deleted", "deletedAndCaptured", "another", "none", "captured", "inAComprehension", "inAGeneratorExpression", "inALambda", "inALambdaWithOne", "inANestedFunction"):
    attempt(name, getattr(Errors(), name))
attempt("inAGenerator", lambda: list(Errors().inAGenerator()))
attempt("isNoClass", Errors().isNoClass)
attempt("emptied", Errors().emptied)


class Early:
    def method(self):
        return super().__init__

    try:
        method(None)
    except RuntimeError as e:
        print("    called from the body:", e)

print("---- whatever goes by the name")


class C:
    def method(self):
        return super().msg

    def two(self):
        return super(1, 2).msg


class Mine:
    msg = "not the one that is built in"

    def __init__(self, *args):
        print("    called with", args)


attempt("the one that is built in", C().method)
super = Mine
attempt("a global", C().method)
attempt("a global, with arguments", C().two)
del super
attempt("no longer", C().method)
builtins.super, kept = Mine, builtins.super
try:
    attempt("in the builtins", C().method)
finally:
    builtins.super = kept


def local():
    class super:
        msg = "a local"

    class C:
        def method(self):
            return super().msg
    return C().method()


attempt("a local", local)

print("---- what is said to be global is called what it would be there")


def defines():
    global GlobalClass, globalFunction, GlobalGeneric

    class GlobalClass:
        class Inner:
            def method(self):
                pass

        def method(self):
            def inner():
                pass
            return inner

    def globalFunction():
        class Local:
            pass
        return Local

    class GlobalGeneric[T]:
        def method(self):
            pass

    class NotGlobal:
        global AlsoGlobal

        class AlsoGlobal:
            pass

        def method(self):
            global fromAMethod

            def fromAMethod():
                pass
    NotGlobal().method()
    return NotGlobal


NotGlobal = defines()
for thing in (GlobalClass, GlobalClass.Inner, GlobalClass.Inner.method, GlobalClass.method, GlobalClass().method(), globalFunction, globalFunction(), GlobalGeneric, GlobalGeneric.method, NotGlobal, AlsoGlobal, fromAMethod):
    print("   ", thing.__qualname__)
print("   ", globalFunction.__code__.co_qualname, GlobalClass.method.__code__.co_qualname)

print("---- and what else is defined with its type parameters is not")


def generics():
    global G, g

    class G[T: int = (lambda: 1)](list[(lambda: 2)]):
        pass

    def g[U: (int, str) = str](x=lambda: 3):
        pass

    class L[V: int = str]:
        pass
    return L


L = generics()
for owner in (G, g, L):
    for p in owner.__type_params__:
        print("   ", owner.__qualname__, p, p.evaluate_bound and p.evaluate_bound.__qualname__, getattr(p, "evaluate_constraints", None) and p.evaluate_constraints.__qualname__, p.evaluate_default.__qualname__)
print("   ", G.__type_params__[0].__default__.__qualname__, G.__orig_bases__[0].__args__[0].__qualname__, g.__defaults__[0].__qualname__)

print("---- sys._git")
print("   ", type(sys._git).__name__, [type(item).__name__ for item in sys._git])
