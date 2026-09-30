# A memoryview of more dimensions than one, and of none.


def t(label, f):
    try:
        r = f()
    except BaseException as e:
        r = type(e).__name__ + ": " + str(e)
    print(label, "=>", r)


def describe(m):
    return (m.format, m.itemsize, m.ndim, m.shape, m.strides, m.suboffsets, m.nbytes, m.readonly, m.c_contiguous, m.f_contiguous, m.contiguous)


class MyStr(str):
    pass


class MyInt(int):
    pass


class Index:
    def __init__(self, value):
        self.value = value

    def __repr__(self):
        return "Index(%r)" % self.value

    def __index__(self):
        return self.value


data = bytes(range(24))
flat = memoryview(data)

# ---- What can be cast to what
for source_name, source in (("flat", flat), ("empty", memoryview(b"")), ("one", memoryview(b"x")), ("2x12", flat.cast("B", (2, 12))), ("none", memoryview(b"x").cast("B", ())), ("every other", flat[::2]), ("backwards", flat[::-1]), ("one of every other", flat[:1:2]),
                            ("none of every other", flat[:0:2]), ("ints", flat.cast("i")), ("2x3 ints", flat.cast("i", (2, 3)))):
    print("----", source_name, describe(source))
    for arguments in (("B",), ("b",), ("c",), ("@B",), ("i",), ("@i",), ("h",), ("q",), ("d",), ("e",), ("?",), ("P",), ("n",), ("N",), ("l",), ("L",), ("Q",), ("H",), ("I",), ("f",), ("",), ("@",), ("ii",), ("x",), ("=i",), ("<i",), ("@@B",), ("\xe9",), ("١",), (MyStr("B"),), (1,), (None,), (b"B",),
                      ("B", None), ("B", 24), ("B", "ab"), ("B", ()), ("B", []), ("B", (24,)), ("B", [24]), ("B", (2, 12)), ("B", [4, 6]), ("B", (2, 3, 4)), ("B", (1, 24)), ("B", (24, 1)), ("B", (1, 1, 24, 1)), ("B", (2, 2, 2, 3)), ("B", (5, 5)), ("B", (0, 24)), ("B", (-1, -24)), ("B", (2.0, 12)),
                      ("B", ("2", 12)), ("B", (True, 24)), ("B", (MyInt(2), 12)), ("B", (Index(2), 12)), ("B", (2 ** 70, 1)), ("B", (2 ** 40, 2 ** 40)), ("B", (2 ** 62, 2)), ("B", (1,) * 64), ("B", (1,) * 65), ("B", (1,)), ("i", (2, 3)), ("i", (6,)), ("i", (3, 2)), ("i", (24,)), ("h", (3, 4)),
                      ("d", (3,)), ("d", (1, 3)), ("q", ()), ("B", range(2)), ("B", {2: 12})):
        t("cast%r" % (arguments if len(repr(arguments)) < 60 else (arguments[0], "%d ones" % len(arguments[1])),), lambda: describe(source.cast(*arguments)))
t("by keyword", lambda: describe(flat.cast(format="B", shape=[2, 12])))
t("only the shape", lambda: flat.cast(shape=[2, 12]))
t("too many", lambda: flat.cast("B", (24,), 1))
t("none", lambda: flat.cast())

