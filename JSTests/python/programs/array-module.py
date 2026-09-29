# The module array.
import array
import collections.abc
import copy
import io
import struct
import sys
import warnings
import weakref

warnings.simplefilter("ignore", DeprecationWarning)
A = array.array


def show(e):
    return type(e).__name__ + ": " + str(e)


def attempt(f, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return show(e)


def shown(r):
    "As ascii() would show it, but that an array that cannot be shown does not take everything else with it"
    if isinstance(r, (list, tuple)):
        inner = ", ".join(shown(v) for v in r)
        return "[%s]" % inner if isinstance(r, list) else "(%s)" % inner
    return attempt(ascii, r)


def t(label, f):
    r = attempt(f)
    print(label, "=>", r if isinstance(r, str) else shown(r))


inf, nan = float("inf"), float("nan")
INTS = "bBhHiIlLqQ"
print("---- what there is")
t("the module", lambda: (array.__name__, array.__package__, array.__loader__.__name__, array.__doc__, sorted(n for n in vars(array) if not n.startswith("__")), array.typecodes, array.ArrayType is A))
t("_array_reconstructor", lambda: [(type(x).__name__, x.__text_signature__, x.__doc__, x.__module__) for x in [array._array_reconstructor]])
for c in (A, type(iter(A("b")))):
    t(c.__name__, lambda: (c.__module__, c.__qualname__, [b.__name__ for b in c.__mro__], sorted(vars(c)), c.__doc__, c.__text_signature__, c.__basicsize__, c.__itemsize__, c.__weakrefoffset__ != 0, c.__dictoffset__, c.__flags__ & 0x7FFF, repr(c), attempt(setattr, c, "x", 1)))
    for name in sorted(vars(c)):
        x = vars(c)[name]
        t("%s.%s" % (c.__name__, name), lambda: (type(x).__name__, getattr(x, "__text_signature__", None), x.__doc__ if not isinstance(x, (str, type(None))) else None))
t("what it is", lambda: (isinstance(A("b"), collections.abc.MutableSequence), issubclass(A, collections.abc.Sequence), attempt(hash, A("b")), A.__hash__, repr(A[int]), bool(A("b")), bool(A("b", [0])), [(c, A(c).itemsize, A(c).typecode) for c in array.typecodes]))


class Index:
    def __init__(self, v): self.v = v
    def __index__(self): return self.v


class Float:
    def __init__(self, v): self.v = v
    def __float__(self): return self.v


class Raises:
    def __index__(self): raise ValueError("no index")
    def __float__(self): raise ValueError("no float")
    def __iter__(self): raise ValueError("no iter")
    def __eq__(self, other): raise ValueError("no eq")


class I(int):
    pass


print("---- how one is made")
t("the typecode", lambda: [attempt(A, *a) for a in ((), ("",), ("bb",), ("x",), ("\xe9",), ("\U0001f600",), (5,), (None,), (b"b",), ("b", [], 1), ("B",), (type("S", (str,), {})("i"),), ("\0",), (" ",), ("e",), ("?",), ("n",), ("P",), ("c",))] + [attempt(lambda: A(typecode="b")), attempt(lambda: A("b", initializer=[])), attempt(lambda: A("b", x=1))])
for c in array.typecodes:
    good = "ab" if c in "uw" else [1, 2]
    t("array(%r, ...)" % c, lambda: [attempt(A, c, x) for x in (good, tuple(good), iter(good), (v for v in good), [], (), "", b"", bytearray(), b"\1" * 8, bytearray(b"\1" * 8), b"\1" * 7, memoryview(b"\1" * 8), "ab", A(c, good), A("b", [1]), A("d", [1.0]), A("w", "x"), range(3), {1: 2}, {1}, None, 5, 1.5, Raises(), [None], ["a"], [1.5], [b"a"], ["ab"], [1, "a"])])
t("what a list is asked", lambda: [attempt(A, "i", type("L", (list,), {"__iter__": lambda s: iter([9]), "__getitem__": lambda s, i: 7, "__len__": lambda s: 5})([1, 2])), attempt(A, "i", type("T", (tuple,), {"__iter__": lambda s: iter([9]), "__getitem__": lambda s, i: 7})((1, 2)))])

print("---- what fits")
for c in INTS:
    n = A(c).itemsize * 8
    lo, hi = (-2 ** (n - 1), 2 ** (n - 1) - 1) if c.islower() else (0, 2 ** n - 1)
    t(c, lambda: [attempt(A, c, [v]) for v in (lo, hi, lo - 1, hi + 1, 0, 1, -1, 2 ** 15, -2 ** 15 - 1, 2 ** 16, 2 ** 31, -2 ** 31 - 1, 2 ** 32, 2 ** 63, -2 ** 63 - 1, 2 ** 64, 10 ** 30, -10 ** 30, True, I(5), Index(5), Index(-1), Index(hi + 1), Index("a"), 1.0, "1", None, b"1", Raises(), Float(1.0), 1j)])
for c in "fd":
    t(c, lambda: [attempt(A, c, [v]) for v in (0.0, -0.0, 1.5, 0.1, 1e38, 1e39, -1e39, 1e308, 5e-324, 1e-46, inf, -inf, nan, 1, True, 10 ** 30, 10 ** 400, I(2), Index(2), Float(2.5), Float("a"), "1", None, b"1", Raises(), 1j)])
for c in "uw":
    t(c, lambda: [attempt(A, c, [v]) for v in ("a", "\xe9", "一", "\U0001f600", "\ud800", "\0", "", "ab", "\U0001f600\U0001f600", 97, None, b"a", ["a"], type("S", (str,), {})("z"))])

print("---- as a sequence")
a = A("i", [10, 20, 30, 40, 50])
t("items", lambda: (len(a), a[0], a[-1], a[Index(1)], a[True], [attempt(a.__getitem__, k) for k in (5, -6, 10 ** 30, -10 ** 30, 1.0, "a", None, (0,), Raises())], list(a), list(reversed(a)), 30 in a, 31 in a, "a" in a, None in a, 30.0 in a, type(a[0]).__name__))
SLICES = [slice(*s) for s in ((None,), (2,), (1, 3), (-2, None), (None, None, 2), (None, None, -1), (1, None, 2), (4, 0, -2), (10, 20), (3, 1), (-100, 100), (None, None, 3), (None, None, -3), (0, 0), (5, None), (2, 2, -1), (None, None, 100), (None, None, -100), (Index(1), Index(4), Index(2)))]
t("slices", lambda: [(a[s].tolist(), type(a[s]).__name__, a[s].typecode) for s in SLICES] + [attempt(a.__getitem__, slice(None, None, 0)), attempt(a.__getitem__, slice("a")), attempt(a.__getitem__, slice(Raises()))])


def changed(f, start=(10, 20, 30, 40, 50), c="i"):
    x = A(c, start)
    r = attempt(f, x)
    return (r if isinstance(r, str) else None), x.tolist()


t("assigning an item", lambda: [changed(lambda x: x.__setitem__(k, v)) for k, v in ((0, 1), (-1, 1), (4, 1), (5, 1), (-6, 1), (Index(2), Index(7)), (0, "a"), (0, 2 ** 31), (0, None), (1.0, 1), ("a", 1), (None, 1), (10 ** 30, 1))])
t("deleting an item", lambda: [changed(lambda x: x.__delitem__(k)) for k in (0, -1, 2, 4, 5, -6, Index(1), 1.0, "a", 10 ** 30)])
t("deleting a slice", lambda: [changed(lambda x: x.__delitem__(s)) for s in SLICES])
for new in ([], [1], [1, 2], [1, 2, 3], [1, 2, 3, 4, 5, 6, 7]):
    t("assigning %d to a slice" % len(new), lambda: [changed(lambda x: x.__setitem__(s, A("i", new))) for s in SLICES])
t("assigning what will not do to a slice", lambda: [changed(lambda x: x.__setitem__(slice(1, 3), v)) for v in ([1, 2], (1,), "ab", b"ab", None, 5, A("h", [1]), A("I", [1]), A("l", [1]), A("d", [1.0]), iter([1]))])
t("assigning itself", lambda: [changed(lambda x: x.__setitem__(s, x)) for s in (slice(None), slice(1, 3), slice(0, 0), slice(5, None), slice(None, None, 1), slice(None, None, -1), slice(None, None, 2), slice(2, 2))])
t("+ and *", lambda: (A("i", [1]) + A("i", [2, 3]), A("i") + A("i"), A("i", [1, 2]) * 3, 3 * A("i", [1, 2]), A("i", [1]) * 0, A("i", [1]) * -1, A("i") * 5, A("i", [1, 2]) * Index(2), A("i", [1]) * True, [attempt(lambda: A("i", [1]) + v) for v in ([1], (1,), 1, None, "a", b"a", A("h", [1]), A("I", [1]), A("d", [1.0]))], [attempt(lambda: v + A("i", [1])) for v in ([1], (1,), 1, b"a")], [attempt(lambda: A("i", [1]) * v) for v in (1.5, "a", None, [1], A("i", [1]), 2 ** 62, 2 ** 63, 10 ** 30, -10 ** 30, Raises())], [attempt(lambda: v * A("i", [1])) for v in (1.5, "a", None)]))


def inplace(op, v, start=(1, 2)):
    x = y = A("i", start)
    try:
        if op == "+":
            x += v
        else:
            x *= v
    except BaseException as e:
        return show(e), y.tolist()
    return x is y, y.tolist()


t("+= and *=", lambda: [inplace("+", v) for v in (A("i", [3]), A("i"), [3], (3,), 3, None, "a", b"a", A("h", [3]), A("d", [3.0]), iter([3]))] + [inplace("*", v) for v in (0, 1, 2, 3, -1, True, Index(2), 1.5, "a", None, 2 ** 62, 2 ** 63, 10 ** 30, Raises())] + [inplace("*", 5, ()), [(x.__iadd__(x), x.tolist())[1] for x in [A("i", [1, 2])]]])
t("a derived class comes to an array", lambda: [type(f(type("D", (A,), {})("i", [1, 2, 3]))).__name__ for f in (lambda d: d[:], lambda d: d[::2], lambda d: d + d, lambda d: d * 2, lambda d: 2 * d, copy.copy, copy.deepcopy, lambda d: d.__copy__())])

print("---- its methods")
t("append", lambda: [changed(lambda x: x.append(v)) for v in (1, Index(2), True, "a", 2 ** 31, None, 1.5)] + [attempt(A("i").append), attempt(A("i").append, 1, 2)])
t("insert", lambda: [changed(lambda x: x.insert(i, 99)) for i in (0, 1, 5, 6, 100, -1, -5, -6, -100, Index(2), True, 2 ** 63 - 1, -2 ** 63)] + [changed(lambda x: x.insert(*a)) for a in ((0, "a"), ("a", 1), (1.5, 1), (None, 1), (2 ** 63, 1), (0,), (), (0, 1, 2), (Raises(), 1), (0, Raises()))])
t("extend", lambda: [changed(lambda x: x.extend(v)) for v in (A("i", [1, 2]), A("i"), [1, 2], (1, 2), iter([1, 2]), range(3), {1: 2}, "", b"\1\2", bytearray(b"\1"), [], [1, "a", 3], [1, 2 ** 31], A("h", [1]), A("I", [1]), A("d", [1.0]), "ab", None, 5, Raises())] + [changed(lambda x: x.extend(x)), attempt(A("i").extend), attempt(A("i").extend, [], [])])
t("pop", lambda: [(changed(lambda x: got.append(x.pop(*a))), got) for a in ((), (0,), (-1,), (2,), (4,), (5,), (-5,), (-6,), (Index(1),), (True,), (1.5,), ("a",), (None,), (2 ** 63,), (0, 0)) for got in [[]]] + [attempt(A("i").pop), attempt(A("i").pop, 0)])
t("remove", lambda: [changed(lambda x: x.remove(v), (1, 2, 3, 2, 1)) for v in (1, 2, 3, 4, 2.0, True, "a", None, Raises())] + [attempt(A("i").remove), attempt(A("i").remove, 1)])
t("count and index", lambda: [(x.count(1), x.count(2), x.count(9), x.count(1.0), x.count(True), x.count("a"), x.count(None), attempt(x.count, Raises()), attempt(x.count), x.index(1), x.index(2), x.index(1, 1), x.index(1, -1), x.index(3, 1, 3), [attempt(x.index, *a) for a in ((9,), (1, 5), (1, 1, 4), (3, 3), (3, 0, 2), (1, -100), (1, 0, -100), (1, 0, -1), (1, 100), (1, 2 ** 70), (1, -2 ** 70), (1, 0, 2 ** 70), (1, "a"), (1, None), (1, 0, None), (1, 1.5), (1, Index(1)), (), (1, 0, 5, 1), (Raises(),), ("a",))]) for x in [A("i", [1, 2, 3, 2, 1])]])
t("a NaN is not to be found", lambda: [(x.count(nan), nan in x, attempt(x.index, nan), attempt(x.remove, nan), x == x, x == A(c, [nan]), x != A(c, [nan]), x < x, x <= x, A(c, [1.0, nan]) == A(c, [1.0, nan]), x.tolist()[0] != x.tolist()[0]) for c in "fd" for x in [A(c, [nan])]])
t("reverse", lambda: [changed(lambda x: x.reverse(), s, c) for c in "bhiqd" for s in ((), (1,), (1, 2), (1, 2, 3), (1, 2, 3, 4))] + [attempt(A("i").reverse, 1)])
t("byteswap", lambda: [(c, [(x.byteswap(), x.tobytes())[1] for x in [A(c, bytes(range(1, 17)))]]) for c in array.typecodes] + [attempt(A("i").byteswap, 1), [(x.byteswap(), x.tolist())[1] for x in [A("d", [1.0, -2.5])]], [(x.byteswap(), x.byteswap(), x.tolist())[2] for x in [A("f", [1.0, -2.5])]]])
t("clear", lambda: [changed(lambda x: x.clear()), changed(lambda x: x.clear(), ()), attempt(A("i").clear, 1)])
t("tolist and fromlist", lambda: [(A(c, [1, 2]).tolist(), {type(v).__name__ for v in A(c, [1, 2]).tolist()}) for c in INTS + "fd"] + [A("w", "ab").tolist(), A("u", "ab").tolist()] + [changed(lambda x: x.fromlist(v)) for v in ([1, 2], [], [1, "a"], [1, 2 ** 31, 3], (1, 2), iter([1]), "ab", None, A("i", [1]), type("L", (list,), {})([7]))] + [attempt(A("i").fromlist), attempt(A("i").tolist, 1)])
t("tobytes and frombytes", lambda: [(c, A(c, bytes(range(16))).tobytes() == bytes(range(16)), len(A(c, bytes(16)))) for c in array.typecodes] + [changed(lambda x: x.frombytes(v), (1,)) for v in (b"", b"\2\0\0\0", bytearray(b"\2\0\0\0"), memoryview(b"\2\0\0\0"), b"\2\0\0", b"\2", b"\2\0\0\0\3", "abcd", None, 5, [2, 0, 0, 0], memoryview(b"\2\0\0\0").cast("i"), memoryview(b"\2\0\0\0\3\0\0\0")[::2], A("b", [2, 0, 0, 0]), A("B", [2, 0, 0, 0]), A("i", [2]), A("h", [2, 0]))] + [attempt(A("i").frombytes), attempt(A("i").tobytes, 1), type(A("i").tobytes()).__name__])
t("tounicode and fromunicode", lambda: [(c, A(c, "a\xe9一\U0001f600").tounicode(), A(c).tounicode(), [(x.fromunicode("cd\U0001f600"), x.tounicode(), len(x))[1:] for x in [A(c, "ab")]], [(x.fromunicode(""), x.tounicode())[1] for x in [A(c, "ab")]], [attempt(A(c).fromunicode, v) for v in (b"a", 5, None, ["a"])], attempt(A(c).fromunicode), attempt(A(c).tounicode, 1)) for c in "uw"] + [attempt(A(c).tounicode) for c in "bid"] + [attempt(A(c).fromunicode, "a") for c in "bid"] + [attempt(A("i").fromunicode, 5)])
t("characters that are none", lambda: [(c, [(attempt(x.tounicode), attempt(x.__getitem__, 0), attempt(x.tolist), attempt(repr, x), attempt(list, x), len(x)) for b in (b"\0\0\x11\0", b"\xff\xff\xff\xff", b"\0\xd8\0\0", b"\xff\xff\x10\0", b"\xff\xfe\0\0a\0\0\0", b"\0\0\xfe\xff", b"a\0\0\0\0\0\x11\0") for x in [A(c, b)]]) for c in "uw"])
t("buffer_info", lambda: [(type(i).__name__, len(i), type(i[0]).__name__, i[0] != 0, i[1]) for i in [A("i", [1, 2, 3]).buffer_info()]] + [A("i").buffer_info(), A("d").buffer_info(), [(x.clear(), x.buffer_info()[0] != 0, x.buffer_info()[1]) for x in [A("i", [1])]], [(x.clear(), x.buffer_info()) for x in [A("i", range(100))]], [(x.pop(), x.buffer_info()[0] != 0, x.buffer_info()[1]) for x in [A("i", [1])]], attempt(A("i").buffer_info, 1)])
t("repr", lambda: [repr(x) for x in (A("b"), A("i", [1, -2]), A("d", [1.0, -0.0, inf, nan]), A("f", [0.1]), A("w"), A("w", "a'b\"\n\xe9\U0001f600"), A("u", "ab"), A("Q", [2 ** 64 - 1]), type("Sub", (A,), {})("h", [1]), type("Sub", (A,), {})("h"))] + [str(A("b", [1]))])

print("---- compared")
PAIRS = [(A(c1, x), A(c2, y)) for c1, c2 in (("i", "i"), ("i", "h"), ("b", "B"), ("i", "d"), ("f", "d"), ("d", "d"), ("q", "Q")) for x, y in (([], []), ([1], [1]), ([1], [2]), ([2], [1]), ([1], [1, 2]), ([1, 2], [1]), ([], [1]), ([1, 2, 3], [1, 2, 4]), ([1, 5], [2]), ([1, 2], [1, 2]))]
t("with one another", lambda: ["".join("01"[bool(r)] for r in (x == y, x != y, x < y, x <= y, x > y, x >= y)) for x, y in PAIRS])
t("of characters", lambda: ["".join("01"[bool(r)] for r in (x == y, x != y, x < y, x <= y, x > y, x >= y)) for x, y in [(A(c1, p), A(c2, q)) for c1, c2 in (("w", "w"), ("u", "w"), ("u", "u")) for p, q in (("", ""), ("a", "a"), ("a", "b"), ("ab", "a"), ("\U0001f600", "￿"))]] + [attempt(lambda: A("w", "a") < A("i", [1])), A("w", "a") == A("i", [97]), A("w", "a") != A("i", [97]), A("w") == A("i"), A("w") < A("i", [1])])
t("with something else", lambda: [(A("i", [1]) == v, A("i", [1]) != v, attempt(lambda: A("i", [1]) < v), A("i", [1]).__eq__(v), A("i", [1]).__lt__(v)) for v in ([1], (1,), 1, None, b"\1\0\0\0", "a", memoryview(A("i", [1])))])
t("signed and not", lambda: (A("b", [-1]) < A("b", [1]), A("B", [255]) > A("B", [1]), A("b", [-1]) == A("B", [255]), A("q", [-1]) < A("Q", [0]), A("i", [-1]) < A("I", [0]), A("h", [-2 ** 15]) < A("h", [2 ** 15 - 1])))

print("---- how much room it has")
t("as it is made", lambda: [A(c, x).__sizeof__() - A(c).__sizeof__() for c in "bid" for x in ([], [1], [1, 2, 3], range(3), range(10), tuple(range(10)), bytes(16), A(c, [1, 2, 3]))] + [A("w", "abc").__sizeof__() - A("w").__sizeof__(), A("u", "abc").__sizeof__() - A("u").__sizeof__(), A("w", "").__sizeof__() - A("w").__sizeof__(), attempt(A("b").__sizeof__, 1)])


def growth(f, n, start=()):
    x = A("b", start)
    base = A("b").__sizeof__()
    out = []
    for i in range(n):
        f(x, i)
        s = x.__sizeof__() - base
        if not out or out[-1] != s:
            out.append(s)
    return out


t("as it grows", lambda: (growth(lambda x, i: x.append(1), 300), growth(lambda x, i: x.extend([1, 2, 3]), 60), growth(lambda x, i: x.extend(A("b", [1, 2, 3])), 60), growth(lambda x, i: x.insert(0, 1), 100), growth(lambda x, i: x.frombytes(b"abcde"), 40), growth(lambda x, i: x.fromlist([1] * 7), 30), growth(lambda x, i: x.__iadd__(A("b", [1] * 20)), 20)))
t("as it shrinks", lambda: (growth(lambda x, i: x.pop(), 200, bytes(200)), growth(lambda x, i: x.__delitem__(slice(0, 3)), 60, bytes(200)), growth(lambda x, i: x.__delitem__(slice(None, None, 2)), 8, bytes(200)), growth(lambda x, i: x.remove(0), 100, bytes(100)), growth(lambda x, i: x.__imul__(2), 8, [1]), growth(lambda x, i: x.__setitem__(slice(0, 1), A("b", [1, 2, 3])), 40, [1]), growth(lambda x, i: (x.clear(), x.append(1)), 3, bytes(50))))

print("---- files")
f = io.BytesIO()
t("tofile", lambda: (A("i", [1, 2, 3]).tofile(f), f.getvalue(), A("i").tofile(f), len(f.getvalue()), [attempt(A("i", [1]).tofile, v) for v in (None, 5, io.StringIO(), io.BytesIO(b"").detach if False else object())], attempt(A("i").tofile, None), attempt(A("i").tofile), attempt(A("i").tofile, f, f)))


class Writes:
    def __init__(self): self.sizes = []
    def write(self, b): self.sizes.append((type(b).__name__, len(b)))


t("a block at a time", lambda: [(A("b", bytes(n)).tofile(w), w.sizes)[1] for n in (0, 1, 65535, 65536, 65537, 200000) for w in [Writes()]] + [[(A("d", bytes(8 * 20000)).tofile(w), w.sizes)[1] for w in [Writes()]]])
t("fromfile", lambda: [changed(lambda x: x.fromfile(io.BytesIO(struct.pack("<3i", 7, 8, 9)), n), (1,)) for n in (0, 1, 2, 3, 4, 100, -1, Index(2), True)] + [changed(lambda x: x.fromfile(io.BytesIO(b"\7\0\0\0\10\0"), 2), (1,)), changed(lambda x: x.fromfile(io.BytesIO(b"\7\0"), 1), (1,))] + [attempt(A("i").fromfile, *a) for a in ((), (io.BytesIO(),), (io.BytesIO(), "a"), (io.BytesIO(), 1.5), (io.BytesIO(), None), (io.BytesIO(), 2 ** 63), (io.BytesIO(), 2 ** 62), (None, 1), (io.StringIO("abcd"), 1), (io.BytesIO(), 1, 1))])


class Reads:
    def __init__(self, v): self.v, self.asked = v, []
    def read(self, n):
        self.asked.append(n)
        return self.v


t("what read() gives", lambda: [(changed(lambda x: x.fromfile(r, 1), ()), r.asked) for v in (b"\1\0\0\0", bytearray(b"\1\0\0\0"), memoryview(b"\1\0\0\0"), "abcd", None, 5, b"", b"\1\0\0\0\2\0\0\0", b"\1\0\0\0\2", type("B", (bytes,), {})(b"\1\0\0\0")) for r in [Reads(v)]])

print("---- pickled")
for c in array.typecodes:
    x = A(c, "ab" if c in "uw" else [1, 2])
    t("__reduce_ex__ %s" % c, lambda: [(r[0].__name__, r[1][0].__name__ if isinstance(r[1][0], type) else r[1][0], r[1][1:], r[2]) for p in (0, 1, 2, 3, 4, 5) for r in [x.__reduce_ex__(p)]])
t("__reduce_ex__ called otherwise", lambda: [attempt(A("i").__reduce_ex__, *a) for a in ((), ("a",), (1.5,), (None,), (Index(3),), (2 ** 70,), (True,), (-1,), (100,), (3, 3))])


class D(A):
    pass


d = D("i", [1, 2])
d.extra = 5
t("of a derived class", lambda: [(r[0].__name__, r[1][0].__name__, r[1][1:], r[2]) for p in (2, 4) for r in [d.__reduce_ex__(p)]] + [(type(y).__name__, y.tolist(), y.extra) for y in [copy.copy(d), copy.deepcopy(d)]] + [attempt(getattr, A("i"), "__dict__")])
R = array._array_reconstructor
t("_array_reconstructor", lambda: [attempt(R, *a) for a in ((), (A,), (A, "i"), (A, "i", 8), (A, "i", 8, b"", 1), (5, "i", 8, b""), (int, "i", 8, b""), (A, "x", 8, b""), (A, "", 8, b""), (A, "ii", 8, b""), (A, 5, 8, b""), (A, "i", -1, b""), (A, "i", 22, b""), (A, "i", "a", b""), (A, "i", 1.5, b""), (A, "i", 2 ** 40, b""), (A, "i", 8, "a"), (A, "i", 8, bytearray()), (A, "i", 8, None), (A, "i", 8, b"abc"), (A, "i", 9, b"abc"), (D, "i", 8, b"\1\0\0\0"), (A, "i", Index(8), b"\1\0\0\0"))])
DATA = bytes(range(1, 17))
t("from every kind of machine", lambda: [(m, [attempt(R, A, c, m, DATA) for c in "bBhiqd"]) for m in range(18)])
t("floats from another", lambda: [attempt(R, A, c, m, b) for c in "fd" for m, b in ((14, struct.pack("<2f", 1.5, -2.0)), (15, struct.pack(">2f", 1.5, -2.0)), (16, struct.pack("<2d", 1.5, -2.0)), (17, struct.pack(">2d", 1.5, -2.0)), (15, b"abc"), (17, b"abcd"))])
t("characters from another", lambda: [attempt(R, A, c, m, b) for c in "uw" for m, b in ((18, "a\xe9\U0001f600".encode("utf-16-le")), (19, "a\xe9\U0001f600".encode("utf-16-be")), (20, "a\xe9\U0001f600".encode("utf-32-le")), (21, "a\xe9\U0001f600".encode("utf-32-be")), (18, b"ab"), (18, b"abcd"), (18, b"\0\xd8\0\0"), (21, b"\0\x11\0\0\0\0\0\0"), (20, b"abcd"), (21, b"\0\0\0a"))] + [attempt(R, A, "i", 20, b"a\0\0\0b\0\0\0"), attempt(R, A, "w", 8, b"a\0\0\0"), attempt(R, A, "w", 9, b"\0\0\0a")])

print("---- what goes through one")
it = iter(A("i", [1, 2, 3]))
t("the iterator", lambda: (type(it).__name__, iter(it) is it, next(it), [(r[0].__name__, r[1][0].tolist(), r[2]) for r in [it.__reduce__()]], list(it), attempt(next, it), [(r[0].__name__, r[1]) for r in [it.__reduce__()]], attempt(type(it)), attempt(it.__reduce__, 1), attempt(setattr, it, "x", 1), attempt(len, it)))
t("__setstate__", lambda: [[(i.__setstate__(n), list(i))[1] for i in [iter(A("i", [1, 2, 3]))]] for n in (0, 1, 3, 4, 100, -1, -100, True)] + [attempt(iter(A("i")).__setstate__, v) for v in ("a", 1.5, None, 2 ** 63, Index(1))] + [attempt(iter(A("i")).__setstate__), [(list(i), i.__setstate__(0), list(i))[2] for i in [iter(A("i", [1]))]]])
t("while it changes", lambda: [[(out.append(v), x.append(v + 10) if v < 3 else None) and None for v in x] and None or out for x in [A("i", [1, 2])] for out in [[]]] + [[[(out.append(v), x.pop()) and None for v in x] and None or out for x in [A("i", [1, 2, 3, 4])] for out in [[]]]])

print("---- its bytes")
for c in array.typecodes:
    x = A(c, bytes(range(16)))
    m = memoryview(x)
    t("memoryview %s" % c, lambda: (m.format, m.itemsize, m.nbytes, m.ndim, m.shape, m.strides, m.readonly, m.obj is x, len(m), m.contiguous, m.tobytes() == bytes(range(16)), attempt(m.tolist), attempt(m.__getitem__, 0), attempt(list, m), m[1:].nbytes, attempt(lambda: m == m), attempt(lambda: m == x), attempt(lambda: m == memoryview(A(c, bytes(range(16))))), attempt(hash, m), attempt(lambda: m.cast("B").tolist()[:4]), m.hex()[:8], attempt(m.__setitem__, 0, "a" if c in "uw" else 1), x.tobytes()[:8]))
x = A("i", [1, 2, 3])
t("what takes bytes takes it", lambda: (bytes(x), bytearray(x), b"".join([x, x])[:8], struct.unpack_from("<i", x, 4), int.from_bytes(x, "little") & 0xFFFFFFFF, io.BytesIO(x).getvalue()[:4], (lambda f: (f.write(x), f.getvalue()))(io.BytesIO()), b"\2\0\0\0" in bytes(x), bytes(x).find(x), attempt(lambda: b"" + x), attempt(lambda: bytearray() + x), attempt(str, x, "latin-1"), attempt(struct.pack_into, "<i", x, 0, 9), x.tolist(), (lambda f: (f.readinto(x), x.tolist()))(io.BytesIO(bytes(range(12))))))
t("__buffer__", lambda: [(m.format, m.itemsize, m.shape, m.readonly, m.nbytes, m.obj is x) for fl in (0, 1, 4, 8, 0x18, 0x1C, 0x11C, 0x38) for m in [x.__buffer__(fl)]] + [attempt(x.__buffer__), attempt(x.__buffer__, "a"), attempt(x.__buffer__, 2 ** 40), attempt(x.__release_buffer__, 5), attempt(x.__release_buffer__, memoryview(b""))] + [[(x.__release_buffer__(m), attempt(m.tolist))[1] for m in [x.__buffer__(0x11C)]]])
t("written through", lambda: [(m.__setitem__(0, 77), m.__setitem__(slice(1, 3), memoryview(A("i", [88, 99]))), y.tolist(), attempt(m.__setitem__, 0, 2 ** 31), attempt(m.__setitem__, 0, "a")) for y in [A("i", [1, 2, 3])] for m in [memoryview(y)]])
t("empty", lambda: [(m.nbytes, m.shape, m.tolist(), m.tobytes(), m.format) for m in [memoryview(A("i"))]])

print("---- and the rest")
t("weakly referred to", lambda: [(weakref.ref(y)() is y, weakref.proxy(y)[0]) for y in [A("i", [5])]])
t("matched", lambda: [(lambda v: (lambda: "seq" if isinstance(v, collections.abc.Sequence) else "not")())(A("i", [1, 2]))])


def match(v):
    match v:
        case [p, q]:
            return "two", p, q
        case [*rest]:
            return "some", rest
        case _:
            return "none"


t("in a match statement", lambda: [match(v) for v in (A("i", [1, 2]), A("i"), A("i", [1, 2, 3]), A("w", "ab"))])
t("unpacked", lambda: ((lambda p, q: (p, q))(*A("i", [1, 2])), [*A("d", [1.5])], dict(zip(A("w", "ab"), A("i", [1, 2]))), sorted(A("i", [3, 1, 2])), sum(A("i", [1, 2, 3])), max(A("d", [1.5, 2.5])), "".join(A("w", "abc")), list(map(str, A("b", [1, 2]))), tuple(A("h", [1])), set(A("B", [1, 1, 2]))))


class WithInit(A):
    def __init__(self, typecode, data=(), *, name=None):
        self.name = name


class WithNew(A):
    def __new__(cls, data):
        return super().__new__(cls, "i", data)


t("derived from", lambda: (WithInit("i", [1], name="n").name, WithInit("i", [1], name="n").tolist(), attempt(lambda: D("i", [1], name="n")), WithNew([1, 2]).tolist(), type(WithNew([1])).__name__, attempt(lambda: WithInit("i", data=[1])), D("i", [1]) == A("i", [1]), D("i", [1]) + A("i", [2]), isinstance(D("i"), A), D.__mro__[1] is A))
seen = []
sys.addaudithook(lambda e, a: seen.append((e, a)) if e == "array.__new__" else None)
t("what is told of", lambda: (A("i"), A("i", [1]), attempt(A, "x"), attempt(A, "i", "a"), R(A, "i", 9, b"\0\0\0\1"), seen))
warnings.simplefilter("always", DeprecationWarning)
with warnings.catch_warnings(record=True) as caught:
    A("u"), A("u", "a"), A("w"), attempt(A, "u", 5), R(A, "u", 20, b"a\0\0\0")
t("what is warned of", lambda: [(w.category.__name__, str(w.message), w.lineno > 0) for w in caught])
