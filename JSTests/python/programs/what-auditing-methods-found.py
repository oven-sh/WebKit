# What was found to be wrong by calling every method of the built-in classes with what it is meant for and what it is not quite: audits/methods.py.


def t(label, f):
    try:
        r = f()
    except BaseException as e:
        r = type(e).__name__ + ": " + str(e)
    print(label, "=>", r)


def s(value):
    """A set in an order that does not depend on how it is kept."""
    return type(value).__name__ + str(sorted(value, key=repr))


class MyStr(str):
    pass


class MyInt(int):
    pass


class Fs(frozenset):
    pass


class St(set):
    pass


huge = 2 ** 70

# ---- Sets
for kind in (set, frozenset, St, Fs):
    a = kind({1, 2, 3})
    t(kind.__name__, lambda: [s(x) for x in (a.union("ab"), a.union([4], (5,)), a.intersection([1, 2, 9]), a.intersection([1, 2], [2, 3]), a.intersection(""), a.difference([1]), a.difference([1], [2]), a.symmetric_difference([1, 9]), a.union(), a.intersection(),
                                             a.difference(), a.copy(), a)])
for name in ("update", "intersection_update", "difference_update"):
    a = {1, 2, 3}
    t(name + " with something that cannot be gone through after something that can", lambda: getattr(a, name)([1, 5], None))
    print("   ", s(a))

# ---- Where something begins and ends
for text, arguments in (("", ("", 1)), ("", ("", 0)), ("a", ("", 1)), ("a", ("", 2)), ("a", ("", 1, 0)), ("abc", ("", 3)), ("abc", ("", 4)), ("abc", ("", 2, 1)), ("abc", ("b", -2, -1)), ("abc", ("c", huge)), ("abc", ("a", -huge, huge)), ("abc", ("a", None, None)),
                        ("abc", ("a", 1.5)), ("abc", ("a", 0, "x")), ("abc", (None,)), ("abc", (1,)), ("abc", (MyStr("b"),)), ("\U0001d54fy", ("y", 1)), ("\U0001d54fy", ("", 2)), ("\U0001d54fy", ("", 3))):
    for name in ("count", "find", "rfind", "index", "rindex", "startswith", "endswith"):
        t("%r.%s%r" % (text, name, arguments), lambda: getattr(text, name)(*arguments))
t("startswith", lambda: "abc".startswith(MyInt(1)))
t("startswith a tuple", lambda: ("abc".startswith(("x", MyStr("a"))), "abc".endswith(("x", "c"), 0, 2), "abc".startswith((), 5), "abc".startswith(("a", 1))))
t("startswith a tuple with something else in it", lambda: "abc".startswith(("x", 1)))
for sequence in ([1, 2, 3], (1, 2, 3)):
    for arguments in ((2, None), (2, 0, None), (2, 1.5), (2, -huge, huge), (2, huge), (2, 2), (2, 0, 1), (2, MyInt(1))):
        t("%r.index%r" % (sequence, arguments), lambda: sequence.index(*arguments))

# ---- What is too large, and which of what is wrong is said
for name, arguments in (("center", (huge,)), ("ljust", (huge,)), ("rjust", (huge, "x")), ("zfill", (huge,)), ("expandtabs", (huge,)), ("expandtabs", (2 ** 31,)), ("split", (None, huge)), ("split", (1, huge)), ("rsplit", ("", huge)), ("split", ("", 1)), ("replace", ("a", "b", huge)),
                        ("center", (1, 0)), ("ljust", (1, "")), ("rjust", (1, "ab")), ("center", (1, None)), ("center", (5, MyStr("*"))), ("split", (1,)), ("rsplit", (b"a",)), ("strip", (1,)), ("lstrip", (b"",)), ("rstrip", ([],)), ("strip", (None,)), ("partition", (1,)),
                        ("rpartition", (None,)), ("partition", ("",)), ("removeprefix", (1,)), ("removesuffix", (None,)), ("replace", (None, "a")), ("replace", ("a", None)), ("encode", (None,)), ("encode", ("ascii", None)), ("encode", (MyStr("ascii"), MyStr("strict"))),
                        ("encode", (MyStr("nope"),)), ("join", (None,)), ("splitlines", (None,))):
    t("str.%s%r" % (name, arguments), lambda: getattr(" a\tb ", name)(*arguments))
for name, arguments in (("center", (huge,)), ("ljust", (huge,)), ("rjust", (huge, b"x")), ("zfill", (huge,)), ("expandtabs", (huge,)), ("split", (None, huge)), ("split", (1, huge)), ("rsplit", (b"", huge)), ("replace", (b"a", b"b", huge)), ("center", (1, None)), ("center", (1, b"")),
                        ("center", (1, 0)), ("hex", (None, None)), ("hex", ("", None)), ("hex", ((), None)), ("hex", (":", huge)), ("hex", ((),)), ("hex", ((1,),)), ("hex", ("",)), ("hex", (None,)), ("hex", (MyStr(":"),)), ("hex", (bytearray(b":"),)), ("hex", (b":",)), ("hex", ("\xe9",)),
                        ("hex", ("١",)), ("hex", (b"\xff",)), ("hex", (":", 2)), ("hex", (":", -2)), ("hex", (":", 0)), ("hex", ([1],)), ("fromhex", (MyStr("61"),)), ("fromhex", (MyStr("6"),)), ("decode", (None,)), ("decode", (MyStr("ascii"),))):
    t("bytes.%s%r" % (name, arguments), lambda: getattr(b" a\tbc", name)(*arguments))
    t("bytearray.%s%r" % (name, arguments), lambda: getattr(bytearray(b" a\tbc"), name)(*arguments))
