# What comes of the methods of the built-in classes, given what they are meant for and what they are not quite: each method of several instances of each class, and of an instance of a class derived from it, with no
# argument, with each of some ninety, with each two of some forty, with each three of a few, and by keyword. What is looked at is what is returned and its class, or the exception and what it says, and what has
# become of the instance. The methods that the operators and the built-in functions are made of are called as well, with fewer things, since what matters there is mostly whether it is NotImplemented.
#
# There is a line for each method of each instance, with how many things were tried and a number that stands for all that came of them. To see them all:
#
#     methods.py [--] <class> <method> <which instance>
#     methods.py [--] everything [<class>]

import _warnings
import sys

_warnings.filters.insert(0, ("ignore", None, Warning, None, 0))

inf = float("inf")
nan = float("nan")


class MyInt(int):
    pass


class MyStr(str):
    pass


class MyFloat(float):
    pass


class MyComplex(complex):
    pass


class MyBytes(bytes):
    pass


class MyByteArray(bytearray):
    pass


class MyList(list):
    pass


class MyTuple(tuple):
    pass


class MyDict(dict):
    pass


class MySet(set):
    pass


class MyFrozenSet(frozenset):
    pass


class Index:
    def __repr__(self):
        return "Index()"

    def __index__(self):
        return 1


INSTANCES = {
    "int": [0, 5, -7, 255, 256, 2 ** 70, -2 ** 70, True, MyInt(5)],
    "float": [0.0, -0.0, 1.5, -2.5, 0.1, 3.0, 1e16, 1e308, 5e-324, inf, -inf, nan, MyFloat(1.5)],
    "complex": [0j, 1 + 2j, complex(-0.0, inf), MyComplex(1 + 2j)],
    "str": ["", "a", "abc", "Hello World", "  pad  ", "a,b,,c", "aXbXc", "MiXeD cAsE", "Title Case Here", "123", "12.5", "-7", "line1\nline2\r\nline3\rline4", "tab\tsep\t", "\xc0\xc9 \xdf ǅ İ Σσς", "\U0001d54fy\U0001f600",
            "%s {} {0}", "aaa", "abcabcabc", "١٢", "Ⅷ\xb2\xbd", "_name1", "a b c\x1fd", "ﬁ ẞ ŉ", "x" * 40, "{a}{b!r:>4}", "ab\x00cd", "'q\"", "\ud800", "a\udc80\udcffb\ud801\ud800\xe9\u0661", MyStr("sub")],
    "bytes": [b"", b"a", b"abc", b"Hello World", b"  pad  ", b"a,b,,c", b"aXbXc", b"MiXeD cAsE", b"123", b"line1\nline2\r\nline3", b"tab\tsep", b"\xff\x00\x80", b"aaa", b"abcabcabc", b"%s %d", b"x" * 40, MyBytes(b"sub")],
    "bytearray": [bytearray(b""), bytearray(b"a"), bytearray(b"abc"), bytearray(b"Hello World"), bytearray(b"  pad  "), bytearray(b"a,b,,c"), bytearray(b"\xff\x00\x80"), bytearray(b"abcabcabc"), MyByteArray(b"sub")],
    "list": [[], [1], [1, 2, 3], [3, 1, 2], ["b", "a", "c"], [1, "a"], [1, 1, 2, 1], [[1], [0]], [1.5, 1, True], list(range(10)), MyList([3, 1, 2])],
    "tuple": [(), (1,), (1, 2, 3), (1, 1, 2, 1), ("a", 1), (1.0, True, 1), MyTuple((1, 2, 1))],
    "dict": [{}, {1: 2}, {"a": 1, "b": 2}, {1: "x", "a": None, (1, 2): []}, {True: 1, 1.0: 2}, MyDict({"a": 1, 2: "b"})],
    "set": [set(), {1}, {1, 2, 3}, {"a"}, {1, "a", (1, 2)}, MySet({1, 2})],
    "frozenset": [frozenset(), frozenset({1}), frozenset({1, 2, 3}), frozenset({"a"}), MyFrozenSet({1, 2})],
    "range": [range(0), range(5), range(1, 10, 3), range(5, 0, -1), range(2 ** 70), range(-3, 3)],
    "slice": [slice(None), slice(1), slice(1, 5, 2), slice(-1, None, -1), slice(None, None, 0), slice("a", 1.5)],
    "memoryview": [memoryview(b""), memoryview(b"abc"), memoryview(bytearray(b"abcd")), memoryview(b"abcdef").cast("B", (2, 3)), memoryview(bytearray(b"abcdefgh")).cast("i", (2, 1)), memoryview(b"a").cast("B", ()), memoryview(b"abcdefgh").cast("h"), memoryview(b"abcdef")[::2],
                   memoryview(b"abcdefgh").cast("B", (4, 2))[::-2]],
}

