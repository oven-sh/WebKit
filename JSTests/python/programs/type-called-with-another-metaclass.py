# type(name, bases, namespace, **keywords), where a base has a metaclass that is more derived than the one that is called.
log = []


def show(e):
    return type(e).__name__ + ": " + str(e)


def attempt(f, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return show(e)


def t(label, f):
    del log[:]
    r = attempt(f)
    print(label, "=>", r if isinstance(r, str) else ascii(r), log)


class Meta(type):
    def __new__(metacls, name, bases, namespace, **keywords):
        log.append(("Meta.__new__", metacls.__name__, name, sorted(keywords)))
        return super().__new__(metacls, name, bases, namespace)

    def __init__(cls, *args, **keywords):
        log.append(("Meta.__init__", cls.__name__, len(args), sorted(keywords)))


class Plain(type):
    pass


class Deeper(Meta):
    pass


class Other(type):
    pass


class A(metaclass=Meta): pass
class P(metaclass=Plain): pass
class D(metaclass=Deeper): pass
class O(metaclass=Other): pass


class Hooked:
    def __init_subclass__(cls, **keywords):
        log.append(("__init_subclass__", cls.__name__, sorted(keywords)))


kind = lambda c: (type(c).__name__, c.__name__, [b.__name__ for b in c.__mro__])
t("type() of a base with a metaclass", lambda: kind(type("X", (A,), {})))
t("with keywords, which are the metaclass's", lambda: kind(type("X", (A,), {}, a=1, b=2)))
t("one that has no __new__ of its own", lambda: kind(type("X", (P,), {})))
t("and keywords", lambda: kind(type("X", (P,), {}, a=1)))
t("and something to take them", lambda: kind(type("X", (P, Hooked), {}, a=1)))
t("the most derived of them", lambda: kind(type("X", (A, D), {}, a=1)))
t("in the other order", lambda: kind(type("X", (D, A), {}, a=1)))
t("a metaclass called, and a base with one more derived", lambda: kind(Meta("X", (D,), {}, a=1)))
t("a metaclass called, and a base with one less derived", lambda: kind(Deeper("X", (A,), {}, a=1)))
t("type.__new__ itself", lambda: kind(type.__new__(type, "X", (A,), {}, a=1)))
t("type.__new__ of a metaclass", lambda: kind(type.__new__(Meta, "X", (D,), {}, a=1)))
t("type.__new__ of the same", lambda: kind(type.__new__(Meta, "X", (A,), {})))
t("neither is derived from the other", lambda: type("X", (A, O), {}))
t("nor from the one that is called", lambda: Other("X", (A,), {}))
t("type.__new__ likewise", lambda: type.__new__(Other, "X", (A,), {}))
t("no bases", lambda: kind(type("X", (), {})))
t("no bases, and keywords", lambda: type("X", (), {}, a=1))


class Returns(type):
    def __new__(metacls, *args, **keywords):
        log.append(("Returns.__new__", len(args), sorted(keywords)))
        return "not a class"


class R(metaclass=type): pass
R2 = type.__new__(Returns, "R2", (), {})
t("a metaclass whose __new__ returns something else", lambda: type("X", (R2,), {}, a=1))


class Raises(type):
    def __new__(metacls, *args, **keywords):
        raise KeyError("from __new__")


R3 = type.__new__(Raises, "R3", (), {})
t("one that raises", lambda: type("X", (R3,), {}))


class Static(type):
    @staticmethod
    def __new__(metacls, name, bases, namespace, **keywords):
        log.append(("Static.__new__", metacls.__name__, sorted(keywords)))
        return type.__new__(metacls, name, bases, namespace)


class S(metaclass=Static): pass
t("one that says that it is a static method", lambda: kind(type("X", (S,), {}, a=1)))


class Stands:
    def __mro_entries__(self, bases): return (A,)


class Instance:
    pass


t("what stands for other bases is looked for first", lambda: (attempt(type, "X", (Stands(),), {}), attempt(type, "X", (A, Stands()), {}), attempt(type, "X", (A, O, Stands()), {}), attempt(Other, "X", (A, Stands()), {})))
t("and then which metaclass, and only then whether they are classes", lambda: (attempt(type, "X", (Instance(),), {}), attempt(type, "X", (A, Instance()), {}), attempt(type, "X", (5,), {}), attempt(Meta, "X", (5,), {})))
import enum
t("enum", lambda: (enum.EnumCheck.UNIQUE, list(enum.FlagBoundary), type(enum.EnumCheck).__name__))
