# A generator that is waiting on another is not woken for something to be thrown into that one. It is running all the same, and its frame is between the frame that threw and the one that is thrown into.
import sys
def names(frame):
    out = []
    while frame.f_code.co_name != "<module>": out.append(frame.f_code.co_name); frame = frame.f_back
    return out
def attempt(label, f):
    try: r = f()
    except BaseException as e: r = "%s: %s" % (type(e).__name__, e)
    print(label, "->", r)
seen = []
def inner():
    try:
        seen.append(("started", names(sys._getframe())))
        yield 1
    except RuntimeError:
        seen.append(("thrown", names(sys._getframe())))
        yield 2
    finally:
        seen.append(("finally", names(sys._getframe())))
def middle(): yield from inner()
def outer(): yield from middle()
def run(how):
    seen.clear()
    g = outer(); g.send(None)
    how(g)
    return list(seen)
attempt("send", lambda: run(lambda g: g.send(None)))
attempt("throw", lambda: run(lambda g: g.throw(RuntimeError)))
attempt("close", lambda: run(lambda g: g.close()))
attempt("next", lambda: run(next))
class Custom:
    def __iter__(self): return self
    def __next__(self): seen.append(("next", names(sys._getframe()))); return 42
    def send(self, v): seen.append(("send", names(sys._getframe()))); return 43
    def throw(self, *a): seen.append(("throw", names(sys._getframe()))); return 44
    def close(self): seen.append(("close", names(sys._getframe())))
def over(t): yield from t
def run2(how):
    seen.clear()
    g = over(Custom()); g.send(None)
    how(g)
    return list(seen)
for n, how in {"send None": lambda g: g.send(None), "send 1": lambda g: g.send(1), "throw": lambda g: g.throw(RuntimeError), "close": lambda g: g.close(), "next": next}.items():
    attempt("custom, " + n, lambda: run2(how))
async def leaf():
    try:
        await Awaitable()
    except RuntimeError:
        seen.append(("thrown in a coroutine", names(sys._getframe())))
class Awaitable:
    def __await__(self): yield 1
async def mid(): await leaf()
async def top(): await mid()
def run3():
    seen.clear(); c = top(); c.send(None)
    try: c.throw(RuntimeError)
    except StopIteration: pass
    return list(seen)
attempt("coroutines", run3)
def tb():
    def i():
        yield 1
    def m(): yield from i()
    def o(): yield from m()
    g = o(); next(g)
    try: g.throw(ValueError("x"))
    except ValueError as e:
        out = []; t = e.__traceback__
        while t: out.append((t.tb_frame.f_code.co_name, t.tb_lineno - t.tb_frame.f_code.co_firstlineno)); t = t.tb_next
        return out
attempt("the traceback", tb)
print("===== what can be asked of one meanwhile")
import inspect, traceback
def describe(g):
    f = g.gi_frame if hasattr(g, "gi_frame") else g.cr_frame
    running = g.gi_running if hasattr(g, "gi_running") else g.cr_running
    waits = (g.gi_yieldfrom if hasattr(g, "gi_yieldfrom") else g.cr_await) is not None
    def cleared():
        try: f.clear()
        except RuntimeError as e: return str(e)
    def resumed(how):
        try: how()
        except (ValueError, RuntimeError) as e: return str(e)
    return (f.f_code.co_name, running, waits, f.f_lineno - f.f_code.co_firstlineno, f.f_lasti >= 0, sorted(f.f_locals), f.f_back and f.f_back.f_code.co_name, cleared(), resumed(lambda: g.send(None)), resumed(lambda: g.throw(KeyError)), resumed(g.close), f is (g.gi_frame if hasattr(g, "gi_frame") else g.cr_frame))
chain = []
def leaf(kind):
    try:
        yield "first"
    except RuntimeError:
        print("   thrown:", [describe(g) for g in chain])
        print("   by depth:", [sys._getframe(n).f_code.co_name for n in range(len(chain) + 2)])
        print("   extract_stack:", [s.name for s in traceback.extract_stack()][-len(chain) - 2:])
        print("   inspect.stack:", [s.function for s in inspect.stack()][:len(chain) + 2])
        chain[0].gi_frame.f_locals["mine"] = "changed"
        yield "caught"
    finally:
        if kind == "close": print("   closed:", [describe(g) for g in chain])
def link(inner, depth):
    mine = depth
    got = yield from inner
    return (mine, got)
def build(depth, kind):
    chain.clear()
    g = leaf(kind)
    for d in range(depth):
        g = link(g, d); chain.insert(0, g)
    next(g)
    return g
for depth in (1, 2, 3, 6):
    print("depth", depth)
    g = build(depth, "throw")
    attempt("   throw", lambda: g.throw(RuntimeError))
    attempt("   afterwards", lambda: ([describe(x)[:7] for x in chain], chain[0].gi_frame.f_locals["mine"]))
    attempt("   to the end", lambda: list(g))
    g = build(depth, "close")
    attempt("   close", g.close)
    attempt("   afterwards", lambda: [(x.gi_frame, x.gi_running) for x in chain])
print("===== one throw inside another")
def inner_a():
    try: yield 1
    except RuntimeError:
        other = outer_b(); next(other)
        print("  ", other.throw(KeyError))
        yield 2
def outer_a(): yield from inner_a()
def inner_b():
    try: yield 1
    except KeyError:
        yield names(sys._getframe())
def outer_b(): yield from inner_b()
ga = outer_a(); next(ga)
attempt("nested", lambda: ga.throw(RuntimeError))
print("===== what comes out")
def raiser():
    try: yield 1
    except RuntimeError: raise ValueError("from the leaf")
def through(g): yield from g
def out():
    g = through(through(raiser())); next(g)
    try: g.throw(RuntimeError("thrown"))
    except ValueError as e:
        t, rows = e.__traceback__, []
        while t: rows.append(t.tb_frame.f_code.co_name); t = t.tb_next
        return rows, type(e.__context__).__name__, g.gi_frame, g.gi_running
attempt("raised in the leaf", out)
def not_caught():
    def plain(): yield 1
    g = through(through(plain())); next(g)
    try: g.throw(RuntimeError("thrown"))
    except RuntimeError as e:
        t, rows = e.__traceback__, []
        while t: rows.append(t.tb_frame.f_code.co_name); t = t.tb_next
        return rows
attempt("not caught", not_caught)
print("===== coroutines and asynchronous generators")
class Wait:
    def __await__(self): return (yield "waiting")
async def co_leaf():
    try: await Wait()
    except RuntimeError:
        print("   thrown:", [describe(c)[:8] for c in coros], names(sys._getframe()))
        return "caught"
async def co_link(c): return await c
coros = []
def run_coros(depth):
    coros.clear(); c = co_leaf()
    for _ in range(depth): c = co_link(c); coros.insert(0, c)
    c.send(None)
    try: c.throw(RuntimeError)
    except StopIteration as e: return e.value
for depth in (1, 3): attempt("coroutines, %d" % depth, lambda: run_coros(depth))
async def agen():
    try:
        yield 1
    except RuntimeError:
        print("   in an asynchronous generator:", names(sys._getframe()))
        yield 2
async def uses():
    a = agen()
    await a.__anext__()
    return await a.athrow(RuntimeError)
def drive(c):
    try:
        while True: c.send(None)
    except StopIteration as e: return e.value
attempt("athrow", lambda: drive(uses()))
print("done")
