# The module _symtable, and symtable, which is written over it.
import _symtable
import binascii
import symtable
import sys
import warnings

warnings.simplefilter("ignore")


def attempt(f, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)


def t(label, f):
    r = attempt(f)
    print(label, "=>", r if isinstance(r, str) else ascii(r))


def symbols(e):
    """The names are in the order in which they were first seen. After them come those that something inside uses and this does not, which are what is in a set, in whatever order that gives them: it is not the same from one run
    of CPython to the next."""
    passed_through = _symtable.FREE << _symtable.SCOPE_OFF
    return [i for i in e.symbols.items() if i[1] != passed_through] + sorted(i for i in e.symbols.items() if i[1] == passed_through)


def dump(e):
    "All that there is to see of an entry, but for its id"
    return (e.name, e.type, e.nested, e.lineno, symbols(e), e.varnames, [dump(c) for c in e.children])


def attempt_exception(source, mode):
    try:
        return dump(_symtable.symtable(source, "file", mode))
    except BaseException as e:
        return e


def ids(e):
    return [e.id] + [i for c in e.children for i in ids(c)]


print("---- what there is")
names = sorted(n for n in vars(_symtable) if not n.startswith("__"))
t("the module", lambda: (_symtable.__name__, _symtable.__package__, _symtable.__loader__.__name__, _symtable.__doc__, [(n, getattr(_symtable, n)) for n in names if n != "symtable"]))
f = _symtable.symtable
t("symtable", lambda: (type(f).__name__, f.__text_signature__, f.__doc__, f.__module__))
E = type(f("", "f", "exec"))
t("what it gives", lambda: (E.__name__, E.__module__, E.__qualname__, repr(E), [b.__name__ for b in E.__mro__], [(n, type(v).__name__, v.__doc__) for n, v in sorted(vars(E).items())], E.__basicsize__, E.__flags__ & 0xFFFFF, attempt(E), attempt(E.__new__, E), attempt(lambda: type("S", (E,), {})), attempt(setattr, E, "x", 1)))
e = f("def g(a): return a", "f", "exec")
t("an entry", lambda: (dump(e), [type(getattr(e, n)).__name__ for n in ("id", "name", "symbols", "varnames", "children", "nested", "type", "lineno")], repr(e).replace(str(e.id), "ID"), repr(e.children[0]).replace(str(e.children[0].id), "ID"), [attempt(setattr, e, n, 1) for n in ("id", "name", "symbols", "nested", "x")], attempt(getattr, e, "__dict__"), attempt(hash, e) != 0, e == e, e == f("def g(a): return a", "f", "exec"), len(set(ids(e))) == len(ids(e)), e.symbols is e.symbols, e.children is e.children))

print("---- how it is called")
t("what is wrong with it", lambda: [attempt(f, *a, **k) for a, k in (((), {}), (("",), {}), (("", "f"), {}), (("", "f", "exec", 1), {}), ((), {"source": "", "filename": "f", "startstr": "exec"}), (("", "f", "x"), {}), (("", "f", ""), {}), (("", "f", "EXEC"), {}), (("", "f", None), {}), (("", "f", b"exec"), {}), (("", "f", "exec\0"), {}), (("", "f", 5), {}), ((5, "f", "exec"), {}), ((None, "f", "exec"), {}), (([], "f", "exec"), {}),
                                                                (("", 5, "exec"), {}), (("", None, "exec"), {}), (("\0", "f", "exec"), {}), ((b"\0", "f", "exec"), {}), (("\ud800", "f", "exec"), {}), (("", "f\0", "exec"), {}), ((5, "f", "x"), {}), ((5, 5, 5), {}))])
