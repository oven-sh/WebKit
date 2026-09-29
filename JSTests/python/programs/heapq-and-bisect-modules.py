# The modules _heapq and _bisect. What comes of them, which things are compared with which and in what order, and what is made of a list that is changed by what its elements are compared by.
import _bisect
import _heapq
import binascii
import bisect
import collections
import heapq
import random
import re


def attempt(f, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)


def t(label, f):
    r = attempt(f)
    # Where things are in memory is not part of it.
    print(label, "=>", re.sub(r" at 0x[0-9a-f]+", " at 0x", r if isinstance(r, str) else ascii(r)))


def crc(x):
    return binascii.crc32(repr(x).encode())


calls = []


class X:
    "Something that says what it is compared with"
    def __init__(self, v): self.v = v
    def __lt__(self, other):
        calls.append((self.v, other.v))
        return self.v < other.v
    def __repr__(self): return "X%r" % (self.v,)


print("---- what there is")
for module in (_heapq, _bisect):
    names = sorted(n for n in vars(module) if not n.startswith("__"))
    t(module.__name__, lambda: (module.__name__, module.__package__, module.__loader__.__name__, module.__doc__, names, sorted(n for n in vars(module) if n.startswith("__"))))
    for name in names:
        x = getattr(module, name)
        t(name, lambda: (type(x).__name__, x.__text_signature__, x.__doc__, x.__module__))
t("__about__", lambda: (type(_heapq.__about__).__name__, len(_heapq.__about__), crc(_heapq.__about__)))
t("heapq and bisect take them", lambda: ([n for n in vars(_heapq) if not n.startswith("__") and getattr(heapq, n, None) is not getattr(_heapq, n)], [n for n in vars(_bisect) if not n.startswith("__") and getattr(bisect, n, None) is not getattr(_bisect, n)], bisect.bisect is _bisect.bisect_right, bisect.insort is _bisect.insort_right))

