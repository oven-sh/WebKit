# What audits/other-objects.py found to be otherwise than in CPython, a case or two of each.
import _warnings

_warnings.filters.insert(0, ("ignore", None, Warning, None, 0))


def t(label, f):
    try:
        r = f()
    except BaseException as e:
        r = type(e).__name__ + ": " + str(e)
    print(label, "=>", r)


def nowhere(text):
    parts = text.split(" at 0x")
    return parts[0] + "".join(" at" + part.lstrip("0123456789abcdef") for part in parts[1:])


class Plain:
    __slots__ = ("slot", "empty", "__dict__")

    def __init__(self):
        self.slot = 5

    def other(self):
        self.slot = 6
        self.another = 7

    def method(self):
        return 1

    @classmethod
    def of_class(cls):
        return cls


class Derived(Plain):
    __slots__ = ()


class Meta(type):
    def __repr__(cls):
        return "<Meta>"


class OfMeta(metaclass=Meta):
    pass


print("---- what object has is for anything, and takes it for no more than an object")
t("object.__new__(int, 1)", lambda: object.__new__(int, 1))
t("object.__new__(ValueError, 1, 2)", lambda: object.__new__(ValueError, 1, 2))
t("object.__new__(Plain, 1)", lambda: type(object.__new__(Plain, 1)).__name__)
t("object.__repr__(int)", lambda: nowhere(object.__repr__(int)))
t("object.__repr__(5)", lambda: nowhere(object.__repr__(5)))
t("object.__repr__([])", lambda: nowhere(object.__repr__([])))
t("OfMeta.__repr__(int)", lambda: nowhere(OfMeta.__repr__(int)))
t("repr(OfMeta)", lambda: repr(OfMeta))
t("int.__repr__(True)", lambda: int.__repr__(True))
t("object.__getattribute__(int, 'real')", lambda: type(object.__getattribute__(int, "real")).__name__)
t("object.__getattribute__(bool, 'real')", lambda: object.__getattribute__(bool, "real"))
t("object.__getattribute__(Plain, 'method')", lambda: type(object.__getattribute__(Plain, "method")).__name__)
t("object.__getattribute__(Derived, 'method')", lambda: object.__getattribute__(Derived, "method"))
t("object.__getattribute__(Plain, '__dict__')", lambda: type(object.__getattribute__(Plain, "__dict__")).__name__)
t("object.__getattribute__(int, '__name__')", lambda: object.__getattribute__(int, "__name__"))
t("object.__getattribute__(int, 'nope')", lambda: object.__getattribute__(int, "nope"))
t("type.__getattribute__(int, 'nope')", lambda: type.__getattribute__(int, "nope"))
t("type.__getattribute__(bool, 'real')", lambda: type(type.__getattribute__(bool, "real")).__name__)
t("object.__setattr__(Plain, 'a', 1)", lambda: object.__setattr__(Plain, "a", 1))
t("object.__setattr__(OfMeta, 'a', 1)", lambda: object.__setattr__(OfMeta, "a", 1))
t("object.__delattr__(int, None)", lambda: object.__delattr__(int, None))
t("type.__setattr__(Plain, 'a', 1)", lambda: (type.__setattr__(Plain, "a", 1), Plain.a))
t("type.__setattr__(int, None, 1)", lambda: type.__setattr__(int, None, 1))
t("type.__delattr__(int, 'a')", lambda: type.__delattr__(int, "a"))
t("type.__setattr__(Plain, None, 1)", lambda: type.__setattr__(Plain, None, 1))
t("type.__init__(int)", lambda: type.__init__(int))
t("type.__init__(int, 1)", lambda: type.__init__(int, 1))
t("type.__init__(int, 1, 2)", lambda: type.__init__(int, 1, 2))
t("type.__init__(int, 1, 2, 3)", lambda: type.__init__(int, 1, 2, 3))
t("type.__init__(int, 1, a=1)", lambda: type.__init__(int, 1, a=1))

