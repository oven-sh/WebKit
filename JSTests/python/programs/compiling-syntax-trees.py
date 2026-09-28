# compile(tree, ...): code from a syntax tree, which a program may have made or changed.

import _warnings
import sys
from _ast import *

sys.stderr = sys.stdout
_warnings.filters.insert(0, ("ignore", None, Warning, None, 0))


def show(label, function):
    try:
        result = function()
    except RecursionError:
        # What it says has in it how much stack there was.
        result = "RecursionError"
    except BaseException as error:
        result = type(error).__name__ + ": " + str(error)
    print(label, "=>", result)


def parse(source, mode="exec", filename="<source>"):
    return compile(source, filename, mode, PyCF_ONLY_AST)


def run(tree, filename="<tree>", **variables):
    exec(compile(tree, filename, "exec"), variables)
    return variables


AT = dict(lineno=1, col_offset=0)

# ---- What was written, by way of its tree

SOURCE = '''
def fib(n):
    "doc"
    return n if n < 2 else fib(n - 1) + fib(n - 2)
class C:
    x: int = 3
    def m(self, *a, k=2, **kw): return (self.x, a, k, kw, __class__.__name__)
def outer(a, b=2, *c, d=4, **e):
    v = a
    @staticmethod
    def inner(x: int) -> str:
        nonlocal v
        v += x
        return (v, [i * x for i in range(3)], (lambda q: q + v)(1))
    return inner
def gen():
    yield 1
    yield from (i for i in "ab")
def boom():
    return (1 +
        None)
result = (fib(10), fib.__doc__, C().m(1, 2, k=5, z=1), C.__annotations__, outer(1)(2), list(gen()), f"{fib(5)!r:>4}|{fib(3)=}", outer(1).__annotations__)
'''
variables = run(parse(SOURCE))
print(variables["result"])
from_tree = compile(parse(SOURCE), "<both>", "exec")
from_source = compile(SOURCE, "<both>", "exec")


def all_code(code):
    yield code
    for constant in code.co_consts:
        if hasattr(constant, "co_code"):
            yield from all_code(constant)


for a, b in zip(all_code(from_source), all_code(from_tree), strict=True):
    print(a.co_qualname, [getattr(a, name) == getattr(b, name) for name in ("co_qualname", "co_firstlineno", "co_varnames", "co_freevars", "co_cellvars", "co_names", "co_flags", "co_argcount", "co_filename")],
          list(a.co_positions()) == list(b.co_positions()), list(a.co_lines()) == list(b.co_lines()))

# ---- The three ways of compiling
show("eval", lambda: eval(compile(parse("1 + 2", "eval"), "<tree>", "eval")))
show("single", lambda: exec(compile(parse("1 + 2", "single"), "<tree>", "single")))
for mode in ("exec", "eval", "single"):
    for tree in (Module([], []), Expression(Constant(1, **AT)), Interactive([]), Constant(1, **AT), AST(), FunctionType([], Constant(1, **AT))):
        show("%s of %s" % (mode, type(tree).__name__), lambda: type(compile(tree, "<tree>", mode)).__name__)
show("the tree over again", lambda: (tree := parse("x = 1"), again := compile(tree, "<tree>", "exec", PyCF_ONLY_AST), again is tree, again.body[0] is tree.body[0], again.body[0].targets[0].ctx is tree.body[0].targets[0].ctx, again)[2:])
show("the name of the file", lambda: (compile(Module([], []), "some name", "exec").co_filename, compile(Module([], []), b"bytes", "exec").co_filename))


class MyModule(Module):
    pass


class MyName(Name):
    pass


show("of classes derived from them", lambda: run(MyModule([Assign([MyName("x", Store(), **AT)], Constant(5, **AT), **AT)], []))["x"])

# ---- Constants that cannot be written
for value in (-5, -(1 << 100), 1 << 100, 1 + 2j, -0.0, complex(-0.0, -0.0), 1e400, -1e400, (), (1, "a", (None, ..., b"b")), frozenset(), frozenset({1}), (frozenset({(1, 2)}),), True, "\ud800", "\x00", b"\xff\x00", 0, 18446744073709551615, 18446744073709551616, -9223372036854775808):
    show("constant %r" % (value,), lambda: (result := eval(compile(Expression(Constant(value, **AT)), "<tree>", "eval")), type(result).__name__, result == value, repr(result) == repr(value))[1:])
show("nan", lambda: repr(eval(compile(Expression(Constant(float("nan"), **AT)), "<tree>", "eval"))))
show("among the constants", lambda: [compile(Expression(Constant(value, **AT)), "<tree>", "eval").co_consts for value in (-5, (1, 2), 1 + 2j, frozenset({3}))])


class MyInt(int):
    pass