print("---- heaps")
rng = random.Random(4321)
shapes = {"in order": lambda n: list(range(n)), "backwards": lambda n: list(range(n, 0, -1)), "all the same": lambda n: [5] * n, "at random": lambda n: [rng.randrange(n * 2 + 1) for _ in range(n)], "few different": lambda n: [rng.randrange(3) for _ in range(n)]}
SIZES = (0, 1, 2, 3, 4, 5, 7, 8, 15, 16, 31, 100, 1000, 2500, 2501, 2502, 4095, 4096, 5000)
for suffix in ("", "_max"):
    heapify, push, pop, replace, pushpop = (getattr(heapq, n + suffix) for n in ("heapify", "heappush", "heappop", "heapreplace", "heappushpop"))
    for name, make in shapes.items():
        def traced(n):
            calls.clear()
            h = [X(v) for v in make(n)]
            heapify(h)
            return crc([x.v for x in h]), len(calls), crc(calls)
        t("heapify%s, %s" % (suffix, name), lambda: [traced(n) for n in SIZES])

        def worked(n):
            calls.clear()
            h = []
            out = []
            for v in make(n):
                push(h, X(v))
            for i, v in enumerate(make(n // 2)):
                out.append((replace if i % 2 else pushpop)(h, X(v)).v)
            while h:
                out.append(pop(h).v)
            return crc(out), len(calls), crc(calls)
        t("push, replace, pushpop and pop%s, %s" % (suffix, name), lambda: [worked(n) for n in (0, 1, 2, 3, 5, 8, 33, 100, 500)])
    t("a few" + suffix, lambda: [(heapify(h), h, pop(h), h, push(h, 4), h, replace(h, 0), h, pushpop(h, 9), h, pushpop(h, -9), h) for h in [[5, 1, 8, 3, 9, 2]]])
    t("nothing in it" + suffix, lambda: (attempt(pop, []), attempt(replace, [], 1), pushpop([], 1), heapify([]), [(push(h, 1), h) for h in [[]]]))
    t("what is no list" + suffix, lambda: [attempt(f, *a) for v in ((), None, 5, "ab", collections.deque(), {}, collections.UserList()) for f, a in ((heapify, (v,)), (pop, (v,)), (push, (v, 1)), (replace, (v, 1)), (pushpop, (v, 1)))])
    t("a class derived from list" + suffix, lambda: [(heapify(h), list.__repr__(h), pop(h), push(h, 0), list.__repr__(h)) for h in [type("L", (list,), {"__getitem__": lambda s, i: 1 / 0, "__len__": lambda s: 0, "__setitem__": lambda s, i, v: 1 / 0, "append": lambda s, v: 1 / 0, "pop": lambda s: 1 / 0})([3, 1, 2])]])
    t("how they are called" + suffix, lambda: [attempt(f, *a, **k) for f in (heapify, pop) for a, k in (((), {}), (([], 1), {}), ((), {"heap": []}))] + [attempt(f, *a, **k) for f in (push, replace, pushpop) for a, k in (((), {}), (([],), {}), (([1], 1, 2), {}), ((), {"heap": [1], "item": 1}))])
    t("what cannot be compared" + suffix, lambda: (attempt(heapify, [1, "a"]), attempt(push, [1], "a"), attempt(pushpop, [1], "a"), attempt(replace, [1, 2, "a"], 0), attempt(pop, [1, "a", 2, 3]), attempt(heapify, [object(), object()]), heapify([object()]), attempt(heapify, [1j, 2j])))

    for act_name, act in (("empties it", lambda h: h.clear()), ("takes one off", lambda h: h and h.pop()), ("adds one", lambda h: len(h) < 40 and h.append(E(h, 50))), ("puts others there", lambda h: h.__setitem__(slice(None), [E(h, 100 + i) for i in range(len(h))])), ("raises", lambda h: 1 / 0)):
        class E:
            count = 0
            def __init__(self, heap, v): self.heap, self.v = heap, v
            def __lt__(self, other):
                E.count += 1
                if E.count == when:
                    act(self.heap)
                return self.v < other.v
            def __repr__(self): return "E%d" % self.v

        def interfered(operation):
            out = []
            global when
            for when in (1, 2, 3, 5):
                E.count = 0
                h = []
                h[:] = [E(h, v) for v in ((1, 3, 2, 7, 4, 5, 6, 9, 8) if not suffix else (9, 7, 8, 3, 6, 5, 4, 1, 2))]
                out.append((attempt(operation, h), h if len(h) < 12 else len(h)))
            return out
        t("what %s%s" % (act_name, suffix), lambda: [interfered(o) for o in (lambda h: heapify(h), lambda h: pop(h), lambda h: push(h, E(h, 0 if not suffix else 99)), lambda h: replace(h, E(h, 10 if not suffix else 0)), lambda h: pushpop(h, E(h, 10 if not suffix else 0)))])

t("what is written over them", lambda: (heapq.nsmallest(3, [5, 1, 8, 3, 9, 2]), heapq.nlargest(3, [5, 1, 8, 3, 9, 2]), list(heapq.merge([1, 4, 7], [2, 5, 8], [3, 6, 9])), list(heapq.merge([7, 4, 1], [8, 5, 2], reverse=True)), heapq.nsmallest(2, ["bb", "a", "ccc"], key=len), list(heapq.merge(["a", "ccc"], ["bb"], key=len))))

print("---- bisection")
for name in ("bisect_left", "bisect_right", "insort_left", "insort_right"):
    f = getattr(bisect, name)
    is_insort = name.startswith("insort")

    def run(a, x, *rest, **k):
        a = list(a) if isinstance(a, list) else a
        r = attempt(f, a, x, *rest, **k)
        return (r, a) if is_insort else r
    t(name, lambda: [run(a, x) for a in ([], [1], [1, 1], [1, 2, 3], [1, 2, 2, 2, 3], list(range(10))) for x in (0, 1, 2, 2.5, 3, 4)])
    t(name + ", between", lambda: [run([1, 2, 2, 3, 4, 5], 2, *r) for r in ((0,), (2,), (6,), (7,), (0, 0), (0, 3), (3, 3), (4, 2), (0, 6), (0, None), (1, 100), (100, 200), (-1,), (0, -1), (0, -2), (-5, -5), (2 ** 62, 2 ** 63 - 1), (2 ** 70,), (0, 2 ** 70), ("a",), (0, "a"), (1.5,), (None,), (True, True))])
    t(name + ", by name", lambda: [run([1, 2, 3], 2, **k) for k in ({"lo": 1}, {"hi": 1}, {"lo": 0, "hi": 3}, {"key": None}, {"key": lambda v: -v}, {"key": 5}, {"x": 1}, {"lo": 0, "hi": None, "key": None})] + [attempt(f, a=[1], x=1), attempt(f, [1], x=1), attempt(f), attempt(f, [1]), attempt(f, [1], 1, 0, 1, None), attempt(f, [1], 1, 0, 1, key=None, z=1)])
    t(name + ", by a key", lambda: [run(a, x, key=k) for a, x, k in (([(1, "a"), (2, "b"), (3, "c")], 2 if not is_insort else (2, "z"), lambda p: p[0]), (["a", "bb", "ccc"], 2 if not is_insort else "zz", len), ([3, 2, 1], -2 if not is_insort else 2, lambda v: -v), ([1, 2], 1, lambda v: 1 / 0), ([], 1, lambda v: 1 / 0), ([1], 1, lambda: 0))])
    calls.clear()
    t(name + ", which are compared", lambda: [(calls.clear(), attempt(f, [X(v) for v in a], X(x)) if not is_insort else None, calls[:])[1:] for a in ([], [1], [1, 2, 3, 4, 5, 6, 7], [2] * 6, list(range(20))) for x in (0, 2, 4, 99)])
    t(name + ", of what", lambda: [run(a, x) for a, x in (((1, 2, 3), 2), ("abc", "b"), (range(10), 5), (b"abc", 98), (bytearray(b"ac"), 98), (collections.deque([1, 3]), 2), (collections.UserList([1, 3]), 2), ({1: 1}, 1), ({1, 2}, 1), (5, 1), (None, 1), (iter([1]), 1), (memoryview(b"ac"), 98), ([1, "a"], 2), ([1, 2], "a"), ([None], None), ([1j], 1j))])


class Seq:
    def __init__(self, *v): self.v, self.log = list(v), []
    def __len__(self):
        self.log.append("len")
        return len(self.v)
    def __getitem__(self, i):
        self.log.append(i)
        return self.v[i]
    def insert(self, i, x):
        self.log.append(("insert", i, x))
        self.v.insert(i, x)


t("what is asked of what it is done to", lambda: [(f.__name__, r, attempt(f, s, 4, *r), s.log, s.v) for f in (bisect.bisect_left, bisect.bisect_right, bisect.insort_left, bisect.insort_right) for r in ((), (1,), (1, 3), (0, None)) for s in [Seq(1, 3, 5, 7, 9)]])
t("what has not all of that", lambda: [(f.__name__, attempt(f, s, 1)) for f in (bisect.bisect_left, bisect.insort_right) for s in (type("A", (), {"__len__": lambda s: 1})(), type("B", (), {"__getitem__": lambda s, i: 1})(), type("C", (), {"__len__": lambda s: 1, "__getitem__": lambda s, i: 0})(), type("D", (), {"__len__": lambda s: -1, "__getitem__": lambda s, i: 0})(), type("E", (), {"__len__": lambda s: 1 / 0, "__getitem__": lambda s, i: 0})(), type("F", (), {"__len__": lambda s: 3, "__getitem__": lambda s, i: 1 / 0})(), type("G", (), {"__len__": lambda s: "a", "__getitem__": lambda s, i: 0})())])
t("a class derived from list, whose insert is called", lambda: [(bisect.insort(l, 2), list.__repr__(l), l.log) for l in [type("L", (list,), {"log": [], "insert": lambda s, i, x: s.log.append((i, x))})([1, 3])]])


def giving(result):
    class G:
        def __init__(self, v): self.v = v
        def __lt__(self, other):
            calls.append(("lt", self.v, other.v))
            return result(self.v, other.v)
        def __gt__(self, other):
            calls.append(("gt", self.v, other.v))
            return self.v > other.v
    return G


for label, result in (("NotImplemented", lambda a, b: NotImplemented), ("an int", lambda a, b: int(a < b)), ("None", lambda a, b: None), ("what will not say", lambda a, b: type("B", (), {"__bool__": lambda s: 1 / 0})()), ("what raises", lambda a, b: 1 / 0)):
    G = giving(result)
    t("__lt__ gives " + label, lambda: [(calls.clear(), attempt(f, [G(v) for v in (1, 2, 3, 4, 5)], G(3)), calls[:])[1:] for f in (bisect.bisect_left, bisect.bisect_right)])
t("what changes the list meanwhile", lambda: [(attempt(f, l, C(3)), len(l)) for f in (bisect.bisect_left, bisect.bisect_right, bisect.insort_left, bisect.insort_right) for l in [[]] for C in [type("C", (), {"__init__": lambda s, v: setattr(s, "v", v), "__lt__": lambda s, o: (l.clear(), s.v < o.v)[1]})] for _ in [l.extend(C(v) for v in range(8))]])
