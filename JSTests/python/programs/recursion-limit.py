import sys
def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
def attempt(f):
    try: return f()
    except BaseException as e: return type(e).__name__ + ": " + str(e)
def depth_reached():
    n = 0
    def f():
        nonlocal n
        n += 1
        f()
    try: f()
    except RecursionError as e: return (n, str(e))
def with_limit(limit, f):
    old = sys.getrecursionlimit(); sys.setrecursionlimit(limit)
    try: return f()
    finally: sys.setrecursionlimit(old)
show("default", lambda: (sys.getrecursionlimit(), depth_reached()))
for limit in (20, 50, 100, 500, 3000):
    show("limit " + str(limit), lambda: with_limit(limit, depth_reached))
show("again, so it is counted down", lambda: (depth_reached(), depth_reached(), depth_reached()))
def here():
    # How deep this is: the least limit that can be set.
    for n in range(1, 100):
        try: sys.setrecursionlimit(n)
        except RecursionError as e: last = str(e)
        else:
            sys.setrecursionlimit(1000); return (n, last)
show("how deep", here)
def nested(k): return here() if k == 0 else nested(k - 1)
show("deeper", lambda: nested(5))
# ---- every way out of a frame counts it down
base = here()[0]
def level(): return here()[0] - base
def raises(k):
    if k == 0: raise ValueError("v")
    raises(k - 1)
def after_exception():
    try: raises(30)
    except ValueError: pass
    return level()
show("after an exception", after_exception)
def gen(k):
    for i in range(k): yield level()
show("in a generator", lambda: list(gen(3)))
def after_generator():
    g = gen(5); next(g); next(g); a = level(); g.close(); return (a, level())
show("after a generator", after_generator)
def gen_raises():
    yield 1
    raise KeyError("k")
def after_generator_raises():
    g = gen_raises(); next(g)
    try: next(g)
    except KeyError: pass
    return level()
show("after a generator raises", after_generator_raises)
def after_throw():
    g = gen(5); next(g)
    try: g.throw(ValueError("t"))
    except ValueError: pass
    return level()
show("after throw()", after_throw)
def gen_catches():
    while True:
        try: yield level()
        except ValueError: pass
def after_caught_throw():
    g = gen_catches(); a = next(g); b = g.throw(ValueError()); c = g.throw(ValueError()); g.close(); return (a, b, c, level())
show("after throw() that is caught", after_caught_throw)
def delegating(): return (yield from gen(3))
show("yield from", lambda: (list(delegating()), level()))
async def co(): return level()
async def outer_co(): return (level(), await co(), level())
def drive(c):
    try: c.send(None)
    except StopIteration as e: return e.value
show("coroutines", lambda: (drive(outer_co()), level()))
class Awaitable:
    def __await__(s):
        yield level()
        return level()
async def awaits(): return await Awaitable()
def suspended():
    c = awaits(); a = c.send(None); b = level()
    try: c.send(None)
    except StopIteration as e: return (a, b, e.value, level())
show("suspended", suspended)
async def agen():
    yield level()
    yield level()
async def uses_agen(): return [x async for x in agen()]
show("asynchronous generators", lambda: (drive(uses_agen()), level()))
show("generator expressions", lambda: (list(level() for _ in "ab"), level()))
show("comprehensions", lambda: ([level() for _ in "ab"], {level() for _ in "ab"}, level()))
class C:
    def __init__(s): s.l = level()
    def __add__(s, o): return level()
    def __getattr__(s, n): return level()
    def __enter__(s): return level()
    def __exit__(s, *a): return True
    def __iter__(s): return iter([level()])
    def __call__(s): return level()
    @property
    def p(s): return level()
def special():
    c = C()
    with c as w:
        raise ValueError
    return (c.l, c + 1, c.zzz, w, list(c), c(), c.p, level())
show("special methods", special)
show("by way of built-in functions", lambda: (list(map(lambda x: level(), "a")), sorted("a", key=lambda x: level()) and level(), max("a", key=lambda x: level()) and level()))
class K:
    l = level()
show("class bodies", lambda: (K.l, level()))
show("exec and eval", lambda: (eval("level()"), (lambda d: (exec("r = level()", d), d["r"]))({"level": level}), level()))
def through_finally():
    try:
        try: raises(5)
        finally: a = level()
    except ValueError: return (a, level())
show("through finally", through_finally)
def recursion_caught_deep():
    def f(n):
        try: return f(n + 1)
        except RecursionError: return n
    return f(0) > 900
show("caught at the bottom", lambda: (recursion_caught_deep(), level()))
def in_generators():
    def g(n):
        yield from g(n + 1)
    try: list(g(0))
    except RecursionError as e: return (str(e), level())
show("generators all the way down", in_generators)
def mutual():
    class A:
        def __repr__(s): return repr(s)
    try: repr(A())
    except RecursionError as e: return (type(e).__name__, level())
show("by way of repr()", mutual)
def by_getattr():
    class A:
        def __getattr__(s, n): return getattr(s, n)
    try: A().x
    except RecursionError as e: return (type(e).__name__, level())
show("by way of __getattr__", by_getattr)
# ---- setting it
for v in (0, -1, 1, "a", 1.5, None, 1 << 40, True):
    show("set " + repr(v), lambda: sys.setrecursionlimit(v))
sys.setrecursionlimit(1000)
show("set nothing", lambda: sys.setrecursionlimit())
show("high", lambda: with_limit(20000, lambda: depth_reached()[0] > 15000 or "stack"))
show("RecursionError", lambda: (RecursionError.__mro__[1].__name__, issubclass(RecursionError, RuntimeError)))
