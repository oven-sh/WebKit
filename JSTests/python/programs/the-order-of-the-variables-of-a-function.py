# co_varnames has the parameters and then each variable from when it is first stored to or loaded, as the code is gone through: in `a = (b := 1)`, b comes first. locals() is in that order.
import types
sources = {
 "assign": "a = b = 1", "value first": "a = (b := 1)", "comprehension": "a = [x for x in p]", "two comprehensions": "a = [x for x in p]; b = {y: z for y, z in p}", "nested": "a = [[y for y in x] for x in p]", "a condition": "a = [x for x in p if (w := x)]",
 "tuple": "a, b = c, d = p", "star": "a, *b = p", "augmented": "a = 0; a += (b := 1)", "annotated": "a: int = (b := 1)", "annotated, no value": "a: int; b = 1", "for": "for a in (b := p): c = 1", "for else": "for a in p: b = 1\nelse: c = 1", "for tuple": "for a, (b, c) in p: pass",
 "while": "while (a := p): b = 1", "with": "with p as a, (c := p) as b: d = 1", "with tuple": "with p as (a, b): pass", "try": "try: a = 1\nexcept E as b: c = 1\nelse: d = 1\nfinally: e = 1", "try star": "try: a = 1\nexcept* E as b: c = 1",
 "import": "import a, b.c, d as e", "from": "from m import a, b as c", "def": "def a(): pass\nb = 1", "class": "class a: pass\nb = 1", "decorated": "@(d := p)\ndef a(): pass", "defaults": "def a(x=(b := 1)): pass", "lambda": "a = lambda: 0; b = 1",
 "del": "a = 1; del a; b = 1", "del first": "del a; a = 1", "used before": "print(a); a = 1; b = 1", "used only": "print(a, b)", "if": "if p: a = 1\nelse: b = 1", "if false": "if 0: a = 1\nb = 1", "unreachable": "return\na = 1", "after a loop that never ends": "while 1: a = 1\nb = 1",
 "match": "match p:\n case [a, b]: c = 1\n case {'k': d, **e}: pass\n case f: pass", "match as": "match p:\n case (1 | 2) as a: pass\n case C(b, k=c): pass\n case [*d]: pass", "global": "global g; g = 1; a = 1", "ternary": "a = (b := 1) if (c := 2) else (d := 3)", "bool": "a = (b := 1) and (c := 2)", "call": "a = f(b := 1, *(c := p), k=(d := 2))",
 "subscript": "p[a := 1] = (b := 2)", "attribute": "(a := p).x = (b := 2)", "f-string": "a = f'{(b := 1)}{(c := 2)!r:{(d := 3)}}'", "generator": "a = (x for x in (b := p))", "comprehension in a comprehension's source": "a = [x for x in [y for y in p]]", "the same name": "a = [a for a in p]", "the same name as a parameter": "b = [p for p in p]",
 "type alias": "type a = int; b = 1", "async for": "async for a in p: b = 1", "async with": "async with p as a: b = 1", "async comprehension": "a = [x async for x in p]", "yield": "a = yield (b := 1)", "await": "a = await (b := p)", "chained comparison": "a = (b := 1) < (c := 2) < (d := 3)", "assert": "assert (a := p), (b := 1)", "raise": "raise (a := p) from (b := p)", "return": "return (a := 1)",
 "comprehension with two fors": "a = [(x, y) for x in p for y in x]", "walrus in a comprehension": "a = [(w := x) for x in p]", "lambda in a comprehension": "a = [lambda: x for x in p]", "comprehension under a lambda's capture": "q = 1; a = [x + q for x in p]; f = lambda: q",
}
def all_code(c):
    yield c
    for k in c.co_consts:
        if isinstance(k, types.CodeType): yield from all_code(k)
for label, body in sources.items():
    is_async = "async " in body or "await" in body
    src = ("async " if is_async else "") + "def f(p):\n" + "".join("    " + l + "\n" for l in body.split("\n"))
    try:
        ns = {}; exec(src, ns)
        print(label, "->", [(c.co_name, c.co_varnames, c.co_nlocals, c.co_names) for c in all_code(ns["f"].__code__)])
    except SyntaxError as e: print(label, "-> SyntaxError", e.msg)
print("===== locals()")
def one(p):
    a = (b := 1)
    c = [x for x in p]
    return list(locals())
def two(p, *q, r=1, **s):
    z = y = (w := 2)
    for v in p: u = v
    return list(locals()), list(__import__("sys")._getframe().f_locals)
print(one([1]), two([1]))
print("===== what an import names is in co_names")
for src in ("import a", "import a.b.c", "import a.b.c as d", "import a as b", "import a, a.b, c.a", "from m import a", "from m import a as b, c", "from m import *", "from . import a", "from .. import a, b", "from .m.n import a", "from m import m", "import os\nos.path", "x = 1\nimport x", "from a import b\nb.c\nimport c", "import a.b as a", "from __future__ import annotations\nimport a",
            "def f():\n import a.b.c as d\n from m import e as g, h\n from . import i\n return a, d, g", "def f():\n global a\n import a, b", "class C:\n import a.b\n from m import c as d", "try:\n import a\nexcept ImportError:\n from b import a"):
    print(repr(src), [(c.co_name, c.co_names, c.co_varnames) for c in all_code(compile(src, "<s>", "exec"))])
