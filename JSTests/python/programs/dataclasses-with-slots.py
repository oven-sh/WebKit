# sys._clear_type_descriptors(), and what it is for: a dataclass that is given __slots__, which is made a second time.
import copy
import dataclasses
import pickle
import sys
import weakref
from dataclasses import dataclass, field


def attempt(f, /, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)


def t(label, f):
    r = attempt(f)
    print(label, "=>", r if isinstance(r, str) else ascii(r))


F = sys._clear_type_descriptors
print("---- the function")
t("what it is", lambda: (type(F).__name__, F.__name__, F.__qualname__, F.__module__, F.__text_signature__, F.__doc__))


def own(c):
    return sorted(n for n in vars(c) if n in ("__dict__", "__weakref__"))


t("what it takes out", lambda: [(own(c), F(c), own(c), F(c), own(c)) for c in [type("C", (), {})]])
t("of what has only one of them", lambda: [(own(c), F(c), own(c)) for c in (type("A", (), {"__slots__": ("__dict__",)}), type("B", (), {"__slots__": ("__weakref__",)}), type("C", (), {"__slots__": ()}), type("D", (type("Base", (), {}),), {}), type("E", (int,), {}), type("M", (type,), {}), type("X", (Exception,), {}))])
t("and what then", lambda: [(F(c), attempt(getattr, x, "__dict__"), attempt(lambda: vars(x)), x.a, attempt(setattr, x, "b", 2), attempt(getattr, x, "b"), attempt(weakref.ref, x) is not None, attempt(getattr, x, "__weakref__"), type(c.__dict__).__name__, attempt(lambda: c().a)) for c in [type("C", (), {})] for x in [c()] for _ in [setattr(x, "a", 1)]])
t("what is derived from it goes on as it was", lambda: [(F(c), own(d), attempt(getattr, d(), "__dict__"), attempt(getattr, c(), "__dict__"), attempt(setattr, d(), "x", 1), own(e), e().__dict__) for c in [type("C", (), {})] for d in [type("D", (c,), {})] for e in [type("E", (type("Other", (), {}),), {})]])
t("whatever is there by that name", lambda: [(F(c), sorted(n for n in vars(c) if not n.startswith("__") or n in ("__dict__", "__weakref__"))) for c in [type("C", (), {"x": 1})] for _ in [type.__setattr__(c, "__weakref__", 5)]])
t("what will not do", lambda: [attempt(F, *a, **k) for a, k in (((), {}), ((int,), {}), ((object,), {}), ((type,), {}), ((list,), {}), ((5,), {}), ((None,), {}), (("a",), {}), ((object(),), {}), ((type("C", (), {}), 1), {}), ((), {"type": type("C", (), {})}), ((type("C", (), {})(),), {}))])
t("with a class of its own that has something to say", lambda: [(F(c), own(c), log) for log in [[]] for m in [type("M", (type,), {"__delattr__": lambda s, n: log.append(("del", n)), "__setattr__": lambda s, n, v: log.append(("set", n)), "__getattribute__": lambda s, n: (log.append(("get", n)) if n in ("__weakref__",) else None, type.__getattribute__(s, n))[1]})] for c in [m("C", (), {})]])

print("---- what it is for")


@dataclass(slots=True)
class P:
    x: int
    y: int = 2


t("a dataclass with slots", lambda: (P.__slots__, own(P), repr(P(1)), P(1) == P(1, 2), attempt(getattr, P(1), "__dict__"), attempt(setattr, P(1), "z", 3), attempt(weakref.ref, P(1)), [f.name for f in dataclasses.fields(P)], dataclasses.asdict(P(1)), dataclasses.astuple(P(3, 4)), dataclasses.replace(P(1), y=5), P.__qualname__, P.__module__, P.__match_args__, P.__mro__ == (P, object)))
t("copied and pickled", lambda: (copy.copy(P(1)), copy.deepcopy(P(1, [2])), [attempt(lambda: pickle.loads(pickle.dumps(P(1, 2), p))) for p in range(pickle.HIGHEST_PROTOCOL + 1)], P(1).__getstate__() if hasattr(P(1), "__getstate__") else None))


