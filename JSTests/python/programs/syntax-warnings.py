import sys
sys.modules["warnings"] = None
import _warnings
class Captured:
    def __init__(self): self.text = []
    def write(self, text): self.text.append(text)
    def flush(self): pass
    def take(self): r = "".join(self.text); self.text.clear(); return r
err = sys.stderr = Captured()
original = list(_warnings.filters)
def tell(source, mode="exec", **k):
    _warnings.filters[:] = original; err.take()
    try: compile(source, "<test>", mode, **k); r = "compiled"
    except SyntaxError as e: r = ("SyntaxError", e.msg, e.lineno, e.offset, e.end_lineno, e.end_offset)
    shown = err.take().replace("<test>:", "").split("\n")[:-1]
    _warnings.filters[:] = [("error", None, SyntaxWarning, None, 0)]
    try: compile(source, "<test>", mode, **k); e2 = "compiled"
    except SyntaxError as e:
        # Where CPython says that an escape is goes by where the string ends, if it is an f-string, and leaves out what comes before the quotes. Here it is where it is.
        where = () if "escape sequence" in e.msg and source.lstrip("(\n")[0] not in "'\"" else (e.offset, e.end_lineno, e.end_offset)
        e2 = (type(e).__name__, e.msg, e.filename, e.lineno) + where + (e.text,)
    except Warning as e: e2 = ("still a warning", type(e).__name__, str(e))
    _warnings.filters[:] = original
    print(repr(source), "=>", r, shown, "| as an error:", e2)
# ---- is
for s in ("x is 1", "x is not 1", "1 is x", "x is 'a'", "x is b'a'", "x is 1.5", "x is 1j", "x is ()", "x is (1,)", "x is []", "x is {}", "x is None", "x is True", "x is False", "x is ...", "None is x",
          "x is -1", "x is f'a'", "x is f'{a}'", "x is t'a'", "x is y", "x is 1 is 2", "x == 1 is y", "a < b is 1", "1 is 1", "x is (y)", "x is (1)", "x is not None is 1", "x is 'a' 'b'", "x is 1_0", "x is 0x10", "x is __debug__",
          "if x is 1: pass", "while x is 1: pass", "y = 1 if x is 1 else 2", "assert x is 1", "not x is 1", "a and x is 1", "[i for i in y if i is 1]", "x is\\\n 1", "(x\n is\n 1)"):
    tell(s)
# ---- assert
for s in ("assert (x, 'm')", "assert ()", "assert (x,)", "assert x, 'm'", "assert (1, 2)", "assert [x, 'm']", "assert (x, 'm'), 'n'", "assert ((x, 'm'))", "assert (*x,)"):
    tell(s)
tell("assert (x, 'm')", optimize=1)
tell("assert (x, 'm')", optimize=2)
# ---- what is not callable
for s in ("1()", "'a'()", "b'a'()", "1.5()", "None()", "True()", "...()", "()()", "(1,)()", "(a, b)()", "[]()", "[a]()", "{}()", "{a: b}()", "{a}()", "[i for i in x]()", "{i for i in x}()", "{i: i for i in x}()", "(i for i in x)()",
          "f'a'()", "f'{a}'()", "t'a'()", "x()", "(lambda: 0)()", "x.y()", "x[0]()", "x()()", "(-1)()", "(1)(2)", "1(a=1)", "1(*a)", "[1, 2] (3)", "[\n 1\n](2)", "(a if b else c)()", "(yield)()", "__debug__()"):
    tell(s)
# ---- what is not subscriptable
for s in ("1[0]", "1.5[0]", "1j[0]", "None[0]", "...[0]", "True[0]", "'a'[0]", "b'a'[0]", "()[0]", "[][0]", "{}[0]", "{a}[0]", "{i for i in x}[0]", "(i for i in x)[0]", "(lambda: 0)[0]", "t'a'[0]", "f'a'[0]", "x[0]", "1[0] = 2", "del 1[0]", "1[0] += 2",
          "{1, 2}[0]", "1[a]", "1[:]", "(1)[0]", "None[int]", "-1[0]"):
    tell(s)
# ---- what will not do for an index
for s in ("'a'['b']", "'a'[1.5]", "'a'[None]", "'a'[()]", "'a'[(1, 2)]", "'a'[1, 2]", "'a'[[]]", "'a'[{}]", "'a'[0]", "'a'[True]", "'a'[:]", "'a'[1:2]", "'a'[x]", "'a'[-1]", "'a'[f'x']", "'a'[lambda: 0]", "'a'[...]",
          "b'a'['b']", "()['a']", "(1, 2)['a']", "(a, b)['a']", "[]['a']", "[a]['a']", "[i for i in x]['a']", "f'a'['b']", "f'{a}'['b']", "{}['a']", "x['a']", "[1, 2]\n[3.5]", "[1]['a'] = 2", "'a'[(i for i in x)]", "'a'[t'x']", "[1][{1}]", "[1][b'a']", "[1][1j]"):
    tell(s)
