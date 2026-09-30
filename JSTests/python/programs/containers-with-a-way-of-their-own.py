# What goes through something that is derived from a container of Python's, and has a way of its own of being gone through, goes through it in that way. But not always: CPython
# takes what is in a set as it is, whatever class it is of, when it is making a set of it.
class MyStr(str):
    def __iter__(self): return iter(("MyStr", "__iter__"))
class MyBytes(bytes):
    def __iter__(self): return iter(("MyBytes", "__iter__"))
class MyList(list):
    def __iter__(self): return iter(("MyList", "__iter__"))
class MyTuple(tuple):
    def __iter__(self): return iter(("MyTuple", "__iter__"))
class MyDict(dict):
    def __iter__(self): return iter(("MyDict", "__iter__"))
class MySet(set):
    def __iter__(self): return iter(("MySet", "__iter__"))
class MyFrozen(frozenset):
    def __iter__(self): return iter(("MyFrozen", "__iter__"))
class MyArray(bytearray):
    def __iter__(self): return iter(("MyArray", "__iter__"))
import collections, itertools
for v in (MyStr("ab"), MyBytes(b"ab"), MyList([1, 2]), MyTuple((1, 2)), MyDict({1: 2}), MySet({1}), MyFrozen({1}), MyArray(b"ab")):
    x = [0]; x += v
    y = [0]; y.extend(v)
    z = [0]; z[1:] = v
    a, b = v
    s = set(); s.update(v)
    d = collections.deque(); d.extend(v)
    print(type(v).__name__, x, y, z, list(v), tuple(v), sorted(set(v)), sorted(frozenset(v)), [*v], (*v,), sorted({*v}), (a, b), sorted(s), list(d), ",".join(v), list(map(str, v)), list(zip(v)), list(enumerate(v)), sorted(v), list(itertools.chain(v)), dict.fromkeys(v), max(v), any(v), "MyStr" in list(v), (lambda *a: a)(*v), list(reversed(list(v))), bytes(v) if False else "", list(filter(None, v)))
