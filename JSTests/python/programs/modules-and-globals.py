import sys, builtins

def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)

me = sys.modules[__name__]
show("one dict", lambda: (globals() is vars(me), globals() is me.__dict__, me.__dict__ is me.__dict__, type(me).__name__))
show("attribute is global", lambda: (setattr(me, "via_attr", 1), via_attr, globals()["via_attr"])[1:])
show("global is attribute", lambda: (globals().__setitem__("via_dict", 2), me.via_dict, via_dict)[1:])

# what has been found is found again until it changes
def use_len(x):
    return len(x)
def hot(f, *a):
    r = None
    for _ in range(3000):
        r = f(*a)
    return r
show("builtin", lambda: hot(use_len, [1, 2]))
len = lambda x: "shadowed"
show("shadowed", lambda: hot(use_len, [1, 2]))
del len
show("revealed", lambda: hot(use_len, [1, 2]))
globals()["len"] = lambda x: "by dict"
show("shadowed by dict", lambda: hot(use_len, [1, 2]))
del globals()["len"]
me.len = lambda x: "by attribute"
show("shadowed by attribute", lambda: hot(use_len, [1, 2]))
del me.len
original = builtins.len
builtins.len = lambda x: "builtins changed"
show("builtins changed", lambda: hot(use_len, [1, 2]))
builtins.len = original
show("builtins restored", lambda: hot(use_len, [1, 2]))
builtins.brand_new = "everywhere"
show("added to builtins", lambda: brand_new)
del builtins.brand_new
show("removed from builtins", lambda: brand_new)

counter = 0
def bump():
    global counter
    counter += 1
    return counter
show("store", lambda: (hot(bump), counter))
def read_late():
    return late
show("not yet", read_late)
late = "now"
show("now", lambda: hot(read_late))
del late
show("gone", read_late)
none_global = None
show("None is a value", lambda: hot(lambda: none_global))

# many globals
for i in range(700):
    globals()["g%d" % i] = i
show("many", lambda: (g0, g699, hot(use_len, "abc"), hot(lambda: g350), len(globals()) > 700))
g350 = "changed"
show("many changed", lambda: hot(lambda: g350))
del g350
show("many deleted", lambda: g350)
abs = lambda x: "shadow in a big module"
show("many shadow", lambda: hot(lambda: abs(-1)))
del abs
show("many reveal", lambda: hot(lambda: abs(-1)))

# builtins of one's own
ns = {"__builtins__": {"len": lambda x: "mine"}}
exec("def f(x): return len(x)\ndef g(): return abs(1)", ns)
show("own builtins", lambda: ns["f"]([1]))
show("only those", lambda: ns["g"]())
show("__builtins__ of function", lambda: (sorted(ns["f"].__builtins__), use_len.__builtins__ is vars(builtins)))
ns["__builtins__"]["abs"] = lambda x: "added"
show("added to own builtins", lambda: ns["g"]())
plain = {}
exec("x = len('ab')", plain)
show("exec adds __builtins__", lambda: (sorted(plain), plain["__builtins__"] is vars(builtins), plain["x"]))
show("main has module", lambda: type(__builtins__).__name__)
show("frame", lambda: (sys._getframe().f_builtins is vars(builtins), sys._getframe().f_globals is globals()))

# modules are instances
M = type(sys)
show("new module", lambda: (m := M("fresh"), m.__name__, sorted(vars(m)), repr(m), m.__dict__ is vars(m))[1:])
show("module needs a name", lambda: M())
class Lazy(M):
    @property
    def computed(self):
        return "computed " + self.__name__
    def __repr__(self):
        return "<Lazy>"
show("derived", lambda: (z := Lazy("z"), z.computed, repr(z), isinstance(z, M), sorted(vars(z))[:2])[1:])
show("missing", lambda: sys.nothing_here)
def __getattr__(name):
    if name == "magic":
        return "from __getattr__"
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
show("PEP 562", lambda: (me.magic, hasattr(me, "other")))
show("PEP 562 is not for globals", lambda: magic)
show("truth and hash", lambda: (bool(me), hash(me) == hash(me), me == me, me != sys))
show("dir", lambda: [n for n in dir(M("d"))])
show("builtins module", lambda: (builtins.__name__, sys.modules["builtins"] is builtins, builtins.len is original, "print" in vars(builtins)))
show("PEP 562 says why", lambda: me.other)

# A function that imports, and has nothing else to do with any global variable.
def only_imports():
    from sys import maxsize
    return maxsize > 0
def only_imports_a_module():
    import sys
    return sys.maxsize > 0
class OnlyImports:
    def method(self):
        from sys import maxsize as m
        return m > 0
print(only_imports(), only_imports_a_module(), OnlyImports().method(), (lambda: __import__("sys").maxsize > 0)())
