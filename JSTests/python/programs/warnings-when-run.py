import sys
sys.modules["warnings"] = None
import _warnings
class Captured:
    directory = __file__[:__file__.rfind("/") + 1]
    def __init__(self): self.text = []
    def write(self, text): self.text.append(text.replace(self.directory, "") if self.directory else text)
    def flush(self): pass
    def take(self): r = "".join(self.text); self.text.clear(); return r
err = sys.stderr = Captured()
original = list(_warnings.filters)
def attempt(f):
    try: return f()
    except BaseException as e: return type(e).__name__ + ": " + str(e)
def show(label, f):
    # What is said, without the line that it is about, and then what happens if it is to be an error.
    _warnings.filters[:] = [("always", None, Warning, None, 0)]; err.take()
    r = attempt(f); said = [l.partition(": ")[2] for l in err.take().split("\n") if l and not l.startswith("  ")]
    _warnings.filters[:] = [("error", None, Warning, None, 0)]
    e = attempt(f); _warnings.filters[:] = original
    print(label, "=>", r, said, "| as an error:", e if isinstance(e, str) and "Warning" in e else "no error")
class I(int): pass
class F(float): pass
class C(complex): pass
def returning(name, value): return type("R", (), {name: lambda s: value})()
# ---- a strict subclass where the class itself is wanted
show("__index__", lambda: ([1, 2, 3][returning("__index__", I(1))], type(returning("__index__", I(1)).__index__()).__name__))
show("operator index of it", lambda: (hex(returning("__index__", I(10))), bin(returning("__index__", I(2))), range(returning("__index__", I(3))), "ab" * returning("__index__", I(2))))
show("__index__ gives True", lambda: [1, 2][returning("__index__", True)])
show("__index__ gives an int", lambda: [1, 2][returning("__index__", 1)])
show("__int__", lambda: (int(returning("__int__", I(5))), type(int(returning("__int__", I(5)))).__name__))
show("__int__ gives True", lambda: (int(returning("__int__", True)), type(int(returning("__int__", True))).__name__))
show("int() by way of __index__", lambda: (int(returning("__index__", I(5))), type(int(returning("__index__", I(5)))).__name__))
show("__float__", lambda: (float(returning("__float__", F(1.5))), type(float(returning("__float__", F(1.5)))).__name__))
show("__float__ where a float is wanted", lambda: (round(returning("__float__", F(1.5)).__float__()), complex(returning("__float__", F(1.5))), (1.0).__add__(1)))
show("float() by way of __index__", lambda: float(returning("__index__", I(5))))
show("__complex__", lambda: (complex(returning("__complex__", C(1j))), type(complex(returning("__complex__", C(1j)))).__name__))
show("of a subclass itself", lambda: (int(I(5)), float(F(1.5)), complex(C(1j)), [1, 2][I(1)]))
# ---- ~ of a bool
show("~True", lambda: (~True, ~False))
t = True
show("~ of a variable", lambda: ~t)
show("bool.__invert__", lambda: (True.__invert__(), bool.__invert__(False)))
show("~ of an int", lambda: (~1, ~I(1)))
show("not", lambda: not t)
# ---- complex()
show("complex(complex)", lambda: complex(1j))
show("complex(complex, real)", lambda: complex(1j, 2))
show("complex(real, complex)", lambda: complex(1, 2j))
show("complex(complex, complex)", lambda: complex(1j, 2j))
show("complex(C, 1)", lambda: complex(C(1j), 1))
show("complex(what has __complex__, 1)", lambda: complex(returning("__complex__", 1j), 1))
show("complex(real, real)", lambda: complex(1, 2.5))
show("by name", lambda: (complex(real=1j, imag=1), complex(imag=1j)))
# ---- code
def plain(): return 1
def gen(): yield 1
async def co(): return 1
async def agen(): yield 1
show("co_lnotab", lambda: type(plain.__code__.co_lnotab).__name__)
def assign(f, g):
    F = type(plain); h = F(f.__code__, {}); h.__code__ = g.__code__; return h.__code__ is g.__code__
for a, b in ((plain, gen), (gen, plain), (plain, co), (co, agen), (gen, agen), (plain, plain), (gen, gen), (co, co)):
    show("__code__ of %s = that of %s" % (a.__name__, b.__name__), lambda: assign(a, b))
# ---- throw()
def thrown(make, *a):
    g = make()
    try: return g.throw(*a)
    except BaseException as e: return type(e).__name__ + repr(e.args)
    finally: g.close()
show("throw(exception)", lambda: thrown(gen, ValueError("v")))
show("throw(class)", lambda: thrown(gen, ValueError))
show("throw(class, value)", lambda: thrown(gen, ValueError, "v"))
show("throw(class, value, None)", lambda: thrown(gen, ValueError, "v", None))
show("throw(class, None, None)", lambda: thrown(gen, ValueError, None, None))
show("coroutine.throw(class, value)", lambda: thrown(co, ValueError, "v"))
show("coroutine.throw(exception)", lambda: thrown(co, ValueError("v")))
def athrown(*a):
    g = agen(); x = g.athrow(*a)
    try: x.send(None)
    except BaseException as e: return type(e).__name__ + repr(e.args)
show("athrow(exception)", lambda: athrown(ValueError("v")))
show("athrow(class, value)", lambda: athrown(ValueError, "v"))
def asend_thrown(*a):
    g = agen(); x = g.asend(None)
    try: return x.throw(*a)
    except BaseException as e: return type(e).__name__ + repr(e.args)
show("asend().throw(exception)", lambda: asend_thrown(ValueError("v")))
show("asend().throw(class, value)", lambda: asend_thrown(ValueError, "v"))
def wrapper_thrown(*a):
    c = co(); w = c.__await__()
    try: return w.throw(*a)
    except BaseException as e: return type(e).__name__ + repr(e.args)
    finally: c.close()
show("__await__().throw(class, value)", lambda: wrapper_thrown(ValueError, "v"))
# ---- classes
show("a key that is no string", lambda: type("K", (), {1: 2}).__name__)
show("keys that are strings", lambda: type("K", (), {"a": 2}).__name__)
class S(str): pass
show("a key of a subclass of str", lambda: type("K", (), {S("a"): 2}).__name__)
# ---- sys
show("sys._clear_type_cache()", lambda: sys._clear_type_cache())
show("sys._clear_internal_caches()", lambda: sys._clear_internal_caches())
# ---- whose fault it is
def helper(): return ~t
_warnings.filters[:] = [("always", None, Warning, None, 0)]; err.take(); helper(); print("where =>", err.take().split(": ")[0], "|", helper.__code__.co_firstlineno); _warnings.filters[:] = original