ONE = [
    None, 0, 1, -1, 2, 3, 5, 10, 100, -100, 2 ** 70, -2 ** 70, True, False, 1.5, 0.0, nan, 1j, "", "a", "b", "c", "abc", " ", ",", "X", "ab", "l", "\n", "\xc9", "utf-8", "ascii", "latin-1", "utf-16", "strict", "replace", "ignore", "big", "little", "B", "b", "c", "h", "i", "@B", "C", "F", "A",
    b"", b"a", b"b", b"abc", b" ", b",", b"X", bytearray(b"a"), (), ("a", "b"), (1, 2), (b"a", b"b"), [], [1], [1, 2], ["a", "b"], [b"a", b"b"], {}, {"a": "b"}, {1: 2}, {97: "z", 98: None, 99: 120}, set(), {1}, {1, 2}, frozenset({1}), range(3), range(0),
    str, int, len, str.upper, MyStr("a"), MyInt(1), Index(), memoryview(b"a"), slice(1), ..., MyFloat(1.5), MyComplex(1j), MyBytes(b"a"), MyByteArray(b"a"), MyList([1, 2]), MyTuple((1, 2)), MyTuple(("a", "b")), MyDict({"a": "b"}), MySet({1}), MyFrozenSet({1}), NotImplemented,
]
TWO = [None, 0, 1, -1, 2, 100, 2 ** 70, True, 1.5, "", "a", "b", "abc", "X", " ", "B", "i", "big", "little", "utf-8", "strict", "replace", b"", b"a", b"b", b"X", (), (6,), (2, 3), [3, 2], [1], ["a"], {"a": "b"}, {1}, Index(), MyStr("a"), MyBytes(b"a"), MyInt(1), MyTuple((2, 3))]
FEW = [None, 0, 1, -1, 2 ** 70, 1.5, "a", b"a", (), [1], {"a": 1}, slice(1), MyInt(1), MyStr("a")]
THREE = [None, 0, 1, -1, 2 ** 70, "a", "", b"a", (2, 3)]
KEYWORDS = [
    {"key": None}, {"key": len}, {"key": str}, {"reverse": True}, {"key": str, "reverse": 1}, {"sep": None}, {"sep": ","}, {"sep": "X", "maxsplit": 1}, {"maxsplit": 1}, {"maxsplit": 0}, {"maxsplit": -1}, {"sep": b","}, {"sep": b"X", "maxsplit": 1},
    {"keepends": True}, {"keepends": 0}, {"tabsize": 4}, {"tabsize": 0}, {"encoding": "ascii"}, {"encoding": "utf-16"}, {"errors": "replace"}, {"encoding": "ascii", "errors": "ignore"}, {"encoding": "ascii", "errors": "backslashreplace"},
    {"encoding": "utf-16", "errors": "surrogatepass"}, {"encoding": "utf-32-be", "errors": "surrogatepass"}, {"encoding": "ascii", "errors": "surrogatepass"}, {"encoding": "latin-1", "errors": "surrogatepass"}, {"encoding": "utf-8", "errors": "surrogateescape"},
    {"encoding": "utf-16-le", "errors": "surrogateescape"}, {"encoding": "utf-32", "errors": "surrogateescape"}, {"encoding": "latin-1", "errors": "surrogateescape"}, {"encoding": "utf-16"}, {"encoding": "utf-32"}, {"encoding": "utf-16", "errors": "replace"},
    {"encoding": "utf-32", "errors": "ignore"}, {"encoding": "utf-16", "errors": "backslashreplace"}, {"encoding": "utf-8", "errors": "xmlcharrefreplace"}, {"encoding": "latin-1", "errors": "namereplace"}, {"encoding": "utf-8", "errors": "replace"},
    {"encoding": "ascii", "errors": "xmlcharrefreplace"}, {"encoding": "ascii", "errors": "namereplace"}, {"encoding": "ascii", "errors": "surrogateescape"}, {"encoding": "utf-8", "errors": "surrogatepass"}, {"encoding": "nope"}, {"errors": "nope"},
    {"length": 2, "byteorder": "big"}, {"length": 2, "byteorder": "little", "signed": True}, {"signed": True}, {"length": 20}, {"byteorder": "little"}, {"length": 0}, {"length": -1}, {"byteorder": "middle"}, {"a": 1, "b": "x"}, {"count": 1}, {"nope": 1},
    {"bytes_per_sep": 2}, {"sep": ":", "bytes_per_sep": -2}, {"sep": ":"}, {"default": 1}, {"ndigits": 1}, {"order": "C"}, {"format": "B"},
]


