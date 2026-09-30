import _thread
import js


def called(self, /, *a, **k):
    return "%s called with %r %r" % (type(self).__name__, a, k)


def derived(base, *args):
    return type("From_" + base.__name__, (base,), {"__call__": called})(*args)


# Each is a kind of cell of its own.
with_call = [derived(object), derived(int, 5), derived(float, 1.5), derived(complex, 1j), derived(str, "text"), derived(bytes, b"ab"), derived(bytearray, b"ab"), derived(tuple, (1, 2)), derived(list, [1, 2]), derived(dict, {"a": 1}),
             derived(set, [1]), derived(frozenset, [1]), derived(ValueError, "message"), derived(enumerate, []), derived(zip), derived(_thread.RLock)]
without = [type("Plain_" + type(x).__mro__[1].__name__, (type(x).__mro__[1],), {})() for x in with_call if type(x).__mro__[1] not in (enumerate,)]
built_in = [(1, 2), {"a": 1}, {1}, frozenset([1]), 1j, ValueError("message"), enumerate([]), b"ab", bytearray(b"ab"), [1, 2]]


class Later(tuple):
    pass


later = Later((1, 2))


def give():
    Later.__call__ = called


def take():
    del Later.__call__


class Raises(dict):
    def __call__(self):
        raise KeyError("from __call__")


class Calls(list):
    "What it is given is a function of JavaScript's."
    def __call__(self, f, *a):
        return f(*a, *self)


# What a constructor of JavaScript's makes is a Map, and no one asks a Map whether it can be called. Python goes by the class.
class OfMap(js.Map):
    __call__ = called


class OfDate(js.Date):
    __call__ = called


class OfError(js.Error):
    __call__ = called


made_by_javascript = [OfMap(), OfDate(0), OfError("message")]


def call(x, *a, **k):
    return x(*a, **k)


def with_keywords(x):
    return x(1, k=2)


def can_be_called(x):
    return callable(x)


def name(x):
    return type(x).__name__
