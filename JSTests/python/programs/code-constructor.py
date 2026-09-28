import sys
# What is written in C leaves a good deal to the typing module. This stands in for it, here and in CPython alike.
stub = type(sys)("typing")
class _GenericAlias:
    def __init__(self, origin, args): self.__origin__ = origin; self.__args__ = args
    def __mro_entries__(self, bases): return (self.__origin__,)
stub._generic_class_getitem = lambda cls, args: _GenericAlias(cls, args)
stub._generic_init_subclass = lambda cls, *a, **k: None
sys.modules["typing"] = stub
def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
def attempt(f):
    try: return f()
    except BaseException as e: return type(e).__name__ + ": " + str(e)
Function = type(show); Code = type(show.__code__); Cell = type((lambda x: lambda: x)(1).__closure__[0])
FIELDS = ("co_argcount", "co_posonlyargcount", "co_kwonlyargcount", "co_nlocals", "co_stacksize", "co_flags", "co_code", "co_consts", "co_names", "co_varnames", "co_filename", "co_name", "co_qualname", "co_firstlineno", "co_linetable", "co_exceptiontable", "co_freevars", "co_cellvars")
def parts(code, **changes): return [changes.get(n, getattr(code, n)) for n in FIELDS]
# As what pickles a function by value does: what is in it first, and then it.
def rebuild(code):
    return Code(*parts(code, co_consts=tuple(rebuild(c) if isinstance(c, Code) else c for c in code.co_consts)))
def walk(code):
    yield code
    for c in code.co_consts:
        if isinstance(c, Code): yield from walk(c)
def same(a, b): return all(getattr(a, n) == getattr(b, n) for n in FIELDS)
def f(a, b=2, /, c=3, *d, e=5, **g):
    "doc"
    return (a, b, c, d, e, g)
show("round trip", lambda: (lambda r: (r == f.__code__, r is f.__code__, same(r, f.__code__), hash(r) == hash(f.__code__), Function(r, {}, "f", (2, 3), None, {"e": 5})(1), Function(r, {}).__doc__))(rebuild(f.__code__)))
SOURCE = '''"module doc"
from __future__ import annotations
import sys
x: int = 1000
def plain(a, b=2): return a + b
def documented():
    "the doc"
def closure(v):
    def inner(): return v
    return inner
def gen(n):
    for i in range(n): yield i
async def co(): return "co"
async def agen():
    yield 1
lam = lambda q, *r, **s: (q, r, s)
comp = [i * 2 for i in range(3)]
genexp = tuple(i for i in range(3))
class K:
    "K doc"
    attr: str = "a"
    def method(self): return __class__.__name__
    def __private(self): return self.__hidden
    @staticmethod
    def s(): return "s"
    class Inner:
        def deep(self): return "deep"
def annotated(p: int, q: str = "x") -> bool: return True
def generic[T: int, *Ts, **P](t: T = 1) -> T: return t
class G[T = int]:
    def m(self, t: T) -> T: return t
type Alias[T] = list[T]
def decorated(fn): return fn
@decorated
def with_decorator(): return "decorated"
def multi(
        a,
        b=(1,
           2)):
    return (a,
            b)
def wide(): return "é€𝄞"
def tries():
    try: raise ValueError("v")
    except ValueError as e: return str(e)
    finally: pass
'''
module = compile(SOURCE, "mod.py", "exec", dont_inherit=True)
show("how many", lambda: len(list(walk(module))))
show("what they say of themselves", lambda: [(c.co_name, c.co_argcount, c.co_posonlyargcount, c.co_kwonlyargcount, hex(c.co_flags), c.co_freevars, c.co_firstlineno) for c in sorted(walk(module), key=lambda c: (c.co_firstlineno, c.co_name))])
show("all round trip", lambda: [c.co_name for c in walk(module) if not (rebuild(c) == c and same(rebuild(c), c))])
def run(code):
    ns = {"__name__": "m"}
    exec(code, ns)
    def drive(c):
        try: c.send(None)
        except StopIteration as e: return e.value
    return (ns["__doc__"], ns["x"], ns["plain"](1), ns["documented"].__doc__, ns["closure"](7)(), list(ns["gen"](3)), drive(ns["co"]()), ns["lam"](1, 2, z=3), ns["comp"], ns["genexp"], ns["K"].__doc__, ns["K"]().method(), ns["K"].s(), ns["K"].Inner().deep(),
        ns["K"].__annotations__, ns["annotated"].__annotations__, ns["generic"](5), [t.__name__ for t in ns["generic"].__type_params__], ns["generic"].__type_params__[0].__bound__, ns["G"].__type_params__[0].__default__, ns["G"]().m(1), ns["Alias"].__name__, str(ns["Alias"].__value__),
        ns["with_decorator"](), ns["multi"](0), ns["wide"](), ns["tries"](), ns["K"].method.__qualname__, ns["closure"](0).__qualname__, ns["__annotations__"], sorted(n for n in dir(ns["K"]) if "private" in n))
