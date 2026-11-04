#!/usr/bin/env python3
"""Bytes per object, measured: N objects of a kind are made in a fresh process and kept; the growth of the process is divided by N.

    python3 memory.py [-j N] [-o out.json] [part of a name ...]      every kind (or those that match) under the interpreter that runs this
    python3 memory.py --table ours.json reference.json              the two side by side

anon: growth of the anonymous memory of the process (RssAnon), after a full collection: what the object really costs, with slack and fragmentation.
heap, extra (only where the engine tells): growth of the collector's live cells, and of the memory outside the heap that cells have reported.
getsizeof: sys.getsizeof() of one of them (shallow).
The list that keeps the objects exists before the first measurement.
"""
import gc, json, os, subprocess, sys

# name, how many, setup, expression in i
KINDS = [
    ("object()", 400000, "", "object()"),
    ("instance, no attributes", 400000, "class C: pass", "C()"),
    ("instance, 1 attribute", 400000, "class C:\n def __init__(self, i): self.a = i", "C(i)"),
    ("instance, 3 attributes", 400000, "class C:\n def __init__(self, i): self.a = i; self.b = i; self.c = i", "C(i)"),
    ("instance, 10 attributes", 200000, "class C:\n def __init__(self, i):\n  self.a = self.b = self.c = self.d = self.e = self.f = self.g = self.h = self.j = self.k = i", "C(i)"),
    ("instance, 3 attributes, __dict__ asked for", 200000, "class C:\n def __init__(self, i): self.a = i; self.b = i; self.c = i; self.__dict__", "C(i)"),
    ("instance, 3 attributes set from outside", 200000, "class C: pass\ndef mk(i):\n o = C(); o.a = i; o.b = i; o.c = i\n return o", "mk(i)"),
    ("instance, 3 attributes by setattr()", 200000, "class C: pass\ndef mk(i):\n o = C()\n for n in ('a', 'b', 'c'): setattr(o, n, i)\n return o", "mk(i)"),
    ("instance, 3 attributes by __dict__.update", 200000, "class C:\n def __init__(self, **kw): self.__dict__.update(kw)", "C(a=i, b=i, c=i)"),
    ("__slots__, none", 400000, "class C: __slots__ = ()", "C()"),
    ("__slots__, 1", 400000, "class C:\n __slots__ = ('a',)\n def __init__(self, i): self.a = i", "C(i)"),
    ("__slots__, 3", 400000, "class C:\n __slots__ = ('a', 'b', 'c')\n def __init__(self, i): self.a = i; self.b = i; self.c = i", "C(i)"),
    ("__slots__, 10", 200000, "class C:\n __slots__ = tuple('abcdefghjk')\n def __init__(self, i):\n  self.a = self.b = self.c = self.d = self.e = self.f = self.g = self.h = self.j = self.k = i", "C(i)"),
    ("instance of a subclass of list", 200000, "class C(list): pass", "C()"),
    ("instance of a subclass of dict", 200000, "class C(dict): pass", "C()"),
    ("instance of a subclass of int", 200000, "class C(int): pass", "C(i)"),
    ("instance of a subclass of str", 200000, "class C(str): pass", "C('x')"),
    ("instance of a subclass of tuple", 200000, "class C(tuple): pass", "C((i, i))"),

    ("dict, empty", 400000, "", "{}"),
    ("dict, 1 str key", 400000, "", "{'a': i}"),
    ("dict, 3 str keys", 200000, "", "{'a': i, 'b': i, 'c': i}"),
    ("dict, 5 str keys", 200000, "", "{'a': i, 'b': i, 'c': i, 'd': i, 'e': i}"),
    ("dict, 8 str keys", 200000, "", "{'a': i, 'b': i, 'c': i, 'd': i, 'e': i, 'f': i, 'g': i, 'h': i}"),
    ("dict, 100 str keys", 10000, "ks = ['k%d' % j for j in range(100)]", "dict.fromkeys(ks, i)"),
    ("dict, 100000 str keys", 10, "ks = ['k%d' % j for j in range(100000)]", "dict.fromkeys(ks, i)"),
    ("dict, 5 int keys", 200000, "", "{0: i, 1: i, 2: i, 3: i, 4: i}"),
    ("dict, 100000 int keys", 10, "ks = list(range(100000))", "dict.fromkeys(ks, i)"),
    ("dict, 5 keys, filled one by one", 200000, "def mk(i):\n d = {}\n for k in ('a', 'b', 'c', 'd', 'e'): d[k] = i\n return d", "mk(i)"),
    ("dict(a=, b=, c=)", 200000, "", "dict(a=i, b=i, c=i)"),
    ("dict from json.loads, 5 keys", 100000, "import json\ns = '{\"id\": 1, \"name\": \"x\", \"value\": 2.5, \"ok\": true, \"tags\": null}'", "json.loads(s)"),
    ("set, empty", 400000, "", "set()"),
    ("set, 1 int", 200000, "", "{i}"),
    ("set, 5 ints", 200000, "", "{i, i + 1, i + 2, i + 3, i + 4}"),
    ("set, 100 ints", 10000, "", "set(range(i, i + 100))"),
    ("set, 100000 ints", 10, "", "set(range(i, i + 100000))"),
    ("frozenset, 5 ints", 200000, "", "frozenset((i, i + 1, i + 2, i + 3, i + 4))"),
    ("list, empty", 400000, "", "[]"),
    ("list, 1 item", 400000, "", "[i]"),
    ("list, 3 items", 400000, "", "[i, i, i]"),
    ("list, 10 items", 200000, "", "[i, i, i, i, i, i, i, i, i, i]"),
    ("list, 100 items", 20000, "", "[i] * 100"),
    ("list, 100000 items", 10, "", "[i] * 100000"),
    ("list, 10 items appended", 200000, "def mk(i):\n l = []\n for j in range(10): l.append(i)\n return l", "mk(i)"),
    ("list, 1000 items appended", 2000, "def mk(i):\n l = []\n for j in range(1000): l.append(i)\n return l", "mk(i)"),
    ("list of 3 floats", 200000, "", "[i + 0.5, i + 0.25, i + 0.125]"),
    ("list of 3 strs (shared)", 200000, "", "['a', 'b', 'c']"),
    ("tuple, 1 item", 400000, "", "(i,)"),
    ("tuple, 2 items", 400000, "", "(i, i)"),
    ("tuple, 3 items", 400000, "", "(i, i, i)"),
    ("tuple, 10 items", 200000, "", "(i, i, i, i, i, i, i, i, i, i)"),
    ("tuple, 100 items", 20000, "", "(i,) * 100"),

    ("str, 8 ASCII", 400000, "", "'%08d' % i"),
    ("str, 8 ASCII, str(i)", 400000, "", "str(i + 10000000)"),
    ("str, 8 ASCII, f-string", 400000, "", "f'{i:08d}'"),
    ("str, 20 ASCII", 400000, "", "'%020d' % i"),
    ("str, 100 ASCII", 100000, "", "'%0100d' % i"),
    ("str, 1000 ASCII", 20000, "", "'%01000d' % i"),
    ("str, 20 Latin-1", 200000, "", "'\\xe9%019d' % i"),
    ("str, 20 BMP", 200000, "", "'\\u4e00%019d' % i"),
    ("str, 20 astral", 200000, "", "'\\U0001f600%019d' % i"),
    ("str, 1000 BMP", 20000, "", "'\\u4e00%0999d' % i"),
    ("str, 1000 astral, indexed", 20000, "def mk(i):\n s = '\\U0001f600%0999d' % i\n s[500]\n return s", "mk(i)"),
    ("str, a + b of 8 + 8", 200000, "", "('%08d' % i) + ('%08d' % i)"),
    ("str, a + b of 100 + 100", 100000, "", "('%0100d' % i) + ('%0100d' % i)"),
    ("str, a + b of 100 + 100, then hashed", 100000, "def mk(i):\n s = ('%0100d' % i) + ('%0100d' % i)\n hash(s)\n return s", "mk(i)"),
    ("str, slice of 20 of a str of 10000", 20000, "def mk(i):\n s = '%010000d' % i\n return s[100:120]", "mk(i)"),
    ("str, slice of 20 of one shared str", 200000, "big = 'x' * 1000000", "big[i:i + 20]"),
    ("str, ''.join of 10 of 8", 100000, "", "''.join(['%08d' % i] * 10)"),
    ("str, split of a line into 5 (per line)", 100000, "", "('%08d,%08d,%08d,%08d,%08d' % (i, i, i, i, i)).split(',')"),
    ("str, 8 ASCII, used as a dict key", 200000, "d = {}\ndef mk(i):\n s = '%08d' % i\n d[s] = None\n return s", "mk(i)"),
    ("str, 8 ASCII, sys.intern", 200000, "import sys", "sys.intern('%08d' % i)"),
    ("bytes, 8", 400000, "", "b'%08d' % i"),
    ("bytes, 100", 100000, "", "b'%0100d' % i"),
    ("bytes, 1000", 20000, "", "b'%01000d' % i"),
    ("bytearray, empty", 200000, "", "bytearray()"),
    ("bytearray, 8", 200000, "", "bytearray(8)"),
    ("bytearray, 1000", 20000, "", "bytearray(1000)"),
    ("memoryview of shared bytes", 200000, "b = bytes(100)", "memoryview(b)"),
    ("io.BytesIO()", 100000, "import io", "io.BytesIO()"),
    ("io.StringIO()", 100000, "import io", "io.StringIO()"),
    ("array('i', 10 items)", 100000, "import array", "array.array('i', range(10))"),

    ("int, below 2**31", 400000, "", "i + 1000"),
    ("int, 2**40", 400000, "", "i + 2**40"),
    ("int, 2**60", 400000, "", "i + 2**60"),
    ("int, 2**70", 400000, "", "i + 2**70"),
    ("int, 2**200", 200000, "", "i + 2**200"),
    ("float, with a fraction", 400000, "", "i + 0.5"),
    ("float, whole", 400000, "", "float(i)"),
    ("complex", 400000, "", "complex(i, 1)"),
    ("range", 400000, "", "range(i)"),
    ("slice", 400000, "", "slice(i)"),

    ("function (def in a function)", 200000, "def mk(i):\n def f(): pass\n return f", "mk(i)"),
    ("lambda", 200000, "", "lambda: None"),
    ("closure, 1 cell", 200000, "def mk(i):\n def f(): return i\n return f", "mk(i)"),
    ("closure, 3 cells", 200000, "def mk(i):\n a = b = i\n def f(): return i, a, b\n return f", "mk(i)"),
    ("function with 2 defaults", 200000, "def mk(i):\n def f(a=i, b=None): pass\n return f", "mk(i)"),
    ("function with annotations", 200000, "def mk(i):\n def f(a: int, b: str) -> None: pass\n return f", "mk(i)"),
    ("function with an attribute", 200000, "def mk(i):\n def f(): pass\n f.x = i\n return f", "mk(i)"),
    ("function, called once", 200000, "def mk(i):\n def f(): pass\n f()\n return f", "mk(i)"),
    ("bound method", 400000, "class C:\n def m(self): pass\no = C()", "o.m"),
    ("bound method of a built-in", 400000, "l = []", "l.append"),
    ("cell", 400000, "import types", "types.CellType(i)"),
    ("functools.partial", 200000, "import functools\ndef f(a): pass", "functools.partial(f, i)"),
    ("property", 200000, "def f(s): pass", "property(f)"),
    ("staticmethod", 200000, "def f(s): pass", "staticmethod(f)"),
    ("classmethod", 200000, "def f(s): pass", "classmethod(f)"),
    ("code object, small function, compiled", 5000, "", "compile('def f(a, b):\\n    c = a + b + %d\\n    return c * 2\\n' % i, 'x.py', 'exec')"),
    ("code object, small function, defined", 5000, "def mk(i):\n ns = {}\n exec(compile('def f(a, b):\\n    c = a + b + %d\\n    return c * 2\\n' % i, 'x.py', 'exec'), ns)\n return ns['f']", "mk(i)"),
    ("code object, small function, defined and called", 5000, "def mk(i):\n ns = {}\n exec(compile('def f(a, b):\\n    c = a + b + %d\\n    return c * 2\\n' % i, 'x.py', 'exec'), ns)\n ns['f'](1, 2)\n return ns['f']", "mk(i)"),
    ("code object, 50-line function, defined and called", 2000, "body = ''.join('    x%d = a.attr%d(b, %d) + len(c[%d:]) if a else None\\n' % (j, j, j, j) for j in range(50))\ndef mk(i):\n ns = {}\n exec(compile('def f(a, b, c):\\n' + body + '    return %d\\n' % i, 'x.py', 'exec'), ns)\n ns['f'](None, 1, 2)\n return ns['f']", "mk(i)"),
    ("class, empty (type())", 5000, "", "type('C%d' % i, (), {})"),
    ("class, empty (statement)", 5000, "def mk(i):\n class C: pass\n return C", "mk(i)"),
    ("class, 5 methods (statement)", 5000, "def mk(i):\n class C:\n  def __init__(self): self.a = 1\n  def m1(self): pass\n  def m2(self): pass\n  def m3(self): pass\n  def __repr__(self): return ''\n return C", "mk(i)"),
    ("class, 5 methods, one instance made", 5000, "def mk(i):\n class C:\n  def __init__(self): self.a = 1\n  def m1(self): pass\n  def m2(self): pass\n  def m3(self): pass\n  def __repr__(self): return ''\n C().m1()\n return C", "mk(i)"),
    ("module object", 50000, "import types", "types.ModuleType('m')"),
    ("types.SimpleNamespace(a=, b=)", 200000, "import types", "types.SimpleNamespace(a=i, b=i)"),

    ("frame object", 100000, "import sys\ndef g(a, b): return sys._getframe()", "g(i, i)"),
    ("exception, not raised", 200000, "", "ValueError(i)"),
    ("exception, raised and caught", 100000, "def mk(i):\n try: raise ValueError(i)\n except ValueError as e: return e", "mk(i)"),
    ("exception, raised 5 calls deep", 50000, "def r(n, i):\n if n: r(n - 1, i)\n else: raise ValueError(i)\ndef mk(i):\n try: r(5, i)\n except ValueError as e: return e", "mk(i)"),
    ("exception, raised, traceback dropped", 100000, "def mk(i):\n try: raise ValueError(i)\n except ValueError as e: return e.with_traceback(None)", "mk(i)"),
    ("exception from a built-in (KeyError)", 100000, "d = {}\ndef mk(i):\n try: d[i]\n except KeyError as e: return e", "mk(i)"),
    ("AttributeError from a missing attribute", 100000, "class C: pass\no = C()\ndef mk(i):\n try: o.x\n except AttributeError as e: return e", "mk(i)"),
    ("generator, not started", 200000, "def g(a):\n yield a", "g(i)"),
    ("generator, suspended", 200000, "def g(a):\n yield a\n yield a\ndef mk(i):\n x = g(i); next(x)\n return x", "mk(i)"),
    ("generator, suspended, 10 locals", 100000, "def g(a):\n b = c = d = e = f = h = j = k = l = a\n yield a\n yield b, c, d, e, f, h, j, k, l\ndef mk(i):\n x = g(i); next(x)\n return x", "mk(i)"),
    ("generator, finished", 200000, "def g(a):\n yield a\ndef mk(i):\n x = g(i); list(x)\n return x", "mk(i)"),
    ("generator expression, not started", 200000, "l = []", "(x for x in l)"),
    ("coroutine, not started", 200000, "import warnings; warnings.simplefilter('ignore')\nasync def c(a): return a", "c(i)"),
    ("coroutine, suspended", 100000, "import warnings, types; warnings.simplefilter('ignore')\n@types.coroutine\ndef y(): yield\nasync def c(a):\n await y()\ndef mk(i):\n x = c(i); x.send(None)\n return x", "mk(i)"),
    ("async generator, not started", 100000, "async def g(a):\n yield a", "g(i)"),
    ("list iterator", 400000, "l = [1]", "iter(l)"),
    ("dict items view", 400000, "d = {}", "d.items()"),
    ("enumerate", 200000, "l = [1]", "enumerate(l)"),
    ("zip", 200000, "l = [1]", "zip(l, l)"),
    ("map", 200000, "l = [1]", "map(str, l)"),

    ("weakref.ref (with its instance)", 200000, "import weakref\nclass C: pass\nos_ = []\ndef mk(i):\n o = C(); os_.append(o)\n return weakref.ref(o)", "mk(i)"),
    ("weakref.ref with a callback (with its instance)", 200000, "import weakref\nclass C: pass\nos_ = []\ndef cb(r): pass\ndef mk(i):\n o = C(); os_.append(o)\n return weakref.ref(o, cb)", "mk(i)"),
    ("instance with __del__", 200000, "class C:\n def __del__(self): pass", "C()"),
    ("namedtuple, 3 fields", 200000, "import collections\nP = collections.namedtuple('P', 'a b c')", "P(i, i, i)"),
    ("dataclass, 3 fields", 200000, "import dataclasses\n@dataclasses.dataclass\nclass P:\n a: int\n b: int\n c: int", "P(i, i, i)"),
    ("dataclass(slots=True), 3 fields", 200000, "import dataclasses\n@dataclasses.dataclass(slots=True)\nclass P:\n a: int\n b: int\n c: int", "P(i, i, i)"),
    ("enum member (class of 1000)", 20, "import enum", "enum.Enum('E%d' % i, ['m%d' % j for j in range(1000)])"),
    ("datetime.datetime", 200000, "import datetime", "datetime.datetime(2020, 1, 1, 0, 0, 0, i)"),
    ("datetime.date", 200000, "import datetime", "datetime.date.fromordinal(i + 1)"),
    ("datetime.timedelta", 200000, "import datetime", "datetime.timedelta(i)"),
    ("decimal.Decimal", 100000, "import decimal", "decimal.Decimal(i)"),
    ("fractions.Fraction", 100000, "import fractions", "fractions.Fraction(i, 7)"),
    ("uuid.UUID", 100000, "import uuid", "uuid.UUID(int=i)"),
    ("pathlib.Path", 100000, "import pathlib", "pathlib.Path('/a/b/%d' % i)"),
    ("re.Pattern", 2000, "import re", "re.compile('a%db+(c|d)*' % i)"),
    ("re.Match", 100000, "import re\np = re.compile('(a)(b)')", "p.match('ab')"),
    ("threading.Lock", 200000, "import threading", "threading.Lock()"),
    ("threading.RLock", 200000, "import threading", "threading.RLock()"),
    ("threading.Event", 50000, "import threading", "threading.Event()"),
    ("threading.Condition", 50000, "import threading", "threading.Condition()"),
    ("queue.Queue", 20000, "import queue", "queue.Queue()"),
    ("collections.deque, empty", 100000, "import collections", "collections.deque()"),
    ("collections.deque, 10 items", 100000, "import collections", "collections.deque(range(10))"),
    ("collections.OrderedDict, 3 keys", 100000, "import collections", "collections.OrderedDict(a=i, b=i, c=i)"),
    ("collections.defaultdict, empty", 100000, "import collections", "collections.defaultdict(list)"),
    ("collections.Counter, 3 keys", 100000, "import collections", "collections.Counter(a=i, b=i, c=i)"),
    ("contextvars.ContextVar", 100000, "import contextvars", "contextvars.ContextVar('v')"),
    ("contextvars.copy_context(), 3 variables set", 100000, "import contextvars\nfor n in 'abc': contextvars.ContextVar(n).set(1)", "contextvars.copy_context()"),
    ("asyncio.Future", 100000, "import asyncio\nloop = asyncio.new_event_loop()", "loop.create_future()"),
    ("asyncio.Event", 50000, "import asyncio", "asyncio.Event()"),
    ("asyncio task waiting in sleep()", 20000, "ASYNC", "asyncio.create_task(asyncio.sleep(100000))"),
    ("asyncio task waiting, 3 awaits deep", 20000, "ASYNC\nasync def a3(): await asyncio.sleep(100000)\nasync def a2(): await a3()\nasync def a1(): await a2()", "asyncio.create_task(a1())"),
    ("logging.LogRecord", 50000, "import logging", "logging.LogRecord('n', 10, 'p', i, 'msg %s', (i,), None)"),
    ("inspect.Parameter", 50000, "import inspect", "inspect.Parameter('a', inspect.Parameter.POSITIONAL_ONLY)"),
    ("ast of 'a = b + 1'", 20000, "import ast", "ast.parse('a = b + 1')"),
]

