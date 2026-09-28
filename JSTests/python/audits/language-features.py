#!/usr/bin/env python3
#
# Copyright (C) 2026 Apple Inc. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
# PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
# OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


# What the language reference describes, a piece at a time: lexical analysis, expressions, statements, the data model, generators and coroutines, and the
# execution model. Each piece is a few lines that leave something in `r`. Nothing here is imported but what has to be part of an implementation of the
# language. See run-audits.sh.

import sys
PROBES = []
def probe(group, label, code): PROBES.append((group, label, code))

# ---- lexical
probe("lexical", "named escape", r'''r = "\N{GREEK SMALL LETTER ALPHA}\N{DASH}"''')
probe("lexical", "bytes escapes", r'''r = b"\x00\101\n\'"''')
probe("lexical", "raw and joined", r'''r = r"\n" "a" f"{1}" rb"\x"[0:1].decode()''')
probe("lexical", "numbers", '''r = (0b1_01, 0o17, 0xF_F, 1_000.5e-1_0, 1j, .5j, 1e3j, 10**30)''')
probe("lexical", "unicode identifiers", '''ñ = 1; ℌ = 2; r = (ñ, H)''')
probe("lexical", "line joining", '''r = 1 + \\\n 2''')
probe("lexical", "invalid escape warns", r'''r = eval("'\\d'")''')
probe("lexical", "f-string nesting", '''x = 3; r = f"{x!r:>{x+2}} {f'{x:{"0"}2}'} {x=} { {1:2}[1] } {'a' 'b'}"''')
probe("lexical", "f-string format spec", '''r = f"{3.14159:.2f}|{255:#x}|{1234567:,}|{'s':^7}|{-5:+d}|{0.5:%}|{1e10:g}|{True:d}"''')
probe("lexical", "f-string multi-line and comments", '''r = f"""{
    1 + # comment
    2
}"""''')
probe("lexical", "f-string lambda and walrus", '''r = f"{(lambda: 5)()} {(y := 7)} {y}"''')
probe("lexical", "t-string", '''t = t"a{1+1}b{'x'!r:>4}"; r = (type(t).__name__, t.strings, [(i.value, i.expression, i.conversion, i.format_spec) for i in t.interpolations])''')
probe("lexical", "t-string iter and add", '''t = t"x{1}" + t"y"; r = [type(p).__name__ for p in t]''')

# ---- expressions
probe("expr", "walrus scopes", '''r = [y := 5, y**2, [z := i for i in range(3)], z]''')
probe("expr", "conditional chains", '''r = 1 < 2 < 3 != 4 is not None in [False]''')
probe("expr", "star in displays", '''r = ([*range(2), *"ab"], {*"aa", 1} == {"a", 1}, {**{"a": 1}, "b": 2, **{"a": 3}}, (*[1], 2))''')
probe("expr", "star in index", '''class G:\n    def __getitem__(self, k): return k\nr = (G()[*[1, 2]], G()[1, *"ab", 2:3], G()[...], G()[::], G()[1:2, ::3])''')
probe("expr", "matmul", '''class M:\n    def __matmul__(s, o): return "mm"\n    def __rmatmul__(s, o): return "rmm"\n    def __imatmul__(s, o): return "imm"\nm = M(); a = m @ 1; b = 1 @ m; m @= 2; r = (a, b, m)''')
probe("expr", "power and unary", '''r = (-2**2, 2**-1, (-8)**(1/3) .__class__.__name__, ~5, +True, not 0, 2**3**2, -1 // 2, -7 % 3, 7 % -3, divmod(-7, 2))''')
probe("expr", "int float ops", '''r = (7 // 2.0, 7.5 % 2, 1e308 * 10, 0.1 + 0.2, 5 / 2, 2**0.5, 10**-2, (1 << 70) >> 68, 3 & -2, round(2.5), round(-0.5), round(1234.567, -2), round(2.675, 2))''')
probe("expr", "comparisons mixed", '''r = (1 == 1.0 == True, 1 < 1.5, "a" < "b", [1, 2] < [1, 3], (1,) < (1, 0), {1} < {1, 2}, None is None, 2**64 == 2.0**64, float("nan") != float("nan"))''')
probe("expr", "generator expressions", '''g = (x * y for x in range(3) if x for y in range(x)); r = (type(g).__name__, list(g), sum(i for i in range(4)))''')
probe("expr", "comprehension scoping", '''x = "outer"; r = ([x for x in range(2)], x, [[j for j in range(i)] for i in range(3)], {k: v for k, v in zip("ab", (1, 2))}, sorted({c for c in "aab"}))''')
probe("expr", "class scope comprehension", '''class C:\n    a = 2\n    b = [a for _ in range(2)] if False else [i for i in range(a)]\nr = C.b''')
probe("expr", "lambda defaults and kw", '''f = lambda a, b=2, *c, d, e=5, **k: (a, b, c, d, e, k); r = f(1, d=4, z=9)''')
probe("expr", "await outside", '''r = compile("await x", "<s>", "eval")''')
probe("expr", "slices", '''s = list(range(10)); r = (s[::-2], s[-3:], s[1:8:3], s[100:], s[:-100], "hello"[::-1], (1,2,3)[1:], slice(1, 2, 3).indices(10), s[True:])''')
probe("expr", "slice assign and delete", '''s = list(range(10)); s[2:4] = "abc"; s[::3] = [0] * 4; del s[1::2]; r = s''')
probe("expr", "ellipsis and notimplemented", '''r = (..., Ellipsis is ..., NotImplemented, type(...).__name__)''')
probe("expr", "bool of NotImplemented", '''r = bool(NotImplemented)''')
probe("expr", "call unpacking", '''def f(*a, **k): return a, k\nr = f(1, *[2, 3], 4, *(5,), x=1, **{"y": 2}, **{"z": 3})''')
probe("expr", "call errors", '''def f(a, /, b, *, c): pass\nr = []\nfor args, kw in [((), {}), ((1, 2, 3), {}), ((1,), {"a": 2, "b": 1, "c": 1}), ((1, 2), {"c": 1, "d": 2}), ((1, 2), {"b": 3, "c": 1})]:\n    try: f(*args, **kw)\n    except TypeError as e: r.append(str(e))''')
probe("expr", "dup keyword", '''def f(**k): return k\nr = f(**{"a": 1}, **{"a": 2})''')
probe("expr", "string ops", '''r = ("ab" * 2, "a" in "cat", "%s-%d-%5.2f-%r-%x-%c-%%" % ("s", 3, 2.5, "q", 255, 65), "%(a)s %(b)03d" % {"a": 1, "b": 2}, "{0}{1!r}{a:>3}{0.real}{2[0]}".format(1, "x", [9], a="k"))''')
probe("expr", "bytes ops", '''r = (b"a%db" % 5, b"ab"[0], b"ab"[:1], bytes(2), bytearray(b"x") + b"y", b"%s" % b"z", b"abc".hex(":"), bytes.fromhex("41 42"))''')
probe("expr", "identity of small things", '''a = 256; b = 256; r = (a is b, () is (), "" is "", None is None)''')

