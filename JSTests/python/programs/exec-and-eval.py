import sys
def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
def attempt(f):
    try: return f()
    except BaseException as e: return type(e).__name__ + ": " + str(e)
Cell = type((lambda x: lambda: x)(1).__closure__[0])
def clean(d): return {k: v for k, v in d.items() if k != "__builtins__"}
# ---- mappings for locals
class Logged(dict):
    def __init__(s): s.log = []
    def __getitem__(s, k): s.log.append(("get", k)); return dict.__getitem__(s, k)
    def __setitem__(s, k, v): s.log.append(("set", k, v)); dict.__setitem__(s, k, v)
    def __delitem__(s, k): s.log.append(("del", k)); dict.__delitem__(s, k)
def logged(source, **g):
    l = Logged(); exec(source, dict(g), l); return (l.log, dict(l))
show("derived dict", lambda: logged("a = 1\nb = a + g\ndel a\n", g=10))
class Made(dict):
    def __getitem__(s, k): return "made " + k if k.startswith("z") else dict.__getitem__(s, k)
show("made up", lambda: (lambda l: (exec("q = zz", {}, l), l["q"], eval("zebra", {}, l)))(Made()))
class Missing(dict):
    def __missing__(s, k): return "missing " + k
show("__missing__", lambda: (eval("nowhere", {}, Missing()), attempt(lambda: eval("nowhere", Missing()))))
class Mapping:
    def __init__(s): s.d = {}; s.log = []
    def __getitem__(s, k): s.log.append("get " + k); return s.d[k]
    def __setitem__(s, k, v): s.log.append("set " + k); s.d[k] = v
    def __delitem__(s, k): s.log.append("del " + k); del s.d[k]
show("no dict at all", lambda: (lambda m: (exec("a = 1\nb = a + len('xy')\ndel a\nimport sys as s\ndef f(): pass\nclass C: pass\nfor i in (1, 2): pass\n", {}, m), m.log, sorted(m.d)))(Mapping()))
show("eval in it", lambda: (lambda m: (m.__setitem__("v", 5), eval("v * 2", {}, m), eval("[v for _ in 'ab']", {}, m), eval("(lambda: 1)()", {}, m)))(Mapping()))
class Raises:
    def __getitem__(s, k): raise s.error(k)
    def __setitem__(s, k, v): raise ValueError("set " + k)
def raising(e):
    r = Raises(); r.error = e; return r
show("KeyError goes on to the globals", lambda: eval("g", {"g": "global"}, raising(KeyError)))
show("LookupError does not", lambda: eval("g", {"g": "global"}, raising(LookupError)))
show("nor ValueError", lambda: eval("g", {"g": "global"}, raising(ValueError)))
show("set raises", lambda: exec("a = 1", {}, raising(KeyError)))
show("del of what is not there", lambda: exec("del a", {}, Mapping()))
show("functions do not see it", lambda: (lambda m: (m.__setitem__("v", 1), attempt(lambda: exec("def f(): return v\nr = f()", {}, m))))(Mapping()))
show("classes do", lambda: (lambda m: (m.__setitem__("v", 1), exec("class C:\n    w = v\nr = C.w", {}, m), m.d["r"]))(Mapping()))
show("global statement", lambda: (lambda g, m: (exec("global x\nx = 1\ny = 2", g, m), clean(g), m.d))({}, Mapping()))
show("locals()", lambda: (lambda m: (exec("r = locals()", {}, m), m.d["r"] is m))(Mapping()))
show("sequences", lambda: [attempt(lambda: eval("1", {}, l)) for l in ([], (), "s", b"b", range(1), 1, 1.5, object(), set())])
show("sequences for exec", lambda: [attempt(lambda: exec("1", {}, l)) for l in ([], (), "s", 1, object(), set())])
show("globals", lambda: [attempt(lambda: eval("1", g)) for g in ([], Mapping(), 1, "s", Made())])
show("globals for exec", lambda: [attempt(lambda: exec("1", g)) for g in ([], Mapping(), 1, "s", Made())])
# ---- how they are called
show("keywords", lambda: (eval("a + b", globals={"a": 1}, locals={"b": 2}), eval("a", locals={"a": 3}), (lambda d: (exec("z = 1", globals=d), clean(d)))({}), (lambda d: (exec("z = 1", locals=d), d))({})))
show("source by keyword", lambda: eval(source="1"))
show("exec source by keyword", lambda: exec(source="1"))
show("closure positional", lambda: exec("1", {}, {}, None))
show("eval closure", lambda: eval("1", closure=None))
show("nothing", lambda: (attempt(eval), attempt(exec)))
show("too many", lambda: (attempt(lambda: eval("1", {}, {}, 1)), attempt(lambda: exec("1", {}, {}, 1, 2))))
show("None", lambda: (eval("1", None, None), exec("1", None, None, closure=None)))
def caller():
    v = "local"
    return (eval("v"), eval("v", None), eval("v", None, None), eval("v", globals(), {"v": "given"}), attempt(lambda: eval("v", {})), sorted(eval("locals()")))
show("the caller's", caller)
def writes():
    v = 1
    exec("v = 2\nw = 3")
    return (v, "w" in locals())
show("does not write to the caller", writes)
show("builtins are put in", lambda: (lambda d: (eval("1", d), list(d)))({}))
show("builtins of one's own", lambda: (eval("len('ab')", {"__builtins__": {"len": lambda x: "mine"}}), attempt(lambda: eval("len('ab')", {"__builtins__": {}}))))
show("sources", lambda: [attempt(lambda: eval(s)) for s in ("  1", "\t2", b" 3", bytearray(b"4"), memoryview(b"5"), "1\n", "\n1", 6, None, "1;2", "x = 1", "")][:11])
show("null bytes", lambda: (attempt(lambda: eval("1\0")), attempt(lambda: exec(b"1\0"))))
# ---- code objects that are not what compile() makes
def plain(): return ("ran", g)
def takes(a): return a
def outer(v, w):
    def inner(): return (v, w, g)
    return inner
