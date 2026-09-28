import sys

def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)

# identity and liveness
def who():
    f = sys._getframe()
    a = f.f_lineno
    b = f.f_lineno
    return f is sys._getframe(), b - a, f.f_code is who.__code__, f.f_back.f_code.co_name, f.f_globals is globals()
show("identity", who)

def keep():
    x = 1
    y = "two"
    f = sys._getframe()
    x = 10
    return f
kept = keep()
show("after return", lambda: (dict(kept.f_locals)["x"], kept.f_locals["y"], sorted(kept.f_locals), kept.f_lineno - keep.__code__.co_firstlineno))
show("back after return", lambda: (kept.f_back.f_code.co_name, kept.f_back is sys._getframe(1)))
show("same locals proxy frame", lambda: kept.f_locals == kept.f_locals)
show("type", lambda: (type(kept).__name__, type(kept.f_locals).__name__, type(sys._getframe().f_locals).__name__))
show("module f_locals", lambda: sys._getframe(1).f_locals is globals())

# write-through
def write():
    x = 1
    f = sys._getframe()
    f.f_locals["x"] = 2
    f.f_locals["extra"] = 3
    return x, f.f_locals["extra"], "extra" in locals(), sorted(f.f_locals.keys()), len(f.f_locals)
show("write through", write)

def caller_writes():
    v = "before"
    poke()
    return v
def poke():
    sys._getframe(1).f_locals["v"] = "after"
show("callee writes caller", caller_writes)

# cells and free variables
def outer():
    c = 1
    def inner():
        return c, sorted(sys._getframe().f_locals.items()), sys._getframe(1).f_locals["c"]
    r = inner()
    sys._getframe().f_locals["c"] = 5
    return r, c, inner()[0]
show("cells", outer)

def param_cell(p):
    def g(): return p
    p = p + 1
    return sys._getframe().f_locals["p"], g()
show("parameter that is a cell", lambda: param_cell(1))

# unbound
def unbound():
    f = sys._getframe()
    before = "later" in f.f_locals, sorted(f.f_locals)
    later = 1
    del later
    return before, "later" in f.f_locals
show("unbound", unbound)
def missing():
    return sys._getframe().f_locals["nope"]
show("missing key", missing)
def remove():
    x = 1
    del sys._getframe().f_locals["x"]
show("cannot remove", remove)
def popx():
    x = 1
    return sys._getframe().f_locals.pop("x")
show("cannot pop", popx)
def proxy_ops():
    a = 1
    p = sys._getframe().f_locals
    p["z"] = 26
    return (p.get("a"), p.get("q"), p.get("q", 0), p.setdefault("a", 9), p.setdefault("w", 8), p.pop("z"), p.pop("z", None),
            list(reversed(p)), p | {"k": 1}, {"k": 1} | p, p.copy() == dict(p), p == {"a": 1, "p": p, "w": 8}, repr(p)[:8])
show("proxy ops", proxy_ops)
show("proxy new", lambda: type(kept.f_locals)(kept)["y"])
show("proxy new bad", lambda: type(kept.f_locals)(1))
show("proxy unhashable", lambda: hash(kept.f_locals))
def match_it():
    v = 1
    match sys._getframe().f_locals:
        case {"v": got}:
            return got
show("proxy is a mapping", match_it)

# locals()
def snap():
    x = 1
    d = locals()
    x = 2
    return d["x"], locals()["x"], d is locals()
show("locals snapshot", snap)
class K:
    inside = sys._getframe().f_locals is locals()
    name = sys._getframe().f_code.co_name
show("class body", lambda: (K.inside, K.name))

# clear
show("clear running", lambda: sys._getframe().clear())
def cleared():
    f = keep()
    f.clear()
    return dict(f.f_locals), f.f_code.co_name
show("clear", cleared)

# tracebacks
def boom(n):
    local = n * 2
    raise ValueError(n)
def catcher():
    mine = "here"
    try:
        boom(21)
    except ValueError as e:
        tb = e.__traceback__
        return (tb.tb_frame is sys._getframe(), tb.tb_frame.f_locals["mine"], tb.tb_next.tb_frame.f_locals["local"],
                tb.tb_next.tb_frame.f_back is tb.tb_frame, tb.tb_next.tb_frame.f_code is boom.__code__, tb.tb_frame is tb.tb_frame)
