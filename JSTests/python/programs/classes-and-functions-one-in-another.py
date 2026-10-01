# Classes and functions, with type parameters and without, one in another in every order to four deep. Each class that has something in it that can see the class keeps its namespace for that, and it is its own that it keeps.
import itertools
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
    print(label, "->", r)
KINDS = {"c": "class", "C": "class with a parameter", "f": "function", "F": "function with a parameter"}
def source(shape, last):
    "What is written for a shape: each is in the one before, and each class has a variable that what is in it may look for."
    lines, parameters = [], []
    for depth, kind in enumerate(shape):
        pad = "    " * depth
        name = "n%d" % depth
        inside_class = depth and shape[depth - 1] in "cC"
        if kind in "CF": parameters.append("T%d" % depth)
        bracket = "[T%d]" % depth if kind in "CF" else ""
        if kind in "cC":
            lines.append("%sclass %s%s:" % (pad, name, bracket))
            lines.append("%s    v%d = %d" % (pad, depth, depth))
        else:
            lines.append("%sdef %s%s(%s):" % (pad, name, bracket, "self=None" if inside_class else ""))
    pad = "    " * len(shape)
    seen = ", ".join(parameters) + ("," if parameters else "")
    if last == "parameters":
        body = ["result = (%s)" % seen] if shape[-1] in "cC" else ["return (%s)" % seen]
    elif last == "bound":
        v = [d for d, k in enumerate(shape) if k in "cC"]
        wanted = "v%d" % v[-1] if v and shape[-1] in "cC" else "int"
        body = ["def g[B: %s](self=None): return B" % wanted] + (["result = g.__type_params__[0].__bound__"] if shape[-1] in "cC" else ["return g.__type_params__[0].__bound__"])
    elif last == "alias":
        v = [d for d, k in enumerate(shape) if k in "cC"]
        wanted = "v%d" % v[-1] if v and shape[-1] in "cC" else "int"
        body = ["type A = (%s, %s)" % (wanted, seen or "None,")] + (["result = A.__value__"] if shape[-1] in "cC" else ["return A.__value__"])
    else:
        body = ["def g(self=None): return __class__"] + (["result = g"] if shape[-1] in "cC" else ["return g()"])
    lines += [pad + b for b in body]
    # And each hands up what is in it.
    for depth in range(len(shape) - 1, 0, -1):
        pad = "    " * depth
        inner = "n%d" % depth
        got = "%s.result" % inner if shape[depth] in "cC" else "%s()" % inner
        lines.append("%s%s %s" % (pad, "result =" if shape[depth - 1] in "cC" else "return", got))
    lines.append("answer = %s" % ("n0.result" if shape[0] in "cC" else "n0()"))
    return "\n".join(lines) + "\n"
def shown(v):
    if callable(v) and not isinstance(v, type): v = v()
    return v.__qualname__ if isinstance(v, type) else v
for length in (1, 2, 3, 4):
    for shape in itertools.product("cCfF", repeat=length):
        for last in ("parameters", "bound", "alias", "class"):
            if last == "class" and not set(shape) & set("cC"): continue
            def run():
                ns = {"__name__": "m"}
                exec(source(shape, last), ns)
                return shown(ns["answer"])
            attempt("%s %s" % ("".join(shape), last), run)
print("===== written out")
class A:
    def b(self):
        class C:
            def d[D](self): return D
        return C
attempt("class, method, class, method with a parameter", lambda: A().b()().d())
class ClassA[A1]:
    def funcB[B](self):
        class ClassC[C]:
            def funcD[D](self):
                return lambda: (A1, B, C, D)
        return ClassC
attempt("all four with one", lambda: ClassA().funcB()().funcD()())
class Outer:
    x = "outer"
    class Inner:
        x = "inner"
        type T = x
        def m[B: x](self): pass
        class Innermost:
            type U = x
    type T = x
x = "module"
attempt("each looks in its own", lambda: (Outer.T.__value__, Outer.Inner.T.__value__, Outer.Inner.m.__type_params__[0].__bound__, Outer.Inner.Innermost.U.__value__))
print("===== what __prepare__() gives")
for label, value in {"None": None, "an int": 5, "a list": [], "a str": "x", "a tuple": (), "a set": set(), "an object": object(), "what can be subscripted": type("M", (), {"__getitem__": lambda s, k: 1 / 0})(), "a dict": {}, "a mapping of its own": type("M", (dict,), {})()}.items():
    def with_class():
        class Meta(type):
            @classmethod
            def __prepare__(mcs, name, bases, **k): return value
            def __new__(mcs, name, bases, ns, **k): return super().__new__(mcs, name, bases, dict(ns) if isinstance(ns, dict) else {})
        class X(metaclass=Meta): pass
        return X.__name__
    def with_object():
        class NoClass:
            def __prepare__(self, name, bases, **k): return value
            def __call__(self, name, bases, ns, **k): return "made"
        class X(metaclass=NoClass()): pass
        return X
    attempt("of a metaclass, %s" % label, with_class)
    attempt("   of what is no class", with_object)
print("===== a dict that is given to an instance")
class MyStr(str): pass
class Odd(str):
    def __hash__(self): return 5
    def __eq__(self, o): return False
class Foo: pass
def given(d):
    f = Foo(); f.__dict__ = d
    return [getattr(f, n, "none") for n in ("a", "b")], sorted(map(str, vars(f))), len(vars(f)), f.__dict__ is d
attempt("plain", lambda: given({"a": 1, "b": 2}))
attempt("keys of a class derived from str", lambda: given({MyStr("a"): 1, "b": 2}))
attempt("that has its own idea of what it is equal to", lambda: given({Odd("a"): 1, "b": 2}))
attempt("other keys", lambda: given({1: 1, "b": 2, None: 3}))
attempt("set afterwards", lambda: (lambda f: (f.__dict__.__setitem__(MyStr("a"), 1), f.a, setattr(f, MyStr("b"), 2), f.b, f.__dict__.update({MyStr("c"): 3}), f.c, MyStr("a") in vars(f), "a" in vars(f), vars(f)[MyStr("b")], vars(f).pop(MyStr("c")), hasattr(f, "c")))(Foo()))
print("done")
