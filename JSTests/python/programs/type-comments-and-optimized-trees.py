# compile() with PyCF_TYPE_COMMENTS, with the mode 'func_type', and with PyCF_OPTIMIZED_AST.

import _warnings
from _ast import *
_warnings.filters.insert(0, ("ignore", None, Warning, None, 0))
def dump(node, out):
    if isinstance(node, AST):
        out.append(type(node).__name__ + "(")
        for name, value in vars(node).items():
            out.append(name + "="); dump(value, out); out.append(",")
        out.append(")")
    elif isinstance(node, list):
        out.append("[")
        for item in node: dump(item, out); out.append(",")
        out.append("]")
    else: out.append(type(node).__name__ + ":" + ascii(node))
def tree(source, mode="exec", flags=PyCF_TYPE_COMMENTS, **k):
    try:
        out = []; dump(compile(source, "<test>", mode, PyCF_ONLY_AST | flags, **k), out); return "".join(out)
    except SyntaxError as e: return (type(e).__name__, e.msg, e.lineno, e.offset, e.end_lineno, e.end_offset, e.text)
    except Exception as e: return (type(e).__name__, str(e))
def comments(n, found):
    if isinstance(n, AST):
        if getattr(n, "type_comment", None) is not None: found.append((type(n).__name__, n.type_comment))
        if isinstance(n, TypeIgnore): found.append(("ignore", n.lineno, n.tag))
        for v in vars(n).values(): comments(v, found)
    elif isinstance(n, list):
        for x in n: comments(x, found)
    return found
def short(source, mode="exec"):
    try: return comments(compile(source, "<test>", mode, PyCF_ONLY_AST | PyCF_TYPE_COMMENTS), [])
    except SyntaxError as e: return (type(e).__name__, e.msg, e.lineno, e.offset, e.end_lineno, e.end_offset, e.text)
for s in [
    "x = 1  # type: int", "x = 1 # type:int", "x = 1 #type: int", "x = 1 #	type:	int", "x = 1 # type : int", "x = 1 # Type: int", "x = 1 # type: ", "x = 1 # type:", "x = 1 # type: int # more", "x = 1 # type: é", "x = y = 1 # type: int", "x += 1 # type: int", "x: int = 1 # type: int", "x # type: int", "x = 1; y = 2 # type: int", "x = 1 # type: int\ny = 2 # type: str",
    "x = (1, # type: int\n 2)", "x = [ # type: int\n]", "x = 1 \\\n # type: int", "# type: int\nx = 1", "x = 1\n# type: int", "x = 1\n# type: int\n", "x = 1\n  # type: int\ny = 2", "if a:\n  x = 1\n# type: int\n  y = 2", "pass # type: int", "del x # type: int", "return # type: int", "import a # type: int", "x = yield # type: int", "x = *a, b # type: int", "x = 1 # type: int\n\n\n",
    "for a in b: # type: int\n  pass", "for a in b: # type: int\n  pass\nelse: # type: int\n  pass", "for a in b: pass # type: int", "for a in b:\n  # type: int\n  pass", "for a in b: # type: int", "async def f():\n  async for a in b: # type: int\n    pass", "while a: # type: int\n  pass", "if a: # type: int\n  pass", "class C: # type: int\n  pass", "try: # type: int\n  pass\nfinally: pass",
    "with a: # type: int\n  pass", "with a as b, c: # type: int\n  pass", "with (a as b, c): # type: int\n  pass", "with (a, b): # type: int\n  pass", "async def f():\n  async with a: # type: int\n    pass", "with a: pass # type: int",
    "def f(): # type: () -> int\n  pass", "def f():\n  # type: () -> int\n  pass", "def f():\n  # type: () -> int\n  # type: () -> str\n  pass", "def f(): # type: () -> int\n  # type: () -> str\n  pass", "def f(): # type: () -> int\n  pass # type: int", "def f(): pass # type: () -> int", "def f(): # type: () -> int", "def f():\n  # type: () -> int", "def f():\n# type: () -> int\n  pass", "def f():\n      # type: () -> int\n  pass", "def f():\n\n  # type: () -> int\n  pass", "def f():\n  'doc'\n  # type: () -> int\n  pass", "def f():\n  # comment\n  # type: () -> int\n  pass", "async def f(): # type: () -> int\n  pass", "def f() -> int: # type: () -> int\n  pass", "def f[T](): # type: () -> int\n  pass", "@d\ndef f(): # type: () -> int\n  pass", "def f(): # type: () -> int\n\n  pass",
    "def f(a, # type: int\n  b): pass", "def f(a # type: int\n  ): pass", "def f(a # type: int\n  , b): pass", "def f(a, # type: int\n  ): pass", "def f(a=1, # type: int\n  b=2 # type: str\n  ): pass", "def f(a: int, # type: int\n  b): pass", "def f(a, /, # type: int\n  b): pass", "def f(a, # type: int\n  /, b): pass", "def f(*a, # type: int\n  b): pass", "def f(*a # type: int\n  ): pass", "def f(*, # type: int\n  b): pass", "def f(*, b, # type: int\n  ): pass", "def f(*, b=1 # type: int\n  ): pass", "def f(**k # type: int\n  ): pass", "def f(**k, # type: int\n  ): pass", "def f(a, # type: int\n  *b, # type: str\n  c, # type: bytes\n  **d # type: dict\n  ):\n  # type: (...) -> None\n  pass", "def f(*a: *T, # type: int\n  b): pass", "def f(a, # type: int\n # type: str\n b): pass", "def f( # type: int\n a): pass", "def f(a, b # type: int\n  =1): pass", "lambda a, # type: int\n  b: 0", "f(a, # type: int\n  b)", "def f(a, # type: ignore\n  b): pass", "def f(a): # type: ignore\n  pass",
    "x = 1 # type: ignore", "x = 1 # type: ignore[a]", "x = 1 # type: ignore a", "x = 1 # type: ignorea", "x = 1 # type: ignore1", "x = 1 # type: ignore_", "x = 1 # type: ignoreé", "x = 1 # type: ignore#", "x = 1 # type:ignore", "x = 1 # type: ignor", "x = 1 # type: IGNORE", "# type: ignore\nx = 1", "x = 1\n# type: ignore", "x = 1\n# type: ignore\n", "if a:\n  x = 1\n# type: ignore\n  y = 2", "if a:\n  # type: ignore\n  y = 2", "x = (1, # type: ignore\n 2)", "# type: ignore", "# type: ignore\n# type: ignore[x]\n", "def f():\n  # type: ignore\n  pass", "x = 1 # type: ignore # type: int", "x = 1 \\\n # type: ignore", "\n\n# type: ignore  \n", "x = '''\n# type: ignore\n'''", "x = f'''{\n1 # type: ignore\n}'''", "x = 1 # type: ignore\ny = = 2", "if a: # type: ignore\n  pass", "class C: # type: ignore\n  pass # type: ignore",
]:
    print(ascii(s), "=>", short(s))
    print("   without:", "same" if tree(s, flags=0) == tree(s) else tree(s, flags=0)[:3] if isinstance(tree(s, flags=0), tuple) else "compiles")
    try: compile(s, "<t>", "exec", PyCF_TYPE_COMMENTS); r = "code"
    except SyntaxError as e: r = e.msg
    print("   to code:", r)
