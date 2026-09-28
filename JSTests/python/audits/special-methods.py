# What the language makes of what a program's own special methods do. Each special method is given each of some eighty things to do: to return something, which is mostly not what is wanted of it, to raise
# something, to take the wrong number of arguments, or not to be a function at all. Then everything in the language that would call it is tried. What is looked at is what comes of it and its class, or the
# exception and what it says.
#
# There is a line for each special method and each thing it does, with how many things were tried and a number that stands for all that came of them. To see them all:
#
#     special-methods.py [--] <method> <what it does>
#     special-methods.py [--] everything [<method>]

import _warnings
import sys

_warnings.filters.insert(0, ("ignore", None, Warning, None, 0))


class MyInt(int):
    pass


class MyStr(str):
    pass


class MyFloat(float):
    pass


class MyBytes(bytes):
    pass


class MyTuple(tuple):
    pass


class MyError(Exception):
    pass


class MyBaseError(BaseException):
    pass


class TooLong(BaseException):
    pass


calls = 0


def count():
    """What goes on for ever does it by calling the method for ever, so each of them counts."""
    global calls
    calls += 1
    if calls > 200:
        raise TooLong


class Callable:
    def __call__(self, *arguments, **keywords):
        count()
        return 1


class Sink:
    """Somewhere to print to."""

    def write(self, text):
        pass


def generator():
    yield 1
    yield 2


async def coroutine():
    return 1


class Awaitable:
    def __await__(self):
        return iter(())


# What is returned is made afresh each time, since some of it is used up.
RETURNED = {
    "None": lambda: None, "True": lambda: True, "False": lambda: False, "0": lambda: 0, "1": lambda: 1, "-1": lambda: -1, "2": lambda: 2, "2**63-1": lambda: 2 ** 63 - 1, "2**63": lambda: 2 ** 63, "2**70": lambda: 2 ** 70, "-2**70": lambda: -2 ** 70, "1.5": lambda: 1.5,
    "0.0": lambda: 0.0, "nan": lambda: float("nan"), "inf": lambda: float("inf"), "1j": lambda: 1j, "''": lambda: "", "'a'": lambda: "a", "'ab'": lambda: "ab", "b''": lambda: b"", "b'a'": lambda: b"a", "bytearray": lambda: bytearray(b"a"), "()": lambda: (), "(1,)": lambda: (1,), "(1, 2)": lambda: (1, 2),
    "('a', 'b')": lambda: ("a", "b"), "[]": lambda: [], "[1, 2]": lambda: [1, 2], "['a']": lambda: ["a"], "{}": lambda: {}, "{'a': 1}": lambda: {"a": 1}, "set": lambda: {1}, "NotImplemented": lambda: NotImplemented, "...": lambda: ..., "object": lambda: object(), "int": lambda: int,
    "an iterator": lambda: iter([1, 2]), "an iterator of nothing": lambda: iter(()), "a generator": generator, "a coroutine": coroutine, "an awaitable": Awaitable, "range": lambda: range(2), "memoryview": lambda: memoryview(b"ab"), "MyInt": lambda: MyInt(1), "MyStr": lambda: MyStr("a"),
    "MyFloat": lambda: MyFloat(1.5), "MyBytes": lambda: MyBytes(b"a"), "MyTuple": lambda: MyTuple(("a",)), "a function": lambda: len,
}
RAISED = {
    "TypeError": lambda: TypeError("t"), "ValueError": lambda: ValueError("v"), "AttributeError": lambda: AttributeError("a"), "IndexError": lambda: IndexError("i"), "KeyError": lambda: KeyError("k"), "LookupError": lambda: LookupError("l"), "StopIteration": lambda: StopIteration(),
    "StopIteration(5)": lambda: StopIteration(5), "StopAsyncIteration": lambda: StopAsyncIteration(), "RuntimeError": lambda: RuntimeError("r"), "OverflowError": lambda: OverflowError("o"), "ZeroDivisionError": lambda: ZeroDivisionError("z"), "NotImplementedError": lambda: NotImplementedError("n"),
    "GeneratorExit": lambda: GeneratorExit(), "KeyboardInterrupt": lambda: KeyboardInterrupt(), "SystemExit": lambda: SystemExit(3), "MyError": lambda: MyError("m"), "MyBaseError": lambda: MyBaseError("b"), "the class TypeError": lambda: TypeError,
}


def behaviours():
    """What a special method can be: its name, and something that makes it."""
    for name, make in RETURNED.items():
        yield "returns " + name, lambda make=make: lambda *arguments, **keywords: count() or make()
    yield "returns self", lambda: lambda self, *arguments, **keywords: count() or self

    def raiser(make):
        def method(*arguments, **keywords):
            raise make()
        return method

    for name, make in RAISED.items():
        yield "raises " + name, lambda make=make: raiser(make)
    yield "takes nothing", lambda: lambda: 1
    yield "takes only self", lambda: lambda self: count() or 1
    yield "takes five", lambda: lambda self, a, b, c, d: 1
    yield "is None", lambda: None
    yield "is 5", lambda: 5
    yield "is 'a'", lambda: "a"
    yield "is a static method", lambda: staticmethod(lambda *arguments: count() or 1)
    yield "is a class method", lambda: classmethod(lambda *arguments: count() or 1)
    yield "is a property", lambda: property(lambda self: lambda *arguments: count() or 1)
    yield "is something that can be called", Callable
    yield "is len", lambda: len
    yield "is int", lambda: int
    yield "is a generator function", lambda: lambda *arguments: (yield 1)
    yield "is a coroutine function", lambda: coroutine_function


