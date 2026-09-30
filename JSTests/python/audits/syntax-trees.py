# What comes of compiling a syntax tree that a program has made, and made wrongly. Each field of each node of the trees of some pieces of source is deleted, and is given each of a hundred or so values that
# mostly do not belong there, and if it is a list it is given each of them as one more element. Then the tree is compiled, once for the tree over again and once for code, and the code is run. What is looked at
# is the exception and what it says, or the tree, or what the variables are afterwards. It must never crash.
#
# There is a line for each field, with how many things were tried and a number that stands for all that came of them. To see them all:
#
#     syntax-trees.py [--] tree|code <which source> <which node> <Class.field>

import _ast
import _warnings
import sys
from _ast import *

_warnings.filters.insert(0, ("ignore", None, Warning, None, 0))

# Whether it is run as well says whether it can be without the standard library: a generic class wants typing, and to look for a module is to run a good deal of Python in CPython.
SOURCES = [
    (1, "x = 1"), (1, "x: int = 1"), (1, "x += 1"), (1, "del x, y.z, w[0]"), (1, "def f(a, /, b=1, *c, d=2, **e) -> int:\n  'doc'\n  return a"),
    (1, "async def f():\n  await x\n  async for a in b: pass\n  async with c as d: pass\n  return [e async for e in g]"), (1, "class C(B, m=M):\n  x = 1\n  def f(self): return super().f()"),
    (1, "for a, b in c:\n  break\nelse:\n  pass"), (1, "while a:\n  continue\nelse:\n  pass"), (1, "if a: b\nelif c: d\nelse: e"), (1, "with a as b, c: pass"),
    (1, "try: a\nexcept B as c: d\nexcept: e\nelse: f\nfinally: g"), (1, "try: a\nexcept* B as c: d"), (1, "raise a from b"), (1, "assert a, b"), (0, "import a.b as c, d"), (0, "from ..a import b as c, d"),
    (1, "def f():\n  global a\n  def g():\n    nonlocal b\n  b = 1"), (1, "a and b or not c"), (1, "a + b * -c"), (1, "a < b <= c is not d in e"), (1, "a if b else c"), (1, "lambda a, *b, c=1, **d: a"),
    (1, "{a: b, **c}"), (1, "{a, *b}"), (1, "[a, *b]"), (1, "(a, *b)"), (1, "[a for b in c if d]"), (1, "{a for b in c}"), (1, "{a: b for c in d}"), (1, "(a for b in c)"),
    (1, "def f():\n  yield\n  x = yield a\n  yield from b"), (1, "f(a, *b, c=d, **e)"), (1, "f'{a!r:{b}}c{d=}'"), (1, "t'{a!r:{b}}c'"), (1, "a.b.c"), (1, "a[b:c:d, e]"), (1, "(a := b)"),
    (1, "1; 2.5; 3j; 'a'; b'a'; None; True; ...; -1"), (1, "a, *b = c"), (1, "[a, b] = c"), (1, "a.b = c[d] = e"),
    (1, "match a:\n  case 1 | -2 | 3+4j | 'a' | None | b.c: pass\n  case [d, *e] | (d, e): pass\n  case {'h': i, **j}: pass\n  case K(l, m=n): pass\n  case o as p if q: pass\n  case _: pass"),
    (0, "def f[T: int, *U, **V = [int]](): pass"), (0, "class C[T]: pass"), (0, "type A[T] = list[T]"), (1, "@a\n@b(c)\ndef f(): pass"), (1, "@a\nclass C: pass"), (1, "x = *a, b"), (1, "print(a, sep='')"),
    (0, "from __future__ import annotations\nx: int"), (0, "from __future__ import annotations\nx: f'{a!r:{b}}c{d=}'\ny: t'{a}b'"),
    (0, "from __future__ import annotations\ndef f(a: b + c, *d: e[1:2], **g: (lambda h: i)) -> {j: k}: pass"), (0, "from __future__ import annotations\nclass C:\n  x: [a for b in c if d]\n  y: -1 ** 2 if a else not b"),
    (1, "x: int = 1\ny: 'str'\nz = __annotate__(1)"), (1, "class C:\n  x: a\n  if b:\n    y: c\nz = C.__annotations__"), (1, "def f(x: a = 1, *y: b) -> c: pass\nz = f.__annotations__"),
    (1, "z = [f(1) for f in [lambda x: x + a, lambda x, y=b: x * y]]"), (1, "def f():\n  v = a\n  def g():\n    nonlocal v\n    v += 1\n    return v\n  return g() + g()\nz = f()"),
    (1, "def f():\n  yield a\n  x = yield b\n  yield from c\nz = list(f())"), (1, "z = []\nfor i in c:\n  if i == 2: continue\n  z.append(i)\nelse:\n  z.append(-1)"), (1, "z = 0\nwhile z < 5:\n  z += 1\n  if z == 3: break"),
    (1, "try:\n  z = a / 0\nexcept ZeroDivisionError as e:\n  z = str(e)\nfinally:\n  y = 1"),
    (1, "try:\n  raise ExceptionGroup('g', [ValueError(1), TypeError(2)])\nexcept* ValueError as e:\n  z = repr(e)\nexcept* TypeError:\n  y = 2"), (1, "match c:\n  case [1, *r]: z = r\n  case _: z = 0"),
    (1, "match d:\n  case {'k': v, **r}: z = (v, r)"), (1, "match a:\n  case 1 | 2 as v if v: z = v"), (1, "match p:\n  case P(x=0, y=w): z = w\n  case P(u, w): z = (u, w)"), (1, "z = f'{a:>{b}}|{s!r}|{a=}'"),
    (1, "z = t'{a}x{s!r:>3}'\nz = (z.strings, [(i.value, i.expression, i.conversion, i.format_spec) for i in z.interpolations])"), (1, "x, *y = c\n[u, v] = y\nz = (x, y, u, v)"),
    (1, "z = {**d, 'n': a}\ny = {*c, a}\nx = [*c, *c]\nw = (*c,)"), (1, "z = c[0:2], c[::-1], c[1], d['k']"), (1, "with m as z: y = 1"), (1, "del c[0], d['k']\nz = (c, d)"), (1, "z = (y := a + b) * y"),
    (1, "z = a if b else c\ny = a and b or c\nx = not a"), (1, "z = a < b <= 2 != 3 is not None in [True]"), (1, "global z\nz = a"), (1, "assert a, s\nz = 1"), (1, "assert not a, s"),
    (1, "class C(P, metaclass=type):\n  __slots__ = ()\n  def f(self): return __class__\nz = C(1, 2).f().__name__"),
    (1, "z = 1; z += a; z -= 1; z *= 3; z //= 2; z **= 2; z %= 7; z <<= b; z >>= a; z |= 8; z ^= 3; z &= 14"), (1, "o.x = a\no.x += 1\nc[0] += 5\nz = (o.x, c)\ndel o.x"), (1, "z = -a, +a, ~a"),
]

