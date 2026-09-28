import sys
def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
def attempt(f):
    try: return f()
    except BaseException as e: return type(e).__name__ + ": " + str(e)
Function = type(show); Code = type(show.__code__)
def names(consts): return [c.co_name if isinstance(c, Code) else c for c in consts]
def codes(code): return [c for c in code.co_consts if isinstance(c, Code)]
# ---- co_consts
def simple(): return 1
def nothing(): pass
def documented():
    "the doc"
    return "s"
def nested():
    def inner(): pass
    class K: pass
    return lambda: 0
def gen():
    yield "y"
show("consts", lambda: (names(simple.__code__.co_consts), names(nothing.__code__.co_consts), names(documented.__code__.co_consts), names(gen.__code__.co_consts)))
show("nested code", lambda: ([c.co_name for c in codes(nested.__code__)], [c.co_qualname for c in codes(nested.__code__)], [c.co_firstlineno - nested.__code__.co_firstlineno for c in codes(nested.__code__)]))
show("the same each time", lambda: (nested.__code__.co_consts is nested.__code__.co_consts, type(nested.__code__.co_consts).__name__, codes(nested.__code__)[0] == codes(nested.__code__)[0]))
def returns_inner():
    def inner(): return 5
    return inner
show("equal to the function's", lambda: (codes(returns_inner.__code__)[0] == returns_inner().__code__, hash(codes(returns_inner.__code__)[0]) == hash(returns_inner().__code__), Function(codes(returns_inner.__code__)[0], {})()))
module = compile("'''mdoc'''\nx = 1000\ndef f(): pass\nclass C:\n    def m(self): pass\n", "m.py", "exec")
show("module", lambda: (names(module.co_consts), [c.co_name for c in codes(codes(module)[1])], module.co_name, module.co_firstlineno, module.co_flags, module.co_argcount, module.co_varnames, module.co_names))
def walk(code, depth=0):
    yield (depth, code.co_name, code.co_firstlineno)
    for c in codes(code): yield from walk(c, depth + 1)
show("walk", lambda: list(walk(module)))
show("kinds of constant", lambda: [type(c).__name__ for c in (lambda: (1, 2.5, "s", b"b", None, True, ..., 3j, 10 ** 30, 123456789012345678901234567890, -7, -2.5)).__code__.co_consts if type(c).__name__ != "tuple"][:0] or sorted({type(c).__name__ for c in compile("a = 1; b = 2.5; c = 's'; d = b'b'; e = ...; f = 3j; g = 123456789012345678901234567890; h = True", "f", "exec").co_consts}))
# ---- lines
def lines_of(code): return sorted({l for _, _, l in code.co_lines() if l is not None})
def several():
    a = 1
    b = 2

    pass
    if a:
        c = 3
    else:
        c = 4
    for i in ():
        d = 5
    while False:
        e = 6
    try:
        f = 7
    except E:
        g = 8
    finally:
        h = 9
    with x as y:
        i = 10
    return (a,
        b,
        len(
            "s"))
show("lines", lambda: [l - several.__code__.co_firstlineno for l in lines_of(several.__code__)])
show("lines cover it", lambda: (lambda runs: (runs[0][0], all(a[1] == b[0] for a, b in zip(runs, runs[1:])), all(s < e for s, e, _ in runs), type(runs[0]).__name__))(list(several.__code__.co_lines())))
show("lines iterator", lambda: (lambda i: (type(i).__name__, iter(i) is i, type(next(i)).__name__, len(list(i)) > 0, attempt(lambda: next(i))))(several.__code__.co_lines()))
show("module lines", lambda: lines_of(module))
def gen_lines():
    yield 1
    x = 2
    yield x
show("generator lines", lambda: [l - gen_lines.__code__.co_firstlineno for l in lines_of(gen_lines.__code__)])
# ---- positions
def positioned():
    return alpha.beta + gamma(1)
def spans(code):
    first = code.co_firstlineno
    return sorted({(a - first, b - first, c, d) for a, b, c, d in code.co_positions() if a is not None and c is not None and (c, d) != (0, 0)})
show("positions have", lambda: [p in spans(positioned.__code__) for p in [(1, 1, 11, 21), (1, 1, 24, 32), (1, 1, 11, 32)]])
show("positions iterator", lambda: (lambda i: (type(i).__name__, iter(i) is i, len(next(i)), type(next(i)).__name__))(positioned.__code__.co_positions()))
def wide():
    return "é" + ünï(1)