inner = outer(1, 2)
show("a function's code", lambda: (exec(plain.__code__, {"g": 1}), eval(plain.__code__, {"g": 2})))
show("that takes arguments", lambda: attempt(lambda: exec(takes.__code__, {})))
show("with free variables", lambda: (attempt(lambda: exec(inner.__code__, {})), attempt(lambda: eval(inner.__code__, {}))))
out = []
def collecting(v):
    def inner(): out.append((v, g))
    return inner
show("closure", lambda: (exec(collecting(0).__code__, {"g": "G", "out": out}, closure=(Cell("in a cell"),)), out))
for label, c in [("None", None), ("short", (Cell(1),)), ("long", (Cell(1), Cell(2), Cell(3))), ("a list", [Cell(1), Cell(2)]), ("not cells", (1, 2)), ("an int", 1)]:
    show("closure " + label, lambda: exec(inner.__code__, {"g": 0}, closure=c))
class T(tuple): pass
show("closure derived", lambda: exec(inner.__code__, {"g": 0}, closure=T((Cell(1), Cell(2)))))
show("closure not wanted", lambda: exec(plain.__code__, {"g": 0}, closure=(Cell(1),)))
show("closure empty", lambda: exec(plain.__code__, {"g": 0}, closure=()))
show("closure with a string", lambda: exec("1", closure=(Cell(1),)))
def nonlocal_writer(n):
    def bump():
        nonlocal n
        n += 1
    return bump
show("closure is written", lambda: (lambda c: (exec(nonlocal_writer(0).__code__, {}, closure=(c,)), exec(nonlocal_writer(0).__code__, {}, closure=(c,)), c.cell_contents))(Cell(10)))
def gen():
    yield 1
show("a generator's code", lambda: type(eval(gen.__code__, {})).__name__)
Code = type(plain.__code__)
klass = [c for c in compile("class K:\n    a = 1\n    b = a + g\n", "k", "exec").co_consts if isinstance(c, Code)][0]
show("the body of a class", lambda: (lambda l: (exec(klass, {"g": 10, "__name__": "m"}, l), sorted(l.items())))({}))
# ---- what is told of
events = []
sys.addaudithook(lambda e, a: events.append((e, tuple(x if isinstance(x, (str, bytes, int)) else type(x).__name__ for x in a))) if e in wanted else None)
wanted = {"exec", "compile"}
def told(f):
    events.clear(); f(); r = list(events); events.clear(); return r
show("exec of a string", lambda: told(lambda: exec("a = 1", {})))
show("eval of a string", lambda: told(lambda: eval("  1")))
show("eval of bytes", lambda: told(lambda: eval(b" 2")))
show("compile", lambda: told(lambda: compile("é", "file.py", "eval")))
show("exec of code", lambda: told(lambda: exec(compile("1", "f", "exec"))))
show("bad source", lambda: told(lambda: attempt(lambda: exec("("))))
wanted = {"builtins.id", "sys._getframe", "object.__getattr__", "object.__setattr__"}
show("id", lambda: [e for e, a in told(lambda: id(1))])
show("_getframe", lambda: told(lambda: sys._getframe()))
show("__code__", lambda: told(lambda: plain.__code__))
def refuses(e, a):
    if e == "exec" and refusing: raise RuntimeError("no exec")
    if e == "compile" and refusing: raise RuntimeError("no compile of " + repr(a[0]))
refusing = []
sys.addaudithook(refuses)
refusing.append(1)
show("refused", lambda: (attempt(lambda: exec("1")), attempt(lambda: eval(compile.__self__ and plain.__code__, {"g": 0})), attempt(lambda: compile("2", "f", "eval"))))
refusing.clear()
# ---- a dict is taken from as a dict, unless it is gone through in some way of its own
class G(dict):
    def __getitem__(s, k): return "got"
    def keys(s): return ["other"]
class I(dict):
    def __iter__(s): return iter(["i"])
    def keys(s): return ["k"]
    def __getitem__(s, k): return "item " + k
def kw(**k): return k
show("taken from as a dict", lambda: (dict(G(a=1)), {**G(a=1)}, kw(**G(a=1)), (lambda d: (d.update(G(a=1)), d)[1])({}), {} | G(a=1), G(a=1).copy()))
show("gone through its own way", lambda: (dict(I(a=1)), {**I(a=1)}, kw(**I(a=1)), (lambda d: (d.update(I(a=1)), d)[1])({}), {} | I(a=1)))
def ior(d, x):
    d |= x
    return d
show("|= takes what update() does", lambda: (ior({}, [("a", 1)]), ior({"z": 0}, {"a": 1}), ior({}, I(a=1)), ior({}, iter([("b", 2)])), attempt(lambda: ior({}, 1)), attempt(lambda: ior({}, [1])), attempt(lambda: {} | [("a", 1)]), attempt(lambda: [("a", 1)] | {}), {}.__ior__([("c", 3)]), {}.__or__([("c", 3)])))
# ---- notes that are added to what is raised
def noted(f):
    try: f()
    except BaseException as e: return (type(e).__name__, str(e), getattr(e, "__notes__", None))
show("update note", lambda: (noted(lambda: {}.update([1])), noted(lambda: dict([("a", 1), 2])), noted(lambda: dict([(1, 2, 3)])), noted(lambda: dict(1))))
class BadName:
    def __set_name__(s, owner, name): raise ValueError("no " + name)
def bad_class():
    class Owner:
        attr = BadName()
show("__set_name__ note", lambda: noted(bad_class))