t("what will do", lambda: [dump(f(*a)) for a in ((b"x = 1", "f", "exec"), (bytearray(b"x = 1"), "f", "exec"), (memoryview(b"x = 1"), "f", "exec"), ("x = 1", b"f", "exec"), ("x", "f", "eval"), ("x = 1", "f", "single"), ("x = 1\n", type("P", (), {"__fspath__": lambda s: "f"})(), "exec"), (b"# coding: latin-1\n\xe9 = 1", "f", "exec"), (b"\xef\xbb\xbfx = 1", "f", "exec"), (type("S", (str,), {})("x = 1"), "f", type("S", (str,), {})("exec")))])
t("what is not Python", lambda: [(type(x).__name__, x.msg, x.filename, x.lineno, x.offset, x.end_lineno, x.end_offset, x.text) if isinstance(x, SyntaxError) else x for s, m in (("x =", "exec"), ("def", "exec"), ("x = 1", "eval"), ("", "eval"), ("if x:\npass", "exec"), ("x = 1\ny = 2", "single"), ("(", "exec"), ("'", "exec"), ("\tx\n  y", "exec")) for x in [attempt_exception(s, m)]])

print("---- what is against the rules for names")
WRONG = ("def f():\n x = 1\n global x", "def f():\n print(x)\n global x", "def f(x):\n global x", "def f():\n nonlocal x", "nonlocal x", "def f():\n x = 1\n def g():\n  x = 2\n  nonlocal x", "def f(x):\n def g():\n  global x\n  nonlocal x", "def f(a, a): pass", "def f(a, *, a): pass", "lambda a, a: 0",
         "def f():\n x: int\n global x", "def f():\n global x\n x: int = 1", "class C:\n nonlocal x", "[x := 1 for x in y]", "[[(x := 1) for a in b] for x in y]", "class C:\n [y := 1 for x in z]", "[i for i in (j := 0)]", "def f[T, T](): pass", "class C[T, T]: pass", "type X[T, T] = int", "def f[T](T): pass",
         "def f():\n from x import *", "def f[T: (yield)](): pass", "type X = (yield)", "def f[T: (x := 1)](): pass", "def f[T = (await x)](): pass", "class C[T]((yield)): pass", "def f(x: (yield)): pass", "def f():\n x: (yield) = 1", "x: (y := 1)", "def f(x: (y := 1)): pass", "async def f():\n [(yield) for x in y]",
         "[(yield) for x in y]", "{(yield x) for x in y}", "((yield) for x in y)", "def f():\n return [(yield) for x in y]", "def __debug__(): pass", "__debug__ = 1", "def f(__debug__): pass", "class __debug__: pass", "import __debug__", "del __debug__", "x.__debug__ = 1", "f(__debug__=1)", "for __debug__ in x: pass",
         "with x as __debug__: pass", "def f(*, __debug__): pass", "lambda __debug__: 0", "(__debug__ := 1)", "match x:\n case __debug__: pass", "match x:\n case [*__debug__]: pass", "match x:\n case {**__debug__}: pass", "def f[__debug__](): pass", "type __debug__ = int", "try: pass\nexcept E as __debug__: pass",
         "from __future__ import nope", "x = 1\nfrom __future__ import annotations", "from __future__ import braces", "from __future__ import *")
for source in WRONG:
    x = attempt_exception(source, "exec")
    print(ascii(source), "=>", ascii((type(x).__name__, x.msg, x.lineno, x.offset, x.end_lineno, x.end_offset, x.text) if isinstance(x, SyntaxError) else x))

