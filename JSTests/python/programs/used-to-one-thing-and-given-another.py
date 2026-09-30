# Each of these does one thing. It is run often enough with the usual sort of operand to be compiled for that, and is then given every other sort.
import hashlib
import math
import re
import sys
import warnings

# ~True. Where a warning comes among what is printed depends on what is being printed to.
warnings.simplefilter("ignore", DeprecationWarning)


class MyInt(int):
    def __add__(self, other): return "MyInt.__add__"
    def __radd__(self, other): return "MyInt.__radd__"
    def __eq__(self, other): return "MyInt.__eq__"
    def __hash__(self): return 7
    def __bool__(self): return False
    def __index__(self): return 1
    def __neg__(self): return "MyInt.__neg__"
    def __lt__(self, other): return "MyInt.__lt__"


class MyFloat(float):
    def __mul__(self, other): return "MyFloat.__mul__"
    def __rmul__(self, other): return "MyFloat.__rmul__"
    def __truediv__(self, other): return "MyFloat.__truediv__"


class MyStr(str):
    def __getitem__(self, i): return "MyStr.__getitem__"
    def __len__(self): return 99
    def __add__(self, other): return "MyStr.__add__"
    def __contains__(self, x): return True
    def __iter__(self): return iter(("MyStr", "__iter__"))
    def __mod__(self, other): return "MyStr.__mod__"


class MyList(list):
    def __getitem__(self, i): return "MyList.__getitem__"
    def __setitem__(self, i, v): list.__setitem__(self, 0, "MyList.__setitem__")
    def __len__(self): return 99
    def __iter__(self): return iter(("MyList", "__iter__"))
    def __contains__(self, x): return "yes"
    def __bool__(self): return False
    def append(self, x): list.append(self, "MyList.append")


class MyTuple(tuple):
    def __getitem__(self, i): return "MyTuple.__getitem__"
    def __iter__(self): return iter(("MyTuple", "__iter__"))
    def __len__(self): return 99


class MyDict(dict):
    def __getitem__(self, k): return "MyDict.__getitem__"
    def __setitem__(self, k, v): dict.__setitem__(self, "MyDict", "__setitem__")
    def __missing__(self, k): return "MyDict.__missing__"
    def __contains__(self, k): return True
    def __iter__(self): return iter(("MyDict", "__iter__"))
    def get(self, k, d=None): return "MyDict.get"


class Missing(dict):
    def __missing__(self, k): return "Missing.__missing__"


class Index:
    def __init__(self, i): self.i = i
    def __index__(self): return self.i
    def __repr__(self): return "Index(%d)" % self.i


class Anything:
    def __getattr__(self, name):
        if name.startswith("__"):
            raise AttributeError(name)
        return "Anything." + name
    def __repr__(self): return "Anything()"


class Point:
    def __init__(self, x, y): self.x, self.y = x, y
    def m(self): return self.x
    def __repr__(self): return "Point(%r, %r)" % (self.x, self.y)


class Slotted:
    __slots__ = ("x", "y")
    def __init__(self, x): self.x = x
    def m(self): return "Slotted.m"
    def __repr__(self): return "Slotted"


class WithProperty:
    y = 5
    @property
    def x(self): return "a property"
    @x.setter
    def x(self, v): raise ValueError("cannot be set")
    def m(self): return "WithProperty.m"
    def __repr__(self): return "WithProperty"


class Raises:
    def __bool__(self): raise ValueError("__bool__")
    def __len__(self): raise ValueError("__len__")
    def __iter__(self): raise ValueError("__iter__")
    def __eq__(self, other): raise ValueError("__eq__")
    def __hash__(self): raise ValueError("__hash__")
    def __add__(self, other): raise ValueError("__add__")
    def __radd__(self, other): raise ValueError("__radd__")
    def __getitem__(self, i): raise ValueError("__getitem__")
    def __repr__(self): return "Raises()"


def generator():
    yield 1
    yield 2