show("runs the same", lambda: run(rebuild(module)) == run(module))
show("runs", lambda: run(rebuild(module)))
def lines(code): return [(c.co_name, c.co_firstlineno, sorted({l for _, _, l in c.co_lines() if l})) for c in walk(code)]
show("lines the same", lambda: lines(rebuild(module)) == lines(module))
def raising():
    x = 1
    raise KeyError("k")
def where(code):
    try: Function(code, {"KeyError": KeyError})()
    except KeyError as e:
        tb = e.__traceback__.tb_next
        return (tb.tb_lineno - raising.__code__.co_firstlineno, tb.tb_frame.f_code.co_filename == __file__, tb.tb_frame.f_code.co_name)
show("traceback", lambda: (where(raising.__code__), where(rebuild(raising.__code__))))
show("elsewhere", lambda: (lambda r: (r.co_filename, r.co_name, r.co_qualname, r.co_firstlineno, sorted({l for _, _, l in r.co_lines()}), r == raising.__code__))(Code(*parts(raising.__code__, co_filename="e.py", co_name="n", co_qualname="q.n", co_firstlineno=100))))
def outer(v):
    def inner(): return v
    return inner
show("with a closure", lambda: Function(rebuild(outer(0).__code__), {}, None, None, (Cell("c"),))())
show("optimized", lambda: [(lambda ns: (exec(rebuild(compile("assert False\nd = __debug__\ndef f():\n    'doc'\n    assert False\n    return __debug__\ne = (f(), f.__doc__)", "o", "exec", optimize=o)), ns), ns["d"], ns["e"])[1:])({}) for o in (1, 2)])
show("eval and single", lambda: (eval(rebuild(compile("1 + 2", "e", "eval"))), rebuild(compile("x = 1", "s", "single")).co_name))
show("without the last two", lambda: Code(*parts(f.__code__)[:16]) == f.__code__)
show("without the last one", lambda: Code(*parts(f.__code__)[:17]) == f.__code__)
# ---- what is wrong
show("no arguments", lambda: Code())
show("too few", lambda: Code(*parts(f.__code__)[:15]))
show("too many", lambda: Code(*parts(f.__code__), 1))
show("keywords", lambda: Code(*parts(f.__code__), x=1))
for i, bad in [(0, "a"), (0, None), (0, 1 << 40), (1, 1.5), (5, "f"), (6, "s"), (6, None), (6, bytearray(b"")), (7, [1]), (8, None), (9, "ab"), (10, b"f"), (11, 1), (12, None), (13, "l"), (14, "t"), (15, 1), (16, [1]), (17, None)]:
    def wrong():
        p = parts(f.__code__); p[i] = bad; return Code(*p)
    show("argument " + str(i + 1) + " " + repr(bad), wrong)
for n in ("co_argcount", "co_posonlyargcount", "co_kwonlyargcount", "co_nlocals"):
    show("negative " + n, lambda: Code(*parts(f.__code__, **{n: -1})))
for n in ("co_names", "co_varnames", "co_freevars", "co_cellvars"):
    show("not names " + n, lambda: Code(*parts(f.__code__, **{n: (1,)})))
show("odd", lambda: Code(*parts(f.__code__, co_code=b"x")))
events = []
sys.addaudithook(lambda e, a: events.append((e, a[1:])) if e == "code.__new__" else None)
show("audited", lambda: (rebuild(raising.__code__).co_name, [(e, a[0] == __file__, a[1], a[2:5]) for e, a in events]))
show("derived", lambda: type("X", (Code,), {}))