# ---- leaving a finally
for s in ("def f():\n try: pass\n finally: return 1", "def f():\n try: pass\n finally:\n  if x: return", "for i in x:\n try: pass\n finally: break", "for i in x:\n try: pass\n finally: continue", "while x:\n try: pass\n finally: break",
          "def f():\n try: pass\n finally:\n  def g(): return 1", "def f():\n try: pass\n finally:\n  for i in x: break", "def f():\n try: pass\n finally:\n  for i in x: return", "def f():\n try: pass\n finally:\n  while x: continue",
          "def f():\n try: pass\n finally:\n  lambda: 0\n  return", "def f():\n try: return\n finally: pass", "def f():\n try: pass\n except E: return\n finally: pass", "def f():\n try: pass\n except E: pass\n else: return\n finally: pass",
          "def f():\n try: pass\n finally:\n  try: return\n  finally: pass", "def f():\n try: pass\n finally:\n  try: pass\n  finally: return", "def f():\n try: pass\n finally:\n  with x: return", "def f():\n try: pass\n finally:\n  class C:\n   pass\n  return",
          "for i in x:\n try: pass\n finally:\n  def g():\n   for j in y: break", "for i in x:\n try: pass\n finally:\n  for j in y: pass\n  else: break", "async def f():\n try: pass\n finally: return", "def f():\n try: pass\n finally:\n  match x:\n   case 1: return",
          "for i in x:\n try: pass\n finally:\n  class C:\n   for j in y: break", "def f():\n for i in x:\n  try: pass\n  finally:\n   return\n   break\n   continue", "def f():\n try: pass\n except* E: pass\n finally: return"):
    tell(s)
# ---- what a string has in it
for s in (r"'\d'", r"'\ '", r"b'\d'", r"'\8'", r"'\400'", r"b'\400'", r"'\777'", r"'\377'", r"r'\d'", r"'\\d'", r"'\n\d'", r"'\d\e'", r"'''\d'''", r"f'\d'", r"f'{x}\d'", r"f'\d{x}\e'", r"f'{x:\d}'", r"rf'\d'", r"t'\d'", r"'a' '\d'", r"x = '\d'; y = '\e'", "'\\d'\n'\\e'", "(\n'\\d')", r"u'\d'", r"'\('", r"b'\N'", r"f'{f'\d'}'", r"'\%'"):
    tell(s)
# ---- a number with something straight after it
for s in ("1if x else y", "1 if x else y", "0x1for x in y", "1and x", "1or x", "1in x", "1is x", "1not in x", "1.5if x else y", "1jif x else y", "1_0if x else y", "0b1and x", "0o7or x", "1e5if x else y", "[1for x in y]", "1else", "0xfor x in y", "1.if x else y", "1e1and x"):
    tell(s)
# ---- more than one, and where they come
tell("x is 1\ny is 2")
tell("x is 1; x is 1")
tell("def f():\n x is 1\ny is 2")
tell("y is 2\ndef f():\n x is 1")
tell("@(a is 1)\ndef f(b=c is 2) -> (d is 3):\n e is 4")
tell("@(a is 1)\nclass C(b is 2, k=c is 3):\n d is 4")
tell("def f(a: (b is 1)): pass")
tell("x: (a is 1) = (b is 2)")
tell("x: (a is 1)")
tell("def f():\n x: (a is 1) = 2")
tell("from __future__ import annotations\nx: (a is 1) = (b is 2)\ndef f(a: (b is 3)): pass")
tell("lambda a=(b is 1): c is 2")
tell("[a is 1 for b in (c is 2) if d is 3 for e in (f is 4)]")
tell("class C:\n def m(self):\n  return self is 1")
tell("'\\d'\nx is 1\ndef f():\n try: pass\n finally: return")
tell("def f():\n try: pass\n finally: return 1 is x")
tell("if 0:\n x is 1")
tell("if 1:\n pass\nelse:\n x is 1")
tell("while 0:\n x is 1")
tell("def f():\n return\n x is 1")
tell("x is 1\nreturn")
tell("x is 1\ndef f():\n nonlocal y")
tell("'\\d'\ndef f():\n nonlocal y")
tell("'\\d'\n(")
tell("def f():\n try: pass\n finally: return\ndef g():\n nonlocal y")
tell("match x:\n case 1 if y is 2: z is 3")
tell("type A = (x is 1)")
tell("def f[T: (x is 1) = (y is 2)](): pass")
tell("with (a is 1) as b, (c is 2): pass")
tell("for a in (b is 1): pass\nelse: c is 2")
tell("try: a is 1\nexcept (b is 2): c is 3\nelse: d is 4\nfinally: e is 5")
tell("f'{x is 1}'")
tell("f'{x:{y is 1}}'")
tell("t'{x is 1}'")
tell("del x[y is 1]")
tell("x[y is 1] = z is 2")
tell("x.a = y is 1")
tell("x += y is 1")
tell("global g\ng is 1")
tell("raise (a is 1) from (b is 2)")
tell("(a := b is 1)")
tell("await_ = [(yield x is 1)]")
tell("x is 1", "eval")
tell("x is 1", "single")
tell("1()", "eval")