# ---- statements
probe("stmt", "augmented targets", '''class A: x = [1]\na = A(); a.x += [2]; d = {"k": 1}; d["k"] **= 3; l = [1, 2]; l[0] <<= 4; r = (A.x, a.x, d, l)''')
probe("stmt", "annotated assignment", '''x: int = 5\ny: "str"\nclass C:\n    a: int = 1\n    b: str\nr = (x, __annotations__, C.__annotations__)''')
probe("stmt", "function annotations", '''def f(a: int, b: "s" = 1, *c: float, d: bool, **e: str) -> list: pass\nr = (f.__annotations__, f.__annotate__ is not None)''')
probe("stmt", "deferred annotations", '''def f(a: Undefined): pass\nr = "defined without evaluating"\ntry: f.__annotations__\nexcept NameError as e: r += "; " + str(e)''')
probe("stmt", "annotate function formats", '''def f(a: int) -> str: pass\nr = (f.__annotate__(1), )''')
probe("stmt", "annotate forwardref format", '''def f(a: Undefined): pass\nr = f.__annotate__(2)''')
probe("stmt", "unpacking targets", '''a, (b, *c), *d, e = 1, (2, 3, 4), 5, 6, 7; [f, g] = "xy"; r = (a, b, c, d, e, f, g)''')
probe("stmt", "unpack errors", '''r = []\nfor src in ["a, b = 1,", "a, b = 1, 2, 3", "a, *b, c = 1,", "a, b = 5", "a, = {}"]:\n    try: exec(src)\n    except Exception as e: r.append(type(e).__name__ + ": " + str(e))''')
probe("stmt", "chained assignment order", '''log = []\nclass D(dict):\n    def __setitem__(s, k, v): log.append(k)\nd = D(); d["a"] = d["b"] = 1; r = log''')
probe("stmt", "del forms", '''a = b = c = 1; l = [1, 2, 3]; del a, (b, [c]), l[0]; r = (l, "a" in dir(), "b" in dir())''')
probe("stmt", "assert", '''r = []\ntry: assert 0, "msg"\nexcept AssertionError as e: r.append(e.args)\ntry: assert ()\nexcept AssertionError as e: r.append(e.args)''')
probe("stmt", "for else while else", '''r = []\nfor i in range(2): pass\nelse: r.append("for-else")\nwhile False: pass\nelse: r.append("while-else")\nfor i in range(3):\n    if i: break\nelse: r.append("no")''')
probe("stmt", "try finally flow", '''def f():\n    for i in range(3):\n        try:\n            if i == 1: continue\n            if i == 2: return "ret"\n        finally:\n            log.append(i)\nlog = []; r = (f(), log)''')
probe("stmt", "finally overrides", '''def f():\n    try: return 1\n    finally: return 2\ndef g():\n    try: raise ValueError\n    finally: return 3\nr = (f(), g())''')
probe("stmt", "except star", '''r = []\ntry: raise ExceptionGroup("g", [ValueError(1), TypeError(2), ExceptionGroup("n", [KeyError(3)])])\nexcept* ValueError as e: r.append(("V", e.exceptions))\nexcept* (TypeError, KeyError) as e: r.append(("TK", str(e), len(e.exceptions)))''')
probe("stmt", "except star reraise", '''try:\n    try: raise ExceptionGroup("g", [ValueError(1), OSError(2)])\n    except* ValueError: pass\nexcept BaseException as e: r = repr(e)''')
probe("stmt", "exception chaining", '''try:\n    try: 1 / 0\n    except ZeroDivisionError as z: raise ValueError("v") from z\nexcept ValueError as e: r = (type(e.__cause__).__name__, type(e.__context__).__name__, e.__suppress_context__)''')
probe("stmt", "raise from None", '''try:\n    try: 1 / 0\n    except ZeroDivisionError: raise ValueError("v") from None\nexcept ValueError as e: r = (e.__cause__, type(e.__context__).__name__, e.__suppress_context__)''')
probe("stmt", "except name unbound", '''try: raise ValueError\nexcept ValueError as e: pass\ntry: e\nexcept NameError as n: r = str(n)''')
probe("stmt", "add_note", '''e = ValueError("x"); e.add_note("n1"); e.add_note("n2"); r = e.__notes__''')
probe("stmt", "with multiple and parens", '''log = []\nclass M:\n    def __init__(s, n): s.n = n\n    def __enter__(s): log.append("in" + s.n); return s.n\n    def __exit__(s, *a): log.append("out" + s.n); return s.n == "b"\nwith (M("a") as x, M("b") as y,):\n    raise ValueError\nr = (log, x, y)''')
probe("stmt", "with missing protocol", '''r = []\nfor o in (1, type("E", (), {"__enter__": lambda s: 1})()):\n    try:\n        with o: pass\n    except TypeError as e: r.append(str(e))''')
probe("stmt", "global nonlocal", '''g = 0\ndef f():\n    n = 0\n    def h():\n        global g; nonlocal n\n        g += 1; n += 2\n        return n\n    return h(), h(), n\nr = (f(), g)''')
probe("stmt", "match literals and captures", '''def m(x):\n    match x:\n        case 0 | 1: return "small"\n        case -1.5 | 2+3j: return "num"\n        case "s" | b"b": return "str"\n        case None | True | False: return "const"\n        case [1, *rest] if rest: return ("seq", rest)\n        case (a, b): return ("pair", a, b)\n        case {"k": v, **o}: return ("map", v, o)\n        case str() | int() as z: return ("type", z)\n        case _: return "other"\nr = [m(v) for v in (0, -1.5, 2+3j, "s", b"b", None, True, [1, 2, 3], (5, 6), {"k": 1, "j": 2}, "zz", 99, 1.0, [], {})]''')
probe("stmt", "match classes", '''class P:\n    __match_args__ = ("x", "y")\n    def __init__(s, x, y): s.x = x; s.y = y\ndef m(v):\n    match v:\n        case P(0, y=0): return "origin"\n        case P(x, y) if x == y: return ("diag", x)\n        case P(x=1): return "x1"\n        case P(): return "P"\nr = [m(P(0, 0)), m(P(2, 2)), m(P(1, 5)), m(P(3, 4)), m(1)]''')
probe("stmt", "match value patterns", '''class K: A = 1; B = "b"\ndef m(v):\n    match v:\n        case K.A: return "A"\n        case K.B: return "B"\nr = [m(1), m("b"), m(2)]''')
probe("stmt", "match errors", '''r = []\nfor src in ["match 1:\\n case x: pass\\n case y: pass", "match 1:\\n case [*a, *b]: pass", "match 1:\\n case {**_}: pass", "match 1:\\n case a | 1: pass", "match 1:\\n case P(x=1, x=2): pass"]:\n    try: compile(src, "<s>", "exec")\n    except SyntaxError as e: r.append(e.msg)''')
probe("stmt", "soft keywords as names", '''match = 1; case = 2; type = 3; _ = 4; r = (match, case, type, _)''')
probe("stmt", "type alias", '''type A = int | str\ntype G[T] = list[T]\nr = (A.__name__, type(A).__name__, repr(A.__value__), G.__type_params__, repr(G[int]))''')
probe("stmt", "lazy type alias", '''type A = Undefined\nr = A.__name__\ntry: A.__value__\nexcept NameError as e: r += " " + str(e)''')
probe("stmt", "generic function", '''def f[T, *Ts, **P](a: T) -> T: return a\nr = (f(1), [type(p).__name__ for p in f.__type_params__], [p.__name__ for p in f.__type_params__])''')
probe("stmt", "generic class", '''class C[T: int, U = str]:\n    def m(self, x: T) -> U: pass\nr = (C.__type_params__, C.__type_params__[0].__bound__, C.__type_params__[1].__default__, [b.__name__ for b in C.__mro__], C.__orig_bases__, repr(C[int, str]))''')
probe("stmt", "typevar constraints lazily", '''def f[T: (int, Undefined)](): pass\nr = "ok"\ntry: f.__type_params__[0].__constraints__\nexcept NameError as e: r = str(e)''')
probe("stmt", "decorators any expression", '''d = {"k": lambda f: "decorated"}\n@d["k"]\ndef f(): pass\n@(lambda c: (c.__name__, "cls"))\nclass C: pass\nr = (f, C)''')
probe("stmt", "class keywords and prepare", '''class M(type):\n    @classmethod\n    def __prepare__(m, n, b, **k): return {"prepared": k}\n    def __new__(m, n, b, ns, **k): return super().__new__(m, n, b, ns)\n    def __init__(c, n, b, ns, **k): c.kw = k\nclass C(metaclass=M, a=1): pass\nr = (C.prepared, C.kw)''')
probe("stmt", "mro entries", '''class Fake:\n    def __mro_entries__(s, bases): return (int,)\nclass C(Fake()): pass\nr = (C.__mro__, type(C.__orig_bases__[0]).__name__)''')
probe("stmt", "__class__ cell and super forms", '''class A:\n    def f(s): return "A"\n    @classmethod\n    def c(k): return "Ac"\n    @staticmethod\n    def st(): return "As"\nclass B(A):\n    def f(s): return (super().f(), super(B, s).f(), __class__.__name__)\n    @classmethod\n    def c(k): return super().c()\nr = (B().f(), B.c(), super(B, B).st())''')
probe("stmt", "private name mangling", '''class C:\n    __x = 1\n    def f(s): s.__y = 2; return s.__x, s.__dict__\n    def __g(s): pass\nr = (C().f(), [n for n in dir(C) if "C__" in n])''')
probe("stmt", "import forms", '''import sys as s, sys\nfrom sys import maxsize as m, path\nr = (s is sys, m == sys.maxsize)''')
probe("stmt", "future annotations", '''exec("from __future__ import annotations\\ndef f(a: Undefined): pass\\nr = f.__annotations__", globals())''')
# ---- data model
probe("model", "attribute hooks", '''class C:\n    def __getattr__(s, n): return "ga:" + n\n    def __setattr__(s, n, v): object.__setattr__(s, n, v * 2)\n    def __delattr__(s, n): log.append(n)\n    def __dir__(s): return ["z", "a"]\nlog = []; c = C(); c.x = 2; del c.x; r = (c.x, c.y, log, dir(c))''')
probe("model", "__getattribute__", '''class C:\n    def __getattribute__(s, n): return n.upper() if n != "__class__" else object.__getattribute__(s, n)\nr = (C().abc, getattr(C(), "q"))''')
probe("model", "descriptors", '''class D:\n    def __set_name__(s, o, n): s.n = n\n    def __get__(s, i, o): return (s.n, i is None, o.__name__)\n    def __set__(s, i, v): i.__dict__["_" + s.n] = v\n    def __delete__(s, i): i.__dict__["deleted"] = 1\nclass C: d = D()\nc = C(); c.d = 5; del c.d; r = (C.d, c.d, c.__dict__)''')
probe("model", "non-data descriptor shadowing", '''class N:\n    def __get__(s, i, o): return "desc"\nclass C: n = N()\nc = C(); a = c.n; c.__dict__["n"] = "inst"; r = (a, c.n)''')
probe("model", "property full", '''class C:\n    def __init__(s): s._x = 0\n    @property\n    def x(s): "doc"; return s._x\n    @x.setter\n    def x(s, v): s._x = v\n    @x.deleter\n    def x(s): s._x = None\nc = C(); c.x = 3; a = c.x; del c.x; r = (a, c.x, C.x.__doc__, C.x.fget.__name__, C.x.__name__)''')
probe("model", "slots", '''class S:\n    __slots__ = ("a", "__weakref__")\ns = S(); s.a = 1\ntry: s.b = 2\nexcept AttributeError as e: r = (s.a, str(e), hasattr(s, "__dict__"))''')
probe("model", "__init_subclass__ kwargs", '''class B:\n    def __init_subclass__(c, /, tag=None, **k): c.tag = tag\nclass C(B, tag="t"): pass\nr = C.tag''')
probe("model", "__class_getitem__", '''class C:\n    def __class_getitem__(c, k): return (c.__name__, k)\nr = C[int, 1]''')
probe("model", "builtin generics", '''r = (repr(list[int]), repr(dict[str, list[int]]), list[int].__origin__, list[int].__args__, type(list[int]).__name__, repr(tuple[int, ...]), list[int]([1]), repr(type[int]))''')
probe("model", "unions", '''u = int | str | None; r = (repr(u), u.__args__, isinstance(1, u), isinstance(1.5, u), type(u).__name__, (int | str) == (str | int), repr(int | "x".__class__), issubclass(bool, int | str))''')
probe("model", "callable objects", '''class C:\n    def __call__(s, *a, **k): return (a, k)\nr = (C()(1, x=2), callable(C()), callable(C), callable(1))''')
probe("model", "container protocol", '''class C:\n    def __init__(s): s.d = {}\n    def __getitem__(s, k): return s.d[k]\n    def __setitem__(s, k, v): s.d[k] = v\n    def __delitem__(s, k): del s.d[k]\n    def __contains__(s, k): return k == "magic"\n    def __len__(s): return 7\n    def __missing__(s, k): return "never"\n    def __reversed__(s): return iter("rev")\n    def __length_hint__(s): return 3\nc = C(); c[1] = 2; a = c[1]; del c[1]; r = (a, "magic" in c, len(c), list(reversed(c)), bool(c))''')
probe("model", "dict __missing__", '''class D(dict):\n    def __missing__(s, k): return k * 2\nr = (D()["ab"], D().get("x"))''')
probe("model", "sequence fallback iteration", '''class S:\n    def __getitem__(s, i):\n        if i > 2: raise IndexError\n        return i * 10\nr = (list(S()), 20 in S(), list(reversed([1, 2])), tuple(S()))''')
probe("model", "__index__ __int__ __float__ etc", '''class N:\n    def __index__(s): return 3\n    def __int__(s): return 4\n    def __float__(s): return 5.5\n    def __complex__(s): return 1j\n    def __bool__(s): return False\n    def __round__(s, n=None): return ("r", n)\n    def __trunc__(s): return 6\n    def __floor__(s): return 7\n    def __ceil__(s): return 8\n    def __abs__(s): return 9\n    def __pos__(s): return 10\nn = N(); r = ([0, 1, 2, 3][n], int(n), float(n), complex(n), bool(n), round(n), round(n, 2), abs(n), +n, hex(n), bin(n), range(5)[n], "abcd"[n], 2 * [1][:n].__len__())''')
probe("model", "rich comparison reflection", '''log = []\nclass A:\n    def __lt__(s, o): log.append("A.lt"); return NotImplemented\n    def __eq__(s, o): log.append("A.eq"); return NotImplemented\nclass B:\n    def __gt__(s, o): log.append("B.gt"); return True\n    def __eq__(s, o): log.append("B.eq"); return NotImplemented\nr = (A() < B(), A() == B(), A() != B(), log)''')
probe("model", "subclass reflected priority", '''class A:\n    def __add__(s, o): return "A.add"\n    def __radd__(s, o): return "A.radd"\nclass B(A):\n    def __radd__(s, o): return "B.radd"\nr = (A() + B(), A() + A(), 1 + B())''')
probe("model", "hash rules", '''class E:\n    def __eq__(s, o): return True\nclass H(E):\n    __hash__ = object.__hash__\nr = (E.__hash__, isinstance(hash(H()), int), hash(1) == hash(1.0) == hash(True), hash(-1), hash(2**61 - 1), hash("") == 0, hash(()) == hash(()), hash(float("inf")), hash(0.5))''')
probe("model", "unhashable", '''r = []\nfor v in ([], {}, set(), bytearray(), slice(1)):\n    try: r.append(isinstance(hash(v), int))\n    except TypeError as e: r.append(str(e))''')
probe("model", "__repr__ __str__ __format__ __bytes__", '''class C:\n    def __repr__(s): return "R"\n    def __str__(s): return "S"\n    def __format__(s, f): return "F" + f\n    def __bytes__(s): return b"B"\nc = C(); r = (repr(c), str(c), format(c, "x"), f"{c}{c!r}{c!s}{c:y}", bytes(c), "%s%r" % (c, c), [c], ascii("é"))''')
probe("model", "__new__ returning other", '''class C:\n    def __new__(k): return 5\n    def __init__(s): raise RuntimeError\nr = C()''')
probe("model", "metaclass call and instancecheck", '''class M(type):\n    def __call__(c, *a): return ("called", a)\n    def __instancecheck__(c, i): return i == 1\n    def __subclasscheck__(c, s): return s is int\nclass C(metaclass=M): pass\nr = (C(5), isinstance(1, C), isinstance(2, C), issubclass(int, C), issubclass(str, C))''')
probe("model", "metaclass conflict", '''class M1(type): pass\nclass M2(type): pass\nclass A(metaclass=M1): pass\nclass B(metaclass=M2): pass\ntry:\n    class C(A, B): pass\nexcept TypeError as e: r = str(e)''')
probe("model", "mro C3 and failure", '''class A: pass\nclass B(A): pass\nclass C(A): pass\nclass D(B, C): pass\nr = [k.__name__ for k in D.__mro__]\ntry:\n    class E(A, B): pass\nexcept TypeError as e: r.append(str(e))''')
probe("model", "abstract methods", '''class A:\n    def f(s): pass\n    f.__isabstractmethod__ = True\nA.__abstractmethods__ = frozenset({"f"})\ntry: A()\nexcept TypeError as e: r = str(e)''')
probe("model", "__bases__ assignment", '''class A:\n    def f(s): return "A"\nclass B:\n    def f(s): return "B"\nclass C(A): pass\nc = C(); a = c.f(); C.__bases__ = (B,); r = (a, c.f(), C.__mro__)''')
probe("model", "type attributes", '''class C:\n    "doc"\n    def m(s): pass\nr = (C.__name__, C.__qualname__, C.__module__, C.__doc__, C.__dict__["m"].__qualname__, type(C.__dict__).__name__, C.__firstlineno__, C.__static_attributes__, C.__weakref__.__class__.__name__)''')
probe("model", "function attributes", '''def f(a, b=1, *, c=2):\n    "d"\n    return a\nf.x = 1\nr = (f.__name__, f.__qualname__, f.__defaults__, f.__kwdefaults__, f.__doc__, f.__dict__, f.__module__, f.__closure__, f.__globals__ is globals(), type(f.__code__).__name__, f.__builtins__ is __builtins__.__dict__ if hasattr(__builtins__, "__dict__") else True)''')
probe("model", "set function attributes", '''def f(a, b=1): return (a, b)\nf.__defaults__ = (9,); f.__name__ = "g"; f.__qualname__ = "Q.g"; f.__doc__ = "n"\nr = (f(1), f.__name__, repr(f)[:14])''')
probe("model", "code attributes", '''def f(a, b=1, *c, d, **e):\n    x = a; y = len\n    def g(): return x\n    return g\nc = f.__code__\nr = (c.co_name, c.co_qualname, c.co_argcount, c.co_posonlyargcount, c.co_kwonlyargcount, c.co_nlocals, c.co_varnames, c.co_cellvars, c.co_freevars, c.co_names, c.co_flags, c.co_firstlineno, c.co_consts[0] is None or True, type(c.co_code).__name__)''')
probe("model", "code replace and exec", '''def f(): return 1\nc = f.__code__.replace(co_name="z")\nr = (c.co_name, eval(c))''')
probe("model", "function from code", '''def f(a): return a + n\ng = type(f)(f.__code__, {"n": 10}, "g", (5,))\nr = (g(), g(1), g.__name__)''')
probe("model", "closures and cells", '''def f():\n    x = 1\n    def g(): return x\n    return g\ng = f(); c = g.__closure__[0]; a = c.cell_contents; c.cell_contents = 5; r = (a, g(), type(c).__name__)''')
probe("model", "bound methods", '''class C:\n    def m(s): pass\nc = C(); r = (c.m.__self__ is c, c.m.__func__ is C.m, c.m == c.m, c.m is c.m, hash(c.m) == hash(c.m), c.m.__name__, type(c.m).__name__, type(C.m).__name__)''')
probe("model", "staticmethod classmethod objects", '''class C:\n    @staticmethod\n    def s(): return 1\n    @classmethod\n    def c(k): return k\nr = (type(C.__dict__["s"]).__name__, C.__dict__["s"].__func__(), C.__dict__["s"](), C.__dict__["c"].__func__.__name__, C.__dict__["c"].__wrapped__.__name__, C.c.__self__ is C)''')
probe("model", "module attributes", '''m = type(sys)("m", "doc"); m.x = 1; r = (m.__name__, m.__doc__, m.__dict__["x"], repr(m), m.__spec__, m.__loader__, m.__package__)''')
probe("model", "module getattr dir", '''m = type(sys)("m"); exec("def __getattr__(n): return n * 2\\ndef __dir__(): return ['q']", m.__dict__); r = (m.ab, dir(m))''')
probe("model", "__del__ runs", '''log = []\nclass C:\n    def __del__(s): log.append("del")\nc = C(); del c\nr = log''')
probe("model", "weakref via type", '''class C: pass\nr = C.__weakrefoffset__ != 0''')
probe("model", "context of __eq__ default and ne", '''class C:\n    def __eq__(s, o): return True\nr = (C() != C(), C() == 1, 1 == C())''')
probe("model", "__contains__ fallback and identity", '''n = float("nan"); r = (n in [n], n in (n,), n in {n}, [n] == [n], n == n)''')
probe("model", "object protocol", '''o = object(); r = (o.__sizeof__() > 0, type(o.__reduce_ex__(2)).__name__, o.__init_subclass__() if False else None, object.__subclasshook__(int), type(o.__dir__()).__name__, o.__format__(""), o.__getstate__())''')
probe("model", "copy protocol", '''class C:\n    def __init__(s): s.a = 1\nr = C().__reduce_ex__(4)[1:3]''')
probe("model", "buffer protocol", '''class B:\n    def __buffer__(s, flags): return memoryview(b"xyz")\nr = (bytes(memoryview(B())), bytes(B()))''')
probe("model", "memoryview", '''m = memoryview(bytearray(b"abcd")); m[0] = 65; s = m[1:3]; r = (m.tobytes(), s.tolist(), m.format, m.itemsize, m.shape, m.readonly, len(m), m.cast("H").tolist() != [], m.nbytes)''')