show("positions are in bytes", lambda: (1, 1, 17, 26) in spans(wide.__code__))
# ---- the rest
def many(a, b, /, c, *d, e, **f):
    g = 1
    def h(): return g, a
    return h
show("variable names", lambda: ([many.__code__._varname_from_oparg(i) for i in range(8)], attempt(lambda: many.__code__._varname_from_oparg(99)), attempt(lambda: many.__code__._varname_from_oparg("a")), many().__code__._varname_from_oparg(0) if False else None))
show("types", lambda: [type(getattr(many.__code__, n)).__name__ for n in ("co_code", "co_linetable", "co_exceptiontable", "co_stacksize", "_co_code_adaptive", "co_consts")])
show("code is even", lambda: (len(many.__code__.co_code) % 2, many.__code__.co_code == many.__code__.co_code, many.__code__.co_stacksize > 0))
show("read only", lambda: [attempt(lambda: setattr(many.__code__, n, 1)) for n in ("co_name", "co_consts", "co_code", "co_flags")])
# ---- equality
def eq(a, b): return (a == b, a != b, hash(a) == hash(b))
show("equal", lambda: (eq(compile("1 + x", "a", "eval"), compile("1 + x", "a", "eval")), eq(compile("1 + x", "a", "eval"), compile("1 + x", "b", "eval")), eq(compile("1 + x", "a", "eval"), compile("1 + y", "a", "eval"))[:2], eq(compile("x", "a", "eval"), compile("x", "a", "exec"))[:2]))
show("equal functions", lambda: ((lambda: 0).__code__ == (lambda: 0).__code__, simple.__code__ == simple.__code__, simple.__code__ == nothing.__code__, simple.__code__ == 1, simple.__code__ != 1, attempt(lambda: simple.__code__ < simple.__code__)))
ns1 = {}; ns2 = {}
exec("def f(a, b=1):\n    return a + b\n", ns1); exec("def f(a, b=1):\n    return a + b\n", ns2)
show("compiled twice", lambda: eq(ns1["f"].__code__, ns2["f"].__code__))
exec("\ndef f(a, b=1):\n    return a + b\n", ns2)
show("on another line", lambda: eq(ns1["f"].__code__, ns2["f"].__code__)[:2])
show("in a dict", lambda: len({ns1["f"].__code__: 1, simple.__code__: 2, simple.__code__.replace(): 3}))
# ---- replace
c = simple.__code__
show("replace nothing", lambda: (c.replace() == c, c.replace() is c, c.replace().co_name, Function(c.replace(), {})()))
show("replace name", lambda: (lambda r: (r.co_name, r.co_qualname, c.co_name, r == c, Function(r, {}).__name__, Function(r, {}).__qualname__, Function(r, {})()))(c.replace(co_name="other")))
show("replace qualname", lambda: (lambda r: (r.co_name, r.co_qualname, r == c, Function(r, {}).__qualname__))(c.replace(co_qualname="A.b")))
show("replace filename", lambda: (lambda r: (r.co_filename, r == c, c.co_filename == __file__, Function(r, {})()))(c.replace(co_filename="elsewhere.py")))
show("replace firstlineno", lambda: (lambda r: (r.co_firstlineno, r == c, lines_of(r), Function(r, {})()))(c.replace(co_firstlineno=1000)))
show("replace several", lambda: (lambda r: (r.co_name, r.co_filename, r.co_firstlineno, repr(r).split(" at ")[0], repr(r).split(", ", 1)[1]))(c.replace(co_name="n", co_filename="f.py", co_firstlineno=7)))
show("replace again", lambda: (lambda r: (r.co_name, r.co_qualname, r.co_filename, r.co_firstlineno))(c.replace(co_name="n").replace(co_filename="f.py").replace(co_firstlineno=7).replace(co_qualname="q")))
show("__replace__", lambda: c.__replace__(co_name="viadunder").co_name)
show("replace with the same", lambda: c.replace(co_argcount=c.co_argcount, co_posonlyargcount=c.co_posonlyargcount, co_kwonlyargcount=c.co_kwonlyargcount, co_nlocals=c.co_nlocals, co_stacksize=c.co_stacksize, co_flags=c.co_flags, co_code=c.co_code, co_consts=c.co_consts,
    co_names=c.co_names, co_varnames=c.co_varnames, co_freevars=c.co_freevars, co_cellvars=c.co_cellvars, co_linetable=c.co_linetable, co_exceptiontable=c.co_exceptiontable, co_filename=c.co_filename, co_name=c.co_name, co_qualname=c.co_qualname, co_firstlineno=c.co_firstlineno) == c)