numbers = [0, 1, -1, 2, 7, -7, 255, 2**30, 2**31 - 1, 2**31, -2**31, -2**31 - 1, 2**32, 2**53, 2**53 + 1, 2**62, 2**63, 2**64, -2**64, 10**30, True, False,
           0.0, -0.0, 1.0, -1.0, 2.0, 0.5, -0.5, 1e308, -1e308, 5e-324, 2.0**31, 2.0**53, float("inf"), float("-inf"), float("nan"), 1j, MyInt(5), MyFloat(2.5)]
others = [None, "", "a", "abc", "é", "\U0001f600x", b"", b"ab", bytearray(b"ab"), (), (1,), (1, 2), [], [1], [1, 2], {}, {1: 2}, {"a": 1}, set(), {1}, frozenset({1}), range(3), range(0), slice(1, 2), Ellipsis, NotImplemented,
          int, len, MyStr("abc"), MyList([1, 2]), MyTuple((1, 2)), MyDict({1: 2}), Missing(), Index(1), Index(-1), Index(10**30), Anything(), Point(1, 2), Slotted(1), WithProperty(), Raises()]
everything = numbers + others


def show(value):
    if isinstance(value, float) and value != value:
        return "nan"
    try:
        return "%s %r" % (type(value).__name__, value)
    except BaseException as e:
        return "%s whose repr raises %s" % (type(value).__name__, type(e).__name__)


def outcome(f, *args):
    try:
        return show(f(*args))
    except BaseException as e:
        return "%s: %s" % (type(e).__name__, e)


def copy(value):
    "What may be changed is made afresh."
    if type(value) in (list, dict, set, bytearray, MyList, MyDict, Missing):
        return type(value)(value)
    if type(value) is Point:
        return Point(value.x, value.y)
    if type(value) is Slotted:
        return Slotted(value.x)
    return value


def op(parameters, expression, statements="pass"):
    """The one thing, done three times over: to the parameters of a function, to variables that are given their values on the way, and to the variables of loops. A variable
    begins with nothing in it, and what a loop gets when there is no more to be had is nothing. A compiler that goes by what it has seen has seen that too."""
    indent = lambda text, by: "".join(" " * by + line + "\n" for line in text.split("\n"))
    source = "def toParameters(%s):\n%s    return %s\n" % (parameters, indent(statements, 4), expression)
    source += "def toVariables(*args):\n    for turn in (0,):\n        %s, = args\n%s        result = %s\n    return result\n" % (parameters, indent(statements, 8), expression)
    names = parameters.split(", ")
    source += "def toLoopVariables(*args):\n"
    for depth, name in enumerate(names):
        source += "%sfor %s in args[%d:%d]:\n" % (" " * (4 + 4 * depth), name, depth, depth + 1)
    source += "%s%sresult = %s\n    return result\n" % (indent(statements, 4 + 4 * len(names)), " " * (4 + 4 * len(names)), expression)
    namespace = dict(globals())
    exec(source, namespace)
    return namespace["toParameters"], namespace["toVariables"], namespace["toLoopVariables"]


count = 0
isVerbose = len(sys.argv) > 1


def check(name, forms, usual, *surprises):
    "`usual` is a list of tuples of arguments. `surprises` is, for each argument, what else it might be."
    global count
    for f in forms:
        for turn in range(3000):
            for args in usual:
                try:
                    f(*map(copy, args))
                except (TypeError, ValueError):
                    break
        said = [repr([outcome(f, *map(copy, args)) for args in usual])]
        base = usual[0]
        for position, values in enumerate(surprises):
            for value in values:
                args = list(map(copy, base))
                args[position] = copy(value)
                said.append("%d %s => %s" % (position, show(value), outcome(f, *args)))
                count += 1
        # And it is still right about the usual.
        said.append(repr([outcome(f, *map(copy, args)) for args in usual]))
        said = [re.sub("0x[0-9a-f]+", "0x", line) for line in said]
        # All of it is a great deal. Given anything on the command line, this says it all.
        print("====", name, f.__name__, len(said), hashlib.md5("\n".join(said).encode("utf-8", "backslashreplace")).hexdigest())
        if isVerbose:
            for line in said:
                print("   ", line)