for s in ["x = 1 # type: int", "def f(a, # type: int\n  b):\n  # type: (...) -> None\n  pass", "# type: ignore[x]\nx = 1 # type: ignore"]:
    print(ascii(s), "=>", tree(s))
for s in ["x # type: int", "x # type: ignore", "(x # type: int\n)", "# type: ignore\nx"]:
    print("eval", ascii(s), "=>", tree(s, "eval")); print("single", ascii(s), "=>", tree(s, "single"))
for s in ["() -> int", "(int) -> str", "(int, str) -> None", "(*int) -> a", "(**int) -> a", "(int, *str) -> a", "(int, **str) -> a", "(int, *str, **bytes) -> a", "(*int, **str) -> a", "(*int, str) -> a", "(**int, str) -> a", "(**int, *str) -> a", "(*int, *str) -> a", "(int,) -> a", "(,) -> a", "(int str) -> a", "() -> ", "()", "() -> int\n", "() -> int\n\n\n", "() -> int; x", "() -> a, b", "() -> (a, b)", "( ) ->int", "(a.b[c], d(e)) -> f or g", "(int) -> str # c", "(\nint,\nstr\n) -> a", "\n() -> a", " () -> a", "(x := 1) -> a", "(*a, b=1) -> c", "(lambda: 0) -> a", "(yield) -> a", "(...) -> a", "int -> a", "(int) => a", "", "(int, *) -> a", "(int, **) -> a", "(* int, ** str) -> a", "(int) -> *a", "(a if b else c) -> d", "(int) -> str\n  ", "(int) -> str\nx"]:
    print("func_type", ascii(s), "=>", tree(s, "func_type", 0))
for m in ("func_type",):
    for f in (0, PyCF_TYPE_COMMENTS):
        try: compile("() -> int", "<t>", m, f); r = "ok"
        except Exception as e: r = (type(e).__name__, str(e))
        print(m, f, r)