AT = dict(lineno=1, col_offset=0, end_lineno=1, end_col_offset=1)


def load(name):
    return Name(name, Load(), **AT)


def store(name):
    return Name(name, Store(), **AT)


def loop(is_async=0):
    return comprehension(store("q"), load("c"), [], is_async)


class Odd:
    pass


def values():
    """They are made afresh each time, since a tree can be changed by what it is put in."""
    return [
        ("None", None), ("0", 0), ("-1", -1), ("1 << 40", 1 << 40), ("True", True), ("1.5", 1.5), ("'x'", "x"), ("'None'", "None"), ("b'x'", b"x"), ("[]", []), ("()", ()), ("a class", Odd), ("a name", load("n")),
        ("a name to store", store("n")), ("a name to delete", Name("a", Del(), **AT)), ("pass", Pass(**AT)), ("1", Constant(1, **AT)), ("[1] as a constant", Constant([1], **AT)), ("Load", Load()), ("Store", Store()),
        ("Del", Del()), ("Add", Add()), ("Eq", Eq()), ("AST", AST()), ("expr", expr()), ("*n", Starred(load("n"), Load(), **AT)), ("[a name]", [load("n")]), ("['x']", ["x"]), ("[1]", [1]), ("[pass]", [Pass(**AT)]),
        ("_", MatchAs(**AT)), ("*s in a pattern", MatchStar("s", **AT)), ("a parameter", arg("a", **AT)), ("parameters", arguments()), ("a tuple as a constant", Constant((1, "a", (None, ...), frozenset({2})), **AT)),
        ("-5", Constant(-5, **AT)), ("1+2j", Constant(1 + 2j, **AT)), ("-(1 << 100)", Constant(-(1 << 100), **AT)), ("infinity", Constant(1e400, **AT)), ("*n to store", Starred(store("n"), Store(), **AT)),
        ("*n to delete", Starred(Name("n", Del(), **AT), Del(), **AT)), ("[n] to store", List([store("n")], Store(), **AT)), ("n, *k to store", Tuple([store("n"), Starred(store("k"), Store(), **AT)], Store(), **AT)),
        ("[n] to delete", List([Name("n", Del(), **AT)], Del(), **AT)), ("o.x to store", Attribute(load("o"), "x", Store(), **AT)), ("c[0] to store", Subscript(load("c"), Constant(0, **AT), Store(), **AT)),
        ("o.x to delete", Attribute(load("o"), "x", Del(), **AT)), ("() to store", Tuple([], Store(), **AT)), ("f'' of a name", JoinedStr([load("a"), Constant("x", **AT)], **AT)),
        ("f'' of an int", JoinedStr([Constant(1, **AT), Constant("x", **AT)], **AT)), ("f'' of one name", JoinedStr([load("a")], **AT)), ("f''", JoinedStr([], **AT)),
        ("t'' of what is not str", TemplateStr([load("a"), Constant("x", **AT), Constant(2, **AT)], **AT)), ("{a} of t''", Interpolation(load("a"), "a", -1, None, **AT)),
        ("{a!B:1} of t''", Interpolation(load("a"), 5, 66, Constant(1, **AT), **AT)), ("{a:s}", FormattedValue(load("a"), -1, load("s"), **AT)), ("{a!r}", FormattedValue(load("a"), 114, None, **AT)),
        ("{n!A}", FormattedValue(load("n"), 65, None, **AT)), ("await", Await(load("a"), **AT)), ("lambda", Lambda(arguments(args=[arg("q", **AT)]), load("q"), **AT)), ("a generator", GeneratorExp(load("q"), [loop()], **AT)),
        ("a comprehension", ListComp(load("q"), [loop()], **AT)), ("an async comprehension", ListComp(load("q"), [loop(1)], **AT)), ("w := 7", NamedExpr(store("w"), Constant(7, **AT), **AT)),
        ("__debug__ to store", store("__debug__")), ("a name with a space", load("a b")), ("a name of nothing", load("")), ("return", Return(None, **AT)), ("break", Break(**AT)), ("a as a statement", Expr(load("a"), **AT)),
        ("global a", Global(["a"], **AT)), ("nonlocal a", Nonlocal(["a"], **AT)), ("from __future__", ImportFrom("__future__", [alias("annotations", **AT)], 0, **AT)), ("except:", ExceptHandler(None, None, [Pass(**AT)], **AT)),
        ("k=1", keyword("k", Constant(1, **AT), **AT)), ("**d", keyword(None, load("d"), **AT)), ("an alias", alias("sys", None, **AT)), ("with m", withitem(load("m"), None)), ("for q in c", loop()),
("case 1", MatchValue(Constant(1, **AT), **AT)), ("case 1+2j", MatchValue(Constant(1 + 2j, **AT), **AT)), ("case -1", MatchValue(Constant(-1, **AT), **AT)),
        ("case -(1 << 80)", MatchValue(UnaryOp(USub(), Constant(1 << 80, **AT), **AT), **AT)), ("case None", MatchSingleton(None, **AT)), ("case _: pass", match_case(MatchAs(**AT), None, [Pass(**AT)])),
        ("a slice", Slice(**AT)), ("yield", Yield(**AT)),
    ]