# ---- What is in it
views = {
    "flat": flat, "2x12": flat.cast("B", (2, 12)), "4x6": flat.cast("B", (4, 6)), "2x3x4": flat.cast("B", (2, 3, 4)), "1x24": flat.cast("B", (1, 24)), "24x1": flat.cast("B", (24, 1)), "2x3 ints": flat.cast("i", (2, 3)), "3 doubles": flat.cast("d", (3,)), "none": memoryview(b"x").cast("B", ()),
    "an int": memoryview(b"\x01\x02\x00\x00").cast("i", ()), "chars": flat.cast("c", (4, 6)), "bools": memoryview(b"\x00\x01\x02\x00").cast("?", (2, 2)), "every other row": flat.cast("B", (4, 6))[::2], "rows backwards": flat.cast("B", (4, 6))[::-1], "one row": flat.cast("B", (4, 6))[1:2],
    "no rows": flat.cast("B", (4, 6))[2:2], "last rows": flat.cast("B", (2, 3, 4))[1:], "every other of every other": flat.cast("B", (12, 2))[::2][::2], "no rows of every other": flat.cast("B", (4, 6))[0:0:2],
}
for name, m in views.items():
    print("----", name, describe(m))
    t("len", lambda: len(m))
    t("tolist", lambda: m.tolist())
    for order in (None, "C", "F", "A"):
        t("tobytes(%r)" % order, lambda: m.tobytes(order))
    t("tobytes", lambda: m.tobytes())
    t("bytes", lambda: bytes(m))
    t("bytearray", lambda: bytearray(m))
    t("hex", lambda: m.hex())
    t("hex with a separator", lambda: m.hex(":", 4))
    t("hash", lambda: hash(m) == hash(m.tobytes()))
    t("iter", lambda: list(m))
    t("reversed", lambda: list(reversed(m)))
    t("in", lambda: 3 in m)
    t("count", lambda: m.count(3))
    t("index", lambda: m.index(3))
    t("index from", lambda: m.index(3, 1, 2))
    t("another of it", lambda: describe(memoryview(m)))
    t("toreadonly", lambda: describe(m.toreadonly()))
    t("back to a row", lambda: describe(m.cast("B")))
    t("back to a row of ints", lambda: describe(m.cast("i")))
    t("join", lambda: b"-".join([m, m]))
    t("add", lambda: b"" + m)
    t("equal to itself", lambda: (m == m, m != m, m == memoryview(m), m == m.tobytes(), m.tobytes() == m, m == flat, m == m.tolist()))
    for key in (0, 1, -1, 100, -100, True, Index(1), 2 ** 70, (), (0,), (1,), (0, 0), (1, 2), (-1, -1), (0, 100), (100, 0), (0, -100), (0, 0, 0), (1, 2, 3), (0, 0, 0, 0), (Index(1), True), (0, 2 ** 70), (0, 1.5), (0, None), (0, "a"), ..., None, "a", 1.5, slice(None), slice(1, None), slice(None, None, 2),
                slice(None, None, -1), slice(5, 1), slice(0, 0), slice(None, None, 0), slice("a"), (slice(None),), (slice(None), slice(None)), (slice(None), 0), (0, slice(None)), (..., 0), (slice(None), slice(None), slice(None)), [0], [0, 0]):
        t("[%r]" % (key,), lambda: (lambda r: describe(r) + (r.tolist(),) if isinstance(r, memoryview) else r)(m[key]))
t("what a view of none gives for ... is itself", lambda: (lambda m: m[...] is m)(views["none"]))

# ---- Comparing
pairs = [("2x12", "4x6"), ("2x12", "flat"), ("1x24", "24x1"), ("1x24", "flat"), ("none", "none"), ("none", "an int"), ("every other row", "every other row"), ("no rows", "no rows"), ("chars", "4x6"), ("2x3 ints", "2x3 ints")]
for a, b in pairs:
    t("%s == %s" % (a, b), lambda: (views[a] == views[b], views[a] != views[b]))
t("the same numbers in another format", lambda: (memoryview(bytes([1, 0, 2, 0])).cast("h", (2, 1)) == memoryview(bytes([1, 2])).cast("B", (2, 1)), memoryview(bytes([1, 0, 2, 0])).cast("h", (1, 2)) == memoryview(bytes([1, 2])).cast("B", (2, 1)),
                                                 memoryview(bytes(8)).cast("d", ()) == memoryview(bytes(1)).cast("B", ()), memoryview(bytes(8)).cast("d", (1,)) == memoryview(bytes(1)).cast("B", ())))
t("rows taken another way", lambda: (flat.cast("B", (4, 6))[::2] == memoryview(bytes(list(range(6)) + list(range(12, 18)))).cast("B", (2, 6)), flat.cast("B", (4, 6))[::-1][::-1] == flat.cast("B", (4, 6))))
t("nothing along a dimension, and then it makes no difference", lambda: (flat.cast("B", (4, 6))[0:0] == flat.cast("B", (2, 12))[0:0], flat.cast("B", (4, 6))[0:0] == flat.cast("B", (2, 3, 4))[0:0], flat.cast("B", (4, 6))[0:0] == flat[0:0]))
nan = memoryview(bytes(6) + b"\xf8\x7f").cast("d", ())
t("a NaN", lambda: (nan[()] != nan[()], nan == nan, nan != nan))