async def coroutine_function(*arguments):
    return 1


def show(value, depth=0):
    kind = type(value)
    if kind.__name__ in ("X", "Y", "Meta"):
        return "<" + kind.__name__ + ">"
    if isinstance(value, type) and value.__name__ in ("X", "Y", "Meta"):
        return "<class " + value.__name__ + ">"
    if depth < 3 and kind in (list, tuple):
        return kind.__name__ + "(" + ", ".join(show(item, depth + 1) for item in value) + ")"
    if depth < 3 and kind is dict:
        return "{" + ", ".join(show(key, depth + 1) + ": " + show(item, depth + 1) for key, item in value.items()) + "}"
    if depth < 3 and kind in (set, frozenset):
        return kind.__name__ + "{" + ", ".join(sorted(show(item, depth + 1) for item in value)) + "}"
    try:
        text = repr(value)
    except BaseException as error:
        text = "<repr raised " + type(error).__name__ + ">"
    return kind.__name__ + ":" + text.partition(" at 0x")[0][:200]


def run(awaitable_function):
    """What comes of a coroutine function, when there is nothing to wait for."""
    c = awaitable_function()
    try:
        for step in range(10):
            c.send(None)
        c.close()
        return "still waiting"
    except StopIteration as stop:
        return stop.value


# ---- What calls what. Each is given an instance of a class that has the method, and the class.


def statement(source):
    """A function of x, and of X, that does what a statement does."""
    variables = {"run": run}
    exec("def use(x, X):\n" + "\n".join("    " + line for line in source.split("\n")), variables)
    return variables["use"]


def binary(operator, name):
    return {
        "__%s__" % name: ["x %s 1" % operator, "x %s x" % operator, "x %s 1.5" % operator, "x %s 'a'" % operator, "x %s None" % operator, statement("y = x\ny %s= 1\nreturn y" % operator)],
        "__r%s__" % name: ["1 %s x" % operator, "1.5 %s x" % operator, "'a' %s x" % operator, "None %s x" % operator, "[] %s x" % operator, "x %s x" % operator, statement("y = 1\ny %s= x\nreturn y" % operator)],
        "__i%s__" % name: [statement("y = x\ny %s= 1\nreturn y" % operator), statement("y = [x]\ny[0] %s= 1\nreturn y" % operator), statement("y = x\ny %s= x\nreturn y" % operator), "x %s 1" % operator],
    }