print("---- what a class is called, and where it is from")
for name in ("__name__", "__qualname__", "__module__"):
    t("del C." + name, lambda: delattr(type("C", (), {}), name))
    t("C.%s = 5" % name, lambda: (lambda C: (setattr(C, name, 5), repr(C), getattr(C, name)))(type("C", (), {})))
    t("C.%s = 'x'" % name, lambda: (lambda C: (setattr(C, name, "x"), repr(C), getattr(C, name)))(type("C", (), {})))
    t("int.%s = 'x'" % name, lambda: setattr(int, name, "x"))
    t("del int." + name, lambda: delattr(int, name))
t("C.__name__ = 'a\\0b'", lambda: setattr(type("C", (), {}), "__name__", "a\0b"))
t("C.__module__ = 'builtins'", lambda: (lambda C: (setattr(C, "__module__", "builtins"), repr(C)))(type("C", (), {"__qualname__": "Q.C"})))
t("__static_attributes__", lambda: Plain.__static_attributes__)
t("a slot with nothing in it", lambda: Plain().empty)


def inner():
    class Local:
        __slots__ = ("empty",)
    return Local()


t("of a class that is inside something", lambda: inner().empty)

print("---- methods")
Method = type(Plain().method)
t("MethodType(len, [1, 2])", lambda: (lambda m: (type(m).__name__, repr(m), m(), m.__func__ is len, m.__self__))(Method(len, [1, 2])))
t("classmethod(len).__get__(None, int)", lambda: (lambda m: (type(m).__name__, repr(m)))(classmethod(len).__get__(None, int)))
t("classmethod(f).__get__(None, 5)", lambda: repr(Plain.__dict__["of_class"].__get__(None, 5)).replace("Plain.", ""))
t("classmethod(f).__get__(5)", lambda: repr(Plain.__dict__["of_class"].__get__(5)).replace("Plain.", ""))
t("str.upper.__get__(5)", lambda: str.upper.__get__(5))
t("str.upper.__get__('a')()", lambda: str.upper.__get__("a")())
t("str.upper.__get__(None, str)", lambda: str.upper.__get__(None, str))
t("str.upper.__get__(None, None)", lambda: str.upper.__get__(None, None))
t("int.__add__.__get__('a')", lambda: int.__add__.__get__("a"))
t("int.__add__.__get__(True)(1)", lambda: int.__add__.__get__(True)(1))
t("dict.__dict__['fromkeys']()", lambda: dict.__dict__["fromkeys"]())
t("dict.__dict__['fromkeys'](5)", lambda: dict.__dict__["fromkeys"](5))
t("[].count.__module__ = 5", lambda: (lambda m: (setattr(m, "__module__", 5), m.__module__))([].count))
t("[].count.__module__", lambda: [].count.__module__)
t("property().__set_name__(a=1)", lambda: property().__set_name__(a=1))
t("got first, and then called", lambda: (lambda m: m(a=1))(property().__set_name__))
t("property.__set_name__(property(), a=1)", lambda: property.__set_name__(property(), a=1))
t("property().__set_name__(1)", lambda: property().__set_name__(1))


def function():
    pass


for label, make in (("a function", lambda: function), ("a staticmethod", lambda: staticmethod(function)), ("a classmethod", lambda: classmethod(function)), ("an exception", lambda: ValueError())):
    t("del __dict__ of " + label, lambda: delattr(make(), "__dict__"))
t("of an instance", lambda: delattr(Plain(), "__dict__"))

print("---- super")
s = super(Derived, Derived())
t("s.__init__(Derived)", lambda: (s.__init__(Derived), repr(s)))
s = super(Derived, Derived())
t("s.__init__(Plain, 5)", lambda: s.__init__(Plain, 5))
t("and it is as it was", lambda: repr(s))
t("super(Derived, Derived).__eq__(1, 1)", lambda: super(Derived, Derived).__eq__(1, 1))
t("super(Derived, Derived).__setattr__(int, 'a', 1)", lambda: super(Derived, Derived).__setattr__(int, "a", 1))