for value in ([], {}, set(), MyInt(1), object(), (1, []), frozenset({(1, MyInt(2))}), bytearray(b"a"), range(1), int, Constant(1), lambda: 0):
    show("not a constant: %s" % type(value).__name__, lambda: compile(Expression(Constant(value, **AT)), "<tree>", "eval"))

# ---- Names that cannot be written
show("odd names", lambda: sorted(k for k in run(Module([Assign([Name(name, Store(), **AT)], Constant(1, **AT), **AT) for name in ("a b", "", "@x", "1", "é", "if", "\ud800")], [])) if k != "__builtins__"))
show("an odd attribute", lambda: run(parse("class O: pass\no = O()\no.x = 1").__replace__(body=parse("class O: pass\no = O()").body + [Assign([Attribute(Name("o", Load(), **AT), "a b", Store(), **AT)], Constant(1, **AT), **AT)]))["o"].__dict__)
for name in ("None", "True", "False", "__debug__"):
    show("the name %s" % name, lambda: compile(Module([Assign([Name(name, Store(), **AT)], Constant(1, **AT), **AT)], []), "<tree>", "exec"))
    show("loading %s" % name, lambda: eval(compile(Expression(Name(name, Load(), **AT)), "<tree>", "eval")))

# ---- Changing a tree
tree = parse("def f(x): return x + 1\ny = f(2)")
tree.body[0].body[0].value.op = Mult()
tree.body[0].body[0].value.right = Constant(10, lineno=1, col_offset=0)
show("changed", lambda: run(tree)["y"])
tree.body.append(tree.body[1])
tree.body[0].name = "g"
show("changed again", lambda: run(tree, f=lambda x: -x)["y"])
shared = Name("a", Load(), **AT)
show("the same node twice", lambda: eval(compile(Expression(BinOp(shared, Add(), shared, **AT)), "<tree>", "eval"), {"a": 4}))

# ---- Where it says that it is
tree = parse("def f():\n    return g()\ndef g():\n    raise ValueError('here')\nf()")
for node in [tree.body[0], tree.body[0].body[0], tree.body[0].body[0].value, tree.body[0].body[0].value.func]:
    node.lineno += 100
    node.end_lineno += 100
try:
    run(tree, "<moved>")
except ValueError as error:
    traceback = error.__traceback__.tb_next
    lines = []
    while traceback:
        lines.append((traceback.tb_frame.f_code.co_name, traceback.tb_lineno, traceback.tb_frame.f_code.co_firstlineno))
        traceback = traceback.tb_next
    print("lines", lines)
show("no place to end", lambda: list(compile(Expression(Name("a", Load(), lineno=3, col_offset=4)), "<tree>", "eval").co_positions())[-1])
show("a place to end", lambda: list(compile(Expression(Name("a", Load(), lineno=3, col_offset=4, end_lineno=5, end_col_offset=6)), "<tree>", "eval").co_positions())[-1])
show("None for a place to end", lambda: list(compile(Expression(Name("a", Load(), lineno=3, col_offset=4, end_lineno=None, end_col_offset=None)), "<tree>", "eval").co_positions())[-1])
show("an attribute on a later line", lambda: sorted(set(compile(parse("(a\n  .b\n  .c)", "eval"), "<tree>", "eval").co_positions())))
for place in (dict(lineno=2, col_offset=0, end_lineno=1, end_col_offset=0), dict(lineno=1, col_offset=5, end_lineno=1, end_col_offset=2), dict(lineno=-1, col_offset=0, end_lineno=1, end_col_offset=0), dict(lineno=1, col_offset=-1, end_lineno=1, end_col_offset=0),
              dict(lineno=1 << 40, col_offset=0), dict(lineno="1", col_offset=0), dict(lineno=1.0, col_offset=0), dict(lineno=None, col_offset=0), dict(lineno=True, col_offset=False), dict(col_offset=0), dict(lineno=1), dict(lineno=0, col_offset=0), dict(lineno=2147483647, col_offset=2147483647)):
    show("at %r" % (place,), lambda: type(compile(Expression(Name("a", Load(), **place)), "<tree>", "eval")).__name__)

# What is quoted is what is in the file that it is said to be from, in the place that it says.
HERE = 1000  # This line is quoted below, and the one after it.
THERE = HERE + None if HERE < 0 else 0
here = sys._getframe().f_lineno - 2
this_file = __file__.rpartition("/")[2]


def display(error):
    # Without what is in this file, whose name is not the same everywhere.
    error.__traceback__ = error.__traceback__.tb_next.tb_next
    sys.excepthook(type(error), error, None)


tree = parse("def f():\n    return 1000 + None\nf()")
addition = tree.body[0].body[0].value
for node, first, last in ((addition, 8, 19), (addition.left, 8, 12), (addition.right, 15, 19), (tree.body[0].body[0], 0, 19)):
    node.lineno = node.end_lineno = here
    node.col_offset, node.end_col_offset = first, last