USES = {
    "__len__": ["len(x)", "bool(x)", "not x", "1 if x else 2", "list(reversed(x))", "x[-1]", "[*x]", "all([x])", "x and 1", "(lambda *a: a)(*x)", "(lambda *a: a)(0, *x)", "(*x,)", "tuple(x)", "list(x)", "sorted(x)", "''.join(map(str, x))", "set(x)", "[].extend(x)", "bytes(x)", "bytearray(x)", "bytearray().extend(x)", statement("l = [0]\nl += x\nreturn l"),
                statement("l = [0]\nl[:] = x\nreturn l"), statement("a, *b = x\nreturn a, b"), statement("a, b = x\nreturn a, b"), statement("match x:\n    case [a, b]:\n        return a, b\n    case _:\n        return 'no'")],
    "__bool__": ["bool(x)", "not x", "1 if x else 2", "x and 1", "x or 1", "all([x])", "any([x])", "list(filter(None, [x]))", statement("while x:\n    return 1\nreturn 2"), statement("assert x, 'no'"), "[1 for y in [1] if x]", statement("if not x:\n    return 1\nreturn 2"), "x is True",
                 statement("match 1:\n    case 1 if x:\n        return 1\n    case _:\n        return 2")],
    "__hash__": ["hash(x)", "{x}", "{x: 1}", "x in {1}", "x in {1: 2}", "frozenset([x])", "{}.get(x)", "{1: 2}.get(x)", "dict.fromkeys([x])", "{1}.discard(x)", "hash((x,))", "{1: 2}.pop(x, 3)", "{}.pop(x, 3)", "{}.setdefault(x, 1)", "set().add(x)", statement("d = {}\nd[x] = 1\nreturn len(d)")],
    "__index__": ["[1, 2, 3][x]", "range(x)", "'a' * x", "x * 'a'", "bin(x)", "hex(x)", "oct(x)", "int(x)", "slice(x).indices(5)", "bytes(x)", "chr(x)", "[1, 2, 3][x:]", "[1, 2, 3][:x]", "[1, 2, 3][::x]", "round(1.5, x)", "(5).to_bytes(x)", "float(x)", "complex(x)", "'abc'[x]", "(1, 2, 3)[x]", "b'abc'[x]",
                  "range(5)[x]", "'%d' % x", "'%x' % x", "'%c' % x", "format(5, 'd').zfill(x)", "[].insert(x, 1)", "[1, 2].pop(x)", "bytearray(b'ab').pop(x)", "int('1', x)", "pow(2, x, 5)", "1 << x", "divmod(5, x)", "'a'.center(x)", "'abc'.find('b', x)", "memoryview(b'abc')[x]", "bytearray([x])",
                  "bytes([x])", "list(range(0, 5, x))", "enumerate([], x)", "'{:{}}'.format(1, x)", "sum([x])", "1 + x", "x.__index__()", "(1).__add__(x)", "[0] * x", "isinstance(x, int)"],
    "__int__": ["int(x)", "float(x)", "'%d' % x", "'%x' % x", "round(x)", "range(x)", "[1, 2][x]", "complex(x)", "bytes(x)", "chr(x)", "int(x, 10)"],
    "__float__": ["float(x)", "complex(x)", "complex(1, x)", "int(x)", "'%f' % x", "'%g' % x", "'%d' % x", "round(x)", "1.5 + x", "float.__add__(1.5, x)", "format(x, 'f')", "sum([x], 0.0)", "float.fromhex(x)", "pow(x, 2)", "abs(x)"],
    "__complex__": ["complex(x)", "complex(x, 1)", "complex(1, x)", "float(x)", "1j + x", "abs(x)"],
    "__trunc__": ["int(x)", "round(x)"],
    "__round__": ["round(x)", "round(x, 1)", "round(x, None)", "round(x, ndigits=2)", "round(x, 'a')"],
    "__abs__": ["abs(x)"], "__neg__": ["-x", "--x"], "__pos__": ["+x"], "__invert__": ["~x", "not ~x"],
    "__repr__": ["repr(x)", "f'{x!r}'", "'%r' % x", "'%r' % (x,)", "str([x])", "repr((x,))", "repr({x: x}) if X.__hash__ else 0", "ascii(x)", "'{!r}'.format(x)", "str(x)", "format(x)", "f'{x}'", "'%s' % x", "'%a' % x", "repr({1: x})", "str(x) + 'a'", "print(x, file=sink)"],
    "__str__": ["str(x)", "f'{x}'", "f'{x!s}'", "'%s' % x", "format(x)", "'{}'.format(x)", "'{!s}'.format(x)", "repr(x)[:2]", "str([x])[:3]", "''.join([str(x)])", "str(x, 'ascii')", "f'{x:}'", "f'{x:>5}'", "MyStr(x)", "type(MyStr(x)).__name__", "type(str(x)).__name__", "print(x, file=sink)"],
    "__format__": ["format(x)", "format(x, '')", "format(x, 'a')", "f'{x}'", "f'{x:a}'", "f'{x!r:a}'[:2]", "'{}'.format(x)", "'{:a}'.format(x)", "'{0:{1}}'.format(x, 'b')", "format(x, MyStr('a'))", "str(x)[:2]", "'%s' % x != ''", "type(format(x)).__name__"],
    "__bytes__": ["bytes(x)", "bytearray(x)", "b'%b' % x", "b'%s' % x", "b''.join([x])", "bytes(x, 'ascii')", "MyBytes(x)", "type(bytes(x)).__name__", "b'a' + x", "memoryview(x)"],
    "__iter__": ["iter(x)", "list(x)", "tuple(x)", "[*x]", "(*x,)", "{*x}", "[y for y in x]", "1 in x", "1 not in x", "zip(x)", "list(zip(x, x))", "list(map(str, x))", "sum(x)", "min(x)", "max(x)", "sorted(x)", "dict.fromkeys(x)", "set(x)", "frozenset(x)", "''.join(x)", "b''.join(x)", "list(enumerate(x))",
                 "all(x)", "any(x)", "print(*x, file=sink)", "dict(x)", "list(filter(None, x))", "bytes(x)", "bytearray(x)", "[].extend(x)", "{1}.union(x)", "{1}.issubset(x)", "list(reversed(x))", "next(x)", "next(iter(x))", statement("a, b = x\nreturn a, b"), statement("a, *b = x\nreturn a, b"),
                 statement("for y in x:\n    return y\nreturn 'nothing'"), statement("def g():\n    r = yield from x\n    return r\nreturn list(g())"), statement("l = [0]\nl[:] = x\nreturn l"), statement("l = [0]\nl += x\nreturn l"), statement("match x:\n    case [a, b]:\n        return a, b\n    case _:\n        return 'no'"),
                 statement("async def f():\n    return [y async for y in x]\nreturn run(f)"), "(lambda *a: a)(*x)", "{}.update(x)", "set().update(x)", "(y for y in x)", "list(y for y in x)", "isinstance(iter(x), type(iter(x)))"],
    "__next__": ["next(x)", "next(x, 'default')", "list(x)", "[*x][:3]", "iter(x) is x", "1 in x", "sum(x)", "list(zip(x, [1, 2]))", "list(zip([1, 2], x))", "list(map(str, x))[:3]", "min(x)", "dict(x)", "''.join(x)", "all(x)", "any(x)", statement("a, b = x\nreturn a, b"), statement("a, *b = x\nreturn a"),
                 statement("for y in x:\n    return y\nreturn 'nothing'"), statement("n = 0\nfor y in x:\n    n += 1\n    if n > 3:\n        break\nelse:\n    return 'to the end', n\nreturn n"), statement("def g():\n    r = yield from x\n    return 'returned', r\ng = g()\nreturn [next(g, 'end') for i in range(3)]"),
                 "list(enumerate(x))[:2]", "list(filter(None, x))[:2]", "sorted(x)", "tuple(x)[:3]", "set(x)", "next(iter(x))", "list(iter(x.__next__, 1))[:3]"],
    "__getitem__": ["x[0]", "x[-1]", "x[1:2]", "x['a']", "x[()]", "x[1, 2]", "x[...]", "x[None]", "x[:]", "x[::2]", "x[1:2, 3]", "x[x]", "iter(x)", "list(x)[:3]", "1 in x", "[*x][:3]", "list(reversed(x))", "list(zip(x, [1]))", "sum(x)", "'%(a)s' % x", "'{0[a]}'.format(x)", "'{a}'.format_map(x)", "dict(x)",
                    "{**x}", "(lambda **k: k)(**x)", statement("a, b = x\nreturn a, b"), statement("for y in x:\n    return y\nreturn 'nothing'"), statement("match x:\n    case {'a': a}:\n        return a\n    case [a]:\n        return a\n    case _:\n        return 'no'"), "x[0][0]", "next(iter(x))", "min(x)",
                    "tuple(x)[:2]", "x[*(1, 2)]", "x[*()]", "x[1:2:3]", "x[MyInt(1)]"],
    "__setitem__": [statement("x[0] = 1"), statement("x['a'] = 1"), statement("x[1:2] = [1]"), statement("x[1, 2] = 1"), statement("x[0] += 1"), statement("x[0], x[1] = 1, 2"), statement("for x[0] in [1]:\n    pass"), statement("x[:] = x"), statement("x[0] = x[1] = 1"),
                    statement("[x[0], *x[1]] = 1, 2")],
    "__delitem__": [statement("del x[0]"), statement("del x['a']"), statement("del x[1:2]"), statement("del x[1, 2]"), statement("del x[0], x[1]"), statement("del (x[0], [x[1]])"), statement("del x[:]")],
    "__contains__": ["1 in x", "1 not in x", "x in x", "None in x", "'a' in x", "(1 in x) is True", "[1 in x]", "1 in x in x", statement("if 1 in x:\n    return 'yes'\nreturn 'no'"), statement("match 1:\n    case y if y in x:\n        return 'yes'\n    case _:\n        return 'no'"), "any(y in x for y in [1])"],
    "__reversed__": ["reversed(x)", "list(reversed(x))", "[*reversed(x)]", "next(reversed(x))", statement("for y in reversed(x):\n    return y\nreturn 'nothing'")],
    "__length_hint__": ["list(x)", "tuple(x)", "[*x]", "bytes(x)", "bytearray(x)", "[].extend(x)", "list(map(str, x))", "list(zip(x))", "sorted(x)", "set(x)", "''.join(map(str, x))", "x.__length_hint__()", "(lambda *a: a)(*x)", "(lambda *a: a)(0, *x)", "(*x,)", "bytearray().extend(x)", "b''.join(x)", "''.join(x)", "list(reversed(list(x)))", "dict.fromkeys(x)", "min(x)",
                        statement("l = [0]\nl += x\nreturn l"), statement("l = [0]\nl[:] = x\nreturn l"), statement("a, *b = x\nreturn a, b"), statement("a, b = x\nreturn a, b")],
    "__call__": ["x()", "x(1)", "x(a=1)", "x(*[1], **{'a': 1})", "callable(x)", "list(map(x, [1]))", "list(filter(x, [1]))", "sorted([2, 1], key=x)", "min([2, 1], key=x)", "list(iter(x, 1))[:2]", "x()()", statement("@x\ndef f():\n    pass\nreturn f"), statement("@x\nclass C:\n    pass\nreturn C"),
                 "type(x).__call__(x)", "x.__call__()", "isinstance(x, type(len))", "max([1, 2], key=x)", "(lambda f: f())(x)"],
    "__enter__": [statement("with x as y:\n    return y"), statement("with x:\n    return 'in'"), statement("with x as (a, b):\n    return a, b"), statement("with x, x:\n    return 'in'"), statement("with (x as y, x as z):\n    return y, z"), statement("async def f():\n    async with x:\n        return 'in'\nreturn run(f)")],
    "__exit__": [statement("with x:\n    pass\nreturn 'after'"), statement("with x:\n    raise ValueError('inside')\nreturn 'after'"), statement("with x:\n    return 'returned'"), statement("for i in [1]:\n    with x:\n        break\nreturn 'after'"), statement("for i in [1]:\n    with x:\n        continue\nreturn 'after'"),
                 statement("with x:\n    raise KeyboardInterrupt\nreturn 'after'"), statement("with x, x:\n    raise ValueError('inside')\nreturn 'after'"), statement("try:\n    with x:\n        raise ValueError('inside')\nexcept ValueError as e:\n    return 'caught', e.__context__\nreturn 'after'"),
                 statement("with x:\n    raise StopIteration\nreturn 'after'"), statement("def g():\n    with x:\n        yield 1\nreturn list(g())"), statement("def g():\n    with x:\n        yield 1\ng = g()\nnext(g)\ng.close()\nreturn 'closed'"), statement("with x:\n    1 / 0\nreturn 'after'")],
    "__eq__": ["x == 1", "1 == x", "x == x", "x != 1", "1 != x", "x in [1]", "1 in [x]", "x in (x,)", "[x] == [1]", "[1] == [x]", "(x,) == (1,)", "[x].index(1)", "[x].count(1)", "[1].index(x)", "{1: x} == {1: 1}", "x == None", "None == x", "[x].remove(1)", "x is x", "hash(x)", "{x}", "x == 1 == 2", "not x == 1",
               "(x == 1) is True", "[1, x].count(x)", "x in {1: 1}.values()", "(1, x) == (1, 1)", "(1, x) < (1, 1)", statement("match 1:\n    case x.a:\n        return 'yes'\n    case _:\n        return 'no'"), statement("match x:\n    case 1:\n        return 'yes'\n    case _:\n        return 'no'"),
               statement("match x:\n    case None:\n        return 'yes'\n    case _:\n        return 'no'"), statement("if x == 1:\n    return 'yes'\nreturn 'no'"), "range(3).index(x)", "range(3).count(x)", "x in range(3)", "b'a' == x", "'a' == x", "1.5 == x", "[] == x", "x == NotImplemented"],
    "__ne__": ["x != 1", "1 != x", "x != x", "x == 1", "[x] != [1]", "(x,) != (1,)", "not x != 1", "x != None", "{1: x} != {1: 1}", "x != 1 != 2"],
    "__lt__": ["x < 1", "1 < x", "x < x", "1 > x", "x > 1", "sorted([x, x])", "sorted([x, 1])", "sorted([1, x])", "min(x, x)", "min(x, 1)", "min(1, x)", "max(x, 1)", "max(1, x)", "[x] < [1]", "[1] < [x]", "(x,) < (1,)", "x < 1 < 2", "[x, x].sort()", "x <= 1", "not x < 1", "min([x, x])", "x < None", "x < 'a'",
               "sorted([x, x], reverse=True)", "1.5 < x", "1.5 > x"],
    "__le__": ["x <= 1", "1 <= x", "1 >= x", "x <= x", "[x] <= [1]", "(x,) <= (1,)", "x < 1", "x >= 1", "{1} <= x", "x <= {1}"],
    "__gt__": ["x > 1", "1 > x", "1 < x", "x > x", "max(x, x)", "max(x, 1)", "max(1, x)", "min(1, x)", "[x] > [1]", "sorted([1, x])", "sorted([x, 1])", "x < 1", "max([x, x])"],
    "__ge__": ["x >= 1", "1 >= x", "1 <= x", "x >= x", "[x] >= [1]", "x > 1", "x <= 1"],
    "__getattr__": ["x.a", "x.__len__", "getattr(x, 'a')", "getattr(x, 'a', 'default')", "hasattr(x, 'a')", "x.a.b", "len(x)", "x.__class__ is X", "x.__dict__", "x()", "x.a()", "iter(x)", "str(x)[:2]", "x + 1", "dir(x)[:1]", "x.__name__", "callable(x)", "bool(x)", "hash(x) is not None", "x == x",
                    statement("x.a = 1\nreturn x.a"), statement("del x.a"), statement("x.a += 1\nreturn x.a"), statement("match x:\n    case X(a=1):\n        return 'yes'\n    case _:\n        return 'no'"), statement("with x:\n    pass"), "vars(x)", "isinstance(x, X)", "[y.a for y in [x]]", "x.__doc__", "x.__module__"],
    "__getattribute__": ["x.a", "x.__class__", "x.__dict__", "getattr(x, 'a', 'default')", "hasattr(x, 'a')", "type(x) is X", "len(x)", "str(x)[:2]", "repr(x)[:2]", "x == x", "hash(x) is not None", "isinstance(x, X)", "isinstance(x, int)", "x.__getattribute__", "dir(x)[:1]", "vars(x)", "callable(x)", "bool(x)",
                         statement("x.a = 1\nreturn x.a"), "x.a()", "object.__getattribute__(x, '__class__') is X", "x.__doc__", statement("match x:\n    case X(a=1):\n        return 'yes'\n    case _:\n        return 'no'"), "issubclass(type(x), X)", "super(X, x).__class__ is X", "x.__init__"],
    "__setattr__": [statement("x.a = 1\nreturn x.__dict__"), "setattr(x, 'a', 1)", statement("x.a += 1"), statement("x.a = x.b = 1\nreturn x.__dict__"), statement("x.__class__ = X"), statement("x.__dict__ = {}"), statement("for x.a in [1]:\n    pass\nreturn x.__dict__"), "object.__setattr__(x, 'a', 1)",
                    statement("x.a, x.b = 1, 2\nreturn x.__dict__"), statement("X.a = 1\nreturn X.a"), statement("import sys as y\nx.a = y")],
    "__delattr__": [statement("del x.a"), "delattr(x, 'a')", statement("x.__dict__['a'] = 1\ndel x.a\nreturn x.__dict__"), statement("del x.a, x.b"), "object.__delattr__(x, 'a')", statement("del x.__dict__")],
    "__dir__": ["dir(x)", "dir(X)[:1]", "x.__dir__()", "sorted(dir(x))", "len(dir(x))", "type(dir(x)).__name__"],
    "__init__": ["X()", "X(1)", "X(a=1)", "X.__new__(X)", "x.__init__()", "type(X()) is X", "X.__call__()", "type.__call__(X)", "[X() for i in [1]]", statement("class Y(X):\n    pass\nreturn Y()"), statement("class Y(X):\n    def __init__(self):\n        return super().__init__()\nreturn Y()")],
    "__new__": ["X()", "X(1)", "X(a=1)", "X.__new__(X)", "X.__new__()", "type(X())", "isinstance(X(), X)", "type.__call__(X)", statement("class Y(X):\n    pass\nreturn Y()"), statement("class Y(X):\n    def __init__(self, *a):\n        self.a = 1\nreturn getattr(Y(), 'a', 'not initialized')"), "object.__new__(X)"],
    "__get__": ["Y.a", "Y().a", "getattr(Y, 'a')", "getattr(Y(), 'a', 'default')", "hasattr(Y(), 'a')", "Y.__dict__['a'] is x", "Y().a()", "Y.a.b", "vars(Y)['a'] is x", "super(Z, Z()).a", "Z().a", "Z.a", statement("y = Y()\ny.__dict__['a'] = 'own'\nreturn y.a"), statement("y = Y()\ny.a = 1\nreturn y.a"),
                "type(Y()).a", "[y.a for y in [Y()]]", statement("match Y():\n    case Y(a=1):\n        return 'yes'\n    case _:\n        return 'no'"), "dir(Y())[-1:]", "len(Y2())", "Y2().__len__", "bool(Y2())"],
    "__set__": [statement("y = Y()\ny.a = 1\nreturn y.__dict__"), "setattr(Y(), 'a', 1)", statement("Y.a = 1\nreturn Y.a"), statement("y = Y()\ny.a += 1"), statement("y = Y()\ny.__dict__['a'] = 'own'\nreturn y.a is x"), statement("y = Y()\ndel y.a"), "Y().a is x", statement("y = Y()\nfor y.a in [1]:\n    pass\nreturn y.__dict__"),
                "object.__setattr__(Y(), 'a', 1)"],
    "__delete__": [statement("y = Y()\ndel y.a"), "delattr(Y(), 'a')", statement("del Y.a\nreturn hasattr(Y, 'a')"), statement("y = Y()\ny.a = 1\nreturn y.__dict__"), statement("y = Y()\ny.__dict__['a'] = 'own'\nreturn y.a is x"), "object.__delattr__(Y(), 'a')"],
    "__set_name__": [statement("class C:\n    a = x\nreturn C.a is x"), "type('C', (), {'a': x}).a is x", statement("class C:\n    a = x\n    b = x\nreturn 'made'"), statement("class C:\n    pass\nC.a = x\nreturn 'made'"), statement("class C:\n    a = X\nreturn 'made'")],
    "__init_subclass__": [statement("class C(X):\n    pass\nreturn 'made'"), statement("class C(X, a=1):\n    pass\nreturn 'made'"), "type('C', (X,), {}).__name__", statement("class C(X):\n    pass\nclass D(C):\n    pass\nreturn 'made'"), "X.__init_subclass__()", "type('C', (X,), {}, a=1).__name__"],
    "__class_getitem__": ["X[int]", "X[1, 2]", "X[()]", "x[int]", "X[int][str]", "X[:]", "type(X[int]).__name__", statement("def f(a: X[int]):\n    pass\nreturn f.__annotations__"), statement("class C(X[int]):\n    pass\nreturn C.__mro__[1:2]"), "X.__class_getitem__(int)", "X[X]"],
    "__mro_entries__": [statement("class C(x):\n    pass\nreturn C.__bases__, C.__dict__.get('__orig_bases__') == (x,)"), statement("class C(x, int):\n    pass\nreturn C.__bases__"), "type('C', (x,), {})", statement("class C(int, x):\n    pass\nreturn C.__bases__"), statement("class C(x, x):\n    pass\nreturn C.__bases__")],
    "__instancecheck__": ["isinstance(1, M)", "isinstance(M(), M)", "isinstance(1, (int, M))", "isinstance(1, (M, int))", "isinstance('a', (int, M))", "isinstance(1, M | int)", "isinstance('a', int | M)", statement("try:\n    raise ValueError\nexcept M:\n    return 'caught'"),
                          statement("match 1:\n    case M():\n        return 'yes'\n    case _:\n        return 'no'"), "isinstance(M, M)", "isinstance(1, x)"],
    "__subclasscheck__": ["issubclass(int, M)", "issubclass(M, M)", "issubclass(int, (str, M))", "issubclass(int, (M, int))", "issubclass(1, M)", "issubclass(int, M | str)", statement("try:\n    raise ValueError\nexcept M:\n    return 'caught'"), "issubclass(M, int)", "issubclass(int, x)"],
    "__prepare__": [statement("class C(metaclass=Meta):\n    a = 1\nreturn C.a"), statement("class C(metaclass=Meta):\n    pass\nreturn 'made'"), statement("class C(M):\n    a = 1\nreturn C.a"), "Meta('C', (), {}).__name__", statement("class C(metaclass=Meta, b=1):\n    pass\nreturn 'made'"),
                    statement("class C(metaclass=Meta):\n    def f(self):\n        return __class__\nreturn C().f() is C")],
    "__await__": [statement("async def f():\n    return await x\nreturn run(f)"), statement("async def f():\n    return [await x, await x]\nreturn run(f)"), "x.__await__()", statement("async def f():\n    async with x:\n        pass\nreturn run(f)"), statement("def g():\n    return (yield from x)\nreturn list(g())"),
                  statement("async def f():\n    return await x.__await__()\nreturn run(f)"), statement("async def f():\n    try:\n        return await x\n    except BaseException as e:\n        return 'caught', type(e).__name__\nreturn run(f)")],
    "__aiter__": [statement("async def f():\n    async for y in x:\n        return y\n    return 'nothing'\nreturn run(f)"), statement("async def f():\n    return [y async for y in x]\nreturn run(f)"), "aiter(x)", statement("async def f():\n    return {y async for y in x}\nreturn run(f)"),
                  statement("async def f():\n    return await anext(aiter(x))\nreturn run(f)"), statement("for y in x:\n    return y"), statement("async def g():\n    async for y in x:\n        yield y\nasync def f():\n    return [y async for y in g()]\nreturn run(f)")],
    "__anext__": [statement("async def f():\n    async for y in x:\n        return y\n    return 'nothing'\nreturn run(f)"), statement("async def f():\n    return [y async for y in x][:3]\nreturn run(f)"), "anext(x)", "anext(x, 'default')", statement("async def f():\n    return await anext(x)\nreturn run(f)"),
                  statement("async def f():\n    return await anext(x, 'default')\nreturn run(f)"), statement("async def f():\n    n = 0\n    async for y in x:\n        n += 1\n        if n > 3:\n            break\n    else:\n        return 'to the end', n\n    return n\nreturn run(f)"), "next(x)"],
    "__aenter__": [statement("async def f():\n    async with x as y:\n        return y\nreturn run(f)"), statement("async def f():\n    async with x:\n        return 'in'\nreturn run(f)"), statement("async def f():\n    async with x, x:\n        return 'in'\nreturn run(f)"), statement("with x:\n    return 'in'"),
                   statement("async def f():\n    async with x as (a, b):\n        return a, b\nreturn run(f)")],
    "__aexit__": [statement("async def f():\n    async with x:\n        pass\n    return 'after'\nreturn run(f)"), statement("async def f():\n    async with x:\n        raise ValueError('inside')\n    return 'after'\nreturn run(f)"), statement("async def f():\n    async with x:\n        return 'returned'\nreturn run(f)"),
                  statement("async def f():\n    for i in [1]:\n        async with x:\n            break\n    return 'after'\nreturn run(f)"), statement("async def f():\n    async with x:\n        raise KeyboardInterrupt\n    return 'after'\nreturn run(f)")],
    "__missing__": ["D()[1]", "D({1: 2})[1]", "D().get(1)", "1 in D()", "D().pop(1, 'default')", "D().setdefault(1, 2)", "'%(a)s' % D()", "'{a}'.format_map(D())", "D().__getitem__(1)", "dict.__getitem__(D(), 1)", "{**D()}", "D()[[]]", statement("d = D()\nd[1] += 1\nreturn d"), "D()[1][2]",
                    statement("match D():\n    case {1: a}:\n        return a\n    case _:\n        return 'no'"), "eval('a', D())", "eval('a', {}, D())", "x[1]"],
    "__buffer__": ["memoryview(x)", "bytes(x)", "bytearray(x)", "b'a' + x", "b''.join([x])", "b'a'.startswith(x)", "memoryview(x).tolist()", "int.from_bytes(x)", "bytearray(b'a').extend(x)", "b'a' == x", "str(x, 'ascii')", "b'%b' % x", "x.__buffer__(0)", "b'a' in x", "x in b'a'", "b'a'.find(x)",
                   statement("with memoryview(x) as m:\n    return bytes(m)"), "int(x)", "float(x)", "compile(x, 'f', 'eval')", "bytes(memoryview(x))"],
    "__match_args__": [statement("match x:\n    case X(1):\n        return 'yes'\n    case _:\n        return 'no'"), statement("match x:\n    case X(a):\n        return a\n    case _:\n        return 'no'"), statement("match x:\n    case X(a, b):\n        return a, b\n    case _:\n        return 'no'"),
                       statement("match x:\n    case X():\n        return 'yes'\n    case _:\n        return 'no'"), statement("match x:\n    case X(a, a=1):\n        return a\n    case _:\n        return 'no'"), statement("match x:\n    case X(a, b, c):\n        return a, b, c\n    case _:\n        return 'no'"),
                       statement("match x:\n    case X(a=1):\n        return 'yes'\n    case _:\n        return 'no'")],
    "__slots__": [], "__hash__ and __eq__": [],
    "__fspath__": ["str(compile('1', x, 'eval').co_filename)"],
    "__sizeof__": ["x.__sizeof__()"],
    "__getnewargs__": [], "__copy__": [],
    "__bases__": [], "__class__": ["x.__class__", "isinstance(x, int)", "isinstance(x, X)", "type(x) is X", "isinstance(x, (str, int))", "x.__class__.__name__", "issubclass(x.__class__, int)", "repr(x)[:3]", statement("match x:\n    case int():\n        return 'an int'\n    case X():\n        return 'an X'")],
    "__name__": [], "__doc__": ["x.__doc__", "X.__doc__"],
    "__annotations__": ["X.__annotations__", "x.__annotations__"], "__annotate__": ["X.__annotations__", "X.__annotate__", "X.__annotate__(1)"],
    "__dict__": ["x.__dict__", "vars(x)", "x.a", "dir(x)[:1]", statement("x.a = 1\nreturn x.a")],
    "__type_params__": ["X.__type_params__"], "__orig_bases__": ["X.__orig_bases__"], "__qualname__": [], "__module__": ["X.__module__", "repr(X)", "repr(x)[:12]", "x.__module__"],
    "__firstlineno__": ["X.__firstlineno__"], "__static_attributes__": ["X.__static_attributes__"], "__weakref__": ["x.__weakref__"],
}
for operator, name in (("+", "add"), ("-", "sub"), ("*", "mul"), ("/", "truediv"), ("//", "floordiv"), ("%", "mod"), ("**", "pow"), ("@", "matmul"), ("<<", "lshift"), (">>", "rshift"), ("&", "and"), ("|", "or"), ("^", "xor")):
    USES.update(binary(operator, name))