ints = [(3, 4), (10, 3), (-5, 2)]
floats = [(1.5, 2.5), (10.0, 4.0), (-0.5, 3.25)]
for name, f in (("add", op('a, b', 'a + b')), ("sub", op('a, b', 'a - b')), ("mul", op('a, b', 'a * b')), ("truediv", op('a, b', 'a / b')), ("floordiv", op('a, b', 'a // b')), ("mod", op('a, b', 'a % b')),
                ("pow", op('a, b', 'a ** b')), ("lshift", op('a, b', 'a << b')), ("rshift", op('a, b', 'a >> b')), ("and", op('a, b', 'a & b')), ("or", op('a, b', 'a | b')), ("xor", op('a, b', 'a ^ b')),
                ("lt", op('a, b', 'a < b')), ("le", op('a, b', 'a <= b')), ("eq", op('a, b', 'a == b')), ("ne", op('a, b', 'a != b')), ("gt", op('a, b', 'a > b')), ("ge", op('a, b', 'a >= b')),
                ("is", op('a, b', 'a is b')), ("divmod", op('a, b', 'divmod(a, b)')), ("min", op('a, b', 'min(a, b)')), ("max", op('a, b', 'max(a, b)'))):
    small = [n for n in numbers if name not in ("pow", "lshift") or not isinstance(n, int) or abs(n) < 2**16] if name in ("pow", "lshift") else numbers
    check(name + " of ints", f, ints, small + others[:12] + others[-14:], small + others[:12] + others[-14:])
    if name not in ("lshift", "rshift", "and", "or", "xor"):
        check(name + " of floats", f, floats, small, small)


iadd = op("a, b", "a", "a += b")
imul = op("a, b", "a", "a *= b")
check("+= of ints", iadd, ints, everything, everything)
check("+= of strs", iadd, [("a", "b"), ("", "xyz")], everything, everything)
check("+= of lists", iadd, [([1], [2]), ([], [3, 4])], everything, everything)
check("*= of ints", imul, ints, [v for v in everything if not (isinstance(v, int) and abs(v) > 2**16)], [v for v in everything if not (isinstance(v, int) and abs(v) > 2**16)])
check("add of strs", op('a, b', 'a + b'), [("a", "b"), ("xy", "")], everything, everything)
check("mod of a str", op('a, b', 'a % b'), [("%s!", "b"), ("%r", 1)], everything, everything)
check("mul of a str", op('a, b', 'a * b'), [("ab", 2), ("x", 0)], others, [v for v in everything if not (isinstance(v, int) and abs(v) > 2**16) and not (type(v) is Index and abs(v.i) > 2**16)])

for name, f in (("neg", op('a', '-a')), ("pos", op('a', '+a')), ("invert", op('a', '~a')), ("not", op('a', 'not a')), ("bool", op('a', 'bool(a)')), ("abs", op('a', 'abs(a)')), ("if", op('a', '"yes" if a else "no"')),
                ("int", op('a', 'int(a)')), ("float", op('a', 'float(a)')), ("str", op('a', 'str(a)')), ("hash is an int", op('a', 'type(hash(a)).__name__')), ("while", op('a', '[a for _ in "x" if a]')),
                ("and", op('a', 'a and 5')), ("or", op('a', 'a or 5')), ("is None", op('a', 'a is None')), ("is not None", op('a', 'a is not None')), ("== 0", op('a', 'a == 0')), ("+ 1", op('a', 'a + 1')), ("- 1", op('a', 'a - 1')),
                ("* 2", op('a', 'a * 2')), ("// 2", op('a', 'a // 2')), ("% 2", op('a', 'a % 2')), ("/ 2", op('a', 'a / 2')), ("& 1", op('a', 'a & 1')), (">> 1", op('a', 'a >> 1')), ("<< 1", op('a', 'a << 1')), ("** 2", op('a', 'a ** 2')),
                ("< 5", op('a', 'a < 5')), ("isinstance int", op('a', 'isinstance(a, int)')), ("type", op('a', 'type(a).__name__')), ("round", op('a', 'round(a)')), ("math.floor", op('a', 'math.floor(a)')), ("math.sqrt", op('a', 'math.sqrt(a)'))):
    check(name + " of an int", f, [(3,), (0,), (-8,)], everything)
    check(name + " of a float", f, [(1.5,), (0.0,), (-8.25,)], numbers + others[:6])