print("---- exceptions")
t("with_traceback(0)", lambda: ValueError().with_traceback(0))
t("with_traceback(None)", lambda: ValueError("x").with_traceback(None))
t("NameError(a=1, b=2)", lambda: NameError(a=1, b=2))
t("NameError(a=1)", lambda: NameError(a=1))
t("AttributeError(a=1, b=2, c=3)", lambda: AttributeError(a=1, b=2, c=3))
t("ImportError(a=1, b=2, c=3, d=4)", lambda: ImportError(a=1, b=2, c=3, d=4))

print("---- throw")


def generator():
    try:
        x = yield 1
        yield (2, x)
    except KeyError as e:
        yield ("caught", e.args)


def delegating():
    try:
        return (yield from generator())
    except TypeError as e:
        yield ("outer caught", str(e))


def begun(make):
    g = make()
    next(g)
    return g


class Fussy(Exception):
    def __init__(self, a, b):
        super().__init__(a, b)


class NotOne:
    def __new__(cls, *a):
        return 5


class Odd(Exception):
    def __new__(cls, *a):
        return 5


def traceback():
    try:
        raise ValueError
    except ValueError as e:
        return e.__traceback__


for label, make in (("new", generator), ("begun", lambda: begun(generator)), ("delegating", lambda: begun(delegating)), ("done", lambda: (lambda g: (list(g), g)[1])(generator()))):
    for said, arguments in (("KeyError", (KeyError,)), ("KeyError, 1", (KeyError, 1)), ("KeyError, (1, 2)", (KeyError, (1, 2))), ("KeyError, ()", (KeyError, ())), ("KeyError, None", (KeyError, None)), ("KeyError(1)", (KeyError(1),)),
                            ("KeyError(1), None", (KeyError(1), None)), ("KeyError(1), 2", (KeyError(1), 2)), ("KeyError, KeyError(3)", (KeyError, KeyError(3))), ("LookupError, KeyError(3)", (LookupError, KeyError(3))),
                            ("KeyError, ValueError(3)", (KeyError, ValueError(3))), ("None", (None,)), ("5", (5,)), ("int", (int,)), ("KeyError, 1, 2", (KeyError, 1, 2)), ("None, None, 2", (None, None, 2)), ("KeyError(1), None, 2", (KeyError(1), None, 2)),
                            ("KeyError, None, None", (KeyError, None, None)), ("KeyError, None, a traceback", (KeyError, None, traceback())), ("StopIteration", (StopIteration,)), ("StopIteration(5)", (StopIteration(5),)), ("GeneratorExit", (GeneratorExit,)),
                            ("Fussy", (Fussy,)), ("Fussy, 1", (Fussy, 1)), ("Fussy, (1, 2)", (Fussy, (1, 2))), ("Odd", (Odd,))):
        def thrown():
            g = make()
            try:
                r = g.throw(*arguments)
            except BaseException as e:
                r = type(e).__name__ + ": " + str(e)
            return r, list(g), g.gi_yieldfrom
        t("%s: throw(%s)" % (label, said), thrown)

t("the traceback that it was given", lambda: (lambda tb: (lambda g: catch(lambda: g.throw(ValueError, None, tb)).__traceback__.tb_next.tb_next is tb)(generator()))(traceback()))


def catch(f):
    try:
        f()
    except BaseException as e:
        return e


def outer():
    try:
        yield from generator()
    except ValueError:
        yield "after"


g = begun(outer)
print(type(g.gi_yieldfrom).__name__, g.throw(ValueError), g.gi_yieldfrom)

print("---- what is waited for")


class Suspend:
    def __await__(self):
        return (yield 1)


async def coroutine():
    await Suspend()