show("tb_frame", catcher)
def twice():
    out = []
    for i in range(2):
        try:
            boom(i)
        except ValueError as e:
            out.append(e.__traceback__.tb_frame)
    return out[0] is out[1]
show("same frame for two exceptions", twice)

# a frame that has not got its arguments is no frame
def needs(a, b): pass
def bad_call():
    try:
        needs(1)
    except TypeError as e:
        out = []
        tb = e.__traceback__
        while tb:
            out.append(tb.tb_frame.f_code.co_name)
            tb = tb.tb_next
        return out
show("incomplete", bad_call)
def default_frame(x=None):
    return sys._getframe().f_back.f_code.co_name
show("with defaults", default_frame)

# generators
def gen(a):
    b = a + 1
    got = yield sys._getframe()
    dead = "not read again"
    yield b
    c = 3
    yield
g = gen(1)
show("not started", lambda: (g.gi_frame.f_lasti >= 0, g.gi_frame.f_lineno - gen.__code__.co_firstlineno, dict(g.gi_frame.f_locals), g.gi_frame.f_back, g.gi_frame is g.gi_frame))
first = g.gi_frame
inside = next(g)
show("same inside and out", lambda: (inside is first, g.gi_frame is first, first.f_generator is g))
show("suspended", lambda: (first.f_lineno - gen.__code__.co_firstlineno, dict(first.f_locals), first.f_back, first.f_lasti > 0))
next(g)
show("dead local is seen", lambda: (first.f_lineno - gen.__code__.co_firstlineno, dict(first.f_locals)))
first.f_locals["b"] = 100
show("clear suspended", lambda: first.clear())
show("gi_code", lambda: (g.gi_code is gen.__code__, first.f_code is gen.__code__, gen.__code__.co_varnames))
next(g)
show("finish", lambda: next(g))
show("finished", lambda: (g.gi_frame, dict(first.f_locals), first.f_generator, first.f_lineno - gen.__code__.co_firstlineno))
def wgen():
    x = 1
    yield
    yield x
w = wgen(); next(w); w.gi_frame.f_locals["x"] = 42
show("write to suspended", lambda: next(w))
def running():
    yield (r.gi_frame.f_lineno - running.__code__.co_firstlineno, r.gi_running, r.gi_frame.f_back.f_code.co_name, r.gi_frame is sys._getframe())
r = running()
show("running", lambda: next(r))
n = gen(5); nf = n.gi_frame; nf.clear()
show("clear not started", lambda: (n.gi_frame, list(n), dict(nf.f_locals)))
def gboom():
    z = 9
    yield 1
    raise KeyError("x")
def gtb():
    it = gboom(); next(it); fr = it.gi_frame
    try:
        next(it)
    except KeyError as e:
        t = e.__traceback__.tb_next
        return t.tb_frame is fr, t.tb_frame.f_locals["z"], it.gi_frame, t.tb_frame.f_back is sys._getframe()
show("generator traceback", gtb)
async def co():
    q = 1
    return sys._getframe()
c = co(); cf = c.cr_frame
try:
    c.send(None)
except StopIteration as e:
    show("coroutine", lambda: (e.value is cf, c.cr_frame, dict(cf.f_locals), c.cr_code is co.__code__))
show("genexpr", lambda: sorted(k for k in next(sys._getframe().f_locals for _ in [1])))

# code identity
def mk():
    def inner(): pass
    return inner
show("code identity", lambda: (mk().__code__ is mk().__code__, mk.__code__ is mk.__code__, (lambda: 0).__code__ is (lambda: 0).__code__))
show("repr", lambda: repr(kept).split(" at ")[0] + repr(kept)[repr(kept).index(", line"):])
show("depth", lambda: sys._getframe(100))
show("f_lineno set", lambda: setattr(kept, "f_lineno", 3))
show("f_trace", lambda: (kept.f_trace, kept.f_trace_lines, kept.f_trace_opcodes, setattr(kept, "f_trace", print), kept.f_trace is print))
show("f_builtins", lambda: kept.f_builtins["len"] is len)
