# The module _struct, which struct is all but all of.
import _struct
import struct
import sys
import weakref


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


def digest(values):
    h = 0
    for v in values:
        for c in repr(v):
            h = (h * 1000003 + ord(c)) % (2 ** 61 - 1)
    return h


inf, nan = float("inf"), float("nan")
S = struct.Struct
print("---- what there is")
t("the module", lambda: (_struct.__name__, _struct.__package__, _struct.__loader__.__name__, _struct.__doc__, sorted(n for n in vars(_struct) if not n.startswith("__"))))
for name in sorted(n for n in vars(_struct) if not n.startswith("__")):
    x = getattr(_struct, name)
    if not isinstance(x, type):
        t(name, lambda: (type(x).__name__, x.__text_signature__, x.__doc__, x.__module__))
E = struct.error
t("error", lambda: (E.__name__, E.__module__, E.__qualname__, [b.__name__ for b in E.__mro__], sorted(vars(E)), E.__doc__, repr(E), repr(E("x")), _struct.error is E))
for c in (S, type(struct.iter_unpack("b", b""))):
    t(c.__name__, lambda: (c.__module__, c.__qualname__, [b.__name__ for b in c.__mro__], sorted(vars(c)), c.__doc__, c.__text_signature__, c.__basicsize__, c.__weakrefoffset__ != 0, c.__dictoffset__, c.__flags__ & 0x7FFF, repr(c), attempt(setattr, c, "x", 1)))
    for name in sorted(vars(c)):
        x = vars(c)[name]
        t("%s.%s" % (c.__name__, name), lambda: (type(x).__name__, getattr(x, "__text_signature__", None), x.__doc__ if not isinstance(x, str) else None))


class Index:
    def __init__(self, v): self.v = v
    def __index__(self): return self.v


class Float:
    def __init__(self, v): self.v = v
    def __float__(self): return self.v


class Complex:
    def __init__(self, v): self.v = v
    def __complex__(self): return self.v


class Raises:
    def __index__(self): raise ValueError("no index")
    def __float__(self): raise ValueError("no float")
    def __complex__(self): raise ValueError("no complex")
    def __bool__(self): raise ValueError("no bool")


class I(int):
    pass


class F(float):
    pass


class B(bytes):
    pass


ORDERS = ("", "@", "=", "<", ">", "!")
print("---- how large")
for order in ORDERS:
    t("calcsize %r" % order, lambda: [(c, attempt(struct.calcsize, order + c)) for c in "xbBc?hHiIlLqQnNefdFDspP"])
    t("lined up %r" % order, lambda: [(f, attempt(struct.calcsize, order + f)) for f in ("bh", "bi", "bl", "bq", "bd", "bf", "be", "bP", "bn", "b?", "bF", "bD", "hb", "ib", "bhb", "bib", "bqb", "bxh", "b0h", "b0i", "b0q", "b0d", "0hb", "bhi", "bhiq", "3bh", "b3h", "sh", "3sh", "ph", "bs", "b0s", "xxh", "cd", "?q", "bbbbi", "b0P", "i0q", "q0i")])
t("counts and spaces", lambda: [(f, attempt(struct.calcsize, f)) for f in ("", " ", "  b  ", "2b", " 2b", "2 b", "b2", "2", "10s", "0s", "0b", "0x", "00b", "007b", "b b", "b\tb\nb\rb\x0bb\x0cb", "1000000b", "<", ">", "@", "=", "!", "<<", "<>b", "b<", "@@", " <b", "9223372036854775807x", "9223372036854775808x", "9223372036854775807b", "9223372036854775807s", "4611686018427387904h", "4611686018427387903h", "9223372036854775807xx", "9223372036854775800x9x", "b9223372036854775807x", "99999999999999999999b", "9223372036854775807sb", "2305843009213693952q", "1152921504606846976D")])
t("what is no format", lambda: [(f, attempt(struct.calcsize, f)) for f in ("z", "a", "bz", "1z", "-1b", "+b", "b,b", "(b)", "\xe9", "b\0", "\0", "b\0b", "<n", "<N", "<P", ">n", "=P", "!N", "@n", "T", "Z", "u", "w", "g", "t", "?!", "b@")])
t("what is not text", lambda: [attempt(struct.calcsize, f) for f in (5, None, 1.5, [], bytearray(b"b"), memoryview(b"b"), b"b", b"<h", b"\xff", b"b\0", B(b"i"), type("T", (str,), {})("q"), "٣b", "\ud800")] + [attempt(struct.calcsize), attempt(struct.calcsize, "b", "b"), attempt(lambda: struct.calcsize(format="b"))])

