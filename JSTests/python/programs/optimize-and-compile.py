import sys
def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
def attempt(f):
    try: return f()
    except BaseException as e: return type(e).__name__ + ": " + str(e)
Code = type(show.__code__)
def run(source, optimize=-1, mode="exec", **k):
    ns = {}
    r = eval(compile(source, "<t>", mode, optimize=optimize, **k), ns)
    return r if mode == "eval" else {n: v for n, v in ns.items() if n != "__builtins__"}
# ---- __debug__ and optimize
show("__debug__", lambda: (__debug__, [run("__debug__", o, "eval") for o in (-1, 0, 1, 2)]))
show("assert", lambda: [attempt(lambda: run("assert False, 'message'\nx = 1", o)) for o in (-1, 0, 1, 2)])
show("assert has effects", lambda: [attempt(lambda: run("l = []\nassert l.append(1) is None\n", o)) for o in (0, 1)])
show("if __debug__", lambda: [run("if __debug__:\n    x = 'debug'\nelse:\n    x = 'not'\n", o) for o in (0, 1, 2)])
show("not __debug__", lambda: [run("x = 'a' if not __debug__ else 'b'\nwhile not __debug__:\n    y = 1\n    break\n", o) for o in (0, 1)])
DOCS = '''"module doc"
def f():
    "f doc"
class C:
    "C doc"
    def m(self):
        "m doc"
async def a():
    "a doc"
def g():
    "g doc"
    yield
'''
def docs(o):
    ns = run(DOCS, o)
    return (ns.get("__doc__"), ns["f"].__doc__, ns["C"].__doc__, ns["C"].m.__doc__, ns["a"].__doc__, ns["g"].__doc__, ns["f"].__code__.co_flags & 0x4000000, "__doc__" in ns["C"].__dict__)
for o in (0, 1, 2):
    show("docstrings at " + str(o), lambda: docs(o))
show("nested", lambda: [run("def f():\n    def g():\n        assert False\n        return __debug__\n    return g()\nx = f()", o).get("x") if o else None for o in (0, 1, 2)])
show("in a lambda and a class", lambda: [(lambda ns: (ns["l"](), ns["C"].d, ns["c"]))(run("l = lambda: __debug__\nclass C:\n    d = __debug__\nc = [__debug__ for _ in 'a']\n", o)) for o in (0, 1)])
show("exec inherits nothing", lambda: run("ns = {}\nexec('x = __debug__', ns)\ny = ns['x']\nz = eval('__debug__')", 1)["y"])
show("optimize wrong", lambda: [attempt(lambda: compile("1", "f", "eval", optimize=o)) for o in (-2, 3, "a", None, 1.5, 1 << 40)])
show("__debug__ cannot be assigned", lambda: [attempt(lambda: compile(s, "f", "exec")) for s in ("__debug__ = 1", "del __debug__", "def f(__debug__): pass", "x.__debug__ = 1", "import __debug__", "for __debug__ in x: pass", "f(__debug__=1)", "(__debug__ := 1)")])
show("builtins", lambda: (__builtins__.__debug__ if hasattr(__builtins__, "__debug__") else __builtins__["__debug__"]))
# ---- what is never come to
def lines_of(code): return sorted({l for _, _, l in code.co_lines() if l})
def consts_of(code): return [c.co_name if isinstance(c, Code) else c for c in code.co_consts]
DEAD = '''a = 1000
if 0:
    b = 2000
if False:
    c = 3000
else:
    d = 4000
if 1:
    e = 5000
else:
    f = 6000
while 0:
    g = 7000
else:
    h = 8000
if None: i = 9000
if "": j = 1100
if "x": k = 1200
if (): l = 1300
if not 1: m = 1400
if ...: n = 1500
if 0.0: o = 1600
if b"": p = 1700
if 0j: q = 1800
'''
show("dead lines", lambda: lines_of(compile(DEAD, "f", "exec")))
show("dead constants", lambda: [c for c in consts_of(compile(DEAD, "f", "exec")) if isinstance(c, int) and c >= 1000])
show("dead names run", lambda: sorted(run(DEAD)))
show("dead functions", lambda: (lambda c: [x for x in c if x != 0])(consts_of(compile("if 0:\n    def f(): pass\n    class C: pass\ndef g(): pass\n", "f", "exec"))))
for s in ("if 0:\n    return", "if 0:\n    break", "if 0:\n    continue", "while 0:\n    yield", "if 0:\n    await x", "if 0:\n    x = 1\n    global x", "if 0:\n    nonlocal y", "if 0:\n    from __future__ import nope", "def f():\n    if 0:\n        return\n    x: int = 1\n", "if 0:\n    (yield)", "if 0:\n    *a = 1", "if 0:\n    f(a=1, a=2)", "if 0:\n    def f(a, a): pass"):
    show("dead but wrong " + repr(s), lambda: type(compile(s, "f", "exec")).__name__)