def raiser():
    raise ValueError("here")
def traced(code):
    try: Function(code, {"ValueError": ValueError})()
    except ValueError as e:
        tb = e.__traceback__.tb_next
        return (tb.tb_frame.f_code.co_name, tb.tb_frame.f_code.co_filename.replace(__file__, "FILE"), tb.tb_lineno - code.co_firstlineno, tb.tb_frame.f_code is code, tb.tb_frame.f_lineno - code.co_firstlineno)
show("traceback", lambda: traced(raiser.__code__))
show("traceback replaced", lambda: traced(raiser.__code__.replace(co_name="renamed", co_filename="moved.py", co_firstlineno=500)))
def outer_named():
    def inner_named(): pass
    return inner_named
show("what is in it keeps its name", lambda: (lambda f: (f.__qualname__, f().__qualname__, f().__code__.co_qualname))(Function(outer_named.__code__.replace(co_name="x", co_qualname="X.x"), {})))
def line_of_inner():
    def inner(): pass
    return inner
show("what is in it keeps its line", lambda: Function(line_of_inner.__code__.replace(co_firstlineno=1), {})().__code__.co_firstlineno - line_of_inner.__code__.co_firstlineno)
def frame_line(): return sys._getframe().f_lineno
show("f_lineno", lambda: Function(frame_line.__code__.replace(co_firstlineno=300), {"sys": sys})())
def closing(x):
    def uses(): return x
    return uses
show("replace with a closure", lambda: (lambda u: (lambda r: (r.co_freevars, Function(r, {}, None, None, u.__closure__)()))(u.__code__.replace(co_name="z")))(closing(9)))
def gen_named():
    yield 1
show("replace a generator's", lambda: (lambda g: (g.__name__, g.__qualname__, g.gi_code.co_name, list(g)))(Function(gen_named.__code__.replace(co_name="gn", co_qualname="gq"), {})()))
# ---- what is wrong
for k, v in [("co_name", 1), ("co_name", None), ("co_filename", b"x"), ("co_qualname", 1), ("co_firstlineno", "a"), ("co_firstlineno", -1), ("co_firstlineno", 1 << 40), ("co_argcount", -1), ("co_posonlyargcount", -1), ("co_kwonlyargcount", -1), ("co_nlocals", -1), ("co_stacksize", -1), ("co_flags", -1),
        ("co_code", "s"), ("co_code", None), ("co_consts", [1]), ("co_names", 1), ("co_varnames", [1]), ("co_freevars", None), ("co_cellvars", "a"), ("co_linetable", 1), ("co_exceptiontable", 1), ("nope", 1), ("co_lnotab", b"")]:
    show("replace " + k + "=" + repr(v), lambda: c.replace(**{k: v}))
show("replace positional", lambda: c.replace(1))
# ---- CO_ITERABLE_COROUTINE, which is what types.coroutine() sets
def plain_gen():
    got = yield "from the generator"
    return ("returned", got)
async def awaits(g): return await g
def drive(co):
    r = [co.send(None)]
    try: co.send("sent")
    except StopIteration as e: r.append(e.value)
    return r
show("not awaitable", lambda: (lambda co: (attempt(lambda: co.send(None))))(awaits(plain_gen())))
marked = Function(plain_gen.__code__.replace(co_flags=plain_gen.__code__.co_flags | 0x100), {})
show("awaitable", lambda: (marked.__code__.co_flags & 0x100, drive(awaits(marked())), type(marked()).__name__))
def swapped():
    def g():
        yield 1
    g.__code__ = g.__code__.replace(co_flags=g.__code__.co_flags | 0x100); return (g.__code__.co_flags & 0x100, drive2(awaits(g())))
def drive2(co):
    r = [co.send(None)]
    try: co.send(None)
    except StopIteration as e: r.append(e.value)
    return r
show("as types.coroutine does it", swapped)
async def coro(): return "from a coroutine"
def delegating():
    c = coro()
    try: return (yield from c)
    finally: c.close()
def run(g):
    try: g.send(None)
    except StopIteration as e: return e.value
show("yield from a coroutine", lambda: (attempt(lambda: run(delegating())), run(Function(delegating.__code__.replace(co_flags=delegating.__code__.co_flags | 0x100), {"coro": coro})())))
events = []
sys.addaudithook(lambda e, a: events.append((e, a[1:])) if e == "code.__new__" else None)
show("audited", lambda: (c.replace(co_name="aud", co_filename="aud.py").co_name, [(e, a[0], a[1], a[2:5]) for e, a in events]))