USES["__pow__"] += ["pow(x, 2)", "pow(x, 2, 3)", "pow(x, x, x)", "pow(x, 2, None)"]
USES["__rpow__"] += ["pow(2, x)", "pow(2, x, 3)", "pow(2, 2, x)"]
USES["__divmod__"] = ["divmod(x, 1)", "divmod(x, x)", "divmod(x, 'a')", statement("a, b = divmod(x, 1)\nreturn a, b")]
USES["__rdivmod__"] = ["divmod(1, x)", "divmod(1.5, x)", "divmod('a', x)", "divmod(x, x)"]
USES["__add__"] += ["sum([x])", "sum([x, x])", "sum([1], x)", "[] + x", "x + []", "'a' + x", "x + 'a'", "() + x", "b'' + x"]
USES["__radd__"] += ["sum([x])", "sum([1, x])", "sum([x], 1.5)", "() + x", "b'' + x", "1j + x", "True + x"]
USES["__mul__"] += ["x * []", "x * 'a'", "x * ()", "x * 2.0"]
USES["__rmul__"] += ["[1] * x", "'a' * x", "(1,) * x", "b'a' * x", "2.0 * x", "1j * x"]
USES["__mod__"] += ["x % ()", "x % (1,)", "x % {}"]
USES["__rmod__"] += ["'%s' % x", "'a' % x", "b'%s' % x", "'' % x", "MyStr('%s') % x"]
USES["__or__"] += ["x | int", "x | None", "x | {}", "x | {1}"]
USES["__ror__"] += ["int | x", "None | x", "{} | x", "{1} | x", "list[int] | x"]

