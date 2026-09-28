# What comes of the operators and the built-in functions, given what they are meant for and what they are not: every operator between every two of some seventy values, and every built-in function that takes one
# or two things, of each of them. What is looked at is the value and its class, or the exception and what it says.
#
# There is a line for each operator or function and each first operand, with how many things were tried and a number that stands for all that came of them. To see them all:
#
#     operations.py [--] <what> <which value>
#     operations.py [--] everything

import _warnings
import sys

_warnings.filters.insert(0, ("ignore", None, Warning, None, 0))

inf = float("inf")
nan = float("nan")


class Plain:
    def __repr__(self):
        return "Plain()"


class Number:
    """Whatever is asked of it, it says what was asked."""

    def __repr__(self):
        return "Number()"

    def __index__(self):
        return 3

    def __float__(self):
        return 2.5

    def __bool__(self):
        return False

    def __hash__(self):
        return 7


for _name in "add sub mul truediv floordiv mod pow lshift rshift and or xor matmul divmod".split():
    setattr(Number, "__%s__" % _name, (lambda name: lambda self, other, *rest: ("Number.__%s__" % name, other))(_name))
    setattr(Number, "__r%s__" % _name, (lambda name: lambda self, other, *rest: ("Number.__r%s__" % name, other))(_name))


class MyInt(int):
    pass


class MyStr(str):
    pass


class MyList(list):
    pass


class MyTuple(tuple):
    pass


class MyFloat(float):
    pass


VALUES = [
    0, 1, -1, 2, 3, 7, -7, 10, 255, -128, 2 ** 31, -2 ** 31, 2 ** 53 + 1, 2 ** 63, -2 ** 63, 2 ** 64, 10 ** 30, -10 ** 30, True, False,
    0.0, -0.0, 1.0, 0.5, 1.5, -2.5, 3.0, 1e16, 1e308, -1e308, 5e-324, inf, -inf, nan,
    0j, 1j, 1 + 2j, -2 - 0.5j, complex(inf, nan), complex(-0.0, 0.0),
    None, ..., NotImplemented,
    "", "a", "abc", "A b", "\xe9", "\U0001d54f", "12", " x ", "%s", "%d", "{}",
    b"", b"a", b"abc", b"\xff\x00", b"%s", bytearray(b""), bytearray(b"ab"),
    (), (1,), (1, 2, 3), ("a", 1), ((),),
    [], [1], [1, 2, 3], ["a", 1], [[]],
    {}, {1: 2}, {"a": 1, "b": 2},
    set(), {1}, {1, 2, 3}, frozenset(), frozenset({1, 2}),
    range(0), range(5), range(1, 10, 3), range(5, 0, -1), slice(1), slice(1, 5, 2),
    int, str, list, Plain(), Number(), MyInt(5), MyStr("ms"), MyList([1, 2]), MyTuple((1, 2)), MyFloat(1.5),
]


def fresh(value):
    """Another like it, if it is something that can be changed."""
    kind = type(value)
    if kind in (list, MyList):
        return kind(fresh(item) for item in value)
    if kind in (dict, set, bytearray):
        return kind(value)
    return value


def show(value, depth=0):
    """As repr(), but with the class of a number, so that 1 is not 1.0 or True, and with a set in some order of its own."""
    kind = type(value)
    if kind in (set, frozenset):
        return kind.__name__ + "{" + ", ".join(sorted(show(item, depth + 1) for item in value)) + "}"
    if depth < 4 and kind in (list, tuple, MyList, MyTuple):
        return kind.__name__ + "(" + ", ".join(show(item, depth + 1) for item in value) + ")"
    if depth < 4 and kind is dict:
        return "{" + ", ".join(show(key, depth + 1) + ": " + show(item, depth + 1) for key, item in value.items()) + "}"
    if kind is complex:
        # The last few bits depend on whether what CPython was compiled with does a multiplication and an addition in one, which it may.
        return "complex:(%.12g, %.12g)" % (value.real, value.imag)
    text = repr(value)
    if len(text) > 200:
        text = text[:100] + "..." + text[-100:] + " (%d)" % len(text)
    # Where something is in memory is nothing to go by.
    return kind.__name__ + ":" + text.partition(" at 0x")[0]