@dataclass(slots=True, frozen=True)
class Frozen:
    a: int
    b: tuple = ()


t("frozen", lambda: (Frozen(1), hash(Frozen(1)) == hash(Frozen(1)), attempt(setattr, Frozen(1), "a", 2), attempt(delattr, Frozen(1), "a"), {Frozen(1): 1}[Frozen(1)], pickle.loads(pickle.dumps(Frozen(1, (2,)))), copy.copy(Frozen(1)), Frozen.__slots__))


@dataclass(slots=True, weakref_slot=True)
class Weak:
    a: int


t("that can be referred to weakly", lambda: [(Weak.__slots__, own(Weak), weakref.ref(w)() is w) for w in [Weak(1)]] + [attempt(lambda: dataclass(weakref_slot=True)(type("W", (), {"__annotations__": {"a": int}})))])


@dataclass(slots=True)
class Base:
    a: int


@dataclass(slots=True)
class Derived(Base):
    b: int = 0


@dataclass
class Plain(Base):
    c: int = 0


t("derived from one another", lambda: (Base.__slots__, Derived.__slots__, Derived(1, 2), attempt(setattr, Derived(1), "z", 1), Plain(1, 2), Plain(1).__dict__, own(Plain), [c.__name__ for c in Derived.__mro__]))


@dataclass(slots=True)
class WithSuper:
    a: int
    def show(self): return super().__repr__()[:10], __class__.__name__
    @classmethod
    def make(cls): return super().__new__(cls), __class__ is cls
    @property
    def prop(self): return __class__ is type(self)
    @staticmethod
    def static(): return __class__.__name__


t("whose methods know which class they are in", lambda: (WithSuper(1).show(), type(WithSuper.make()[0]).__name__, WithSuper.make()[1], WithSuper(1).prop, WithSuper.static(), WithSuper.show.__closure__[0].cell_contents is WithSuper))


@dataclass(slots=True)
class Defaults:
    a: list = field(default_factory=list)
    b: int = field(default=5, repr=False)
    c: str = field(default="c", compare=False)
    d: int = field(default=0, init=False)
    e: int = field(default=1, kw_only=True)


t("with fields of every kind", lambda: (Defaults(), Defaults([1], 2, "x", e=3), Defaults.__slots__, Defaults() == Defaults(c="other"), Defaults().a is not Defaults().a, attempt(Defaults, d=1), attempt(getattr, Defaults, "b"), type(vars(Defaults)["b"]).__name__))
t("what will not do", lambda: [attempt(lambda: dataclass(slots=True)(type("S", (), {"__slots__": ("a",), "__annotations__": {"a": int}}))), attempt(lambda: dataclass(slots=True)(type("S", (), {"__annotations__": {"a": int}, "a": 1})).__slots__), attempt(lambda: dataclasses.make_dataclass("M", ["a", ("b", int, 5)], slots=True)(1)), attempt(lambda: dataclasses.make_dataclass("M", ["a"], slots=True).__slots__)])


@dataclass(slots=True)
class Generic[T]:
    v: T


t("that is generic", lambda: (Generic(1), Generic[int](2), Generic.__slots__, Generic.__type_params__, Generic.__parameters__, repr(Generic[int]).rsplit(".", 1)[-1], Generic.__orig_bases__))
t("the first of the two is let go of", lambda: [(len(before), sorted(c.__name__ for c in object.__subclasses__() if c.__name__ == "Twice") in (["Twice"], ["Twice", "Twice"])) for before in [[c for c in object.__subclasses__() if c.__name__ == "Twice"]] for _ in [dataclass(slots=True)(type("Twice", (), {"__annotations__": {"a": int}}))]])
import _colorize
t("what stopped a great deal from being imported", lambda: (type(_colorize.ANSIColors).__name__, _colorize.Argparse.__slots__ if hasattr(_colorize, "Argparse") else None, type(_colorize.get_theme(force_no_color=True)).__name__))