def is_left_out(node, name, value, is_element):
    # CPython falls over if there is None in one of these lists. Here it is a ValueError.
    if is_element and value is None and name in ("handlers", "patterns", "kwd_patterns", "type_params", "names", "kwd_attrs"):
        return True
    # What an interpolation says its source is can be anything at all in CPython, which finds out later or never that it cannot be kept, and where it writes it out again leaves an exception lying about if
    # it is not a str. Here it has to be what a constant can be.
    return type(node) is Interpolation and name == "str" and not isinstance(value, str)


def dump(node, out):
    if isinstance(node, AST):
        out.append(type(node).__name__ + "(")
        for name, value in vars(node).items():
            out.append(name + "=")
            dump(value, out)
            out.append(",")
        out.append(")")
    elif isinstance(node, list):
        out.append("[")
        for item in node:
            dump(item, out)
            out.append(",")
        out.append("]")
    else:
        out.append(type(node).__name__ + ":" + ascii(node))


class Limit(BaseException):
    pass


class P:
    __match_args__ = ("x", "y")

    def __init__(self, x=0, y=5):
        self.x = x
        self.y = y


class M:
    def __enter__(self):
        return 9

    def __exit__(self, *arguments):
        return False


def without_address(text):
    # Where something is in memory is nothing to go by.
    return text.partition(" at 0x")[0]