ft = compile("(int, *str) -> a", "<t>", "func_type", PyCF_ONLY_AST)
print(type(ft).__name__, tree(ft, "exec", 0))
try: compile(ft, "<t>", "func_type")
except Exception as e: print(type(e).__name__, e)
# ---- Optimized
for s in ["__debug__", "x = __debug__", "__debug__ = 1", "del __debug__", "f(__debug__, a=__debug__)", "x.__debug__", "def f(a=__debug__, *, b: __debug__ = __debug__) -> __debug__: return __debug__", "class C(__debug__, m=__debug__): x: __debug__ = __debug__", "lambda: __debug__", "[__debug__ for a in __debug__ if __debug__]", "f'{__debug__!r:{__debug__}}'", "t'{__debug__}'", "assert __debug__, __debug__", "if __debug__: pass", "while __debug__: pass", "with __debug__ as a: pass", "match __debug__:\n case 1 if __debug__: pass", "def f[T: __debug__ = __debug__](): pass", "type A[T] = __debug__", "@__debug__\ndef f(): pass", "from __future__ import annotations\ndef f(a: __debug__ = __debug__) -> __debug__: pass\nx: __debug__ = __debug__", "(__debug__ := 1)", "(x := __debug__)", "try: pass\nexcept __debug__: __debug__", "raise __debug__ from __debug__", "a[__debug__:__debug__]", "{__debug__: __debug__, **__debug__}", "await __debug__", "not __debug__", "-__debug__ + __debug__",
          "'%s' % (a,)", "'%s' % a", "'%s' % (a, b)", "'%s %s' % (a,)", "'%s %r %a' % (a, b, c)", "'a%sb%%c%rd' % (a, b)", "'%%' % ()", "'' % ()", "'a' % ()", "'%5s' % (a,)", "'%-5s' % (a,)", "'%.3s' % (a,)", "'%5.3s' % (a,)", "'%-5.3r' % (a,)", "'%05s' % (a,)", "'%+s' % (a,)", "'% s' % (a,)", "'%#s' % (a,)", "'%0s' % (a,)", "'%00s' % (a,)", "'%12s' % (a,)", "'%123s' % (a,)", "'%1234s' % (a,)", "'%.12s' % (a,)", "'%.123s' % (a,)", "'%.s' % (a,)", "'%d' % (a,)", "'%s%d' % (a, b)", "'%(a)s' % (a,)", "'%*s' % (a, b)", "'%s' % (*a,)", "'%s' % [a]", "b'%s' % (a,)", "'%' % (a,)", "'%s%' % (a,)", "'%5' % (a,)", "'%c' % (a,)", "'é%s\\ud800' % (a,)", "'%s' % ('%s' % (a,),)", "x = '%s' % (__debug__,)", "'%s' % (a,) % (b,)", "f'{a}' % (b,)", "'%s' 'x%s' % (a, b)", "u'%s' % (a,)", "'%-s' % (a,)", "'%-0s' % (a,)", "'%s' % ()", "'{}%s' % (a,)", "'%s' % (\n a,\n)",
          "match x:\n case -1: pass", "match x:\n case -1.5: pass", "match x:\n case -2j: pass", "match x:\n case 1+2j: pass", "match x:\n case 1-2j: pass", "match x:\n case -1+2j: pass", "match x:\n case -1.5-2.5j: pass", "match x:\n case -0: pass", "match x:\n case -0.0: pass", "match x:\n case 0+0j: pass", "match x:\n case -0.0-0j: pass", "match x:\n case -0.0+0j: pass", "match x:\n case {-1: a, 1+2j: b, -1-2j: c}: pass", "match x:\n case [-1, (-2 | -3) as y]: pass", "match x:\n case C(-1, a=-2): pass", "match x:\n case -123456789012345678901234567890: pass", "match x:\n case 123456789012345678901234567890+1j: pass", "match x:\n case -0x10: pass", "match x:\n case -1e400: pass", "match x:\n case 1e400-1e400j: pass", "match -1:\n case 1: -1", "match x:\n case 1 if -1: pass", "match x:\n case %s+1j: pass" % ("9" * 400), "-1", "1+2j", "x = -1, 1+2j",
          "'doc'", "'doc'\nx", "def f(): 'doc'", "def f():\n 'doc'\n x", "class C: 'doc'", "class C:\n 'doc'\n x", "async def f(): 'doc'", "def f():\n  def g(): 'doc'", "'a' 'b'", "f'doc'", "b'doc'", "1", "x\n'doc'", "def f():\n x\n 'doc'", "'doc'; x", "  'doc'" if 0 else "if a: 'doc'", "lambda: 'doc'", "'doc'\n'doc2'", "def f():\n    '''a\n    b'''"]:
    for opt in (0, 1, 2):
        a = tree(s, "exec", 0, optimize=opt); b = tree(s, "exec", PyCF_OPTIMIZED_AST, optimize=opt)
        print("optimize", opt, ascii(s[:70]), "=>", b if a != b or opt == 0 else "as it was", "|", "plain differs from level 0" if a != tree(s, "exec", 0, optimize=0) else "")
        t = compile(s, "<t>", "exec", PyCF_ONLY_AST) if not isinstance(a, tuple) else None
        if t is not None: print("    from a tree:", tree(t, "exec", PyCF_OPTIMIZED_AST, optimize=opt) == b, tree(t, "exec", 0, optimize=opt) == a)
print(tree("__debug__", "eval", PyCF_OPTIMIZED_AST), tree("__debug__", "single", PyCF_OPTIMIZED_AST), tree("'doc'", "single", PyCF_OPTIMIZED_AST, optimize=2), tree("'doc'", "eval", PyCF_OPTIMIZED_AST, optimize=2), tree("(__debug__) -> __debug__", "func_type", PyCF_OPTIMIZED_AST))
