# hash(x), x.__hash__() and type(x).__hash__(x) are one int, whatever x is. What it is goes by where things are in memory, so a great many of each are tried.
import functools
import math
import operator
import sys


class C:
    def m(self): pass
    @classmethod
    def k(cls): pass
    @staticmethod
    def s(): pass
    def __call__(self): pass


def agree(x):
    # What a class has by that name is for its instances.
    a, b, c = hash(x), hash(x) if isinstance(x, type) else x.__hash__(), type(x).__hash__(x)
    return type(a) is int and type(b) is int and type(c) is int and a == b == c and {x: 1}[x] == 1 and x in {x}


def check(label, things):
    things = list(things)
    # Which kinds there is no hash of is part of what is looked at.
    without = sorted({type(x).__name__ for x in things if type(x).__hash__ is None})
    things = [x for x in things if type(x).__hash__ is not None]
    print(label, "=>", len(things) > 0, [x for x in things if not agree(x)][:3], without)


N = 3000
kept = [C() for _ in range(N)]
check("instances", kept)
check("methods of them", (c.m for c in kept))
check("methods of the class", (c.k for c in kept))
check("functions", [lambda: 0 for _ in range(N)])
check("functions of modules", [getattr(m, n) for m in (math, operator, sys, functools) for n in dir(m) if callable(getattr(m, n)) and not isinstance(getattr(m, n), type)])
check("built in", [getattr(__builtins__, n) for n in dir(__builtins__) if callable(getattr(__builtins__, n))])
check("methods of what is built in", [getattr(x, n) for x in ([], {}, "", b"", 0, 0.5, (), set(), frozenset(), bytearray(), range(0), 1j, None, True) for n in dir(x) if callable(getattr(x, n))])
check("of a great many lists", ([].append for _ in range(N)))
check("of a great many numbers", ((i * 7919).__add__ for i in range(N)))
check("of big numbers, floats and strings", [f for i in range(300) for f in ((2 ** 70 + i).__add__, (i + 0.5).__add__, ("s%d" % i).upper, (b"b%d" % i).upper, (i, i).count)])
check("what is in a class", [v for t in (int, str, list, dict, object, type, float, bytes) for v in vars(t).values() if v.__hash__ is not None])
check("classes", [type("T%d" % i, (), {}) for i in range(N)])
check("partial and the like", [f for i in range(300) for f in (functools.partial(len, i), operator.itemgetter(i), operator.attrgetter("a%d" % i), operator.methodcaller("m%d" % i))])
check("code, frames and cells", [f.__code__ for f in [lambda: 0 for _ in range(300)]] + [sys._getframe()] + [(lambda: x).__closure__[0] for x in range(300)])
check("generators and coroutines", [(i for i in ()) for _ in range(300)])

print("---- what is equal has the same")
c = C()
print(c.m == c.m, hash(c.m) == hash(c.m), c.m is c.m, len({c.m, c.m, c.k, c.k, C.k}), C().m == c.m, len({k.m for k in kept}))
l = []
print(l.append == l.append, hash(l.append) == hash(l.append), len({l.append, l.append, l.pop}), [].append == l.append, len == len, hash(len) == hash(len))
print((1).__add__ == (1).__add__, hash((1).__add__) == hash((1).__add__), "a".upper == "a".upper, hash("a".upper) == hash("a".upper), (2 ** 70).__add__ == (2 ** 70).__add__)
print(hash(len) != hash(abs), hash(math.sin) != hash(math.cos), len({getattr(math, n) for n in dir(math) if callable(getattr(math, n))}) == sum(1 for n in dir(math) if callable(getattr(math, n))))
print("---- never -1")
print(all(hash(x) != -1 and x.__hash__() != -1 for x in [k.m for k in kept] + [[].append for _ in range(N)]))