# ---- Writing
for shape, key, value in (((2, 3), (0, 0), 9), ((2, 3), (1, 2), 9), ((2, 3), (-1, -1), 9), ((2, 3), (2, 0), 9), ((2, 3), (0, 3), 9), ((2, 3), (0,), 9), ((2, 3), (), 9), ((2, 3), (0, 0, 0), 9), ((2, 3), 0, 9), ((2, 3), 0, b"abc"), ((2, 3), slice(None), b"abcdef"), ((2, 3), slice(0, 1), b"abc"),
                          ((2, 3), (slice(None), slice(None)), b"abcdef"), ((2, 3), (slice(None), 0), 9), ((2, 3), ..., 9), ((2, 3), None, 9), ((2, 3), (0, 0), 256), ((2, 3), (0, 0), -1), ((2, 3), (0, 0), "a"), ((2, 3), (0, 0), None), ((2, 3), (0, 0), 1.5), ((2, 3), (0, 2 ** 70), 9), ((2, 3), (0, 1.5), 9),
                          ((6,), (0,), 9), ((6,), (5,), 9), ((6,), (6,), 9), ((6,), (0, 0), 9), ((6,), (), 9), ((1, 6), (0, 5), 9), ((6, 1), (5, 0), 9), ((1, 2, 3), (0, 1, 2), 9)):
    b = bytearray(b"\x00" * 6)
    m = memoryview(b).cast("B", shape)
    t("%r[%r] = %r" % (shape, key, value), lambda: m.__setitem__(key, value))
    print("   ", b)
for key, value in (((), 9), (..., 9), (0, 9), ((0,), 9), (slice(None), b"a"), (None, 9), ((), 256), ((), "a"), (..., b"a")):
    b = bytearray(b"\x00")
    m = memoryview(b).cast("B", ())
    t("a view of none [%r] = %r" % (key, value), lambda: m.__setitem__(key, value))
    print("   ", b)
b = bytearray(8)
m = memoryview(b).cast("i", (2, 1))
t("ints", lambda: (m.__setitem__((1, 0), -2), m.__setitem__((0, 0), 2 ** 31 - 1), m.tolist(), bytes(b)))
t("an int that is too large", lambda: m.__setitem__((0, 0), 2 ** 31))
t("rows backwards", lambda: (lambda b: (memoryview(b).cast("B", (3, 2))[::-1].__setitem__((0, 1), 9), b)[1])(bytearray(6)))
t("deleting", lambda: memoryview(bytearray(6)).cast("B", (2, 3)).__delitem__((0, 0)))
t("what cannot be written to", lambda: flat.cast("B", (4, 6)).__setitem__((0, 0), 1))
for source in (memoryview(b"abcdef").cast("B", (2, 3)), memoryview(b"abcdef").cast("B", (6,)), memoryview(b"abcdef").cast("B", (1, 6)), memoryview(b"a").cast("B", ()), memoryview(b"abcdef").cast("c"), memoryview(b"abcdef").cast("b"), memoryview(b"abcdef").cast("@B"), memoryview(b"abcdefabcdef")[::2],
               memoryview(b"abcdef")[::-1], memoryview(bytes(24)).cast("i")):
    b = bytearray(6)
    t("a row = %r" % (describe(source)[:4],), lambda: memoryview(b).__setitem__(slice(None), source))
    print("   ", b)

# ---- Hashing
t("of what can be changed", lambda: hash(memoryview(bytearray(b"ab"))))
t("read only, of what can be changed", lambda: hash(memoryview(bytearray(b"ab")).toreadonly()))
for format in ("B", "b", "c", "@B", "h", "i", "?", "d"):
    t("of " + format, lambda: hash(memoryview(bytes(8)).cast(format)) == hash(bytes(8)))
t("every other", lambda: hash(flat[::2]) == hash(data[::2]))
m = memoryview(b"abc")
h = hash(m)
m.release()
t("it is kept when the view has been released", lambda: hash(m) == h)
m = memoryview(b"abc")
m.release()
t("and is not to be had if it was not asked for before", lambda: hash(m))

