# An instance can be called if its class has __call__(), whatever else the class is derived from.
import _functools
import _io
import _operator
import _weakref
import builtins
import collections
import itertools
import sys


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


def called(self, /, *a, **k):
    return "called", a, k


# Every class that is built in and can be derived from, and something to make one of each with.
def classes():
    seen = []
    for module in (builtins, collections, _functools, itertools, _weakref, _operator):
        for name, value in sorted(vars(module).items()):
            if isinstance(value, type) and value not in seen and not name.startswith("_"):
                seen.append(value)
    # Not what opens a file.
    for value in (_io.BytesIO, _io.StringIO, type(sys), type(lambda: 0), type(called.__code__), type(None), type(...), type(iter([])), type({}.keys()), type(sys.flags), type(t.__get__(1))):
        seen.append(value)
    return seen


def one_of(C):
    for args in ((), ("m",), ([],), (len,), (len, []), ([], []), (object,), (0,), (b"",), ("a", "b"), (called,), (1, [])):
        try:
            return C(*args)
        except BaseException:
            pass
    return None


print("---- derived from each class that there is")
derivable = []
for base in classes():
    try:
        C = (type if not issubclass(base, type) else base)("C", (base,), {"__call__": called}) if not issubclass(base, type) else None
        if C is None:
            M = type("M", (base,), {"__call__": called})
            x = M("X", (), {})
        else:
            x = one_of(C)
    except TypeError as e:
        continue
    if x is None:
        print(base.__name__, "none could be made")
        continue
    derivable.append(base)
    print(base.__module__ + "." + base.__name__, [attempt(f) for f in (
        lambda: x(), lambda: x(1), lambda: x(k=1), lambda: x(1, 2, k=3, l=4), lambda: x(*[1], **{"k": 2}), lambda: x(*range(5)), lambda: callable(x), lambda: list(map(x, [1])), lambda: _functools.partial(x, 1)(2, k=3),
        lambda: _operator.call(x, 3, k=4), lambda: type(x).__call__(x, 4), lambda: x.__call__(5, k=6), lambda: sorted([2, 1], key=lambda v: x(v)[1])[0], lambda: next(iter(x, None))[0])])
print(len(derivable) > 60)

print("---- the class itself has none, and neither has one that is derived from it and says nothing")
for base in derivable:
    if issubclass(base, type):
        continue
    plain, D = one_of(base), type("D", (base,), {})
    derived = one_of(D)
    if callable(plain) or callable(derived):
        # A few can be called as they are.
        print(base.__name__, "can be called as it is", callable(plain), callable(derived))
        continue
    r = (attempt(plain), attempt(derived), attempt(lambda: derived(k=1)))
    # Some go by a longer name, if they are written in C.
    if r[0].replace(base.__module__ + ".", "") != "TypeError: '%s' object is not callable" % base.__name__ or r[1:] != ("TypeError: 'D' object is not callable", "TypeError: 'D' object is not callable"):
        print(base.__name__, r)

BASES = (object, int, float, complex, str, bytes, bytearray, tuple, list, dict, set, frozenset, Exception, OSError, enumerate, collections.deque, collections.OrderedDict, property)

print("---- given to the class afterwards, and taken away")
for base in BASES:
    C = type("C", (base,), {})
    x = one_of(C)
    r = [callable(x), attempt(x)]
    C.__call__ = called
    r += [callable(x), attempt(x, 1, k=2)]
    C.__call__ = lambda self: "another"
    r += [attempt(x), attempt(x, 1)]
    del C.__call__
    r += [callable(x), attempt(x)]
    print(base.__name__, r)

print("---- from some other class")


class Mixin:
    def __call__(self, *a, **k):
        return "the mixin's", type(self).__name__, a, k


for base in BASES[1:]:
    first, last = type("First", (Mixin, base), {}), type("Last", (base, Mixin), {})
    further = type("Further", (last,), {})
    print(base.__name__, [(callable(x), attempt(x, 1, k=2)) for x in (one_of(first), one_of(last), one_of(further))])

print("---- what is in the instance does not count")
for base in BASES:
    C = type("C", (base,), {})
    x = one_of(C)
    r = attempt(setattr, x, "__call__", lambda: "the instance's")
    print(base.__name__, r, callable(x), attempt(x), attempt(lambda: x.__call__()))

print("---- what __call__ can be")


class Getter:
    def __get__(self, instance, owner):
        return lambda *a: ("got by way of", type(instance).__name__, owner.__name__, a)


class Raiser:
    def __get__(self, instance, owner):
        raise AttributeError("from __get__")


for base in (object, tuple, list, dict, int, str, Exception):
    r = []
    for value in (staticmethod(lambda *a: ("static", a)), classmethod(lambda cls, *a: ("class", cls.__name__, a)), len, print.__class__, Mixin(), Getter(), Raiser(), None, 5, "text", _functools.partial(called, "first"), type("Inner", (tuple,), {"__call__": called})()):
        x = one_of(type("C", (base,), {"__call__": value, "__repr__": lambda self: "one of C"}))
        r.append((callable(x), attempt(x, ()) if value is not print.__class__ else "-"))
    print(base.__name__, r)

print("---- what it raises, and what it is given")


def raises(self, *a):
    raise KeyError("from __call__", a)


def itself(self):
    return self


def again(self, n):
    return n if n <= 0 else self(n - 1) + 1


def for_ever(self):
    return self()


for base in (object, tuple, list, dict, int, str, Exception):
    x = one_of(type("C", (base,), {"__call__": raises}))
    y = one_of(type("C", (base,), {"__call__": itself}))
    z = one_of(type("C", (base,), {"__call__": again}))
    w = one_of(type("C", (base,), {"__call__": for_ever}))
    v = one_of(type("C", (base,), {"__call__": lambda self, a, b=2, *, c: (a, b, c)}))
    print(base.__name__, attempt(x, 1), y() is y, z(50), attempt(w)[:40], [attempt(v, *a, **k) for a, k in (((1,), {"c": 3}), ((), {}), ((1,), {}), ((1, 2, 3), {"c": 4}), ((1,), {"c": 3, "d": 4}), ((1,), {"a": 1, "c": 2}))])

print("---- with a great many arguments")
for base in (object, tuple, dict, str):
    x = one_of(type("C", (base,), {"__call__": lambda self, *a, **k: (len(a), len(k))}))
    print(base.__name__, x(*range(100000)), x(**{"k%d" % i: i for i in range(20000)}), x(*range(70000), **{"k%d" % i: i for i in range(5)}))

print("---- where something to call is wanted")
import atexit
for base in (tuple, list, dict, int, str, Exception):
    C = type("C_" + base.__name__, (base,), {"__call__": lambda self, *a: (print("   ", type(self).__name__, "called with", len(a)), a[0] if a else 0)[1]})
    x = one_of(C)
    print(base.__name__, sorted([3, 1, 2], key=x), list(filter(x, [0, 1])), max([1, 2], key=x), _functools.reduce(x, [1, 2]), [d for d in [x] if callable(d)] == [x])
    atexit.register(x, base.__name__)


@one_of(type("Decorator", (tuple,), {"__call__": lambda self, f: lambda: "decorated " + f()}))
def decorated():
    return "function"


print(decorated())

print("---- unittest.mock, which is where it was found")
from unittest import mock
print(mock.call(1), mock.call(timeout=None), mock.call.a(b=2), mock.call(1) == mock.call(1), mock.call.a.b(1).c(2))
m = mock.Mock()
m(1, k=2)
m.method(3)
print(m.mock_calls == [mock.call(1, k=2), mock.call.method(3)], m.call_args == mock.call(1, k=2))
