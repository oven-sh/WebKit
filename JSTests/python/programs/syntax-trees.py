# compile(..., PyCF_ONLY_AST): the syntax tree as objects.

import _ast, _warnings, posix

_warnings.filters.insert(0, ("ignore", None, Warning, None, 0))


def dump(node, out):
    if isinstance(node, _ast.AST):
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


def tree(source, mode="exec", flags=0):
    try:
        out = []
        dump(compile(source, "<test>", mode, _ast.PyCF_ONLY_AST | flags), out)
        return "".join(out)
    except SyntaxError as e:
        return (type(e).__name__, e.msg, e.lineno, e.offset)


for source in [
    "", "\n", "x", "x\n", "'doc'\nx = 1", "1; 2.5; 3j; 'a'; b'a'; u'a'; None; True; False; ...", "0xff; 0o17; 0b11; 1_000; 10 ** 100; 123456789012345678901234567890; 18446744073709551615; 18446744073709551616; 9223372036854775808",
    "1e400; 1e-400; .5; 5.; 1.5e10j", "'a' 'b'; 'a' f'{x}' 'b'; b'a' b'b'; '\\ud800'; '\\x00'; '\\N{BULLET}'; b'\\xff'", "a + b - c * d / e // f % g @ h ** i << j >> k | l ^ m & n", "-a; +a; ~a; not a", "a and b or c", "a < b <= c > d >= e == f != g is h is not i in j not in k",
    "a if b else c", "lambda: 0; lambda a, /, b=1, *c, d, e=2, **f: 0", "[a, *b]; (a, *b); {a, *b}; {a: b, **c}; (); []; {}", "[a for b in c if d if e for f in g]; {a for b in c}; {a: b for c in d}; (a for b in c)", "async def f():\n [a async for b in c]\n await x\n async for a in b: pass\n async with a as b: pass",
    "def f():\n yield\n yield a\n yield from b\n return\n return a", "f(a, *b, c=d, **e)", "a.b.c; a[b]; a[b:c]; a[b:c:d]; a[:]; a[::]; a[b, c]; a[b:c, d]; a[*b]", "(a := b)", "f'{a}{b!r}{c:d}{e!s:{f}}{g=}{h = }{i=!a}{j=:k}'", "t'{a}{b!r}{c:d}{e=}'; t'a' t'b'",
    "a = b = c", "a, b = c", "[a, *b] = c", "a.b = c; a[b] = c", "a += b; a -= b; a *= b; a /= b; a //= b; a %= b; a @= b; a **= b; a <<= b; a >>= b; a |= b; a ^= b; a &= b", "a: b; a: b = c; (a): b; a.b: c; a[b]: c", "del a, b.c, d[e]", "pass; break; continue" if 0 else "for a in b: pass; break; continue",
    "if a: b\nelif c: d\nelse: e", "while a: b\nelse: c", "for a in b: c\nelse: d", "for a, b in c: d", "with a: b", "with a as b, c as d: e", "with (a as b, c as d): e", "with (a, b): c", "try: a\nexcept b: c\nexcept d as e: f\nexcept: g\nelse: h\nfinally: i", "try: a\nexcept* b: c", "try: a\nexcept b, c: d", "try: a\nfinally: b",
    "raise; raise a; raise a from b" if 0 else "raise\nraise a\nraise a from b", "assert a; assert a, b", "import a; import a.b; import a as b; import a.b as c, d", "from a import b; from a import b as c, d; from . import a; from .. import a; from .a import b; from ...a.b import c; from a import *; from a import (b, c,)", "global a, b", "def f():\n nonlocal a" if 0 else "def g():\n a = 1\n def f():\n  nonlocal a",
    "@a\n@b.c(d)\ndef f(e: g = h, /, *i: j, k: l = m, **n: o) -> p: q", "@a\nclass B(c, d=e, *f, **g): h", "def f[T, *U, **V](): pass", "def f[T: int = str, *U = [int], **V = [int]](): pass", "class A[T: (int, str)]: pass", "type A = int; type B[T] = list[T]",
    "match a:\n case 1: pass\n case -1: pass\n case 1+2j: pass\n case 'a' 'b': pass\n case None: pass\n case True: pass\n case b.c: pass\n case d: pass\n case _: pass", "match a:\n case [b, *c, d]: pass\n case (e, f): pass\n case [*_]: pass\n case g, h: pass\n case (i): pass\n case []: pass",
    "match a:\n case {'b': c, **d}: pass\n case {}: pass\n case {1: _, e.f: g}: pass", "match a:\n case B(): pass\n case C(d, e=f): pass\n case g.H(i,): pass", "match a:\n case b | c: pass\n case (d | e) as f: pass\n case g if h: pass", "match a, b:\n case c: pass", "match *a, b:\n case c: pass",
    "x = (\n  1 +\n  2\n)", "é = 'é'; 𝕏 = 1", "ﬁ = 1", "if a:\n\tb\n", "a\\\n+ b", "a # c\n# d\nb", "\x0ca", "a;", "def f(): 'doc'", "class A: 'doc'", "print(a, file=b)", "a = yield" if 0 else "def f(): a = yield", "x = *a, b", "for x in *a, b: pass", "return" if 0 else "def f(): return *a, b",
    "from __future__ import annotations\nx: int", "from __future__ import nope", "from __future__ import braces", "x\nfrom __future__ import annotations", "from __future__ import barry_as_FLUFL\na <> b", "1 +", "def f(:", "'\\q'", "break", "return", "yield", "await x", "x = 1 = 2", "nonlocal x", "def f(a, a): pass", "*a", "f(**a, *b)",
]:
    print(ascii(source), "=>", tree(source))
for source in ["x", "x\n", " x", "x, y", "lambda: 0", "", "x = 1", "x;", "(yield)", "\nx", "x\n\n", "x #c", "*x", "*x,", "x := 1", "(x := 1)", "1 if 2 else 3", "await x"]:
    print("eval", ascii(source), "=>", tree(source, "eval"))
for source in ["x", "x\n", "x; y", "if x: y\n", "if x:\n  y\n", "", "\n", "x\ny", "def f(): pass\n", "x = 1"]:
    print("single", ascii(source), "=>", tree(source, "single"))
print("await at the top", tree("await x", "exec", _ast.PyCF_ALLOW_TOP_LEVEL_AWAIT))
print("bytes", tree(b"x = '\xc3\xa9'"), tree(b"# coding: latin-1\nx = '\xe9'"), tree(b"\xef\xbb\xbfx"))
first = compile("a + b", "<test>", "eval", _ast.PyCF_ONLY_AST)
second = compile("c + d", "<test>", "eval", _ast.PyCF_ONLY_AST)
print("there is one of each", first.body.op is second.body.op, first.body.left.ctx is second.body.right.ctx, first.body.op is _ast.Add(), type(first.body.op) is _ast.Add)
print("and a tree of its own", first.body is not second.body, first.body.left is not first.body.right)
print("deep", tree("(" * 90 + "x" + ")" * 90, "eval"), tree("-" * 400 + "x", "eval")[:60], tree("x" + ".y" * 400, "eval")[:60])
print("flags", [type(compile("x", "<test>", "exec", flags)).__name__ for flags in (0, _ast.PyCF_ONLY_AST, _ast.PyCF_ONLY_AST | _ast.PyCF_TYPE_COMMENTS)])

# This.
descriptor = posix.open(__file__, posix.O_RDONLY)
data = b""
while chunk := posix.read(descriptor, 1 << 16):
    data += chunk
posix.close(descriptor)
dumped = tree(data).encode()
print("this", len(dumped), int.from_bytes(dumped, "big") % 1000000007)