sequences = [v for v in others]
indices = [0, 1, -1, 2, -2, 3, -3, 5, -5, 2**31, -2**31, 2**31 - 1, 2**62, 10**30, -10**30, True, False, 0.0, 1.0, None, "0", (), (0,), slice(None), slice(1, None), slice(None, None, -1), slice(0, 0), slice(5, 9), Index(1), Index(-1), Index(9), Index(10**30), MyInt(0), Raises()]


setitem = op("x, i, v", "x", "x[i] = v")
delitem = op("x, i", "x", "del x[i]")
for name, usual in (("a list", [([1, 2, 3], 0), ([4, 5, 6], 2)]), ("a list of strs", [(["a", "b", "c"], 0), (["d", "e", "f"], 1)]), ("a tuple", [((1, 2, 3), 0), ((4, 5, 6), 2)]), ("a str", [("abc", 0), ("def", 2)]),
                    ("bytes", [(b"abc", 0), (b"def", 2)]), ("a dict", [({0: "a", 1: "b"}, 0), ({0: "c", 1: "d"}, 1)]), ("a dict of strs", [({"a": 1, "b": 2}, "a"), ({"a": 3, "b": 4}, "b")]), ("a range", [(range(5), 0), (range(2, 9), 3)])):
    check("getitem of " + name, op('x, i', 'x[i]'), usual, sequences + numbers[:4], indices + ["a", "zz"])
    check("in " + name, op('x, i', 'i in x'), usual, sequences + numbers[:4], indices[:22] + ["a", "zz", Raises()])
for name, usual in (("a list", [([1, 2, 3], 0, 9), ([4, 5, 6], 2, 8)]), ("a list of strs", [(["a", "b", "c"], 0, "z"), (["d", "e", "f"], 1, "y")]), ("a dict", [({0: "a"}, 0, "z"), ({0: "c"}, 1, "y")]), ("a bytearray", [(bytearray(b"abc"), 0, 65), (bytearray(b"def"), 2, 66)])):
    check("setitem of " + name, setitem, usual, sequences + numbers[:4], indices, everything)
    check("delitem of " + name, delitem, [args[:2] for args in usual[:1]], sequences + numbers[:4], indices)

check("slice", op('x, i, j', 'x[i:j]'), [([1, 2, 3, 4], 1, 3), ([5, 6, 7], 0, 2)], sequences, indices[:20] + indices[-6:], indices[:20] + indices[-6:])
check("slice of a str", op('x, i, j', 'x[i:j]'), [("abcd", 1, 3), ("efg", 0, 2)], sequences, indices[:20] + indices[-6:], indices[:20] + indices[-6:])

for name, f in (("len", op('x', 'len(x)')), ("iter", op('x', '[v for v in x]')), ("for", op('x', 'sum(1 for v in x)')), ("list", op('x', 'list(x)')), ("tuple", op('x', 'tuple(x)')), ("unpack two", op('x', '(lambda a, b: (b, a))(*x)')),
                ("a, b =", op('x', '[(b, a) for a, b in [x]]')), ("a, *b =", op('x', '[(b, a) for a, *b in [x]]')), ("sum", op('x', 'sum(x)')), ("sorted", op('x', 'sorted(x)')), ("reversed", op('x', 'list(reversed(x))')), ("enumerate", op('x', 'list(enumerate(x))')),
                ("zip", op('x', 'list(zip(x, x))')), ("any", op('x', 'any(x)')), ("all", op('x', 'all(x)')), ("min", op('x', 'min(x)')), ("max", op('x', 'max(x)')), ("[*x]", op('x', '[*x]')), ("{**x}", op('x', '{**x}')), ("f(**x)", op('x', 'dict(**x)')),
                ("join", op('x', '",".join(x)')), ("set", op('x', 'sorted(set(x))')), ("dict", op('x', 'dict(x)')), ("bool", op('x', 'bool(x)')), ("not", op('x', 'not x')), ("x[0]", op('x', 'x[0]')), ("x[-1]", op('x', 'x[-1]')), ("x[1:]", op('x', 'x[1:]'))):
    if name in ("{**x}", "f(**x)", "dict"):
        check(name + " of a dict", f, [({"a": 1, "b": 2},), ({"c": 3, "d": 4},)], everything + [generator()])
        continue
    if name == "join":
        check(name + " of a list", f, [(["a", "b"],), (["c", "d"],)], everything + [generator()])
        continue
    check(name + " of a list", f, [([1, 2],), ([3, 4],)], everything + [generator()])
    check(name + " of a tuple", f, [((1, 2),), ((3, 4),)], others)