def simple(value, depth=0):
    if isinstance(value, (int, float, complex, str, bytes, type(None), type(...))):
        return without_address(repr(value))
    if depth < 3 and type(value) in (list, tuple):
        return type(value).__name__ + "(" + ",".join(simple(item, depth + 1) for item in value) + ")"
    if depth < 3 and type(value) in (set, frozenset):
        return type(value).__name__ + "(" + ",".join(sorted(simple(item, depth + 1) for item in value)) + ")"
    if depth < 3 and type(value) is dict:
        return "{" + ",".join(simple(key, depth + 1) + ":" + simple(item, depth + 1) for key, item in value.items()) + "}"
    return "<" + type(value).__name__ + ">"


def describe(error):
    return without_address(type(error).__name__ + ": " + str(error))


def run(code):
    steps = 0

    def trace(frame, event, argument):
        nonlocal steps
        steps += 1
        if steps > 400:
            raise Limit
        return trace

    variables = {"__name__": "m", "a": 1, "b": 2, "c": [1, 2, 3], "d": {"k": 4}, "s": "str", "p": P(), "P": P, "m": M(), "o": Odd(), "print": lambda *arguments, **keywords: None, "n": 6}
    before = set(variables) | {"__builtins__"}
    sys.settrace(trace)
    try:
        exec(code, variables)
        result = "ran"
    except Limit:
        result = "went on"
    except RecursionError:
        result = "RecursionError"
    except BaseException as error:
        result = describe(error)
    finally:
        sys.settrace(None)
    return result + " " + " ".join("%s=%s" % (name, simple(variables[name])) for name in sorted(variables) if (name not in before or name in "acdn") and not name.startswith("__"))


def attempt(tree, mode, is_run):
    try:
        # A module can await here whether or not that is asked for. So it is asked for, which is to compare like with like.
        result = compile(tree, "<test>", "exec", PyCF_ONLY_AST if mode == "tree" else PyCF_ALLOW_TOP_LEVEL_AWAIT)
    except RecursionError:
        return "RecursionError"
    except BaseException as error:
        return describe(error)
    if mode == "tree":
        out = []
        dump(result, out)
        return "".join(out)
    # exec() of what awaits runs it here, and in CPython makes a coroutine that nothing comes of.
    if result.co_flags & 0x80:
        return "code that awaits"
    return "code " + run(result) if is_run else "code"


def nodes_of(node, found):
    if isinstance(node, AST):
        found.append(node)
        for value in list(vars(node).values()):
            nodes_of(value, found)
    elif isinstance(node, list):
        for item in node:
            nodes_of(item, found)
    return found


wanted = sys.argv[1:] and (sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4])
for mode in ("tree", "code"):
    for source_index, (is_run, source) in enumerate(SOURCES):
        tree = compile(source, "<test>", "exec", PyCF_ONLY_AST)
        for node_index, node in enumerate(nodes_of(tree, [])):
            if isinstance(node, (expr_context, operator, boolop, unaryop, cmpop)):
                continue
            for name in list(vars(node)):
                field = type(node).__name__ + "." + name
                if wanted and wanted != (mode, source_index, node_index, field):
                    continue
                results = []

                def note(what):
                    results.append(what + ": " + attempt(tree, mode, is_run))

                original = getattr(node, name)
                note("as it is")
                delattr(node, name)
                note("deleted")
                for what, value in values():
                    if not is_left_out(node, name, value, False):
                        setattr(node, name, value)
                        note(what)
                if isinstance(original, list):
                    for what, value in values():
                        if not is_left_out(node, name, value, True):
                            setattr(node, name, original + [value])
                            note("with " + what)
                    setattr(node, name, original[1:])
                    note("without the first")
                    setattr(node, name, original + original)
                    note("twice")
                setattr(node, name, original)
                if wanted:
                    print("\n".join(results))
                print(mode, source_index, node_index, field, "|", len(results), int.from_bytes("\n".join(results).encode("utf-8", "backslashreplace"), "big") % 1000000007)
