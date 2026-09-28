from __future__ import annotations
import sys
def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
def f(a: int, b: "s", *c: *Ts, d: List[ int ] = 1, **e: Undefined) -> None: pass
show("function", lambda: f.__annotations__)
show("annotate", lambda: (f.__annotate__(1), f.__annotate__(2)))
show("format 3", lambda: f.__annotate__(3))
class C:
    a: int = 1
    b: Undefined
    __p: x . y
    (d): int = 2
    if True:
        e: str
    def m(self, x: C) -> C: pass
show("class", lambda: (C.__annotations__, C.__annotate__, sorted(k for k in C.__dict__ if "annot" in k)))
show("method", lambda: C.m.__annotations__)
mx: int = 1
my: Undefined
show("module", lambda: (__annotations__, "__annotate__" in globals(), sys.modules[__name__].__annotations__ is __annotations__))
class Empty: pass
show("empty", lambda: (Empty.__annotations__, sorted(k for k in Empty.__dict__ if "annot" in k)))
def local():
    x: Undefined = 1
    return x
show("local", local)
EXPRESSIONS = r'''
a
a.b.c
a[b]
a[b, c]
a[b:c]
a[b:c:d]
a[:]
a[::2]
a[b:c, d]
a[(b, c)]
a[*b]
a[*b, c]
a[()]
a()
a(b)
a(b, c=d, *e, **f)
a(*b, c)
a(b for b in c)
a(b for b in c if d if e)
a((b for b in c), d)
[a for a in b]
{a for a in b}
{a: b for a, b in c}
(a async for a in b)
[a for a, in b]
[a for (a, b) in c for d in e]
[a for a in (b, c)]
[a for a in b if (c, d)]
[a for a in lambda: b]
[a for a in (b if c else d)]
1
1.0
1.5e100
1e999
-1e999
1j
1.5j
1e999j
0x10
0b101
0o17
1_000_000
123456789012345678901234567890
0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF
0b1111111111111111111111111111111111111111111111111111111111111111111111
'a'
"a"
"a'b"
'a"b'
"a'b\"c"
'\n\t\\'
'é\x00\u1234\U0001F600'
u'a'
b'a'
b"a'\x00\xff"
'a' 'b'
"""multi
line"""
r'\d'
None
True
False
...
()
(a,)
(a, b)
a, b
[]
[a]
[a, b]
[*a, b]
{}
{a}
{a, b}
{*a}
{a: b}
{a: b, **c}
{**a, **b}
a + b
a - b * c
(a - b) * c
a * (b - c)
a - (b - c)
(a - b) - c
a ** b ** c
(a ** b) ** c
a ** (b ** c)
-a ** b
(-a) ** b
a ** -b
- - a
-(-a)
+a
~a
not a
not not a
not (a and b)
(not a) and b
a and b or c
a and (b or c)
(a and b) and c
a or b or c
a or (b or c)
a @ b
a // b % c
a << b >> c
a & b | c ^ d
(a | b) & c
a < b
a < b < c
(a < b) < c
a < (b < c)
a == b != c
a is b
a is not b
a in b
a not in b
not a in b
(not a) in b
a if b else c
a if b else c if d else e
(a if b else c) if d else e
a if (b if c else d) else e
(a if b else c) + d
lambda: a
lambda a: a
lambda a, b=1: a
lambda a, /, b: a
lambda a=1, /, b=2: a
lambda *a: a
lambda *, a: a
lambda *, a=1: a
lambda *a, b, c=1, **d: a
lambda **a: a
lambda a, /: a
lambda a, *, b: a
(lambda: a)()
(lambda: a) + b
lambda: (a, b)
lambda: lambda: a
lambda: (yield)
(yield)
(yield a)
(yield a, b)
(yield from a)
await a
await a.b
await a()
await (a + b)
(await a) + b
await (await a)
-await a
(await a) ** b
(a := b)
[a := b]
a[b := c]
f(a := b)
(a := b, c)
*a
*a, b
*a | b
*(a or b)
f''
f'a'
f'{a}'
f'{a}b{c}'
f'{a!r}'
f'{a!s}'
f'{a!a}'
f'{a:b}'
f'{a:{b}}'
f'{a:{b}.{c}}'
f'{a!r:>{b}}'
f'{{a}}'
f'{{{a}}}'
f'{ {a} }'
f'{ {a: b} }'
f'{a, b}'
f'{(lambda: a)}'
f'{a if b else c}'
f'{a["b"]}'
f"{a['b']}"
f'{a=}'
f'{a = }'
f'{a=!r}'
f'{a=:b}'
f'a' 'b' f'{c}'
f'{f"{a}"}'
f"{'a'}"
f'{"\n"}'
f'{a}\n'
f'{a:\n}'
t'a'
t'{a}'
t'{a!r:b}'
t'{ a }'
t'{a}b{c:{d}}'
1 .real
1.0.real
1j.real
(1).real
(-1).real
a.b().c[d].e
a[b][c]
a()()
(a, b)[c]
[a][b]
{a}[b]
{a: b}[c]
'a'[b]
'a'.b
(a + b).c
(a + b)[c]
(a + b)()
(-a).b
(not a).b
(a if b else c).d
(lambda: a).b
(await a).b
(a := b).c
(a for a in b).c
[a for a in b].c
'''.strip("\n").split("\n")
i = 0
while i < len(EXPRESSIONS):
    src = EXPRESSIONS[i]
    if src.startswith('"""'):
        src = src + "\n" + EXPRESSIONS[i + 1]; i += 1
    i += 1
    ns = {}
    try:
        exec("from __future__ import annotations\nasync def f(a: " + ("(" + src + ")" if False else src) + "): pass" if not src.startswith("*") and "," not in src.split("(")[0].split("[")[0].split("{")[0].split("'")[0].split('"')[0] else "from __future__ import annotations\nx: " + src if not src.startswith("*") else "from __future__ import annotations\ndef f(*a: " + src + "): pass", ns)
        r = ns["f"].__annotations__["a"] if "f" in ns else ns["__annotations__"]["x"]
        print(repr(src), "->", repr(r))
    except BaseException as e:
        print(repr(src), "!!", type(e).__name__, e)