def attempt(function, *arguments):
    try:
        return show(function(*arguments))
    except RecursionError:
        return "RecursionError"
    except BaseException as error:
        return type(error).__name__ + ": " + str(error).partition(" at 0x")[0]


def is_int(value):
    return isinstance(value, int)


def is_large(value, limit):
    return is_int(value) and abs(value) > limit


def is_wide(value):
    """Whether, taken for how something is to be formatted, it asks for more room than is worth making."""
    return is_large(value, 1000) or (isinstance(value, float) and abs(value) > 1000)


def is_hashed_by_value(value):
    """Not by where it is, nor by what is made up afresh each time that CPython is started."""
    if isinstance(value, (tuple, frozenset)):
        return all(is_hashed_by_value(item) for item in value)
    return isinstance(value, (int, float, complex)) and value == value


def is_sequence(value):
    return isinstance(value, (str, bytes, bytearray, tuple, list))


def takes_too_much(what, a, b):
    """Whether it would take up all the room that there is, or all day. An int can be no larger here than a BigInt of JavaScript's."""
    what = what.lstrip("i") if what not in ("in", "is", "is not") else what
    if what == "**":
        return (is_large(b, 300) and not (is_int(a) and abs(a) <= 1)) or (is_large(a, 2 ** 70) and is_large(b, 20)) or (isinstance(b, Number) and False)
    if what == "<<":
        return is_large(b, 300) and a != 0
    if what == "*":
        # Up to what there is no question of making.
        return (is_sequence(a) and len(a) and is_large(b, 1000) and abs(b) < 2 ** 62) or (is_sequence(b) and len(b) and is_large(a, 1000) and abs(a) < 2 ** 62)
    return False


BINARY = {
    "+": lambda a, b: a + b, "-": lambda a, b: a - b, "*": lambda a, b: a * b, "/": lambda a, b: a / b, "//": lambda a, b: a // b, "%": lambda a, b: a % b, "**": lambda a, b: a ** b, "@": lambda a, b: a @ b,
    "<<": lambda a, b: a << b, ">>": lambda a, b: a >> b, "&": lambda a, b: a & b, "|": lambda a, b: a | b, "^": lambda a, b: a ^ b,
    "<": lambda a, b: a < b, "<=": lambda a, b: a <= b, ">": lambda a, b: a > b, ">=": lambda a, b: a >= b, "==": lambda a, b: a == b, "!=": lambda a, b: a != b,
    "in": lambda a, b: a in b, "and": lambda a, b: a and b, "or": lambda a, b: a or b, "[]": lambda a, b: a[b], "divmod": divmod,
}


def in_place(source):
    variables = {}
    # Whether it is still the same object is only a question of what can be changed.
    exec("def operate(a, b):\n    c = a\n    a %s b\n    return (a, c, a is c if isinstance(c, (list, dict, set, bytearray)) else None)" % source, variables)
    return variables["operate"]


for _operator in "+ - * / // % ** @ << >> & | ^".split():
    BINARY["i" + _operator] = in_place(_operator + "=")


def set_item(a, b):
    a[b] = 9
    return a


def delete_item(a, b):
    del a[b]
    return a


BINARY["[]="] = set_item
BINARY["del []"] = delete_item