for name, arguments in (("insert", (huge, 0)), ("insert", (huge, None)), ("insert", (huge, -1)), ("insert", (100, 1)), ("insert", (-100, 1)), ("pop", (huge,)), ("pop", (None,)), ("resize", (huge,)), ("extend", ("a",)), ("extend", ("",)), ("extend", (MyStr("a"),)), ("extend", (1,)),
                        ("extend", ([1, "a"],)), ("extend", ([256],)), ("extend", (b"xy",)), ("extend", (range(3),))):
    b = bytearray(b"ab")
    t("bytearray.%s%r" % (name, arguments), lambda: getattr(b, name)(*arguments))
    print("   ", b)
for name, arguments in (("insert", (huge, 0)), ("insert", (-huge, 0)), ("insert", (100, 0)), ("insert", (None, 0)), ("pop", (huge,)), ("pop", (None,)), ("pop", (5,)), ("pop", (-1,))):
    for start in ([], [1, 2]):
        items = list(start)
        t("%r.%s%r" % (start, name, arguments), lambda: getattr(items, name)(*arguments))
        print("   ", items)
t("popping from a dict that has nothing in it", lambda: ({}.pop([], 5), {}.pop({}, None)))
t("with nothing to give instead", lambda: {}.pop([]))
t("and from one that has something", lambda: {1: 2}.pop([], 5))
for arguments in ((huge,), (-1, ""), (-1, 0), (-1, "big"), (1, None), (1, MyStr("big")), (0,), (1, "middle")):
    t("to_bytes%r" % (arguments,), lambda: (5).to_bytes(*arguments))
for arguments in ((b"a", 0), (b"a", None), (b"a", MyStr("little")), (MyStr("a"),), ("a",), ([1, 2],), (1,)):
    t("from_bytes%r" % (arguments,), lambda: int.from_bytes(*arguments))
t("__reduce_ex__", lambda: object().__reduce_ex__(huge))


class RaisesIndexError:
    def __index__(self):
        raise IndexError("its own")


for label, f in (("center", lambda x: "a".center(x)), ("insert", lambda x: [].insert(x, 0)), ("expandtabs", lambda x: "a".expandtabs(x)), ("times", lambda x: "a" * x), ("__reduce_ex__", lambda x: object().__reduce_ex__(x)), ("find", lambda x: "a".find("a", x))):
    t("what __index__() raises is what is raised: " + label, lambda: f(RaisesIndexError()))

# ---- What was given is what is given back
m = MyStr("sub")
t("what it is of", lambda: [[type(x).__name__ for x in r] for r in (m.split("longer than it"), m.rsplit("١"), m.split("x"), m.split("u"), m.split(), m.partition("x"), m.rpartition("x"), m.partition(MyStr("u")), "sub".rpartition(MyStr("u")), m.partition("longer than it"))])
t("and what is not", lambda: [type(x).__name__ for x in (m.format_map({}), m.strip(), m.center(1), m.zfill(0), m.replace("x", "y"), m.removeprefix("x"), m.expandtabs(), m.lower(), m.ljust(0))])
t("is it", lambda: (m.split("longer than it")[0] is m, m.partition("x")[0] is m, (sep := MyStr("u")) is m.partition(sep)[1], m.format_map({}) is m))

# ---- One character can become more than one
for text in ("\xdf", "\xdfa", "a\xdf", "İ", "aİ", "ﬁ", "x ﬁ", "ŉ", "ǅ", "Ǆx", "Σ", "aΣ", "aΣb", "aΣ b", "a.Σ", "ΣΣ", "AΣ'", "aΣ'b", "ẞ", "ΐ", "ᾈ", "ᾳ", "ᾷ", "o'neil mcǆ", "\ud800a",
             "\U00010400\U00010428", "\U0001d54f"):
    t(ascii(text), lambda: [ascii(x) for x in (text.capitalize(), text.title(), text.swapcase(), text.upper(), text.lower(), text.casefold())])

# ---- What cannot be encoded
text = "a\udc80\udcffb\ud801\ud800\xe9١"
for encoding in ("ascii", "latin-1", "utf-8", "utf-16", "utf-16-le", "utf-16-be", "utf-32", "utf-32-le", "utf-32-be"):
    for errors in ("strict", "ignore", "replace", "backslashreplace", "xmlcharrefreplace", "namereplace", "surrogateescape", "surrogatepass"):
        t(encoding + " " + errors, lambda: text.encode(encoding, errors))
        t(encoding + " " + errors + ", what surrogateescape is for", lambda: "\udc80\udcff".encode(encoding, errors))
