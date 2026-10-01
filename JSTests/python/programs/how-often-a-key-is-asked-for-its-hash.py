# How many times a key is asked for its hash.
import collections
class K:
    count = 0
    def __init__(self, v): self.v = v
    def __hash__(self): K.count += 1; return hash(self.v)
    def __eq__(self, o): return isinstance(o, K) and self.v == o.v
    def __repr__(self): return "K(%r)" % self.v
def counted(label, f):
    K.count = 0
    try:
        f()
    except BaseException as e:
        label += " " + type(e).__name__
    print(label, K.count)
n = 5
keys = [K(i) for i in range(n)]
d = dict.fromkeys(keys)
s = set(keys)
fs = frozenset(keys)
counted("hash of a tuple", lambda: hash((keys[0], 1)))
counted("hash of a tuple twice", lambda: (lambda t: (hash(t), hash(t)))((keys[0], 1)))
hash(fs)
counted("hash of a frozenset that has been asked before", lambda: hash(fs))
counted("dict.fromkeys(list)", lambda: dict.fromkeys(keys))
counted("dict.fromkeys(dict)", lambda: dict.fromkeys(d))
counted("dict.fromkeys(dict, v)", lambda: dict.fromkeys(d, 1))
counted("dict.fromkeys(set)", lambda: dict.fromkeys(s))
counted("dict.fromkeys(frozenset, v)", lambda: dict.fromkeys(fs, 1))
counted("dict.fromkeys(keys view)", lambda: dict.fromkeys(d.keys()))
counted("dict(dict)", lambda: dict(d))
counted("dict.copy()", lambda: d.copy())
counted("{**d}", lambda: {**d})
counted("d | d", lambda: d | d)
counted("d.update(d2)", lambda: {}.update(d))
counted("set(dict)", lambda: set(d))
counted("frozenset(dict)", lambda: frozenset(d))
counted("set(set)", lambda: set(s))
counted("set(frozenset)", lambda: set(fs))
counted("frozenset(set)", lambda: frozenset(s))
counted("set(keys view)", lambda: set(d.keys()))
counted("set(list)", lambda: set(keys))
counted("{*s}", lambda: {*s})
counted("{*d}", lambda: {*d})
counted("s.copy()", lambda: s.copy())
counted("s.update(d)", lambda: set().update(d))
counted("s.update(s)", lambda: set().update(s))
counted("s | s", lambda: s | fs)
counted("s & s", lambda: s & fs)
counted("s - s", lambda: s - fs)
counted("s ^ s", lambda: s ^ fs)
counted("s.union(d)", lambda: s.union(d))
counted("s.intersection(d)", lambda: s.intersection(d))
counted("s.difference(d)", lambda: s.difference(d))
counted("s.symmetric_difference(d)", lambda: s.symmetric_difference(d))
counted("s.difference_update(d)", lambda: set(s).difference_update(d))
counted("s.symmetric_difference_update(d)", lambda: set(s).symmetric_difference_update(d))
counted("s.intersection_update(d)", lambda: set(s).intersection_update(d))
counted("s.isdisjoint(d)", lambda: s.isdisjoint(d))
counted("s.issubset(d)", lambda: s.issubset(d))
counted("s.issuperset(d)", lambda: s.issuperset(d))
counted("s == s", lambda: s == fs)
counted("s <= s", lambda: s <= fs)
counted("d == d", lambda: d == dict(d))
counted("d.keys() & s", lambda: d.keys() & s)
counted("d.keys() | s", lambda: d.keys() | s)
counted("d.keys() - s", lambda: d.keys() - s)
counted("d.keys() ^ s", lambda: d.keys() ^ s)
counted("d.keys() == s", lambda: d.keys() == s)
counted("d.keys() <= s", lambda: d.keys() <= s)
counted("d.keys().isdisjoint(s)", lambda: d.keys().isdisjoint(s))
counted("d.items() & d.items()", lambda: d.items() & d.items())
counted("k in d", lambda: keys[0] in d)
counted("d[k]", lambda: d[keys[0]])
counted("d.get(k)", lambda: d.get(keys[0]))
counted("d[k] = v", lambda: dict(d).__setitem__(keys[0], 1))
counted("d.setdefault(k)", lambda: {}.setdefault(keys[0], 1))
counted("d.pop(k)", lambda: dict(d).pop(keys[0]))
counted("d.pop(k) of an empty one", lambda: {}.pop(keys[0], None))
counted("d.popitem()", lambda: dict(d).popitem())
counted("del d[k]", lambda: dict(d).__delitem__(keys[0]))
counted("s.add(k)", lambda: set().add(keys[0]))
counted("s.discard(k)", lambda: set(s).discard(keys[0]))
counted("s.remove(k)", lambda: set(s).remove(keys[0]))
counted("s.pop()", lambda: set(s).pop())
counted("k in s", lambda: keys[0] in s)
counted("k in an empty set", lambda: keys[0] in set())
counted("k in an empty dict", lambda: keys[0] in {})
counted("Counter(list)", lambda: collections.Counter(keys))
counted("Counter(dict)", lambda: collections.Counter(d))
counted("defaultdict[k]", lambda: collections.defaultdict(int)[keys[0]])
counted("sorted(set)", lambda: list(s))
counted("repr", lambda: repr(d))
counted("dict comprehension", lambda: {k: 1 for k in d})
counted("set comprehension", lambda: {k for k in d})
counted("display", lambda: {keys[0]: 1, keys[1]: 2})
counted("set display", lambda: {keys[0], keys[1]})
class L(list):
    __slots__ = "hashvalue"
    def __init__(self, t): self[:] = t; self.hashvalue = hash(t)
    def __hash__(self): return self.hashvalue
def lru():
    key = L((keys[0], 1))
    c = {}
    c.get(key); key in c; c[key] = 1; c.get(key); del c[key]
counted("a list that has kept its hash", lru)
import functools, unittest.mock
counted("py lru_cache", lambda: (lambda f: (f(keys[0], 1), f(keys[0], 1)))(functools.lru_cache(maxsize=1)(lambda x, y: 0)))