print("---- of every kind of thing")
RIGHT = ("", "x", "x = 1", "x = y", "global x\nx = 1", "import a, b.c, d as e\nfrom f import g, h as i", "from f import *", "del x", "x: int", "x: int = 1", "x.y: int", "x[0]: int", "(x): int", "def f(): pass", "def f(a, b=1, /, c=2, *d, e, f=3, **g): pass", "def f(a: int, *b: str, c: bytes = 1, **d: float) -> list: pass",
         "def f():\n x = 1\n def g():\n  return x\n return g", "def f():\n x = 1\n def g():\n  nonlocal x\n  x = 2", "def f():\n global x\n x = 1\n def g():\n  return x", "def f():\n def g():\n  def h():\n   return x\n x = 1", "x = 1\ndef f():\n return x", "def f():\n return x\nx = 1", "def f(x):\n def g(x):\n  return x",
         "class C: pass", "class C(B, metaclass=M): x = 1", "class C:\n x = 1\n def f(self): return x", "class C:\n def f(self): return __class__", "class C:\n def f(self): return super().f()", "class C:\n def f(self): super", "class C:\n __x = 1\n def f(self): return self.__x + __y", "class C:\n def __f(self, __a): __b = __a",
         "class _C:\n __x = 1", "class __C__:\n __x = 1", "class C:\n class D:\n  __x = 1", "class C:\n x: int\n y: str = ''", "class C:\n def f(self):\n  self.a = 1\n  self.b: int = 2", "def f():\n class C:\n  x = y\n y = 1", "def f():\n x = 1\n class C:\n  x = x", "def f():\n x = 1\n class C:\n  def g(self): return x",
         "class C:\n __class__ = 1", "class C:\n __classdict__ = 1", "class C:\n def f(): __classdict__", "class C:\n x = __class__", "lambda: 0", "lambda a, *b, c=1, **d: (a, b, c, d, e)", "f = lambda x: lambda y: x + y", "[x for x in y]", "[x for x in y if x]", "[(x, z) for x in y for z in x]", "[[a for a in x] for x in y]", "{x for x in y}", "{x: z for x, z in y}",
         "(x for x in y)", "def f():\n return [x for x in y]", "def f(y):\n return [x + y for x in y]", "def f():\n return [lambda: x for x in y]", "def f():\n z = 1\n return [z for x in y]", "def f():\n return [(lambda: z) for x in y for z in x]", "def f():\n x = 1\n return [x for x in y], x", "class C:\n a = [x for x in y]", "class C:\n y = 1\n a = [y for x in y]",
         "class C:\n a = [__class__ for x in y]", "[y := x for x in z]", "def f():\n [y := x for x in z]\n return y", "def f():\n global y\n [y := x for x in z]", "def f():\n y = 0\n def g():\n  nonlocal y\n  [y := x for x in z]", "[(y := x, [w := y for a in b]) for x in z]", "(y := 1)", "async def f():\n return [x async for x in y]", "async def f():\n return [await x for x in y]",
         "async def f():\n async with a as b: pass\n async for c in d: pass\n await e", "def f():\n yield", "def f():\n yield from x", "def f():\n x = yield", "async def f():\n yield", "def f():\n return (yield)", "for x in y: pass", "for x, (a, *b) in y: pass", "with a as b, c as (d, e): pass", "try: pass\nexcept E as e: pass", "try: pass\nexcept* E as e: pass",
         "match x:\n case a: pass", "match x:\n case [a, *b]: pass", "match x:\n case {'k': a, **b}: pass", "match x:\n case C(a, k=b): pass", "match x:\n case a | b: pass" if False else "match x:\n case (1 as a) | (2 as a): pass", "match x:\n case c.d: pass", "match x:\n case _: pass", "def f[T](): pass", "def f[T](a: T) -> T: return a", "def f[T: int, *Ts, **P](): pass",
         "def f[T: (int, str) = int](): pass", "def f[T = x](): pass", "def f[*Ts = y](): pass", "def f[**P = z](): pass", "class C[T]: pass", "class C[T](B[T]): x: T", "class C[T]:\n def f(self) -> T: pass", "class C[T]:\n def f[U](self, a: T, b: U): pass", "class C[T: x]:\n x = 1", "type X = int", "type X[T] = list[T]", "type X[T: int = str] = T", "class C:\n type X = y\n y = 1",
         "def f():\n type X = y\n y = 1", "def f():\n x = 1\n def g[T: x](): pass", "class C:\n x = 1\n def f[T: x](self): pass", "class C:\n x = 1\n class D[T: x]: pass", "def f[T]():\n def g[U]():\n  return T, U", "class C[T]:\n class D[U]:\n  x: T\n  y: U", "class C[__T]: x: __T", "class C:\n def f[__T](self, a: __T): pass", "def f[T](a=T): pass" if False else "def f[T](a=x): pass",
         "def f(a: x): pass", "def f() -> x: pass", "def f(a: lambda: x): pass", "def f(a: [y for y in x]): pass", "x: [y for y in z]", "class C:\n x: [y for y in z]", "class C:\n z = 1\n x: z", "def f():\n z = 1\n def g(a: z): pass", "def f():\n z = 1\n x: z = 2", "def f():\n x: y", "from __future__ import annotations\nx: y\ndef f(a: b) -> c: pass\nclass C:\n d: e",
         "from __future__ import annotations\ndef f():\n x: (yield)", "if x:\n y: int", "class C:\n if x:\n  y: int", "@d\ndef f(): pass", "@d(x)\nclass C: pass", "def f(a=b, *, c=d): pass", "def f():\n def g(a=x): pass\n x = 1", "class C:\n x = 1\n def f(self, a=x): pass", "f'{x}{y!r:{z}}'", "t'{x}{y!r:{z}}'", "x[a:b:c]", "print(*a, **b)", "a if b else c", "not a and b or c", "a < b < c",
         "global a, b\na = b", "def f():\n global a\n def g():\n  a = 1", "def f():\n a = 1\n def g():\n  global a\n  def h():\n   return a", "def f():\n a = 1\n def g():\n  def h():\n   nonlocal a", "def f():\n import a.b as c, d.e", "def f():\n from a import b as c", "def f():\n del x", "def f(x):\n del x", "def f():\n try: pass\n except E as x: pass\n return x", "def f():\n with a as x: pass",
         "def f():\n for x in y: pass\n else: z = 1", "def f():\n while (x := y): pass", "def f():\n x += 1", "x += 1", "def f():\n x.y += 1", "def f():\n x[y] = z", "def f():\n (a, b), *c = d", "def f():\n a = b = c", "def f():\n exec('x = 1')\n return x", "def f():\n locals()", "def f(self): __class__", "__class__", "def f():\n class C:\n  def g(self): __class__\n __class__ = 1",
         "class C:\n def f(self):\n  class D:\n   def g(self): return __class__", "class C:\n f = lambda self: __class__", "class C:\n f = [lambda: __class__ for x in y]", "class C:\n def f(self):\n  return [__class__ for x in y]", "class C(x := 1): pass", "class C:\n (x := 1)", "def f(a=(x := 1)): pass", "def f():\n\n\n x = 1", "\n\n\ndef f(): pass", "def f(\n a,\n b): pass", "@d\n\ndef f(): pass", "class C(\n B): pass", "x = [\n a for a in b]", "x = lambda \\\n a: a")