async def async_generator():
    yield 1
    await Suspend()
    yield 2


def drive(awaitable, method="send", *arguments):
    try:
        return getattr(awaitable, method)(*arguments)
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)


t("athrow(None) is made", lambda: type(async_generator().athrow(None)).__name__)
t("and says so when it is gone on with", lambda: drive(async_generator().athrow(None), "send", None))
t("athrow() is made", lambda: type(async_generator().athrow()).__name__)
t("and says so", lambda: drive(async_generator().athrow(), "send", None))
t("athrow(1, 2, 3, 4)", lambda: drive(async_generator().athrow(1, 2, 3, 4), "send", None))
t("athrow(a=1)", lambda: async_generator().athrow(a=1))
t("got first, and then called", lambda: (lambda m: m(a=1))(async_generator().athrow))
t("asend(None).throw()", lambda: drive(async_generator().asend(None), "throw"))
t("asend(None).throw(None)", lambda: drive(async_generator().asend(None), "throw", None))
t("one that is done with, .throw()", lambda: (lambda a: (drive(a, "send", None), drive(a, "throw"), drive(a, "throw", None)))(async_generator().asend(None)))
t("athrow(ValueError), done with, .throw()", lambda: (lambda a: (drive(a, "send", None), drive(a, "throw"), drive(a, "throw", None)))(async_generator().athrow(ValueError)))
t("anext(g, 0).send(())", lambda: drive(anext(async_generator(), 0), "send", ()))
t("anext(g, 0).send((None,))", lambda: drive(anext(async_generator(), 0), "send", (None,)))
t("anext(g, 0).throw(a=1)", lambda: anext(async_generator(), 0).throw(a=1))
t("got first, and then called", lambda: (lambda m: m(a=1))(anext(async_generator(), 0).throw))
t("coroutine().throw(KeyError, (1, 2))", lambda: drive(coroutine(), "throw", KeyError, (1, 2)))
t("coroutine().throw(None, None, 5)", lambda: drive(coroutine(), "throw", None, None, 5))

print("---- __del__, which a program can call")
for label, make in (("generator", generator), ("coroutine", coroutine), ("async_generator", async_generator), ("asend", lambda: async_generator().asend(None)), ("athrow", lambda: async_generator().athrow(ValueError)), ("aclose", lambda: async_generator().aclose())):
    t(label, lambda: (type(type(make()).__del__).__name__, make().__del__()))
    t(label + " with an argument", lambda: make().__del__(1))
g = begun(generator)
print(g.__del__(), list(g), g.gi_frame)


def stubborn():
    try:
        yield 1
    finally:
        print("closing")


g = begun(stubborn)
g.__del__()
g.__del__()


def refusing():
    try:
        yield 1
    except GeneratorExit:
        yield 2


_warnings.filters[:] = [("error", None, Warning, None, 0)]
shown = []
import sys
sys.unraisablehook = lambda u: shown.append((type(u.exc_value).__name__, str(u.exc_value), u.err_msg and nowhere(u.err_msg)))
# CPython does it again when nothing has them any more, so something goes on having them.
kept = [coroutine(), async_generator().asend(None), async_generator().aclose(), begun(refusing)]
for each in kept:
    each.__del__()
print(shown)
_warnings.filters[:] = [("ignore", None, Warning, None, 0)]

print("---- the rest")
t("{1: [2]}.items() & {1}", lambda: {1: [2]}.items() & {1})
t("{1} & {1: [2]}.items()", lambda: {1} & {1: [2]}.items())
t("{1: 2}.items() & {(1, 2), 3}", lambda: {1: 2}.items() & {(1, 2), 3})
t("f_lineno = None", lambda: setattr(sys._getframe(), "f_lineno", None))
t("f_lineno = True", lambda: setattr(sys._getframe(), "f_lineno", True))
t("f_lineno = 5", lambda: setattr(sys._getframe(), "f_lineno", 5))