try:
    run(tree, this_file)
except TypeError as error:
    display(error)
tree.body[0].body[0].value.end_lineno = tree.body[0].body[0].end_lineno = here + 1
try:
    run(tree, this_file)
except TypeError as error:
    display(error)
for filename in ("<made up>", "there is no such file", ""):
    try:
        run(tree, filename)
    except TypeError as error:
        display(error)
for node in (addition, addition.left, addition.right, tree.body[0].body[0]):
    node.lineno = node.end_lineno = 100000
try:
    run(tree, this_file)
except TypeError as error:
    display(error)

# ---- What is wrong with it is said as it is of source
for source in ("return", "break", "x = *a", "def f():\n  nonlocal x", "def f(a, a): pass", "f(a=1, a=2)", "from __future__ import nope", "x = 1\nfrom __future__ import annotations", "await x", "yield", "class C: return", "def f():\n  x = 1\n  global x", "[(yield) for a in b]", "del f()" if 0 else "*a, *b = c"):
    def attempt():
        try:
            compile(parse(source), "<tree>", "exec")
        except SyntaxError as error:
            return (type(error).__name__, error.msg, error.filename, error.lineno, error.offset, error.end_lineno, error.end_offset, error.text)
    show(ascii(source), attempt)

# ---- Flags
show("await at the top", lambda: hex(compile(parse("await x", "exec") if 0 else compile("await x", "<s>", "exec", PyCF_ONLY_AST | PyCF_ALLOW_TOP_LEVEL_AWAIT), "<tree>", "exec", PyCF_ALLOW_TOP_LEVEL_AWAIT).co_flags & 0x80))
show("annotations as strings", lambda: run(parse("from __future__ import annotations\nx: a + b"), __name__="m").get("__annotations__"))
show("annotations as strings, by a flag", lambda: (v := {}, exec(compile(parse("x: a + b"), "<tree>", "exec", 0x1000000), v), v["__annotations__"])[2])
show("optimize", lambda: [(v := {}, exec(compile(parse("def f():\n  'doc'\n  assert False\n  return __debug__\ny = (f(), f.__doc__)"), "<tree>", "exec", optimize=level), v), v["y"])[2] for level in (1, 2)])
show("optimize 0", lambda: run(parse("assert False, 'said'")))

# ---- Its code is what it is made from
code = compile(parse(SOURCE), "<tree>", "exec")
show("again from co_code", lambda: (again := code.replace(co_code=code.co_code), again == code, again.co_code == code.co_code, (v := {}, exec(again, v), v["result"] == variables["result"])[2])[1:])
function = variables["outer"].__code__
show("a function again", lambda: (again := function.replace(co_code=function.co_code), again == function, [line for _, _, line in again.co_lines()] == [line for _, _, line in function.co_lines()])[1:])
show("moved", lambda: (moved := function.replace(co_firstlineno=100), moved.co_firstlineno, [line - 100 for _, _, line in moved.co_lines() if line] == [line - function.co_firstlineno for _, _, line in function.co_lines() if line])[1:])
show("renamed", lambda: (named := function.replace(co_filename="other", co_name="renamed"), named.co_filename, named.co_name)[1:])

# ---- Deep
deep = Constant(1, **AT)
for _ in range(100000):
    deep = UnaryOp(USub(), deep, **AT)
show("too deep", lambda: compile(Expression(deep), "<tree>", "eval"))
show("too deep for the tree over again", lambda: compile(Expression(deep), "<tree>", "eval", PyCF_ONLY_AST))
around = List([], Load(), **AT)
around.elts.append(around)
show("in itself", lambda: compile(Expression(around), "<tree>", "eval"))
del deep, around


# ---- What is asked of the objects
class Sly(Name):
    @property
    def id(self):
        print("    id is asked for")
        return "a"


show("attributes are got", lambda: eval(compile(Expression(Sly(ctx=Load(), **AT)), "<tree>", "eval"), {"a": 7}))


class Raises(Name):
    @property
    def id(self):
        raise KeyError("no")


show("and may raise", lambda: compile(Expression(Raises(ctx=Load(), **AT)), "<tree>", "eval"))


class Shrinks(Name):
    @property
    def id(self):
        del elements[:]
        return "a"


elements = [Shrinks(ctx=Load(), **AT), Name("b", Load(), **AT)]
show("a list that changes", lambda: compile(Expression(List(elements, Load(), **AT)), "<tree>", "eval"))
seen = []
sys.addaudithook(lambda event, arguments: seen.append((type(arguments[0]).__name__, arguments[1])) if event == "compile" and seen is not None else None)
compile(Module([], []), "<tree>", "exec")
print("audited", seen)
seen = None