UNARY = {
    "-": lambda a: -a, "+": lambda a: +a, "~": lambda a: ~a, "not": lambda a: not a, "bool": bool, "abs": abs, "len": len, "repr": repr, "str": str, "ascii": ascii, "int": int, "float": float, "complex": complex,
    "bytes": lambda a: bytes(a) if not is_large(a, 10000) else None, "bytearray": lambda a: bytearray(a) if not is_large(a, 10000) else None, "list": list, "tuple": tuple, "set": set, "frozenset": frozenset, "dict": dict,
    "sorted": sorted, "reversed": lambda a: list(reversed(a)), "iter": lambda a: list(iter(a)), "min": min, "max": max, "sum": sum, "any": any, "all": all, "round": round, "chr": chr, "ord": ord, "hex": hex, "oct": oct,
    "bin": bin, "callable": callable, "type": lambda a: type(a).__name__, "enumerate": lambda a: list(enumerate(a)), "hash": lambda a: hash(a) if is_hashed_by_value(a) else type(hash(a)) is int,
    "format": format, "next": next, "range": lambda a: range(a), "slice": slice, "memoryview": lambda a: bytes(memoryview(a)), "zip": lambda a: list(zip(a)), "map": lambda a: list(map(str, a)), "filter": lambda a: list(filter(None, a)),
    "star": lambda a: [*a], "double star": lambda a: {**a}, "unpack": lambda a: (lambda x, *y: (x, y))(*a), "f-string": lambda a: f"{a}|{a!r}|{a!s}|{a!a}", "is None": lambda a: a is None, "vars": lambda a: sorted(vars(a))[:0],
    "id is int": lambda a: type(id(a)) is int, "str.join": lambda a: ",".join(a), "bytes.join": lambda a: b",".join(a), "index": lambda a: [10, 20, 30, 40][a], "__index__": lambda a: a.__index__(), "is_integer": lambda a: a.is_integer(),
    "float.fromhex(hex)": lambda a: float.fromhex(a.hex()), "as_integer_ratio": lambda a: a.as_integer_ratio(), "bit_length": lambda a: a.bit_length(), "bit_count": lambda a: a.bit_count(), "conjugate": lambda a: a.conjugate(),
    "real imag": lambda a: (a.real, a.imag), "numerator": lambda a: (a.numerator, a.denominator), "trunc floor ceil": lambda a: (a.__trunc__(), a.__floor__(), a.__ceil__()), "copy": lambda a: a.copy(), "sizeof is int": lambda a: type(a.__sizeof__()) is int,
}

TWO = {
    "round": lambda a, b: None if is_large(b, 400) else round(a, b), "pow": lambda a, b: None if takes_too_much("**", a, b) else pow(a, b), "isinstance": isinstance, "issubclass": issubclass, "getattr": getattr, "hasattr": hasattr, "int": int, "str": str, "bytes": bytes, "complex": complex,
    "format": lambda a, b: None if is_wide(b) else format(a, b), "min": min, "max": max, "sum": sum, "range": lambda a, b: range(a, b), "slice": slice, "zip": lambda a, b: list(zip(a, b)), "map": lambda a, b: list(map(a, b)), "filter": lambda a, b: list(filter(a, b)),
    "enumerate": lambda a, b: list(enumerate(a, b)), "iter": lambda a, b: iter(a, b), "next": next, "sorted key": lambda a, b: sorted(a, key=b), "dict.fromkeys": dict.fromkeys, "is": lambda a, b: (a is b) if type(a) in (type(None), bool, type) else None,
    "tuple compare": lambda a, b: ((a,) < (b,), (a,) == (b,), [a] <= [b]), "dict key": lambda a, b: {a: 1, b: 2}, "set of": lambda a, b: len({a, b}), "list.index": lambda a, b: [a, 0, b].index(b), "list.count": lambda a, b: [a, b, a].count(b),
    "% tuple": lambda a, b: a % (b,), "str.format": lambda a, b: a.format(b), "f-string": lambda a, b: None if is_wide(b) else f"{a:{b}}", "pow mod 7": lambda a, b: None if is_large(b, 10 ** 40) else pow(a, b, 7), "math": lambda a, b: (a + b) * 2 - a / 3 if 0 else None,
}

everything = sys.argv[1:] == ["everything"]
wanted = not everything and sys.argv[1:] and (sys.argv[1], int(sys.argv[2]))


def report(what, index, results):
    if everything:
        print("\n".join(what + " " + result for result in results))
        return
    if wanted:
        print("\n".join(results))
    print(what, index, "|", len(results), int.from_bytes("\n".join(results).encode("utf-8", "backslashreplace"), "big") % 1000000007)


count = len(VALUES)
for group, table in (("binary", BINARY), ("two", TWO)):
    for name, function in table.items():
        what = group + ":" + name.replace(" ", "_")
        for i in range(count):
            if wanted and wanted != (what, i):
                continue
            results = []
            for j in range(count):
                a = fresh(VALUES[i])
                b = fresh(VALUES[j])
                if group == "binary" and takes_too_much(name, a, b):
                    continue
                results.append("%s , %s: %s" % (show(a), show(b), attempt(function, a, b)))
            report(what, i, results)
for name, function in UNARY.items():
    what = "unary:" + name.replace(" ", "_")
    if wanted and wanted[0] != what:
        continue
    report(what, 0, ["%s: %s" % (show(value), attempt(function, fresh(value))) for value in VALUES])