# ---- generators, coroutines
probe("gen", "send throw close", '''def g():\n    try:\n        x = yield 1\n        y = yield x\n    except ValueError as e:\n        yield ("caught", str(e))\n    finally:\n        log.append("fin")\nlog = []; i = g(); a = next(i); b = i.send("s"); c = i.throw(ValueError("v")); i.close(); r = (a, b, c, log)''')
probe("gen", "yield from and return", '''def a():\n    x = yield 1\n    return ("ret", x)\ndef b():\n    v = yield from a()\n    yield v\n    yield from "xy"\ni = b(); r = [next(i), i.send(5), next(i), next(i)]''')
probe("gen", "StopIteration value and conversion", '''def g():\n    return 5\n    yield\ndef h():\n    raise StopIteration\n    yield\nr = []\ntry: next(g())\nexcept StopIteration as e: r.append(e.value)\ntry: next(h())\nexcept RuntimeError as e: r.append((str(e), type(e.__cause__).__name__))''')
probe("gen", "generator attributes", '''def g(): yield\ni = g(); r = (i.gi_running, i.gi_suspended, type(i.gi_frame).__name__, i.gi_code.co_name, i.gi_yieldfrom, i.__name__, i.__qualname__); next(i); r += (i.gi_suspended,); list(i); r += (i.gi_frame,)''')
probe("gen", "close returns value", '''def g():\n    try: yield\n    except GeneratorExit: return 7\ni = g(); next(i); r = i.close()''')
probe("gen", "already executing", '''def g():\n    yield next(me)\nme = g()\ntry: next(me)\nexcept ValueError as e: r = str(e)''')
probe("gen", "coroutine basics", '''async def c(): return 5\nx = c()\ntry: x.send(None)\nexcept StopIteration as e: r = (e.value, type(x).__name__, x.cr_running, x.cr_frame, x.cr_await)''')
probe("gen", "await protocol", '''class A:\n    def __await__(s):\n        v = yield "susp"\n        return v * 2\nasync def c(): return await A()\nx = c(); a = x.send(None)\ntry: x.send(4)\nexcept StopIteration as e: r = (a, e.value)''')
probe("gen", "async for and with", '''class I:\n    def __init__(s): s.n = 0\n    def __aiter__(s): return s\n    async def __anext__(s):\n        s.n += 1\n        if s.n > 2: raise StopAsyncIteration\n        return s.n\nclass M:\n    async def __aenter__(s): return "e"\n    async def __aexit__(s, *a): log.append("x")\nlog = []\nasync def c():\n    out = []\n    async with M() as m: out.append(m)\n    async for v in I(): out.append(v)\n    else: out.append("else")\n    return out + [x async for x in I()] + [await d() for _ in range(1)]\nasync def d(): return "d"\ntry: c().send(None)\nexcept StopIteration as e: r = (e.value, log)''')
probe("gen", "async generator", '''async def ag():\n    x = yield 1\n    yield x\nasync def c():\n    g = ag(); a = await g.__anext__(); b = await g.asend("s"); await g.aclose()\n    return (a, b, type(g).__name__, g.ag_running)\ntry: c().send(None)\nexcept StopIteration as e: r = e.value''')
probe("gen", "async comprehension in sync fn", '''r = compile("def f():\\n [x async for x in y]", "<s>", "exec")''')
probe("gen", "never awaited warning", '''async def c(): pass\nc(); r = "no crash"''')
probe("gen", "reuse coroutine", '''async def c(): pass\nx = c()\ntry: x.send(None)\nexcept StopIteration: pass\ntry: x.send(None)\nexcept RuntimeError as e: r = str(e)''')
probe("gen", "aiter anext builtins", '''async def ag():\n    yield 1\nasync def c():\n    i = aiter(ag()); return (await anext(i), await anext(i, "dflt"))\ntry: c().send(None)\nexcept StopIteration as e: r = e.value''')
probe("gen", "sys async gen hooks", '''r = tuple(sys.get_asyncgen_hooks())''')
probe("gen", "coroutine origin and wrapper", '''r = sys.get_coroutine_origin_tracking_depth()''')