# What else a class has to have for the method to be got at.
WITH = {
    "__next__": {"__iter__": lambda self: self},
    "__length_hint__": {"__iter__": lambda self: iter([1, 2])},
    "__len__": {"__getitem__": lambda self, key: [1, 2][key]},
    "__enter__": {"__exit__": lambda self, *arguments: None, "__aexit__": coroutine_function},
    "__exit__": {"__enter__": lambda self: self},
    "__aenter__": {"__aexit__": coroutine_function, "__exit__": lambda self, *arguments: None},
    "__aexit__": {"__aenter__": coroutine_function},
    "__anext__": {"__aiter__": lambda self: self},
    "__match_args__": {"a": 1, "b": 2},
    "__reversed__": {},
    "__eq__": {"a": 1},
}


def uses_of(name):
    for use in USES[name]:
        if isinstance(use, str):
            yield use, eval("lambda x, X, Y, Y2, Z, M, Meta, D: " + use, {"MyStr": MyStr, "MyInt": MyInt, "MyBytes": MyBytes, "run": run, "sink": Sink()})
        else:
            yield None, use


def attempt(name, make, use):
    global calls
    # Everything is made afresh, since a class remembers what it has been asked.
    try:
        namespace = dict(WITH.get(name, {}))
        namespace[name] = make()
        Meta = type("Meta", (type,), {name: namespace[name]} if name in ("__instancecheck__", "__subclasscheck__", "__prepare__") else {})
        X = type("X", (), namespace)
        x = object.__new__(X) if name in ("__init__", "__new__") else X()
    except BaseException as error:
        return "making it: " + type(error).__name__ + ": " + str(error).partition(" at 0x")[0]
    calls = 0
    try:
        if use.__code__.co_argcount == 2:
            result = use(x, X)
        else:
            Y = type("Y", (), {"a": x, "__match_args__": ("a",)})
            Y2 = type("Y", (), {"__len__": x})
            Z = type("Y", (Y,), {})
            M = Meta("X", (), {})
            D = type("Y", (dict,), {name: namespace[name]})
            result = use(x, X, Y, Y2, Z, M, Meta, D)
        return show(result)
    except RecursionError:
        return "RecursionError"
    except TooLong:
        return "goes on for ever"
    except BaseException as error:
        return type(error).__name__ + ": " + str(error).partition(" at 0x")[0]


everything = sys.argv[1:2] == ["everything"]
wanted = not everything and sys.argv[1:] and (sys.argv[1], sys.argv[2])

for name in USES:
    if not USES[name] or everything and sys.argv[2:] and name not in sys.argv[2:]:
        continue
    for label, make in behaviours():
        if wanted and wanted != (name, label):
            continue
        # These go on for ever without anything of the program's being called.
        if label in ("is int", "is a generator function", "is a coroutine function") and name in ("__next__", "__getitem__", "__anext__", "__call__"):
            continue
        results = []
        for index, (source, use) in enumerate(uses_of(name)):
            results.append("%s: %s" % (source or "statement %d" % index, attempt(name, make, use)))
        what = "%s %s" % (name, label)
        if everything:
            print("\n".join(what + " | " + result for result in results))
            continue
        if wanted:
            print("\n".join(results))
        print(what, "|", len(results), int.from_bytes("\n".join(results).encode("utf-8", "backslashreplace"), "big") % 1000000007)
