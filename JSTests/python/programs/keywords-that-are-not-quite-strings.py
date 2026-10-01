# f(**mapping), where the keys are not what keywords are made of.
import enum, functools, collections, types
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
    print(label, "->", ascii(r))
def f(a=None, **kw): return a, sorted((str(k), type(k).__name__, v) for k, v in kw.items())
def g(a=None, *, b=None): return a, b
def h(**kw): return kw
class S(str): pass
class Color(enum.StrEnum):
    RED = "red"; A = "a"
class Eq:
    def __init__(self, s): self.s = s
    def __hash__(self): return hash(self.s)
    def __eq__(self, o): return o == self.s
    def __repr__(self): return "Eq(%r)" % self.s
class C:
    def __init__(self, a=None, **kw): self.r = (a, sorted(map(str, kw)))
    def m(self, a=None, **kw): return a, sorted(map(str, kw))
    def __call__(self, a=None, **kw): return a, sorted(map(str, kw))
callables = {"f": f, "g": g, "h": h, "dict": dict, "C": lambda **k: C(**k).r, "method": C().m, "instance": C(), "partial": functools.partial(f), "print": print, "int": int, "sorted": sorted,
             "SimpleNamespace": types.SimpleNamespace, "namedtuple": collections.namedtuple("N", "a"), "lambda": lambda **k: k, "type": type, "max": max, "Exception": Exception,
             "classmethod": classmethod(lambda cls, **k: sorted(map(str, k))).__get__(None, C), "builtin method": {}.update, "range": range}
mappings = {"int key": {1: 3}, "str subclass": {S("a"): 1}, "str subclass, other": {S("z"): 1}, "StrEnum": {Color.A: 1}, "StrEnum, other": {Color.RED: 1}, "None": {None: 1}, "tuple": {(1, 2): 1}, "bytes": {b"a": 1},
            "equal to a name": {Eq("a"): 1}, "empty name": {"": 1}, "not an identifier": {"a b": 1}, "mixed": {"a": 1, 2: 3}, "surrogate": {"\ud800": 1}}
for cl, c in callables.items():
    print("=====", cl)
    for ml, m in mappings.items():
        attempt(ml, lambda: ascii(c(**m))[:100])
print("===== more than one")
attempt("int twice", lambda: f(**{1: 3}, **{1: 5}))
attempt("str twice", lambda: f(**{"x": 3}, **{"x": 5}))
attempt("str and its subclass", lambda: f(**{"x": 3}, **{S("x"): 5}))
attempt("written and given", lambda: f(x=1, **{"x": 5}))
attempt("written and given as a subclass", lambda: f(x=1, **{S("x"): 5}))
attempt("written and equal", lambda: f(x=1, **{Eq("x"): 5}))
attempt("int and str", lambda: f(**{1: 3}, **{"x": 5}))
attempt("position and keyword", lambda: f(1, **{"a": 5}))
attempt("position and subclass", lambda: f(1, **{S("a"): 5}))
attempt("dict twice", lambda: dict(**{1: 3}, **{1: 5}))
attempt("None twice", lambda: h(**{None: 3}, **{None: 5}))
attempt("the type of the keys", lambda: [type(k).__name__ for k in h(**{S("a"): 1, Color.RED: 2, "b": 3})])
attempt("the very keys", lambda: (lambda k: [x is k for x in h(**{k: 1})])(S("a")))
attempt("a mapping that is no dict", lambda: f(**collections.UserDict({S("a"): 1, "q": 2})))
attempt("a mapping with an int", lambda: f(**collections.UserDict({1: 1})))
attempt("*args too", lambda: f(*[1], **{S("q"): 2}))