# ---- execution model, builtins that are language
probe("exec", "eval exec compile modes", '''ns = {}; exec("a = 1\\nb = a + 1", ns); c = compile("a * 10", "<s>", "eval"); s = compile("5", "<s>", "single"); r = (ns["b"], eval(c, ns), eval("x", {"x": 1}, {"x": 2}), type(c).__name__)''')
probe("exec", "exec with mapping locals", '''class L(dict):\n    def __getitem__(s, k): return "L" + k if k == "zz" else dict.__getitem__(s, k)\nl = L(); exec("q = zz", {}, l); r = l["q"]''')
probe("exec", "locals semantics", '''def f():\n    x = 1; l = locals(); l["x"] = 2; l["y"] = 3\n    return x, sorted(locals())\nr = f()''')
probe("exec", "frame locals proxy", '''def f():\n    x = 1; p = sys._getframe().f_locals; p["x"] = 2\n    return x, type(p).__name__\nr = f()''')
probe("exec", "class body namespace lookups", '''x = "g"\ndef f():\n    x = "f"\n    class C:\n        y = x\n        x = "c"\n        z = x\n    return C.y, C.z\nr = f()''')
probe("exec", "unbound local and free", '''r = []\ndef f():\n    print(x); x = 1\ndef g():\n    def h(): return y\n    h(); y = 1\nfor fn in (f, g):\n    try: fn()\n    except NameError as e: r.append((type(e).__name__, str(e)))''')
probe("exec", "name error suggestions", '''foo_bar = 1\ntry: foo_baz\nexcept NameError as e: r = (str(e), e.name)''')
probe("exec", "attribute error fields", '''try: (1).nope\nexcept AttributeError as e: r = (str(e), e.name, e.obj)''')
probe("exec", "recursion limit", '''def f(): return f()\ntry: f()\nexcept RecursionError as e: r = str(e)''')
probe("exec", "set recursion limit", '''old = sys.getrecursionlimit(); sys.setrecursionlimit(50)\ndef f(n): return n and f(n - 1)\ntry: f(100); r = "no limit"\nexcept RecursionError: r = "limited"\nsys.setrecursionlimit(old)''')
probe("exec", "traceback objects", '''def f(): 1 / 0\ntry: f()\nexcept ZeroDivisionError as e:\n    t = e.__traceback__; r = (t.tb_lineno, t.tb_next.tb_frame.f_code.co_name, t.tb_next.tb_next, type(t).__name__, t.tb_frame.f_lineno)''')
probe("exec", "co_positions", '''def f(): return 1 + x\nr = len(list(f.__code__.co_positions())) > 0, list(f.__code__.co_lines())[0][2]''')
probe("exec", "settrace", '''ev = []\ndef tr(f, e, a): ev.append((e, f.f_lineno - f.f_code.co_firstlineno)); return tr\ndef f():\n    a = 1\n    return a\nsys.settrace(tr); f(); sys.settrace(None); r = ev''')
probe("exec", "setprofile", '''ev = []\ndef pr(f, e, a): ev.append(e)\ndef f(): len("")\nsys.setprofile(pr); f(); sys.setprofile(None); r = ev''')
probe("exec", "sys.monitoring", '''r = (hasattr(sys, "monitoring"), sys.monitoring.events.CALL > 0)''')
probe("exec", "excepthook and unraisable", '''r = (callable(sys.excepthook), callable(sys.unraisablehook), sys.__excepthook__ is sys.excepthook)''')
probe("exec", "audit hooks", '''log = []\nsys.addaudithook(lambda e, a: log.append(e) if e == "my.event" else None); sys.audit("my.event", 1); r = log''')
probe("exec", "intern getsizeof refcount", '''r = (sys.intern("abc") is sys.intern("abc"), sys.getsizeof(1) > 0, sys.getrefcount(None) > 0)''')
probe("exec", "sys structs", '''r = (sys.flags.optimize, sys.float_info.max > 1e300, sys.int_info.bits_per_digit, sys.hash_info.modulus, sys.implementation.name != "", sys.version_info.major, sys.version_info[:2], type(sys.version_info).__name__)''')
probe("exec", "int max str digits", '''try: str(10**5000)\nexcept ValueError as e: r = str(e)[:40]''')
probe("exec", "__debug__ and optimize", '''r = (__debug__, eval(compile("__debug__", "<s>", "eval", optimize=1)))''')
probe("exec", "compile flags ast", '''import _ast\nt = compile("x = 1", "<s>", "exec", _ast.PyCF_ONLY_AST); r = (type(t).__name__, type(t.body[0]).__name__, t.body[0].targets[0].id)''')
probe("exec", "builtins override", '''ns = {"__builtins__": {"len": lambda x: "mine"}}; r = eval("len(1)", ns)''')
probe("exec", "__build_class__ override", '''import builtins\nr = callable(builtins.__build_class__)''')
probe("exec", "breakpoint hook", '''sys.breakpointhook = lambda *a, **k: log.append("bp"); log = []; breakpoint(); r = log''')
probe("exec", "displayhook", '''out = []; old = sys.displayhook; sys.displayhook = out.append; exec(compile("1 + 1", "<s>", "single")); sys.displayhook = old; r = out''')
probe("exec", "warnings from the compiler", '''r = compile("1 is 1", "<s>", "eval") is not None''')
probe("exec", "gc module", '''import gc\nr = (gc.isenabled(), isinstance(gc.collect(), int))''')
probe("exec", "weakref module", '''import _weakref\nclass C: pass\nc = C(); w = _weakref.ref(c); r = (w() is c, _weakref.getweakrefcount(c))''')
probe("exec", "contextvars", '''import _contextvars\nv = _contextvars.ContextVar("v", default=1); t = v.set(2); a = v.get(); v.reset(t); r = (a, v.get())''')

def strip_addresses(text):
    out = []; i = 0
    while i < len(text):
        if text.startswith(" at 0x", i):
            out.append(" at 0x"); i += 6
            while i < len(text) and text[i] in "0123456789abcdef": i += 1
        else:
            out.append(text[i]); i += 1
    return "".join(out)

def run():
    for group, label, code in PROBES:
        ns = {"__name__": "__main__", "sys": sys}
        try:
            exec(compile(code, "<probe>", "exec"), ns)
            out = strip_addresses(repr(ns.get("r", "<no r>")))
        except BaseException as e:
            out = "!! " + type(e).__name__ + ": " + str(e)
        print(group + " | " + label + " => " + out)
run()
