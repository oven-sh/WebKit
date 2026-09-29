# list.sort() and sorted(): what comes of them, and everything about how they go about it that a program can see. That is which things are compared with which, and in what order, how many times; that there is nothing in the list
# meanwhile; that it is noticed if something is put there; and what is in the list after something has gone wrong.
import random
import binascii as zlib
from functools import cmp_to_key


def attempt(f, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)


def t(label, f):
    r = attempt(f)
    print(label, "=>", r if isinstance(r, str) else ascii(r))


calls = []


class X:
    "Something that says what it is compared with"
    def __init__(self, v): self.v = v
    def __lt__(self, other):
        calls.append((self.v, other.v))
        return self.v < other.v
    def __repr__(self): return "X(%r)" % (self.v,)


def traced(values, **k):
    "What the order is, how many comparisons it took, and a sum over which they were and in what order"
    calls.clear()
    got = [X(v) for v in values]
    got.sort(**k)
    return zlib.crc32(repr([x.v for x in got]).encode()), len(calls), zlib.crc32(repr(calls).encode())


print("---- which are compared with which")
rng = random.Random(12345)
shapes = {
    "in order": lambda n: list(range(n)),
    "backwards": lambda n: list(range(n, 0, -1)),
    "all the same": lambda n: [7] * n,
    "at random": lambda n: [rng.randrange(n * 3 + 1) for _ in range(n)],
    "few different": lambda n: [rng.randrange(4) for _ in range(n)],
    "in order but for the last": lambda n: list(range(n)) + [0],
    "in order but for three swaps": lambda n: (lambda l: [l.__setitem__(slice(i, i + 2), l[i:i + 2][::-1]) for i in (n // 4, n // 2, 3 * n // 4) if n > 8] and l or l)(list(range(n))),
    "up and down": lambda n: [min(i % 40, 40 - i % 40) for i in range(n)],
    "two runs": lambda n: list(range(0, n, 2)) + list(range(1, n, 2)),
    "many runs": lambda n: [(i * 7919) % 97 + (i // 50) * 3 for i in range(n)],
    "backwards with some the same": lambda n: [(n - i) // 3 for i in range(n)],
    "a run that gallops": lambda n: list(range(n)) + list(range(n // 2, n // 2 + 5)) + list(range(n * 2, n * 3)),
}
for name, make in shapes.items():
    t(name, lambda: [traced(make(n)) for n in (0, 1, 2, 3, 5, 8, 16, 31, 32, 33, 63, 64, 65, 100, 127, 128, 129, 200, 256, 257, 500, 1000, 2500)])
    t(name + ", reversed", lambda: [traced(make(n), reverse=True) for n in (2, 3, 33, 64, 65, 200, 1000)])
    t(name + ", by a key", lambda: [traced(make(n), key=lambda x: X(-x.v)) for n in (2, 3, 33, 64, 65, 200, 1000)])
t("every order of up to six", lambda: [zlib.crc32(repr([traced(p) for p in __import__("itertools").permutations(range(n))]).encode()) for n in range(7)])
t("every list of five out of three", lambda: zlib.crc32(repr([traced(p) for p in __import__("itertools").product(range(3), repeat=5)]).encode()))

print("---- those that are equal stay in order")
pairs = [(rng.randrange(5), i) for i in range(300)]
t("forwards", lambda: sorted(pairs, key=lambda p: p[0]) == sorted(pairs))
t("backwards", lambda: sorted(pairs, key=lambda p: p[0], reverse=True) == sorted(pairs, key=lambda p: (-p[0], p[1])))

print("---- of every kind")
kinds = {
    "small ints": [5, -3, 0, 2 ** 20, -2 ** 20, 7, 7],
    "large ints": [2 ** 70, -2 ** 70, 5, 2 ** 31, -2 ** 31 - 1, 2 ** 63, 0],
    "floats": [1.5, -0.0, 0.0, float("inf"), -float("inf"), 1e300, 5.0, -5.0],
    "floats with a nan": [3.0, float("nan"), 1.0, 2.0, float("nan"), 0.0],
    "ints and floats": [1, 0.5, 2, 1.0, -1, 2 ** 70, 1e30],
    "bools": [True, False, True, False],
    "bools and ints": [True, 0, 2, False, 1],
    "strs": ["b", "a", "", "ab", "B", "aa", "\x7f", "\xff", "\x80"],
    "strs that are wide": ["b", "Ā", "a", "\U0001F600", "￿", "퟿", "", "\xe9"],
    "bytes": [b"b", b"a", b"", b"\xff", b"ab"],
    "tuples": [(2, "a"), (1, "b"), (1, "a"), (2,), (1,), (1, "a", 0)],
    "tuples with an empty one": [(2,), (), (1,)],
    "tuples of tuples": [((2,),), ((1, 2),), ((1,),)],
    "tuples of different things first": [(1, "a"), (0.5, "b"), (2, "c")],
    "tuples the same until late": [(1, 2, 3, 5), (1, 2, 3, 4), (1, 2, 3)],
    "lists": [[2], [1, 5], [1], []],
    "sets, which are only partly ordered": [{1, 2}, {1}, {2}, set(), {1, 2, 3}],
    "None": [None, None],
    "what cannot be compared": [1, "a", 2],
    "later on": [1, 2, 3, 4, 5, "a"],
    "tuples of what cannot be compared": [(1, 2), ("a", 1)],
    "tuples that differ where it cannot be": [(1, 2), (1, "a")],
    "objects": [object(), object()],
    "complex": [1j, 2j],
    "dicts": [{}, {}],
    "classes": [int, str],
}
for name, values in kinds.items():
    t(name, lambda: (attempt(sorted, values), attempt(sorted, values, reverse=True), attempt(sorted, values, key=repr) if name != "objects" else None))
t("strs joined up, which are not all in one place", lambda: sorted(["a" * i + "b" * (5 - i) for i in range(6)] + ["x" + str(i) + "y" for i in range(12)]))
t("a class derived from int, str, tuple", lambda: [sorted([k(v) for v in vs]) for k, vs in ((type("I", (int,), {}), (3, 1, 2)), (type("S", (str,), {}), "cab"), (type("T", (tuple,), {}), ((2,), (1,))))])

print("---- what __lt__ can give")


def giving(result):
    class G:
        def __init__(self, v): self.v = v
        def __lt__(self, other):
            calls.append(("lt", self.v, other.v))
            return result(self.v, other.v)
        def __gt__(self, other):
            calls.append(("gt", self.v, other.v))
            return self.v > other.v
        def __repr__(self): return "G%d" % self.v
    return G


class Truth:
    def __init__(self, v): self.v = v
    def __bool__(self):
        calls.append(("bool", self.v))
        return self.v


class BadTruth:
    def __bool__(self): raise KeyError("bool")


for name, result in (("NotImplemented", lambda a, b: NotImplemented), ("an int", lambda a, b: int(a < b)), ("a str", lambda a, b: "yes" if a < b else ""), ("None", lambda a, b: None), ("what says", lambda a, b: Truth(a < b)), ("what will not say", lambda a, b: BadTruth()), ("what raises", lambda a, b: 1 / 0)):
    G = giving(result)
    calls.clear()
    t(name, lambda: (attempt(sorted, [G(3), G(1), G(2)]), calls[:]))
    calls.clear()
    t(name + ", among other things", lambda: (attempt(sorted, [G(3), 1.5 if name == "never" else G(1), giving(result)(2)]), calls[:]))
    calls.clear()
    t(name + ", in tuples", lambda: (attempt(sorted, [(G(3), 0), (G(1), 0), (G(2), 0)]), calls[:]))

print("---- no __lt__, or only the other way")


class OnlyGt:
    def __init__(self, v): self.v = v
    def __gt__(self, other):
        calls.append(("gt", self.v, other.v))
        return self.v > other.v
    def __repr__(self): return "O%d" % self.v


calls.clear()
t("only __gt__", lambda: (sorted([OnlyGt(3), OnlyGt(1), OnlyGt(2)]), calls[:]))
t("__lt__ = None", lambda: attempt(sorted, [type("N", (), {"__lt__": None})() for _ in range(2)]))
t("__lt__ on the instance is not looked at", lambda: attempt(sorted, [(lambda o: (setattr(o, "__lt__", lambda other: True), o)[1])(type("N", (), {})()) for _ in range(2)]))


class Changes:
    "It becomes of another class in the middle"
    def __init__(self, v): self.v = v
    def __lt__(self, other):
        calls.append(("first", self.v, other.v))
        other.__class__ = Changed
        return self.v < other.v
    def __repr__(self): return type(self).__name__[-2:] + str(self.v)


class Changed(Changes):
    def __lt__(self, other):
        calls.append(("second", self.v, other.v))
        return self.v < other.v


calls.clear()
t("what changes class meanwhile", lambda: (sorted([Changes(v) for v in (3, 1, 2, 5, 4)]), calls[:]))

print("---- there is nothing in it meanwhile")
for name, act in (("nothing", lambda l: None), ("append", lambda l: l.append(1)), ("append and pop", lambda l: (l.append(1), l.pop())), ("pop", lambda l: l.pop()), ("clear", lambda l: l.clear()), ("set an item", lambda l: l.__setitem__(0, 5)),
                  ("reverse", lambda l: l.reverse()), ("extend with nothing", lambda l: l.extend([])), ("extend and delete", lambda l: (l.extend([1, 2]), l.__delitem__(slice(None)))), ("insert", lambda l: l.insert(0, 1)),
                  ("assign nothing to all of it", lambda l: l.__setitem__(slice(None), [])), ("assign something", lambda l: l.__setitem__(slice(None), [1])), ("+= nothing", lambda l: l.__iadd__([])), ("+= something", lambda l: l.__iadd__([1])),
                  ("*= 2", lambda l: l.__imul__(2)), ("sort", lambda l: l.sort()), ("copy", lambda l: l.copy()), ("remove", lambda l: l.remove(1)), ("index", lambda l: l.index(1)), ("append then clear", lambda l: (l.append(1), l.clear()))):
    for way in ("key", "compare"):
        z = [5, 3, 8, 1, 9, 2]
        seen = []
        if way == "key":
            def key(v):
                seen.append((len(z), list(z), repr(z)))
                act(z)
                return v
            r = attempt(z.sort, key=key)
        else:
            def compare(a, b):
                seen.append((len(z), list(z), repr(z)))
                act(z)
                return (a > b) - (a < b)
            r = attempt(z.sort, key=cmp_to_key(compare))
        print(name, "in the", way, "=>", r, z, seen[:2], len(seen))

print("---- what is left when something goes wrong")


def failing_after(n, values, **k):
    count = [0]

    class F:
        def __init__(self, v): self.v = v
        def __lt__(self, other):
            count[0] += 1
            if count[0] > n:
                raise KeyError(n)
            return self.v < other.v
    items = [F(v) for v in values]
    r = attempt(items.sort, **k)
    return r, [x.v for x in items]


values = [rng.randrange(100) for _ in range(90)]
t("after so many comparisons", lambda: [(r, zlib.crc32(repr(l).encode()), sorted(l) == sorted(values)) for n in (0, 1, 2, 5, 10, 50, 100, 200, 300, 400, 10000) for r, l in [failing_after(n, values)]])
t("backwards", lambda: [(r, zlib.crc32(repr(l).encode()), sorted(l) == sorted(values)) for n in (0, 1, 5, 50, 200, 400) for r, l in [failing_after(n, values, reverse=True)]])
t("a few", lambda: [failing_after(n, [3, 1, 2, 5, 4]) for n in range(9)])
t("a few, backwards", lambda: [failing_after(n, [3, 1, 2, 5, 4], reverse=True) for n in range(9)])


def key_failing_at(n, values, **k):
    seen = []

    def key(v):
        seen.append(v)
        if len(seen) > n:
            raise KeyError(n)
        return -v
    items = list(values)
    return attempt(items.sort, key=key, **k), items, seen


t("when the key function raises", lambda: [key_failing_at(n, [3, 1, 2]) for n in range(4)] + [key_failing_at(1, [3, 1, 2], reverse=True)])
t("and has put something in the list", lambda: [(attempt(z.sort, key=lambda v: (z.append(9), 1 / 0)), z) for z in [[3, 1, 2]]])
t("when a comparison raises and something has been put there", lambda: [(attempt(z.sort, key=cmp_to_key(lambda a, b: (z.append(9), 1 / 0))), z) for z in [[3, 1, 2]]])

print("---- how it is called")
t("arguments", lambda: [attempt([2, 1].sort, *a, **k) for a, k in (((), {}), ((None,), {}), ((), {"key": None}), ((), {"reverse": None}), ((), {"reverse": 1}), ((), {"reverse": "x"}), ((), {"reverse": []}), ((), {"key": 5}), ((), {"cmp": None}), ((1, 2), {}), ((), {"key": None, "reverse": BadTruth()}), ((), {"reverse": 2 ** 70}), ((), {"reverse": 1.5}))])
t("a key that cannot be called, of nothing and of one", lambda: (attempt([].sort, key=5), attempt([1].sort, key=5)))
t("of one, the key function is called", lambda: [(attempt([7].sort, key=seen.append), seen) for seen in [[]]])
t("sorted", lambda: [attempt(sorted, *a, **k) for a, k in (((), {}), (([2, 1], None), {}), ((5,), {}), (([2, 1],), {"key": None, "reverse": True}), (("cab",), {}), (({3: 0, 1: 0},), {}), ((iter([2, 1]),), {}), (([2, 1],), {"cmp": None}), ((), {"iterable": [1]}))])
t("it gives None, and the same list", lambda: [(l.sort(), l) for l in [[3, 1, 2]]])
t("a class derived from list", lambda: [(type(l).__name__, l.sort(), l, l.tag) for k in [type("L", (list,), {})] for l in [k([3, 1, 2])] for _ in [setattr(l, "tag", "kept")]])
t("with its own __len__, __iter__ and __getitem__, which are not asked", lambda: [(l.sort(), list.__repr__(l)) for l in [type("L", (list,), {"__len__": lambda s: 0, "__iter__": lambda s: iter(()), "__getitem__": lambda s, i: 1 / 0})([3, 1, 2])]])
t("a great many", lambda: (lambda l: (l.sort(), l == list(range(200000)))[1])([(i * 7919) % 200000 for i in range(200000)]))
t("a great many strs", lambda: (lambda l: (l.sort(), zlib.crc32("".join(l).encode())))([str((i * 7919) % 50000) for i in range(50000)]))
t("dir() is sorted the same way", lambda: dir(type("D", (), {"b": 1, "a": 2, "__dir__": lambda s: ["z", "a", "m"]})()))
t("and says so if it cannot be", lambda: attempt(dir, type("D", (), {"__dir__": lambda s: ["z", 1]})()))