def anon():
    with open("/proc/self/status") as f:
        for line in f:
            if line.startswith("RssAnon"):
                return int(line.split()[1]) * 1024
    return 0

try:
    from bun.jsc import heapStats
    def engine():
        s = heapStats()
        return int(s["heapSize"]) - int(s["extraMemorySize"]), int(s["extraMemorySize"]), int(s["heapCapacity"]) - int(s["extraMemorySize"])
except ImportError:
    engine = None

def collect():
    gc.collect(); gc.collect()

def child(index):
    name, count, setup, expression = KINDS[index]
    namespace = {}
    is_async = setup.startswith("ASYNC")
    if is_async:
        setup = "import asyncio\n" + setup[5:]
    exec(setup, namespace)
    make = eval("lambda i: " + expression, namespace)
    result = {}
    def run():
        keep = [None] * count
        for i in range(min(count, 200)):
            keep[i] = make(i)
        for i in range(min(count, 200)):
            keep[i] = None
        collect()
        before, engine_before = anon(), engine and engine()
        for i in range(count):
            keep[i] = make(i)
        return keep, before, engine_before
    def finish(keep, before, engine_before):
        peak = anon()
        collect()
        after, engine_after = anon(), engine and engine()
        result.update(name=name, count=count, anon=round((after - before) / count, 1), anon_before_collection=round((peak - before) / count, 1))
        if engine:
            result.update(heap=round((engine_after[0] - engine_before[0]) / count, 1), extra=round((engine_after[1] - engine_before[1]) / count, 1),
                          capacity=round((engine_after[2] - engine_before[2]) / count, 1))
        try:
            result["getsizeof"] = sys.getsizeof(keep[-1])
        except TypeError:
            pass
    if is_async:
        import asyncio
        async def main():
            state = run()
            await asyncio.sleep(0)
            await asyncio.sleep(0)
            finish(*state)
            os.write(1, (json.dumps(result) + "\n").encode())
            os._exit(0)
        asyncio.run(main())
    finish(*run())
    os.write(1, (json.dumps(result) + "\n").encode())
    os._exit(0)

