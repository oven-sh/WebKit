# The module _abc, which abc.ABCMeta is written over.
import _abc
import abc
import collections.abc
import typing


def show(e):
    return type(e).__name__ + ": " + str(e)


def attempt(f, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return show(e)


def t(label, f):
    r = attempt(f)
    print(label, "=>", r if isinstance(r, str) else ascii(r))


print("---- what there is")
t("the module", lambda: (_abc.__name__, _abc.__package__, _abc.__loader__.__name__, _abc.__doc__, sorted(n for n in vars(_abc) if not n.startswith("__"))))
for name in sorted(n for n in vars(_abc) if not n.startswith("__")):
    x = getattr(_abc, name)
    t(name, lambda: (type(x).__name__, x.__text_signature__, x.__doc__, x.__module__))
D = type(abc.ABC._abc_impl)
t("_abc_data", lambda: (D.__name__, D.__module__, D.__qualname__, [b.__name__ for b in D.__mro__], sorted(vars(D)), D.__doc__, D.__basicsize__, D.__flags__ & 0x7FFF, repr(D), type(D()).__name__, type(D(1, x=2)).__name__, attempt(setattr, D, "x", 1), attempt(setattr, D(), "x", 1), attempt(lambda: type("E", (D,), {})), repr(D()).split(" at ")[0]))
t("abc is written over it", lambda: (abc.ABCMeta.__module__, abc.get_cache_token is _abc.get_cache_token, abc.ABCMeta.register.__module__, type(abc.ABCMeta.__instancecheck__).__name__))


def dump(cls):
    "How many there are in each, and not what they are, which are weak references"
    registry, cache, negative, version = _abc._get_dump(cls)
    return sorted(r().__name__ for r in registry), sorted(r().__name__ for r in cache), sorted(r().__name__ for r in negative), [type(x).__name__ for x in (registry, cache, negative, version)]


print("---- what is abstract")


class A(abc.ABC):
    @abc.abstractmethod
    def f(self): pass

    @property
    @abc.abstractmethod
    def p(self): pass

    @classmethod
    @abc.abstractmethod
    def c(cls): pass

    @staticmethod
    @abc.abstractmethod
    def s(): pass

    def concrete(self): pass


class B(A):
    def f(self): pass


class C(B):
    p = c = s = None


class E(C):
    @abc.abstractmethod
    def concrete(self): pass


t("__abstractmethods__", lambda: [(k.__name__, type(k.__abstractmethods__).__name__, sorted(k.__abstractmethods__), attempt(k) if k.__abstractmethods__ else type(k()).__name__) for k in (A, B, C, E)])
t("what says that it is", lambda: [sorted(type("X", (abc.ABC,), {"a": v}).__abstractmethods__) for v in (type("T", (), {"__isabstractmethod__": True})(), type("T", (), {"__isabstractmethod__": 1})(), type("T", (), {"__isabstractmethod__": 0})(), type("T", (), {"__isabstractmethod__": []})(), 5, None)])
t("what raises when it is asked", lambda: attempt(type, "X", (abc.ABC,), {}) and attempt(lambda: abc.ABCMeta("X", (), {"a": type("T", (), {"__isabstractmethod__": property(lambda s: 1 / 0)})()})))
t("update_abstractmethods", lambda: [(setattr(k, "f", lambda self: None), abc.update_abstractmethods(k) is k, sorted(k.__abstractmethods__)) for k in [type("K", (A,), {})]])

print("---- registered")


class R(abc.ABC):
    pass


class X:
    pass


class Y(X):
    pass


class Z:
    pass


before = abc.get_cache_token()
t("register", lambda: (R.register(X) is X, abc.get_cache_token() - before, issubclass(X, R), issubclass(Y, R), issubclass(Z, R), isinstance(X(), R), isinstance(Y(), R), isinstance(Z(), R), isinstance(5, R), dump(R)))
t("again", lambda: (R.register(X) is X, R.register(Y) is Y, abc.get_cache_token() - before, R.register(R) is R, dump(R)[0]))
t("what cannot be", lambda: [attempt(R.register, v) for v in (5, None, "X", X(), [X])] + [attempt(R.register), attempt(R.register, X, X)])


class P1(abc.ABC):
    pass


class P2(P1):
    pass


t("round in a circle", lambda: (attempt(P2.register, P1), P1.register(P2) is P2, [attempt(a.register, b) for a in [type("Q1", (abc.ABC,), {})] for b in [type("Q2", (abc.ABC,), {})] for _ in (b.register(a),)]))
t("what was found not to be may be, when something has been registered", lambda: [(issubclass(n, k), dump(k)[2], k.register(n) is n, issubclass(n, k), dump(k)[1:3]) for k in [type("K", (abc.ABC,), {})] for n in [type("N", (), {})]])
t("with anything at all", lambda: [(issubclass(n, k), dump(k)[2], type("O", (abc.ABC,), {}).register(int) is int, dump(k)[2], issubclass(n, k), dump(k)[2]) for k in [type("K", (abc.ABC,), {})] for n in [type("N", (), {})]])
t("derived from what counts as derived", lambda: [(sub.register(n) is n, issubclass(n, k), issubclass(n, sub), dump(k)[1], dump(sub)[0]) for k in [type("K", (abc.ABC,), {})] for sub in [type("Sub", (k,), {})] for n in [type("N", (), {})]])
t("_reset_registry and _reset_caches", lambda: [(k.register(n), issubclass(n, k), issubclass(int, k), dump(k)[:3], _abc._reset_caches(k), dump(k)[:3], _abc._reset_registry(k), dump(k)[:3], issubclass(n, k)) for k in [type("K", (abc.ABC,), {})] for n in [type("N", (), {})]])

print("---- __subclasshook__")


def hooked(answer):
    class H(abc.ABC):
        @classmethod
        def __subclasshook__(cls, other):
            asked.append(other.__name__)
            return answer() if callable(answer) else answer
    return H


asked = []
for answer in (True, False, NotImplemented, None, 1, 0, "yes", lambda: 1 / 0):
    H = hooked(answer)
    asked.clear()
    t("gives %r" % (answer if not callable(answer) else "raises",), lambda: (attempt(issubclass, int, H), attempt(issubclass, int, H), attempt(isinstance, 5, H), asked[:], dump(H)[1:3]))

print("---- what it is asked about")
t("what is no class", lambda: [attempt(issubclass, v, R) for v in (5, None, "X", X())] + [attempt(R.__subclasscheck__, 5), attempt(_abc._abc_subclasscheck, R, 5), attempt(_abc._abc_subclasscheck, 5, int), attempt(_abc._abc_instancecheck, 5, 5), attempt(_abc._abc_register, 5, int), attempt(_abc._abc_register, int, int), attempt(_abc._abc_register, int, str), attempt(_abc._get_dump, int), attempt(_abc._reset_caches, 5), attempt(_abc._reset_registry, None)])


class Lies:
    __class__ = property(lambda self: X)


class LiesBadly:
    __class__ = property(lambda self: 1 / 0)


class LiesOddly:
    __class__ = 5


t("what says that it is of another class", lambda: (isinstance(Lies(), R), isinstance(Lies(), P1), attempt(isinstance, LiesBadly(), R), attempt(isinstance, LiesOddly(), R), [(k.register(Lies), isinstance(Lies(), k)) for k in [type("K", (abc.ABC,), {})]]))
t("_abc_impl of the wrong kind", lambda: [(setattr(k, "_abc_impl", v), attempt(issubclass, int, k), attempt(isinstance, 5, k), attempt(k.register, int), attempt(_abc._get_dump, k)) for v in (5, None, object()) for k in [type("K", (abc.ABC,), {})]] + [[(delattr(k, "_abc_impl"), attempt(issubclass, int, k)) for k in [type("K", (), {}, ) and abc.ABCMeta("K", (), {})]]])


class OddMeta(abc.ABCMeta):
    def __subclasses__(cls):
        return answer_for_subclasses


for answer_for_subclasses in ((), None, 5, [5], [int], [str], iter([])):
    t("__subclasses__() gives %r" % (answer_for_subclasses if not hasattr(answer_for_subclasses, "__next__") else "an iterator",), lambda: attempt(issubclass, int, OddMeta("K", (), {})))

print("---- what patterns match")
SEQ, MAP = 1 << 5, 1 << 6
t("the flags", lambda: [(k.__name__, bool(k.__flags__ & SEQ), bool(k.__flags__ & MAP), "__abc_tpflags__" in vars(k)) for k in (collections.abc.Sequence, collections.abc.MutableSequence, collections.abc.Mapping, collections.abc.MutableMapping, collections.abc.Set, collections.abc.Iterable, collections.abc.Collection, collections.abc.ByteString if hasattr(collections.abc, "ByteString") else collections.abc.Sized, list, tuple, dict, str, bytes, range, memoryview, collections.deque, collections.UserList, collections.UserDict, collections.OrderedDict, collections.defaultdict, collections.ChainMap, collections.Counter)])
t("__abc_tpflags__", lambda: [(attempt(lambda: (lambda k: (bool(k.__flags__ & SEQ), bool(k.__flags__ & MAP), "__abc_tpflags__" in vars(k)))(abc.ABCMeta("K", (), {"__abc_tpflags__": v})))) for v in (SEQ, MAP, SEQ | MAP, 0, 1, 1 << 7, SEQ | 1, -1, 2 ** 70, True, "a", None, 1.5, type("I", (int,), {})(SEQ))])


def kind(x):
    match x:
        case [*_]:
            return "seq"
        case {}:
            return "map"
        case _:
            return "neither"


def flags(k):
    return "".join(c for c, f in (("S", SEQ), ("M", MAP)) if k.__flags__ & f) or "-"


def family():
    class Parent: pass
    class ChildPre(Parent): pass
    class GrandchildPre(ChildPre): pass
    return Parent, ChildPre, GrandchildPre


for base in (collections.abc.Sequence, collections.abc.Mapping, collections.abc.MutableSequence, collections.abc.Set):
    Parent, ChildPre, GrandchildPre = family()
    was = [flags(k) for k in (Parent, ChildPre, GrandchildPre)]
    base.register(Parent)
    ChildPost = type("ChildPost", (Parent,), {})
    t("registered late with %s" % base.__name__, lambda: (was, [flags(k) for k in (Parent, ChildPre, GrandchildPre, ChildPost)], [kind(k()) for k in (Parent, ChildPre, GrandchildPre, ChildPost)]))
t("with one and then the other", lambda: [(collections.abc.Sequence.register(k), flags(k), flags(sub), collections.abc.Mapping.register(k), flags(k), flags(sub), kind(k())) for k in [type("K", (), {})] for sub in [type("Sub", (k,), {})]])
t("the middle of a family", lambda: [(collections.abc.Mapping.register(c), [flags(k) for k in (p, c, g)], collections.abc.Sequence.register(p), [flags(k) for k in (p, c, g)]) for p, c, g in [family()]])
t("what cannot be changed is not", lambda: (collections.abc.Mapping.register(int) is int, flags(int), collections.abc.Sequence.register(dict) is dict, flags(dict), kind({}), flags(type("D", (dict,), {}))))


class S1(collections.abc.Sequence):
    __getitem__ = __len__ = None


class M1(collections.abc.Mapping):
    __getitem__ = __len__ = __iter__ = None


t("derived from both", lambda: [(flags(k), kind(k())) for k in (type("SM", (S1, M1), {}), type("MS", (M1, S1), {}), type("LM", (list, M1), {}), type("MD", (S1, dict), {}), type("OS", (type("O", (), {}), S1), {}))])

print("---- protocols, which go by who is asking")


@typing.runtime_checkable
class HasXY(typing.Protocol):
    x: int
    y: int


@typing.runtime_checkable
class HasMethod(typing.Protocol):
    def method(self): ...


class NotChecked(typing.Protocol):
    x: int


class Point:
    def __init__(self): self.x, self.y = 1, 2


class WithMethod:
    def method(self): pass


t("isinstance", lambda: (isinstance(Point(), HasXY), isinstance(5, HasXY), isinstance(WithMethod(), HasMethod), isinstance(5, HasMethod), attempt(isinstance, Point(), NotChecked)))
t("issubclass", lambda: (attempt(issubclass, Point, HasXY), issubclass(WithMethod, HasMethod), issubclass(int, HasMethod), attempt(issubclass, Point, NotChecked), attempt(issubclass, 5, HasMethod)))


def matched(v):
    match v:
        case HasXY(x=a, y=b):
            return a, b
        case HasMethod():
            return "method"
        case _:
            return None


t("in a match statement", lambda: [matched(v) for v in (Point(), WithMethod(), 5)])
t("what is in collections.abc", lambda: [(k.__name__, [isinstance(v, k) for v in ([], (), {}, set(), "", b"", 5, iter([]), (x for x in ()), len, range(0), memoryview(b""))]) for k in (collections.abc.Hashable, collections.abc.Iterable, collections.abc.Iterator, collections.abc.Generator, collections.abc.Reversible, collections.abc.Sized, collections.abc.Container, collections.abc.Callable, collections.abc.Collection, collections.abc.Sequence, collections.abc.MutableSequence, collections.abc.Mapping, collections.abc.Set, collections.abc.Buffer)])
