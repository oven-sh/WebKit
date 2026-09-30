# What goes round for ever, or very deep, in what is written in C. It is to end in RecursionError, or in an answer, and not in the process stopping.
import collections
import functools
import itertools
import types

try:
    import _javascript
    STOPS_CPYTHON = True
except ImportError:
    STOPS_CPYTHON = False


def has_itself(make, put):
    x = make()
    put(x, x)
    return x


def a_dict(): return has_itself(dict, lambda x, y: x.__setitem__("a", y))
def a_list(): return has_itself(list, list.append)


def deep(make, n=300000):
    x = None
    for i in range(n):
        x = make(x)
    return x


def exactly(label, f):
    "It says the same as CPython."
    try:
        print("   ", label, "=>", f())
    except RecursionError:
        print("   ", label, "=> RecursionError")


def ends(label, f):
    "How deep is too deep is not the same from one to another, so either will do."
    try:
        f()
    except RecursionError:
        pass
    print("   ", label, "ends")


print("---- what has itself in it")
for label, f in (("dict == dict", lambda: a_dict() == a_dict()), ("dict != dict", lambda: a_dict() != a_dict()), ("list == list", lambda: a_list() == a_list()), ("list < list", lambda: a_list() < a_list()), ("list >= list", lambda: a_list() >= a_list()),
                 ("a tuple in a list", lambda: has_itself(list, lambda x, y: x.append((y,))) == has_itself(list, lambda x, y: x.append((y,)))), ("a list in a dict", lambda: has_itself(dict, lambda x, y: x.__setitem__(1, [y])) == has_itself(dict, lambda x, y: x.__setitem__(1, [y]))),
                 ("deque == deque", lambda: has_itself(collections.deque, collections.deque.append) == has_itself(collections.deque, collections.deque.append)), ("in", lambda: a_list() in a_list()), ("index()", lambda: a_list().index(a_list())), ("count()", lambda: a_list().count(a_list())),
                 ("remove()", lambda: a_list().remove(a_list())), ("sorted()", lambda: sorted([a_list(), a_list()])), ("max()", lambda: max(a_list(), a_list())), ("the very same is equal", lambda: (lambda x: (x == x, x in x, x.index(x), x.count(x)))(a_list())), ("repr()", lambda: (repr(a_list()), repr(a_dict()))),
                 ("dict values", lambda: list(a_dict().values()) == list(a_dict().values())), ("dict items", lambda: a_dict().items() == a_dict().items())):
    exactly(label, f)

print("---- what is very deep")
for label, f in (("list ==", lambda: deep(lambda x: [x]) == deep(lambda x: [x])), ("tuple ==", lambda: deep(lambda x: (x,)) == deep(lambda x: (x,))), ("dict ==", lambda: deep(lambda x: {1: x}) == deep(lambda x: {1: x})), ("slice ==", lambda: deep(lambda x: slice(x)) == deep(lambda x: slice(x))),
                 ("list <", lambda: deep(lambda x: [x]) < deep(lambda x: [x])), ("sorted()", lambda: sorted([deep(lambda x: [x]), deep(lambda x: [x])])), ("repr() of a list", lambda: repr(deep(lambda x: [x]))), ("of a tuple", lambda: repr(deep(lambda x: (x,)))), ("of a dict", lambda: repr(deep(lambda x: {1: x}))),
                 ("hash() of a frozenset", lambda: hash(deep(lambda x: frozenset([x]), 100000))), ("of a slice", lambda: hash(deep(lambda x: slice(x)))), ("filter()", lambda: list(deep(lambda x: filter(None, x or ()), 200000))), ("enumerate()", lambda: list(deep(lambda x: enumerate(x or ()), 200000))),
                 ("islice()", lambda: list(deep(lambda x: itertools.islice(x or iter(()), 5), 200000))), ("isinstance() of a tuple", lambda: isinstance(1, deep(lambda x: (x or str,), 200000))), ("issubclass() of a tuple", lambda: issubclass(int, deep(lambda x: (x or str,), 200000))),
                 ("SimpleNamespace ==", lambda: deep(lambda x: types.SimpleNamespace(a=x), 100000) == deep(lambda x: types.SimpleNamespace(a=x), 100000)),
                 ("staticmethod", lambda: deep(lambda x: staticmethod(x or abs), 200000)(1)), ("a generator", lambda: list(deep(lambda x: (i for i in (x or ())), 100000)))):
    ends(label, f)
if STOPS_CPYTHON:
    for label, f in (("hash() of a tuple", lambda: hash(deep(lambda x: (x,)))), ("map()", lambda: list(deep(lambda x: map(abs, x or ()), 200000))), ("zip()", lambda: list(deep(lambda x: zip(x or ()), 200000))), ("chain()", lambda: list(deep(lambda x: itertools.chain(x or (), (1,)), 200000))),
                     ("a bound method", lambda: deep(lambda x: types.MethodType(x or (lambda *a: 0), 1), 200000)()), ("a partial of itself", lambda: (lambda p: (p.__setstate__((p, (), {}, {})), p()))(functools.partial(abs)))):
        try:
            f()
        except RecursionError:
            pass

print("---- and afterwards all is as it was")
print(a_list()[0][0][0] is not None, [1, [2, [3]]] == [1, [2, [3]]], (1, (2,)) < (1, (3,)), hash((1, (2, (3,)))) == hash((1, (2, (3,)))), list(map(abs, filter(None, [0, -1]))), isinstance(1, (str, (bytes, (int,)))))

print("---- the hash of a frozenset is worked out once")


def ordinals(n):
    "Each is the set of all that come before it."
    numbers = [frozenset()]
    for i in range(n - 1):
        numbers.append(frozenset(numbers))
    return numbers


numbers = ordinals(60)
print(len({hash(n) for n in numbers}), len(set(numbers)), numbers[59] == frozenset(numbers[:59]), hash(numbers[59]) == hash(frozenset(ordinals(60)[:59])), numbers[30] in numbers[59])


class Counts:
    hashed = 0

    def __hash__(self):
        Counts.hashed += 1
        return 5


f = frozenset([Counts()])
hash(f)
before = Counts.hashed
print(hash(f) == hash(f) == hash(f), Counts.hashed - before, hash(frozenset()) == hash(frozenset()), hash(type("F", (frozenset,), {})([1, 2])) == hash(frozenset([2, 1])))
s = {1, 2}
print({frozenset(s)} == {frozenset([1, 2])}, s in {frozenset([1, 2])}, s.add(3), s in {frozenset([1, 2])}, s in {frozenset([1, 2, 3])})