print("---- whole numbers")
LIMITS = {1: 8, 2: 16, 4: 32, 8: 64}
for order in ORDERS:
    for c in "bBhHiIlLqQnNP":
        size = attempt(struct.calcsize, order + c)
        if not isinstance(size, int):
            continue
        n = LIMITS[size]
        values = (0, 1, -1, 2 ** (n - 1) - 1, 2 ** (n - 1), -2 ** (n - 1), -2 ** (n - 1) - 1, 2 ** n - 1, 2 ** n, -2 ** n, 2 ** 63, 2 ** 64, -2 ** 63 - 1, 10 ** 30, -10 ** 30, 0x0102030405060708 % 2 ** (n - 1))
        t("%s%s" % (order, c), lambda: [attempt(struct.pack, order + c, v) for v in values])
        t("%s%s back" % (order, c), lambda: [struct.unpack(order + c, b)[0] for b in (bytes(size), b"\xff" * size, b"\x80" + bytes(size - 1), bytes(size - 1) + b"\x80", b"\x7f" + b"\xff" * (size - 1), bytes(range(1, size + 1)))] + [type(struct.unpack(order + c, bytes(size))[0]).__name__])
for c in "bBhHiIlLqQnNP":
    t("%s of what is not an int" % c, lambda: [attempt(struct.pack, c, v) for v in (True, False, I(5), Index(5), Index(-1), Index(10 ** 30), Index(-10 ** 30), 1.0, 1.5, "a", None, b"a", [], 1j, Raises(), Index("a"), Index(1.5), F(1.0), Float(1.0))])

print("---- floats")
FLOATS = (0.0, -0.0, 1.0, -1.0, 1.5, 0.1, -0.1, 1 / 3, 65504.0, 65519.0, 65520.0, -65520.0, 1e5, 3.4028234663852886e38, 3.4028235677973366e38, 3.4028235677973362e38, -3.4028235677973366e38, 1e39, 1e308, -1e308, 1.7976931348623157e308, 5e-324, 1e-45, 7e-46, 1.401298464324817e-45, 6e-8, 2.9e-8, 3e-8, 5.96e-8, 1.1754943508222875e-38, inf, -inf)
for order in ORDERS:
    for c in "efd":
        t("%s%s" % (order, c), lambda: [attempt(struct.pack, order + c, v) for v in FLOATS])
        t("%s%s back" % (order, c), lambda: [struct.unpack(order + c, struct.pack(order + c, v))[0] for v in FLOATS if isinstance(attempt(struct.pack, order + c, v), bytes)])
    t("%s NaNs" % order, lambda: [(c, struct.pack(order + c, nan), struct.pack(order + c, -nan), [x != x for x in struct.unpack(order + c, struct.pack(order + c, nan))]) for c in "efd"])
for c in "efd":
    t("%s of what is not a float" % c, lambda: [attempt(struct.pack, c, v) for v in (1, True, -2, I(3), F(1.5), Float(1.5), Index(2), 10 ** 30, 10 ** 39, 10 ** 400, -10 ** 400, "a", None, b"a", [], 1j, Raises(), Float("a"), Index("a"))])
state = 7


