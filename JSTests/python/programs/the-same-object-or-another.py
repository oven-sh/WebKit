# Where what is asked for is what there is already, and cannot be changed, CPython gives that and not another like it. A program can tell, with `is` and with id().
# Numbers are not looked at, since here one is not an object apart from what number it is. Nor is a bytes with nothing in it, of which CPython has only one: see "Where it differs from CPython on purpose" in the README.
import copy


def attempt(f, *a):
    try:
        return f(*a)
    except BaseException as e:
        return type(e).__name__


def made(kind):
    "Something of the kind that has only just been made, and is not kept anywhere else"
    n = int("3")
    return {
        "tuple": lambda: tuple([n, n + 1, n + 2]),
        "empty tuple": lambda: tuple([]),
        "bytes": lambda: bytes([65 + n, 66, 67]),
        "empty bytes": lambda: bytes([]),
        "frozenset": lambda: frozenset([n, n + 1]),
        "empty frozenset": lambda: frozenset([]),
        "str": lambda: "".join(["ab", "cd", str(n)]),
        "wide str": lambda: "".join(["Āb", "cd", str(n)]),
        "large int": lambda: 2 ** 70 + n,
        "float": lambda: 1.5 * n,
        "complex": lambda: complex(n, 1),
        "range": lambda: range(n, 10),
        "list": lambda: [n, n + 1],
        "bytearray": lambda: bytearray([65, 66]),
        "set": lambda: {n, n + 1},
        "dict": lambda: {n: 1},
    }[kind]()


class SubMixin:
    pass