def still_generator():
    if 0:
        yield
show("a generator all the same", lambda: (type(still_generator()).__name__, list(still_generator())))
def still_local():
    if 0:
        v = 1
    return v
show("a local all the same", lambda: still_local())
def loop_forever():
    n = 0
    while 1:
        n += 1
        if n > 3: break
    else:
        n = -1
    while True:
        n += 1
        if n > 6: return n
show("while 1", loop_forever)
def dead_else():
    while 0:
        pass
    else:
        return "else ran"
show("while 0 else", dead_else)
def dead_break():
    for i in range(3):
        if 0:
            break
        while 0:
            continue
    else:
        return "no break"
show("break in what is dead", dead_break)
def dead_try():
    if 0:
        try: pass
        finally: pass
    if 0:
        with x: pass
    return 2
show("try in what is dead", dead_try)
# ---- compile()
show("flags", lambda: [attempt(lambda: type(compile("1", "f", "eval", f)).__name__) for f in (0, 0x10, 0x200, 0x1000, 0x2000, 0x4000, 0x20000, 0x1000000, 1, 2, 0x100, 0x800, 0x10000, 1 << 30, -1)])
show("flags show", lambda: [hex(compile("1", "f", "eval", f, True).co_flags) for f in (0, 0x20000, 0x40000, 0x80000, 0x100000, 0x200000, 0x400000, 0x800000, 0x1000000, 0x2000, 0x10)])
show("future shows", lambda: [hex(compile("from __future__ import " + n + "\n", "f", "exec", 0, True).co_flags) for n in ("annotations", "division", "generator_stop", "barry_as_FLUFL", "print_function", "nested_scopes")])
show("future in functions", lambda: hex(run("from __future__ import annotations\ndef f(): pass\n", dont_inherit=True)["f"].__code__.co_flags))
show("future by flag", lambda: run("def f(x: undefined_name): pass\na = f.__annotations__", flags=0x1000000)["a"])
show("barry in functions", lambda: run("from __future__ import barry_as_FLUFL\ndef f(a, b): return a <> b\nl = lambda a: a <> 1\nx = (f(1, 2), l(1))", dont_inherit=True)["x"])
show("barry by flag", lambda: (run("1 <> 2", mode="eval", flags=0x400000), attempt(lambda: run("1 != 2", mode="eval", flags=0x400000)), attempt(lambda: run("1 <> 2", mode="eval"))))
show("modes", lambda: [attempt(lambda: type(compile("1", "f", m)).__name__) for m in ("exec", "eval", "single", "func_type", "nope", "", 1, None, b"exec")])
show("filenames", lambda: [attempt(lambda: compile("1", f, "eval").co_filename) for f in ("a.py", b"b.py", "", 1, None, [], bytearray(b"c"))])
class P:
    def __fspath__(s): return "from fspath"
show("path", lambda: compile("1", P(), "eval").co_filename)
show("sources", lambda: [attempt(lambda: eval(compile(s, "f", "eval"))) for s in ("1", b"2", bytearray(b"3"), memoryview(b"4"), 5, None, [])])
show("missing", lambda: [attempt(lambda: compile(*a)) for a in ((), ("1",), ("1", "f"))])
show("keywords", lambda: eval(compile(source="7", filename="f", mode="eval", flags=0, dont_inherit=True, optimize=0, _feature_version=-1)))
show("feature version positional", lambda: compile("1", "f", "eval", 0, False, -1, 13))
show("code is not source", lambda: compile(compile("1", "f", "eval"), "f", "eval"))
show("dont_inherit", lambda: [attempt(lambda: type(compile("1", "f", "eval", 0, d)).__name__) for d in (0, 1, True, None, "a", [], 2.5)])
