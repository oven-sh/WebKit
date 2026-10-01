# Small things: what is told when telling begins in the middle of a line, BRANCH and DISABLE, what warn_explicit() makes of the globals of a module, and how an order of resolution that cannot be is put.
import sys, types, warnings, typing, importlib.machinery
M = sys.monitoring
E = M.events

def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
    print(label, "->", r)

print("== telling begins in the middle of a line")
def f1(): pass
def lines():
    seen = []
    first = sys._getframe().f_lineno
    def line(code, n):
        if code.co_name in ("lines", "f1"):
            seen.append((code.co_name, n - (first if code.co_name == "lines" else code.co_firstlineno)))
    M.use_tool_id(1, "a"); M.use_tool_id(2, "b")
    M.register_callback(1, E.LINE, line)
    M.set_events(1, E.LINE); M.set_events(2, E.LINE); x = 1
    f1()
    M.set_events(1, 0); M.set_events(2, 0)
    M.register_callback(1, E.LINE, None)
    M.free_tool_id(1); M.free_tool_id(2)
    return seen
print(lines())
def traced():
    seen = []
    first = sys._getframe().f_lineno
    def tracer(frame, event, arg):
        if frame.f_code.co_name == "traced":
            seen.append((event, frame.f_lineno - first))
        return tracer
    sys._getframe().f_trace = tracer
    sys.settrace(tracer); a = 1; b = 2
    c = 3
    sys.settrace(None)
    return seen
print(traced())

print("== BRANCH was the two as one")
warnings.simplefilter("ignore")
def b(x):
    if x:
        return 1
    return 2
class Counts:
    def __init__(self): self.count = 0
    def __call__(self, *args):
        self.count += 1
        return M.DISABLE
M.use_tool_id(1, "a")
for label, register, events in (("BRANCH", (E.BRANCH,), E.BRANCH), ("both", (E.BRANCH_LEFT, E.BRANCH_RIGHT), E.BRANCH_LEFT | E.BRANCH_RIGHT), ("BRANCH and then LEFT", (E.BRANCH, E.BRANCH_LEFT), E.BRANCH_LEFT | E.BRANCH_RIGHT)):
    c = Counts()
    for e in register: M.register_callback(1, e, c)
    M.set_local_events(1, b.__code__, events)
    for x in (1, 0, 1, 0): b(x)
    M.set_local_events(1, b.__code__, 0)
    M.register_callback(1, E.BRANCH, None)
    M.restart_events()
    print(label, c.count)
M.free_tool_id(1)
warnings.resetwarnings()

print("== warn_explicit(module_globals=...)")
class Loader:
    def get_source(self, name): return "first line\nsecond line\n"
class BadSource(str):
    def splitlines(self): return 42
class BadLoader:
    def get_source(self, name): return BadSource("spam\neggs")
class NotStr:
    def get_source(self, name): return 5
loader = Loader()
cases = {
    "nothing": {"__name__": "bar"},
    "loader None": {"__name__": "bar", "__loader__": None},
    "spec None": {"__name__": "bar", "__spec__": None},
    "both None": {"__name__": "bar", "__loader__": None, "__spec__": None},
    "spec.loader None": {"__name__": "bar", "__loader__": None, "__spec__": types.SimpleNamespace(loader=None)},
    "no spec": {"__name__": "bar", "__loader__": loader},
    "loader, spec None": {"__name__": "bar", "__loader__": loader, "__spec__": None},
    "loader, spec has none": {"__name__": "bar", "__loader__": loader, "__spec__": types.SimpleNamespace()},
    "they disagree": {"__name__": "bar", "__loader__": loader, "__spec__": types.SimpleNamespace(loader=Loader())},
    "no loader, spec has none": {"__name__": "bar", "__spec__": types.SimpleNamespace()},
    "spec.loader alone": {"__name__": "bar", "__spec__": types.SimpleNamespace(loader=loader)},
    "they agree": {"__name__": "bar", "__loader__": loader, "__spec__": types.SimpleNamespace(loader=loader)},
    "no name": {"__loader__": loader, "__spec__": types.SimpleNamespace(loader=loader)},
    "splitlines of its own": {"__name__": "bar", "__spec__": types.SimpleNamespace(loader=BadLoader())},
    "no str": {"__name__": "bar", "__spec__": types.SimpleNamespace(loader=NotStr())},
    "no get_source": {"__name__": "bar", "__spec__": types.SimpleNamespace(loader=object())},
}
for label, module_globals in cases.items():
    for line in (1, 2, 5):
        def run():
            with warnings.catch_warnings(record=True) as w:
                warnings.simplefilter("always")
                warnings.warn_explicit("eggs", UserWarning, "bar", line, module_globals=module_globals)
            return [(x.category.__name__, str(x.message), x.line) for x in w]
        attempt("%s, line %d" % (label, line), run)
attempt("no dict", lambda: warnings.warn_explicit("eggs", UserWarning, "bar", 1, module_globals=5))

print("== an order that cannot be")
class A: pass
class B(A): pass
attempt("A, B", lambda: type("C", (A, B), {}))
attempt("object, Generic", lambda: type("C", (object, typing.Generic), {}))
attempt("object, OrderedDict", lambda: type("C", (object, __import__("collections").OrderedDict), {}))
attempt("class statement", lambda: exec("class X[T](object): pass"))