def fresh(value):
    """Another like it, if it is something that can be changed."""
    kind = type(value)
    if kind is list:
        return [fresh(item) for item in value]
    if kind is dict:
        return {key: fresh(item) for key, item in value.items()}
    if isinstance(value, (list, dict, set, bytearray)):
        return kind(value)
    if kind is memoryview:
        # Another view, since it can be released, and of other bytes if they can be written to.
        return memoryview(value) if value.readonly else memoryview(fresh(value.obj)).cast(value.format, value.shape)
    return value


def show(value, depth=0):
    """As repr(), but with the class of a number, so that 1 is not 1.0 or True, with a set in some order of its own, and with what goes through something gone through."""
    kind = type(value)
    if isinstance(value, (set, frozenset)):
        return kind.__name__ + "{" + ", ".join(sorted(show(item, depth + 1) for item in value)) + "}"
    if depth < 4 and kind in (list, tuple):
        return kind.__name__ + "(" + ", ".join(show(item, depth + 1) for item in value) + ")"
    if depth < 4 and kind is dict:
        return "{" + ", ".join(show(key, depth + 1) + ": " + show(item, depth + 1) for key, item in value.items()) + "}"
    if kind is memoryview:
        return "memoryview:" + repr(bytes(value)) + value.format + repr(value.shape) + repr(value.strides)
    if kind is complex:
        return "complex:(%.12g, %.12g)" % (value.real, value.imag)
    if depth < 4 and hasattr(kind, "__next__") or kind.__name__ in ("dict_keys", "dict_values", "dict_items"):
        items = []
        for item in value:
            items.append(show(item, depth + 1))
            if len(items) >= 30:
                break
        return kind.__name__ + "<" + ", ".join(items) + ">"
    text = repr(value)
    if len(text) > 300:
        text = text[:150] + "..." + text[-150:] + " (%d)" % len(text)
    return kind.__name__ + ":" + text.partition(" at 0x")[0]


def attempt(instance, name, arguments, keywords):
    instance = fresh(instance)
    arguments = [fresh(argument) for argument in arguments]
    before = show(instance)
    try:
        result = show(getattr(instance, name)(*arguments, **keywords))
    except RecursionError:
        result = "RecursionError"
    except BaseException as error:
        result = type(error).__name__ + ": " + str(error).partition(" at 0x")[0]
    try:
        after = show(instance)
    except ValueError as error:
        # It has been released.
        after = str(error)
    return result if after == before else result + " -> " + after


def is_not_to_be_tried(instance, name, arguments):
    # What is not an int is looked for in a range by going through it.
    if type(instance) is range and instance.stop > 1000 and len(arguments) == 1 and type(arguments[0]) not in (int, bool):
        return True
    # Which comes out of a set first is nobody's business.
    if name in ("pop", "__iter__", "__repr__", "__str__", "__format__") and isinstance(instance, (set, frozenset)) and len(instance) > 1:
        return True
    # And these would take up all the room that there is.
    return name in ("__pow__", "__rpow__", "__round__") and any(isinstance(value, int) and abs(value) > 1000 for value in (instance, *arguments))


# What depends on how things are kept, or is looked at by another audit, or needs a module that is not written yet.
NOT_TRIED = {"__sizeof__", "__alloc__", "__hash__", "__dir__", "__reduce__", "__reduce_ex__", "__getstate__", "__doc__"}


everything = sys.argv[1:2] == ["everything"]
wanted = not everything and sys.argv[1:] and (sys.argv[1], sys.argv[2], int(sys.argv[3]))

for kind, instances in INSTANCES.items():
    if everything and sys.argv[2:] and kind not in sys.argv[2:]:
        continue
    for name in sorted(dir(instances[0])):
        if name in NOT_TRIED or not callable(getattr(instances[0], name)):
            continue
        for index, instance in enumerate(instances):
            if wanted and wanted != (kind, name, index):
                continue
            results = []

            def note(*arguments, **keywords):
                if is_not_to_be_tried(instance, name, arguments):
                    return
                said = ", ".join([show(argument) for argument in arguments] + ["%s=%s" % (key, show(value)) for key, value in keywords.items()])
                results.append("(%s): %s" % (said, attempt(instance, name, arguments, keywords)))

            note()
            for a in ONE:
                note(a)
            if name.startswith("__"):
                for a in FEW:
                    for b in FEW:
                        note(a, b)
            else:
                for a in TWO:
                    for b in TWO:
                        note(a, b)
                for a in THREE:
                    for b in THREE:
                        for c in THREE:
                            note(a, b, c)
                for keywords in KEYWORDS:
                    note(**keywords)
                    for a in ("a", b"a", 1, [1]):
                        note(a, **keywords)
            what = "%s %s %d" % (kind, name, index)
            if everything:
                print("\n".join(what + " " + show(instance)[:40] + " " + result for result in results))
                continue
            if wanted:
                print("\n".join(results))
            print(what, "|", len(results), int.from_bytes("\n".join(results).encode("utf-8", "backslashreplace"), "big") % 1000000007)