OPERATIONS = {
    "x * 1": lambda x: x * 1,
    "1 * x": lambda x: 1 * x,
    "x * 0 is x * 0": lambda x: (x * 0, x * 0),
    "x[:]": lambda x: x[:],
    "x[0:]": lambda x: x[0:],
    "x[:len(x)]": lambda x: x[:len(x)],
    "x[::1]": lambda x: x[::1],
    "x[0:100]": lambda x: x[0:100],
    "x[1:]": lambda x: x[1:],
    "x + nothing": lambda x: x + type(x)(),
    "nothing + x": lambda x: type(x)() + x,
    "type(x)(x)": lambda x: type(x)(x),
    "x.copy()": lambda x: x.copy(),
    "copy.copy(x)": lambda x: copy.copy(x),
    "copy.deepcopy(x)": lambda x: copy.deepcopy(x),
    "x | nothing": lambda x: x | type(x)(),
    "x & x": lambda x: x & x,
    "x - nothing": lambda x: x - type(x)(),
    "x.union()": lambda x: x.union(),
    "x.difference()": lambda x: x.difference(),
    "x.intersection()": lambda x: x.intersection(),
    "+x": lambda x: +x,
    "abs(x)": lambda x: abs(x),
    "x + 0": lambda x: x + 0,
    "x * 1.0": lambda x: x * 1.0,
    "int(x)": lambda x: int(x),
    "float(x)": lambda x: float(x),
    "complex(x)": lambda x: complex(x),
    "x.real": lambda x: x.real,
    "x.conjugate()": lambda x: x.conjugate(),
    "x.__index__()": lambda x: x.__index__(),
    "x.__int__()": lambda x: x.__int__(),
    "x.__trunc__()": lambda x: x.__trunc__(),
    "x.__float__()": lambda x: x.__float__(),
    "x.__pos__()": lambda x: x.__pos__(),
    "round(x)": lambda x: round(x),
    "x.numerator": lambda x: x.numerator,
    "str(x)": lambda x: str(x),
    "x.__str__()": lambda x: x.__str__(),
    "format(x)": lambda x: format(x),
    "format(x, '')": lambda x: format(x, ""),
    "f'{x}'": lambda x: f"{x}",
    "'%s' % x": lambda x: "%s" % (x,),
    "'{}'.format(x)": lambda x: "{}".format(x),
    "''.join([x])": lambda x: type(x)().join([x]),
    "x.strip()": lambda x: x.strip(),
    "x.lstrip()": lambda x: x.lstrip(),
    "x.rstrip()": lambda x: x.rstrip(),
    "x.replace, of what is not there": lambda x: x.replace(x[:0] + type(x)(b"~" if isinstance(x, (bytes, bytearray)) else "~"), x[:0]),
    "x.replace, none": lambda x: x.replace(x[:1], x[1:2], 0),
    "x.replace, by the same": lambda x: x.replace(x[:1], x[:1]),
    "x.lower()": lambda x: x.lower(),
    "x.upper().lower()": lambda x: x.upper().lower(),
    "x.ljust(0)": lambda x: x.ljust(0),
    "x.rjust(1)": lambda x: x.rjust(1),
    "x.center(2)": lambda x: x.center(2),
    "x.zfill(0)": lambda x: x.zfill(0),
    "x.expandtabs()": lambda x: x.expandtabs(),
    "x.removeprefix, not there": lambda x: x.removeprefix(x[1:2]),
    "x.removesuffix, not there": lambda x: x.removesuffix(x[:1]),
    "x.removeprefix, nothing": lambda x: x.removeprefix(x[:0]),
    "x.split()[0]": lambda x: x.split()[0],
    "x.split(what is not there)[0]": lambda x: x.split(x[:1] * 9)[0],
    "x.rsplit(what is not there)[0]": lambda x: x.rsplit(x[:1] * 9)[0],
    "x.splitlines()[0]": lambda x: x.splitlines()[0],
    "x.partition(what is not there)[0]": lambda x: x.partition(x[:1] * 9)[0],
    "x.rpartition(what is not there)[2]": lambda x: x.rpartition(x[:1] * 9)[2],
    "x.translate({})": lambda x: x.translate({}),
    "x.translate(None)": lambda x: x.translate(None),
    "x.encode().decode()": lambda x: x.encode().decode(),
    "x.casefold()": lambda x: x.casefold(),
    "x.format()": lambda x: x.format(),
    "x % ()": lambda x: x % (),
    "bytes(x)": lambda x: bytes(x),
    "x.__bytes__()": lambda x: x.__bytes__(),
    "tuple(x)": lambda x: tuple(x),
    "frozenset(x)": lambda x: frozenset(x),
    "x.__getnewargs__()[0]": lambda x: x.__getnewargs__()[0],
    "iter(x) is iter(x)": lambda x: (iter(x), iter(x)),
    "sys.intern(x)": lambda x: __import__("sys").intern(x),
    "memoryview(x).obj": lambda x: memoryview(x).obj,
    "(lambda *a: a)(*x)": lambda x: (lambda *a: a)(*x),
    "[*x] and (*x,)": lambda x: (*x,),
    "x if in a tuple": lambda x: (x,)[0],
    "reversed twice": lambda x: type(x)(reversed(type(x)(reversed(x)))),
}
PAIRS = ("x * 0 is x * 0", "iter(x) is iter(x)")
KINDS = ("tuple", "empty tuple", "bytes", "frozenset", "empty frozenset", "str", "wide str", "range", "list", "bytearray", "set", "dict")
for name, operation in OPERATIONS.items():
    row = []
    for kind in KINDS:
        x = made(kind)
        r = attempt(operation, x)
        if isinstance(r, str) and r.endswith("Error") and not isinstance(x, str):
            continue
        if isinstance(r, str) and r in ("TypeError", "AttributeError", "ValueError", "IndexError", "KeyError"):
            continue
        row.append((kind, r[0] is r[1] if name in PAIRS else r is x))
    print(name, "=>", row)
print("---- of a class derived from it, it is never the same one, unless nothing was asked for but the thing itself")
for name, operation in OPERATIONS.items():
    row = []
    for kind in ("tuple", "bytes", "frozenset", "str"):
        base = made(kind)
        x = type("Sub", (type(base),), {})(base)
        r = attempt(operation, x)
        if isinstance(r, str) and r in ("TypeError", "AttributeError", "ValueError", "IndexError", "KeyError"):
            continue
        row.append((kind, (r[0] is r[1] if name in PAIRS else r is x), type(r).__name__ if name not in PAIRS else None))
    print(name, "=>", row)