setattribute = op("o, v", "o.x", "o.x = v")
delattribute = op("o", 'hasattr(o, "x")', "del o.x")
objects = everything
check("o.x", op('o', 'o.x'), [(Point(1, 2),), (Point(3, 4),)], objects)
check("o.y", op('o', 'o.y'), [(Point(1, 2),), (Point(3, 4),)], objects)
check("o.m()", op('o', 'o.m()'), [(Point(1, 2),), (Point(3, 4),)], objects)
check("o.x = v", setattribute, [(Point(1, 2), 5), (Point(3, 4), 6)], objects, everything[:8])
check("del o.x", delattribute, [(Point(1, 2),)], objects)
check("getattr", op('o, n', 'getattr(o, n, "default")'), [(Point(1, 2), "x"), (Point(3, 4), "y")], objects, ["x", "z", "__class__", "", 1, None, MyStr("x")])
check("hasattr", op('o', 'hasattr(o, "x")'), [(Point(1, 2),), (Point(3, 4),)], objects)
check("append", op('x', '(x.append(1), x)[1]'), [([1],), ([2, 3],)], objects)
check("pop", op('x', '(x.pop(), x)'), [([1, 2],), ([2, 3],)], objects)
check("get", op('x, k', 'x.get(k)'), [({1: 2}, 1), ({3: 4}, 5)], objects, everything)
check("upper", op('x', 'x.upper()'), [("a",), ("bc",)], objects)
check("call", op('f', 'f()'), [(lambda: 1,), (lambda: 2,)], objects)
check("call with one", op('f', 'f(1)'), [(lambda a: a,), (lambda a: -a,)], objects + [lambda: 0, lambda a, b: 0, lambda *a: a, lambda **k: k, lambda a=2, b=3: (a, b), abs, str, Point, generator])
check("range", op('n', 'list(range(n))[-3:]'), [(3,), (5,)], [v for v in everything if not (isinstance(v, int) and abs(v) > 2**16) and not (type(v) is Index and abs(v.i) > 2**16)])
check("for in range", op('n', 'sum(i for i in range(n))'), [(3,), (5,)], [v for v in everything if not (isinstance(v, int) and abs(v) > 2**16) and not (type(v) is Index and abs(v.i) > 2**16)])
check("chained comparison", op('a, b, c', 'a < b < c'), [(1, 2, 3), (3, 2, 1)], everything, everything, everything)
check("f-string", op('a', 'f"{a}|{a!r}|{a:>5}"'), [(1,), (22,)], everything)
check("conditional sum", op('a, b', 'a + b if a > b else a - b'), ints, numbers, numbers)


total = op("items", "t", "t = 0\nfor v in items:\n    t += v")
product = op("items", "t", "t = 1\nfor v in items:\n    t *= v")
for name, f in (("total", total), ("product", product)):
    check(name, f, [([1, 2, 3],), ([4, 5, 6],)], [[2**30, 2**30], [2**31 - 1, 1], [-2**31, -1], [2**62, 2**62], [1, 2.5], [1.5, 2], [0.1] * 3, [True, True], [1, None], ["a"], [[1]], [MyInt(1)], [1, MyInt(1)], [MyFloat(1.5), 2], [2**40] * 5, [-1] * 3, [float("inf"), float("-inf")], (1, 2), range(4), {1, 2}, {1: 2}, generator(), "ab", None, 5])
print(count, "surprises")