def table(ours_path, reference_path):
    ours = {r["name"]: r for r in json.load(open(ours_path))}
    reference = {r["name"]: r for r in json.load(open(reference_path))}
    print("| kind | here: anon | heap | extra | reference: anon | getsizeof | ratio |")
    print("|---|---:|---:|---:|---:|---:|---:|")
    for name, *_ in KINDS:
        o, r = ours.get(name), reference.get(name)
        if not o or not r or "anon" not in o or "anon" not in r:
            print(f"| {name} | {'failed' if not o or 'anon' not in o else o['anon']} | | | {'failed' if not r or 'anon' not in r else r['anon']} | | |")
            continue
        ratio = f"{o['anon'] / r['anon']:.1f}" if r["anon"] > 4 else "-"
        print(f"| {name} | {o['anon']:.0f} | {o.get('heap', 0):.0f} | {o.get('extra', 0):.0f} | {r['anon']:.0f} | {r.get('getsizeof', '')} | {ratio} |")

def main():
    arguments = sys.argv[1:]
    if arguments[:1] == ["--child"]:
        return child(int(arguments[1]))
    if arguments[:1] == ["--table"]:
        return table(arguments[1], arguments[2])
    jobs, out = 4, None
    while arguments and arguments[0] in ("-j", "-o"):
        if arguments[0] == "-j": jobs = int(arguments[1])
        else: out = arguments[1]
        arguments = arguments[2:]
    wanted = [i for i, k in enumerate(KINDS) if not arguments or any(a in k[0] for a in arguments)]
    results, running = {}, {}
    def reap(block):
        for i, p in list(running.items()):
            if block or p.poll() is not None:
                text, _ = p.communicate()
                try:
                    results[i] = json.loads(text.splitlines()[-1])
                except (IndexError, ValueError):
                    results[i] = {"name": KINDS[i][0], "failed": p.returncode}
                del running[i]
                r = results[i]
                print(f"{r['name']:52s}", " ".join(f"{k} {r[k]}" for k in ("anon", "heap", "extra", "capacity", "getsizeof", "anon_before_collection", "failed") if k in r), flush=True)
                if block:
                    return
    for i in wanted:
        while len(running) >= jobs:
            reap(False)
            if len(running) >= jobs:
                reap(True)
        running[i] = subprocess.Popen([sys.executable, __file__, "--child", str(i)], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    while running:
        reap(True)
    if out:
        json.dump([results[i] for i in wanted], open(out, "w"), indent=0)

main()