def bits(n):
    global state
    out = 0
    for _ in range((n + 30) // 31):
        state = (state * 1103515245 + 12345) % 2 ** 31
        out = out << 31 | state
    return out >> ((n + 30) // 31 * 31 - n)


def some_float(low, high):
    return (1 - 2 * bits(1)) * (1 + bits(52) / 2 ** 52) * 2.0 ** (low + bits(20) % (high - low + 1))


t("at random", lambda: [digest(attempt(struct.pack, o + c, some_float(low, high)) for _ in range(3000)) for o in "<>" for c, low, high in (("e", -30, 17), ("f", -155, 130), ("d", -1074, 1023))])
t("every two bytes", lambda: [digest(x for b in range(65536) for x in struct.unpack(o + "e", b.to_bytes(2, "big")) if x == x) for o in "<>"])
t("four bytes at random", lambda: [digest(x for _ in range(20000) for x in struct.unpack(o + "f", bits(32).to_bytes(4, "big")) if x == x) for o in "<>@"])
t("eight bytes at random", lambda: [digest(x for _ in range(20000) for x in struct.unpack(o + "d", bits(64).to_bytes(8, "big")) if x == x) for o in "<>@"])
# Bit 50 of a double, which is not kept here, is left out: that is 0x0100 of two bytes and 0x00200000 of four.
t("what tells one NaN from another", lambda: [[struct.pack(o + c, struct.unpack(o + c, b)[0]) == b for b in patterns] for o in "<>" for c, patterns in (("d", [(p).to_bytes(8, "little" if o == "<" else "big") for p in (0x7FF8000000000001, 0xFFF8000000000001, 0x7FF0000000000001, 0x7FFBFFFFFFFFFFFF, 0xFFF3FFFFFFFFFFFF)]), ("f", [(p).to_bytes(4, "little" if o == "<" else "big") for p in (0x7FC00001, 0xFFC00001, 0x7F800001, 0x7FDFFFFF, 0xFF9FFFFF)]), ("e", [(p).to_bytes(2, "little" if o == "<" else "big") for p in (0x7E01, 0xFE01, 0x7C01, 0x7EFF, 0xFCFF)]))])
t("natively, a signalling NaN of four bytes does not stay one", lambda: [struct.pack("<d", struct.unpack(o + "f", (0x7F800001).to_bytes(4, "little"))[0]) for o in ("@", "", "<", "=")])

print("---- complex numbers")
COMPLEXES = (0j, 1 + 2j, -1.5 - 2.5j, complex(0.0, -0.0), complex(-0.0, 0.0), complex(inf, -inf), 1e39 + 0j, 1e39j, complex(1e308, -1e308), complex(0.1, 0.2), complex(5e-324, 1e-46))
for order in ORDERS:
    for c in "FD":
        t("%s%s" % (order, c), lambda: [attempt(struct.pack, order + c, v) for v in COMPLEXES])
        t("%s%s back" % (order, c), lambda: [struct.unpack(order + c, struct.pack(order + c, v))[0] for v in COMPLEXES if isinstance(attempt(struct.pack, order + c, v), bytes)] + [type(struct.unpack(order + c, bytes(struct.calcsize(order + c)))[0]).__name__])
for c in ("F", "D", ">F", ">D"):
    t("%s of what is not complex" % c, lambda: [attempt(struct.pack, c, v) for v in (1, 1.5, True, I(2), F(2.5), Float(1.5), Index(2), Complex(1 + 2j), type("C", (complex,), {})(1, 2), 10 ** 400, "a", None, b"a", [], Raises(), Complex("a"), Complex(1.5))])

print("---- bool, char, and bytes")
for order in ("", "<", ">"):
    t("%s?" % order, lambda: [attempt(struct.pack, order + "?", v) for v in (True, False, 0, 1, 2, -1, "", "a", None, [], [0], 0.0, 1.5, Raises(), 10 ** 30)] + [struct.unpack(order + "?", bytes([b]))[0] for b in (0, 1, 2, 128, 255)])
    t("%sc" % order, lambda: [attempt(struct.pack, order + "c", v) for v in (b"a", b"\xff", b"\0", b"", b"ab", B(b"z"), bytearray(b"a"), memoryview(b"a"), "a", 97, None)] + [struct.unpack(order + "3c", b"abc"), type(struct.unpack(order + "c", b"a")[0]).__name__])
    t("%ss" % order, lambda: [(f, attempt(struct.pack, order + f, v)) for f in ("s", "0s", "1s", "3s", "5s", "10s") for v in (b"", b"a", b"abc", b"abcdefgh", bytearray(b"xy"), B(b"pq"))] + [attempt(struct.pack, order + "3s", v) for v in ("abc", 5, None, memoryview(b"abc"), [97])])
    t("%sp" % order, lambda: [(f, attempt(struct.pack, order + f, v)) for f in ("p", "0p", "1p", "2p", "3p", "5p", "10p") for v in (b"", b"a", b"abc", b"abcdefgh", bytearray(b"xy"), B(b"pq"))] + [attempt(struct.pack, order + "3p", v) for v in ("abc", 5, None, memoryview(b"abc"))])
    t("%ss and p back" % order, lambda: [(f, struct.unpack(order + f, b)) for f, b in (("0s", b""), ("s", b"a"), ("3s", b"abc"), ("3s", b"\0\0\0"), ("0p", b""), ("p", b"\5"), ("1p", b"\0"), ("3p", b"\0ab"), ("3p", b"\1ab"), ("3p", b"\2ab"), ("3p", b"\3ab"), ("3p", b"\xffab"), ("2s3s", b"abcde"), ("0s0p0s", b""), ("3p2s", b"\1abcd"))])
t("a long p", lambda: [(len(x), x[0], struct.unpack(f, x)[0] == b"z" * min(n, 255)) for f, n in (("255p", 254), ("256p", 255), ("257p", 256), ("300p", 299), ("1000p", 999)) for x in [struct.pack(f, b"z" * 2000)]])
t("0p in the middle", lambda: (struct.pack("b0pb", 1, b"abc", 2), struct.pack("<0pb", b"abc", 7), struct.pack("0p", b"abc"), struct.unpack("b0pb", b"\1\2")))
t("x", lambda: (struct.pack("x"), struct.pack("3x"), struct.pack("bxb", 1, 2), struct.pack("0x"), struct.unpack("3x", b"abc"), struct.unpack("bxb", b"\1\xff\2"), attempt(struct.pack, "x", 1)))

print("---- how many")
t("pack", lambda: [attempt(struct.pack, *a) for a in ((), ("b",), ("b", 1, 2), ("", 1), ("2b", 1), ("2b", 1, 2, 3), ("0b", 1), ("0b",), ("3s",), ("3s", b"a", b"b"), ("x", 1), ("bhi", 1, 2), (5,), (None, 1), ("z", 1))] + [attempt(lambda: struct.pack("b", x=1)), attempt(lambda: struct.pack(format="b"))])
t("unpack", lambda: [attempt(struct.unpack, *a) for a in ((), ("b",), ("b", b""), ("b", b"ab"), ("", b""), ("", b"a"), ("h", b"a"), ("h", b"abc"), ("b", "a"), ("b", 5), ("b", None), ("b", [1]), ("b", b"a", 1), ("2b", bytearray(b"ab")), ("2b", memoryview(b"ab")), ("2b", memoryview(b"abcd")[::2]), ("h", memoryview(b"ab").cast("h")), ("0b", b""), ("0s", b""), ("z", b""))] + [attempt(lambda: struct.unpack("b", buffer=b"a"))])
t("many at once", lambda: (struct.pack("<3h2b", 1, 2, 3, 4, 5), struct.unpack("<3h2b", bytes(range(8))), len(struct.unpack("1000b", bytes(1000))), digest(struct.unpack("<500H", bytes(i % 256 for i in range(1000)))), struct.pack("<bHiq?fd3s3p", 1, 2, 3, 4, True, 1.5, 2.5, b"ab", b"cd")))

print("---- unpack_from and pack_into")
DATA = bytes(range(1, 11))
t("unpack_from", lambda: [attempt(struct.unpack_from, "<H", DATA, o) for o in (0, 1, 8, 9, 10, 11, 100, -1, -2, -3, -10, -11, -100, 2 ** 63 - 1, -2 ** 63, True, Index(2))])
t("unpack_from of nothing", lambda: [attempt(struct.unpack_from, "", DATA, o) for o in (0, 10, 11, -1, -10, -11)] + [attempt(struct.unpack_from, "", b"", o) for o in (0, 1, -1)])
t("unpack_from called otherwise", lambda: [attempt(struct.unpack_from, *a, **k) for a, k in (((), {}), (("b",), {}), (("<H", DATA), {}), (("<H",), {"buffer": DATA}), (("<H",), {"buffer": DATA, "offset": 2}), (("<H", DATA), {"offset": 2}), (("<H", DATA, 2, 3), {}), (("<H", DATA), {"other": 1}), ((), {"format": "<H", "buffer": DATA}), (("<H", DATA, "a"), {}), (("<H", DATA, 1.5), {}), (("<H", DATA, None), {}), (("<H", DATA, 2 ** 63), {}), (("<H", DATA, -2 ** 63 - 1), {}), (("<H", "ab"), {}), (("<H", 5), {}), (("<H", DATA, Raises()), {}))])


def into(fmt, size, offset, *values):
    b = bytearray(b"." * size)
    r = attempt(struct.pack_into, fmt, b, offset, *values)
    return r, bytes(b)


t("pack_into", lambda: [into("<H", 6, o, 0x4142) for o in (0, 1, 4, 5, 6, 7, 100, -1, -2, -3, -6, -7, -100, True, Index(2))])
t("pack_into of nothing", lambda: [into("", 3, o) for o in (0, 3, 4, -1, -3, -4)] + [into("", 0, o) for o in (0, 1, -1)])
t("pack_into fills in between", lambda: (into("<bxh", 6, 1, 1, 2), into("3x", 5, 1), into("5s", 7, 1, b"ab"), into("5p", 7, 1, b"ab"), into("@bi", 10, 1, 1, 2)))
t("pack_into called otherwise", lambda: [attempt(struct.pack_into, *a) for a in ((), ("b",), ("b", bytearray(1)), ("b", bytearray(1), 0), ("b", bytearray(1), 0, 1, 2), ("", bytearray(1)), ("", bytearray(1), 0, 1), ("b", b"a", 0, 1), ("b", "a", 0, 1), ("b", 5, 0, 1), ("b", None, 0, 1), ("b", memoryview(b"a"), 0, 1), ("b", bytearray(1), "a", 1), ("b", bytearray(1), 1.5, 1), ("b", bytearray(1), None, 1), ("b", bytearray(1), 2 ** 63, 1), ("b", bytearray(1), -2 ** 63 - 1, 1), ("b", bytearray(1), Raises(), 1), ("2b", memoryview(bytearray(4))[::2], 0, 1, 2))] + [attempt(lambda: struct.pack_into("b", bytearray(1), 0, x=1))])
t("into a memoryview", lambda: [(struct.pack_into("<H", memoryview(b)[2:], 1, 0x4142), bytes(b)) for b in [bytearray(b"......")]])
t("what has been got through when something goes wrong is written", lambda: (into("<bbb", 5, 1, 1, 2, "x"), into("<bbb", 5, 1, 1, 300, 3), into("<b3sb", 7, 1, 1, "x", 3), into("<hd", 12, 1, 0x4142, "x")))

print("---- iter_unpack")
t("iter_unpack", lambda: (list(struct.iter_unpack("<H", DATA)), list(struct.iter_unpack("<Hb", bytes(6))), list(struct.iter_unpack("b", b"")), list(struct.iter_unpack("2s", bytearray(b"abcd"))), list(struct.iter_unpack("b", memoryview(b"ab")))))
t("iter_unpack of what will not do", lambda: [attempt(struct.iter_unpack, *a) for a in ((), ("b",), ("", b""), ("0s", b""), ("0b", b"a"), ("<H", b"abc"), ("<H", b"a"), ("<H", "ab"), ("<H", 5), ("<H", None), ("b", b"a", 1), ("z", b""), ("b", memoryview(b"abcd")[::2]))])
it = struct.iter_unpack("<H", DATA)
t("what it is", lambda: (type(it).__name__, iter(it) is it, it.__length_hint__(), next(it), it.__length_hint__(), list(it), it.__length_hint__(), attempt(next, it), attempt(next, it), attempt(it.__length_hint__, 1), attempt(type(it)), attempt(setattr, it, "x", 1), repr(it).split(" at ")[0]))

print("---- Struct")
s = S("<hI3s")
t("as it is", lambda: (s.format, s.size, repr(s), type(s.format).__name__, s.pack(1, 2, b"abc"), s.unpack(bytes(range(9))), s.unpack_from(bytes(range(12)), 2), s.unpack_from(buffer=bytes(range(12)), offset=3), list(s.iter_unpack(bytes(18))), s.__sizeof__() - object.__sizeof__(s) >= 0, attempt(setattr, s, "format", "b"), attempt(setattr, s, "size", 1), attempt(setattr, s, "x", 1), attempt(delattr, s, "format")))
t("made of", lambda: [(x.format, x.size, repr(x)) for x in (S("b"), S(b"b"), S(""), S(b""), S(" < h "[1:]), S(format="i"), S(B(b"q")), S("2h 3b"))])
t("what it cannot be made of", lambda: [attempt(S, *a, **k) for a, k in (((), {}), ((5,), {}), ((None,), {}), ((bytearray(b"b"),), {}), (("b", "b"), {}), (("z",), {}), (("\xe9",), {}), ((b"\xff",), {}), (("b\0",), {}), ((), {"fmt": "b"}), (("b",), {"format": "b"}), (("2",), {}))])
t("__sizeof__", lambda: [S(f).__sizeof__() - S("").__sizeof__() for f in ("", "b", "bb", "2b", "bh", "3s", "x", "0b", "0s", "bhilq", "b b b")] + [attempt(S("b").__sizeof__, 1)])
t("made again", lambda: [(x.__init__("<q"), x.format, x.size, attempt(x.__init__, "z"), x.format, x.size, attempt(x.__init__, 5), x.format, attempt(x.__init__), x.pack(1)) for x in [S("b")]])
raw = S.__new__(S)
t("not yet made", lambda: (raw.size, attempt(getattr, raw, "format"), attempt(repr, raw), attempt(raw.pack), attempt(raw.pack, 1), attempt(raw.unpack, b""), attempt(raw.unpack, 5), attempt(raw.unpack_from, b""), attempt(raw.unpack_from, b"", "a"), attempt(raw.iter_unpack, b""), attempt(raw.pack_into, bytearray(1), 0), attempt(raw.pack_into), attempt(raw.__sizeof__), type(S.__new__(S, "anything", at="all")).__name__, attempt(S.__new__), attempt(S.__new__, int)))
t("how its methods are called", lambda: [attempt(f, *a, **k) for f, a, k in ((s.pack, (), {"x": 1}), (s.pack_into, (), {"x": 1}), (s.unpack, (), {}), (s.unpack, (b"", b""), {}), (s.unpack, (), {"buffer": b""}), (s.unpack_from, (), {}), (s.unpack_from, (b"", 0, 0), {}), (s.unpack_from, (b"",), {"other": 1}), (s.iter_unpack, (), {}), (s.iter_unpack, (), {"buffer": b""}), (s.pack, (1, 2), {}), (s.pack_into, (), {}), (s.pack_into, (bytearray(9),), {}), (s.pack_into, (bytearray(9), 0), {}), (s.pack_into, (bytearray(9), 0, 1, 2, b"a", 4), {}))])


class D(S):
    def __init__(self, fmt, extra=None):
        super().__init__(fmt)
        self.extra = extra


t("derived from", lambda: [(d.format, d.size, d.extra, repr(d), d.pack(5), d.__dict__, type(d).__mro__[1] is S) for d in [D("<h", extra=7)]])
t("weakly referred to", lambda: [(weakref.ref(x)() is x, weakref.proxy(x).size) for x in [S("i")]])
t("compared", lambda: (S("b") == S("b"), S("b") != S("b"), s == s, attempt(hash, s) is not None, attempt(lambda: s < s), bool(S(""))))

print("---- what is kept")
struct._clearcache()
t("_clearcache", lambda: (struct._clearcache(), attempt(struct._clearcache, 1)))
made = []


class Counts(str):
    def __hash__(self):
        made.append("hash")
        return str.__hash__(self)


t("a format is looked up by what it hashes to", lambda: (struct.calcsize(Counts("<hh")), struct.calcsize(Counts("<hh")), len(made)))
t("more than there is room for", lambda: [struct.calcsize("%db" % n) for n in range(1, 250)] == list(range(1, 250)))
t("what cannot be hashed", lambda: [attempt(struct.calcsize, x) for x in ([], {}, bytearray(b"b"))])
t("bytes and str are the same", lambda: (struct.pack(b"<h", 1), struct.pack("<h", 1), struct.calcsize(b"<3h"), attempt(struct.pack, b"\xff", 1), attempt(struct.pack, b"<\x80", 1)))

print("---- what is run meanwhile")
order_of = []


class Notes:
    def __init__(self, name, v): self.name, self.v = name, v
    def __index__(self):
        order_of.append(self.name)
        return self.v
    def __float__(self):
        order_of.append(self.name)
        return float(self.v)
    def __bool__(self):
        order_of.append(self.name)
        return bool(self.v)


t("in order, and no further than what goes wrong", lambda: (struct.pack("<bd?h", Notes("a", 1), Notes("b", 2), Notes("c", 3), Notes("d", 4)), order_of[:], order_of.clear(), attempt(struct.pack, "<bbb", Notes("a", 1), Notes("b", 300), Notes("c", 3)), order_of[:], order_of.clear()))
t("nothing is asked if there are the wrong number", lambda: (attempt(struct.pack, "<bb", Notes("a", 1)), order_of[:]))
