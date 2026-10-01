# What code finds its builtins in is whatever its globals have as __builtins__, and that need not be a dict.
import builtins, types, sys, collections

def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
    print(label, "->", r)

class Asked:
    "Has __getitem__ and nothing else."
    def __init__(self, **items): self.items = items; self.asked = []
    def __getitem__(self, key):
        self.asked.append(key)
        return self.items[key]

class Raises:
    def __getitem__(self, key): raise ValueError("asked for %r" % (key,))

class Sub(dict):
    def __missing__(self, key): return "made up " + key

class ModuleSub(types.ModuleType): pass
module_sub = ModuleSub("m"); module_sub.superglobal = "of a module"

def kinds():
    yield "dict", {"superglobal": 1, "len": len}
    yield "empty dict", {}
    yield "proxy", types.MappingProxyType({"superglobal": 1, "len": len})
    yield "empty proxy", types.MappingProxyType({})
    yield "asked", Asked(superglobal=1, len=len)
    yield "raises", Raises()
    yield "sub", Sub(superglobal=1)
    yield "chainmap", collections.ChainMap({"superglobal": 1}, {"len": len})
    yield "module", builtins
    yield "module sub", module_sub
    yield "int", 123
    yield "None", None
    yield "str", "abc"
    yield "list", [1, 2]
    yield "object", object()

sources = [
    ("eval", "superglobal"),
    ("eval", "len('abc')"),
    ("eval", "missing"),
    ("eval", "(lambda: superglobal)()"),
    ("eval", "[superglobal for i in (1,)]"),
    ("exec", "x = superglobal"),
    ("exec", "def f(): return superglobal\nx = f()"),
    ("exec", "class A: pass\nx = A.__name__"),
    ("exec", "class A:\n    y = superglobal\nx = A.y"),
    ("exec", "import foo.bar\nx = foo"),
    ("exec", "from foo import bar\nx = bar"),
    ("exec", "def f():\n    import foo\n    return foo\nx = f()"),
    ("exec", "global g\ng = 1\nx = g"),
    ("exec", "assert 0, 'said'"),
    ("exec", "x = __debug__"),
    ("exec", "x = __builtins__ is given"),
    ("exec", "def f(): pass\nx = f.__builtins__ is given"),
    ("exec", "x = frame(0).f_builtins is given"),
    ("exec", "def f(): return frame(0).f_builtins is given\nx = f()"),
    ("exec", "x = run('superglobal', {})"),
    ("exec", "d = {}\nrun('1', d)\nx = d['__builtins__'] is given"),
    ("exec", "x = it.__reduce__()[0]"),
    ("exec", "x = method.__reduce__()[0]"),
    ("exec", "x = rev.__reduce__()[0]"),
    ("exec", "try:\n    1/0\nexcept ZeroDivisionError:\n    x = 'caught'"),
    ("exec", "x = f'{1!r}'"),
    ("exec", "x = [*range_(2)]"),
    ("exec", "del superglobal"),
    ("exec", "type T = superglobal\nx = T.__value__"),
    ("exec", "def f[T: superglobal](): pass\nx = f.__type_params__[0].__bound__"),
    ("exec", "async def f(): return superglobal\nc = f()\ntry:\n    c.send(None)\nexcept stop as e:\n    x = e.value"),
    ("exec", "def g():\n    yield superglobal\nx = next_(g())"),
]

for mode, source in sources:
    code = compile(source, "<t>", mode)
    print("=====", mode, repr(source))
    for label, given in kinds():
        def run():
            ns = {"__builtins__": given, "given": given, "frame": sys._getframe, "run": eval, "it": iter([1]), "method": [].append, "rev": reversed([1]), "range_": range, "stop": StopIteration, "next_": next}
            r = eval(code, ns) if mode == "eval" else (exec(code, ns), ns.get("x", "no x"))[1]
            return getattr(r, "__name__", r) if callable(r) else r
        attempt("  " + label, run)

print("===== what is asked, and in what order")
a = Asked(superglobal=1, len=len)
eval("(superglobal, len, superglobal)", {"__builtins__": a}); print(a.asked)
a = Asked(**{"__build_class__": builtins.__build_class__})
exec("class A: pass", {"__builtins__": a, "__name__": "n"}); print(a.asked)
a = Asked(**{"__import__": lambda *args: args[0]})
exec("import p.q\nfrom r import s", {"__builtins__": a}) if False else None
ns = {"__builtins__": a}
attempt("import", lambda: exec("import p.q", ns)); print(a.asked, ns.get("p"))

print("===== functions made of code and globals")
def f(): return superglobal
for label, given in kinds():
    def run():
        g = types.FunctionType(f.__code__, {"__builtins__": given})
        return (g.__builtins__ is given or (isinstance(given, types.ModuleType) and g.__builtins__ is given.__dict__) or "other", g())
    attempt("  " + label, run)
attempt("none given", lambda: types.FunctionType(f.__code__, {}).__builtins__ is builtins.__dict__)

print("===== it is settled when the function is made")
ns = {"__builtins__": {"superglobal": "first"}}
exec("def f(): return superglobal", ns)
ns["__builtins__"] = {"superglobal": "second"}
attempt("changed afterwards", ns["f"])
ns["f"].__builtins__["superglobal"] = "third"
attempt("what it has is changed", ns["f"])
attempt("set", lambda: setattr(ns["f"], "__builtins__", {}))

print("===== hot code")
p = types.MappingProxyType({"superglobal": 5})
ns = {"__builtins__": p, "range_": range}
exec("def f():\n    t = 0\n    for i in range_(20000):\n        t += superglobal\n    return t", ns)
attempt("proxy", ns["f"])
