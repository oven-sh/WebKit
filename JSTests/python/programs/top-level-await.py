import sys
def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
def attempt(f):
    try: return f()
    except BaseException as e: return type(e).__name__ + ": " + str(e)
ALLOW = 0x2000
class Awaitable:
    def __init__(s, v): s.v = v
    def __await__(s):
        got = yield ("suspended", s.v)
        return (s.v, got)
def drive(co, *sends):
    out = []; to_send = None; sends = list(sends)
    try:
        while True:
            out.append(co.send(to_send)); to_send = sends.pop(0) if sends else None
    except StopIteration as e:
        return (out, e.value)
def clean(d): return {k: v if not hasattr(v, "__code__") else "function" for k, v in d.items() if k not in ("__builtins__", "A", "AI", "CM")}
class AI:
    def __init__(s, n): s.n = n
    def __aiter__(s): return s
    async def __anext__(s):
        if not s.n: raise StopAsyncIteration
        s.n -= 1; return s.n
class CM:
    async def __aenter__(s): return "entered"
    async def __aexit__(s, *a): return False
G = {"A": Awaitable, "AI": AI, "CM": CM}
def run(source, mode="exec", flags=ALLOW, sends=()):
    ns = dict(G); code = compile(source, "<t>", mode, flags); r = eval(code, ns)
    return (hex(code.co_flags), type(r).__name__, drive(r, *sends) if hasattr(r, "send") else r, clean(ns))
show("await", lambda: run("x = await A(1)\ny = 2", sends=["sent"]))
show("no await", lambda: run("x = 1"))
show("eval", lambda: run("await A(5)", "eval", sends=["s"]))
show("eval without", lambda: run("1 + 1", "eval"))
show("async for", lambda: run("r = []\nasync for i in AI(3):\n    r.append(i)\n"))
show("async with", lambda: run("async with CM() as c:\n    r = c\n"))
show("async comprehension", lambda: run("r = [i async for i in AI(2)]"))
show("await in a comprehension", lambda: run("r = [await A(i) for i in (1, 2)]"))
show("two awaits", lambda: run("a = await A(1)\nb = await A(2)\nc = (a, b)", sends=["p", "q"]))
show("in a try", lambda: run("try:\n    x = await A(1)\n    raise ValueError(x)\nexcept ValueError as e:\n    y = e.args\nfinally:\n    z = await A(2)\n"))
show("functions in it", lambda: run("async def f(): return await A(1)\nx = await f()\ndef g(): return 5\ny = g()"))
show("single", lambda: (lambda c: hex(c.co_flags))(compile("await A(1)", "<t>", "single", ALLOW)))
show("not allowed", lambda: [attempt(lambda: compile(s, "<t>", "eval")) for s in ("await x", "[i async for i in x]", "[await i for i in x]")])
show("still not", lambda: [attempt(lambda: compile(s, "<t>", "exec", ALLOW)) for s in ("def f(): await x", "yield 1", "class C:\n    await x", "lambda: await x", "return 1", "def f():\n    async for i in x: pass")])
show("names are the module's", lambda: run("x = 1\ny = await A(x)\ndel x\nglobal z\nz = 3"))
def with_locals():
    g = dict(G); l = {}
    drive(eval(compile("a = await A(1)\nb = locals() is L", "<t>", "exec", ALLOW), dict(g, L=l), l)); return l
show("locals", with_locals)
def frames():
    ns = dict(G, sys=sys); co = eval(compile("f = sys._getframe()\nn = f.f_code.co_name\nl = f.f_locals is f.f_globals\nx = await A(1)\nline = f.f_lineno", "<t>", "exec", ALLOW), ns)
    a = (co.cr_frame.f_lineno, co.cr_code.co_name, type(co.cr_frame.f_locals).__name__); drive(co)
    return (a, ns["n"], ns["l"], ns["line"], co.__name__, co.__qualname__)
show("frames", frames)
def raising():
    co = eval(compile("x = await A(1)\nraise KeyError(x)", "<t>", "exec", ALLOW), dict(G)); co.send(None)
    try: co.send("v")
    except KeyError as e: return (e.args, e.__traceback__.tb_next.tb_lineno, e.__traceback__.tb_next.tb_frame.f_code.co_name)
show("raises", raising)
def thrown():
    co = eval(compile("try:\n    await A(1)\nexcept ValueError as e:\n    r = 'caught ' + str(e)", "<t>", "exec", ALLOW), (ns := dict(G))); co.send(None)
    try: co.throw(ValueError("thrown"))
    except StopIteration: return ns["r"]
show("throw", thrown)
def closed():
    co = eval(compile("try:\n    await A(1)\nfinally:\n    r = 'closed'", "<t>", "exec", ALLOW), (ns := dict(G))); co.send(None); co.close(); return ns["r"]
show("close", closed)
Code = type(show.__code__)
FIELDS = ("co_argcount", "co_posonlyargcount", "co_kwonlyargcount", "co_nlocals", "co_stacksize", "co_flags", "co_code", "co_consts", "co_names", "co_varnames", "co_filename", "co_name", "co_qualname", "co_firstlineno", "co_linetable", "co_exceptiontable", "co_freevars", "co_cellvars")
show("rebuilt", lambda: (lambda c: (lambda r: (r == c, drive(eval(r, dict(G)))))(Code(*[getattr(c, n) for n in FIELDS])))(compile("await A(3)", "<t>", "eval", ALLOW)))
show("lines", lambda: sorted({l for _, _, l in compile("x = 1\ny = await A(1)\nz = 3\n", "<t>", "exec", ALLOW).co_lines() if l}))