for source in RIGHT:
    print(ascii(source), "=>", ascii(attempt_exception(source, "exec")))
t("eval and single", lambda: [attempt_exception(s, m) if not isinstance(attempt_exception(s, m), BaseException) else type(attempt_exception(s, m)).__name__ for s in ("x", "lambda a: a + b", "[x for x in y]", "(x := 1)", "x = 1", "def f(): pass") for m in ("eval", "single")])

print("---- symtable, which is written over it")
top = symtable.symtable("""
import os
x: int = 1
def f(a, b=2, *c, d, **e):
    global g
    y = a
    def inner():
        nonlocal y
        return y + x
    return inner
class C[T](object):
    z = 1
    def m(self): return __class__
    @property
    def p(self): return self.q
type A[U] = list[U]
l = [i for i in x]
""", "file", "exec")


def describe(table):
    out = [type(table).__name__, repr(table).replace(str(table.get_id()), "ID"), table.get_type().name if hasattr(table.get_type(), "name") else table.get_type(), table.get_name(), table.get_lineno(), table.is_optimized(), table.is_nested(), table.has_children(), sorted(table.get_identifiers())]
    for s in sorted(table.get_symbols(), key=lambda s: s.get_name()):
        out.append((repr(s), s.get_name(), s.is_referenced(), s.is_parameter(), s.is_type_parameter(), s.is_global(), s.is_nonlocal(), s.is_declared_global(), s.is_local(), s.is_annotated(), s.is_free(), s.is_free_class(), s.is_imported(), s.is_assigned(), s.is_comp_iter(), s.is_comp_cell(), s.is_namespace(), len(s.get_namespaces())))
    if isinstance(table, symtable.Function):
        out.append((table.get_parameters(), table.get_locals(), table.get_globals(), table.get_nonlocals(), table.get_frees()))
    if isinstance(table, symtable.Class):
        out.append(attempt(table.get_methods))
    return out, [describe(c) for c in table.get_children()]


t("all of it", lambda: describe(top))
t("lookup", lambda: (repr(top.lookup("f")), attempt(top.lookup, "nope"), top.lookup("f").get_namespace().get_name(), attempt(top.lookup("x").get_namespace), top.lookup("f") is top.lookup("f"), top.get_children()[0] is top.get_children()[0]))