# ---- Once released
m = flat.cast("B", (4, 6))
m.release()
for label, f in (("cast", lambda: m.cast("B")), ("cast to what is not a format", lambda: m.cast(1)), ("tobytes", lambda: m.tobytes()), ("tobytes with what is not an order", lambda: m.tobytes(1)), ("tobytes with an order that there is not", lambda: m.tobytes("X")), ("tolist", lambda: m.tolist()),
                 ("len", lambda: len(m)), ("[]", lambda: m[0, 0]), ("shape", lambda: m.shape), ("ndim", lambda: m.ndim), ("strides", lambda: m.strides), ("c_contiguous", lambda: m.c_contiguous), ("iter", lambda: iter(m)), ("==", lambda: (m == m, m == flat, flat == m)), ("index", lambda: m.index(1)),
                 ("index from what is not a number", lambda: m.index(1, "a")), ("count", lambda: m.count(1)), ("hex", lambda: m.hex()), ("toreadonly", lambda: m.toreadonly()), ("__buffer__", lambda: m.__buffer__(0))):
    t("released: " + label, f)

# ---- tobytes()
for order in ("", "c", "CC", "C\0", "\0", 1, b"C", MyStr("F"), [], "\xe9"):
    t("tobytes(%r)" % (order,), lambda: flat.cast("B", (2, 12)).tobytes(order))
t("by keyword", lambda: flat.cast("B", (2, 12)).tobytes(order="F"))
t("half a character", lambda: flat.tobytes("\ud800"))
t("half a character, of one that is released", lambda: [m := memoryview(b"ab"), m.release(), m.tobytes("\ud800")])

# ---- __exit__(), which is given a tuple in CPython, and is called what it is called according to how it was come by
t("__exit__(a=1)", lambda: memoryview(b"ab").__exit__(a=1))
t("got first, and then called", lambda: [f := memoryview(b"ab").__exit__, f(a=1)])
t("with whatever it is given", lambda: (memoryview(b"ab").__exit__(), memoryview(b"ab").__exit__(1), memoryview(b"ab").__exit__(1, 2, 3, 4)))

# ---- What is asked for with __buffer__()
SIMPLE, WRITABLE, FORMAT, ND, STRIDES, C, F, ANY, INDIRECT = 0, 1, 4, 8, 0x18, 0x38, 0x58, 0x98, 0x118
for name in ("flat", "2x12", "2x3 ints", "none", "an int", "every other row", "rows backwards", "one row", "24x1", "no rows"):
    for label, flags in (("SIMPLE", SIMPLE), ("WRITABLE", WRITABLE), ("FORMAT", FORMAT), ("ND", ND), ("ND|FORMAT", ND | FORMAT), ("STRIDES", STRIDES), ("STRIDES|FORMAT", STRIDES | FORMAT), ("C", C), ("F", F), ("ANY", ANY), ("INDIRECT", INDIRECT), ("FULL_RO", INDIRECT | FORMAT), ("FULL", INDIRECT | FORMAT | WRITABLE)):
        t("%s.__buffer__(%s)" % (name, label), lambda: (lambda r: describe(r) + (r.tolist(),))(views[name].__buffer__(flags)))
for flags in (2 ** 31, -2 ** 31 - 1, 2 ** 70, "a", None, 1.5):
    t("__buffer__(%r)" % (flags,), lambda: flat.__buffer__(flags))
t("of every other, as bytes", lambda: (bytes(flat[::2]), bytearray(flat[::-2]), b"".join([flat[::12]])))
t("what wants them one after another", lambda: int.from_bytes(flat[::12], "big"))
t("and what will not have them otherwise", lambda: b"ab".startswith(flat[::12]))

# ---- What a class of a program's gives


class Exporter:
    def __init__(self, view):
        self.view = view

    def __buffer__(self, flags):
        return self.view


for name in ("2x12", "none", "every other row", "2x3 ints"):
    t("from a class: " + name, lambda: (lambda r: describe(r) + (r.tolist(), r == views[name]))(memoryview(Exporter(views[name]))))
t("hash of what a class gives", lambda: hash(memoryview(Exporter(flat))) == hash(data))


class Unhashable(Exporter):
    __hash__ = None


t("of a class that has none", lambda: hash(memoryview(Unhashable(flat))) == hash(data))
