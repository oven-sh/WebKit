# What type() will and will not take for a name, a docstring and a namespace, and dir() and iter() of odd things.
import collections, types, builtins, functools, warnings
warnings.simplefilter("ignore")  # One says where this file is.

def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
    print(label, "->", r)

names = ["A", "\xc4", "\U0001f40d", "B.A", "42", "", "A\x00B", "\x00", "A\udcdcB", "\ud800", b"A", 1, None, "a b"]
class S(str): pass
names.append(S("sub"))
for name in names:
    attempt("type(%a)" % (name,), lambda: (lambda A: (ascii(A.__name__), ascii(A.__qualname__), type(A.__name__).__name__))(type(name, (), {})))
for name in names:
    C = type("C", (), {})
    attempt("__name__ = %a" % (name,), lambda: (setattr(C, "__name__", name), ascii(C.__name__), C.__qualname__, type(C.__name__).__name__)[1:])
    attempt("  afterwards", lambda: ascii(C.__name__))
for name in names:
    C = type("C", (), {})
    attempt("__qualname__ = %a" % (name,), lambda: (setattr(C, "__qualname__", name), ascii(C.__qualname__), C.__name__)[1:])
    attempt("type(..., {'__qualname__': %a})" % (name,), lambda: ascii(type("C", (), {"__qualname__": name}).__qualname__))
for doc in ["x", "\xc4", "x\x00y", "x\udcdcy", b"x", 42, None, S("d")]:
    attempt("type(..., {'__doc__': %a})" % (doc,), lambda: ascii(type("A", (), {"__doc__": doc}).__doc__))
    C = type("C", (), {})
    attempt("__doc__ = %a" % (doc,), lambda: (setattr(C, "__doc__", doc), ascii(C.__doc__))[1])
attempt("del __name__", lambda: delattr(type("C", (), {}), "__name__"))
attempt("del __qualname__", lambda: delattr(type("C", (), {}), "__qualname__"))
attempt("del __doc__", lambda: (lambda C: (delattr(C, "__doc__"), C.__doc__))(type("C", (), {"__doc__": "d"})))
attempt("del __doc__ twice", lambda: (lambda C: (delattr(C, "__doc__"), delattr(C, "__doc__")))(type("C", (), {"__doc__": "d"})))
attempt("del __module__", lambda: (lambda C: (delattr(C, "__module__"), C.__module__))(type("C", (), {})))
attempt("int.__name__ = 'x'", lambda: setattr(int, "__name__", "x"))
attempt("int.__module__ = 'x'", lambda: setattr(int, "__module__", "x"))
attempt("int.__doc__ = 'x'", lambda: setattr(int, "__doc__", "x"))

print("== __firstlineno__ goes when __module__ is set")
A = type("A", (), {"__firstlineno__": 42})
print(A.__dict__["__firstlineno__"], A.__firstlineno__)
A.__module__ = "testmodule"
print(A.__module__, "__firstlineno__" in A.__dict__)
attempt("get", lambda: A.__firstlineno__)
A.__firstlineno__ = 43
print(A.__dict__["__firstlineno__"])
class B: pass
print("__firstlineno__" in B.__dict__)
B.__module__ = "m"
print("__firstlineno__" in B.__dict__, list(k for k in B.__dict__ if k in ("__module__", "__firstlineno__", "__static_attributes__")))
class D(B): pass
D.__module__ = 5
print(D.__module__, "__firstlineno__" in D.__dict__)

print("== the namespace")
od = collections.OrderedDict([("a", 1), ("b", 2)])
od.move_to_end("a")
print(list(type("C", (), od).__dict__.items())[:2])
class Keys(dict):
    def keys(self): return ["z", "y"]
    def __getitem__(self, k): return "got " + k
attempt("keys()", lambda: [(k, v) for k, v in type("C", (), Keys(a=1)).__dict__.items() if not k.startswith("__")])
class Iter(dict):
    def __iter__(self): return iter(["q"])
    def __getitem__(self, k): return "got " + k
attempt("__iter__()", lambda: [(k, v) for k, v in type("C", (), Iter(a=1)).__dict__.items() if not k.startswith("__")])
class Iter2(dict):
    def __iter__(self): return iter(["q"])
    def keys(self): return ["k1"]
    def __getitem__(self, k): return "got " + k
attempt("both", lambda: [(k, v) for k, v in type("C", (), Iter2(a=1)).__dict__.items() if not k.startswith("__")])
class Get(dict):
    def __getitem__(self, k): return "got " + k
attempt("__getitem__() alone", lambda: [(k, v) for k, v in type("C", (), Get(a=1)).__dict__.items() if not k.startswith("__")])
attempt("a proxy", lambda: type("C", (), types.MappingProxyType({})))
attempt("keys that are no str", lambda: sorted((k for k in type("C", (), {1: 2, "a": 3, (1, 2): 4}).__dict__ if not (isinstance(k, str) and k.startswith("__"))), key=repr))
ns = {"a": 1}
C = type("C", (), ns); ns["b"] = 2
print(hasattr(C, "b"))
class Meta(type):
    @classmethod
    def __prepare__(m, name, bases): return od.__class__([("p", 0)])
class E(metaclass=Meta):
    z = 1
    y = 2
print([k for k in E.__dict__ if len(k) == 1])

print("== dir() asks the locals for their keys")
class M:
    def __getitem__(self, key):
        if key == "a": return 12
        raise KeyError(key)
    def keys(self): return list("zxy")
g = {"dir": dir, "locals": locals, "sorted": sorted}
attempt("list", lambda: eval("dir()", g, M()))
for what in (1, "ba", ("c", "a"), iter("cab"), {"b": 1, "a": 2}, [3, 1, 2], [1, "a"], None):
    class K:
        def __getitem__(self, key): raise KeyError(key)
        def keys(self, what=what): return what
    attempt("keys() gives %a" % (type(what).__name__,), lambda: eval("dir()", g, K()))
class NoKeys:
    def __getitem__(self, key): raise KeyError(key)
attempt("no keys()", lambda: eval("dir()", g, NoKeys()))
class DK(dict):
    def keys(self): return list("zxy")
attempt("dict subclass", lambda: eval("dir()", g, DK(a=1)))
attempt("UserDict", lambda: eval("dir()", g, collections.UserDict(b=1, a=2)))
attempt("ChainMap", lambda: eval("dir()", g, collections.ChainMap({"b": 1}, {"a": 2})))
attempt("in a class", lambda: eval("dir()", g, {"y": 1, "x": 2}))
attempt("keys that do not compare", lambda: eval("dir()", g, {"y": 1, 2: 2}))
attempt("locals", lambda: type(eval("locals()", g, M())).__name__)
attempt("comprehension", lambda: eval("[sorted(locals()) for i in (2, 3)]", g, collections.UserDict()))
attempt("not a mapping", lambda: eval("a", g, object()))
attempt("globals not a dict", lambda: eval("a", M()))

print("== iter(function, sentinel) that is used up by the function")
def exhaust(it): list(it)
def spam():
    if spam.again: return 2
    spam.again = True
    exhaust(spam.it)
    return 1
spam.again = False
spam.it = iter(spam, 2)
attempt("next", lambda: next(spam.it))
attempt("next again", lambda: next(spam.it))
def raises():
    raise StopIteration("said")
it = iter(raises, 0)
attempt("StopIteration from the function", lambda: next(it))
attempt("and then", lambda: next(it))
def other(): raise ValueError("v")
it = iter(other, 0)
attempt("something else", lambda: next(it))
attempt("and then", lambda: next(it))
class Eq:
    def __eq__(self, o): raise KeyError("compared")
it = iter(lambda: 1, Eq())
attempt("comparing raises", lambda: next(it))
attempt("and then", lambda: next(it))
it = iter(lambda: 1, 1)
attempt("reduce", lambda: (lambda r: (r[0].__name__, len(r[1])))(it.__reduce__()))
attempt("done", lambda: next(it))
attempt("reduce of what is done", lambda: (lambda r: (r[0].__name__, r[1]))(it.__reduce__()))

print("== a key of the builtins that is no str")
d = builtins.__dict__
orig = {"iter": iter, "reversed": reversed, "len": len}
class EmptyIterClass:
    def __len__(self): return 0
    def __getitem__(self, i): raise StopIteration
def run(name, item, sentinel=None):
    it = orig["iter"](item) if sentinel is None else orig["iter"](item, sentinel)
    class CustomStr:
        def __init__(self, name, iterator): self.name = name; self.iterator = iterator
        def __hash__(self): return hash(self.name)
        def __eq__(self, other):
            list(self.iterator)
            return other == self.name
    del d[name]
    key = CustomStr(name, it)
    d[key] = orig[name]
    try:
        return it.__reduce__()
    finally:
        del d[key]
        d[name] = orig[name]
for label, args in (("str", ("xyz",)), ("list", ([1, 2, 3],)), ("getitem", (EmptyIterClass(),)), ("bytes", (bytes(3),)), ("bytearray", (bytearray(3),)), ("tuple", ((1, 2, 3),)), ("callable", (lambda: 0, 0)), ("alias", (tuple[int],)),
                    ("range", (range(3),)), ("set", ({1, 2},)), ("dict", ({1: 2},)), ("dict items", ({1: 2}.items(),))):
    attempt(label, lambda: (lambda r: (r[0] is orig["iter"],) + tuple(r[1:]))(run("iter", *args)))
attempt("reversed", lambda: (lambda r: (r[0] is orig["reversed"],) + tuple(r[1:]))(run("reversed", orig["reversed"](list(range(4))))))
class Named:
    def __init__(self, name): self.name = name
    def __hash__(self): return hash(self.name)
    def __eq__(self, other): return other == self.name
del d["len"]
k = Named("len"); d[k] = orig["len"]
attempt("len, found by what is equal to its name", lambda: len("abc"))
def f(): return len("abcd")
attempt("in a function", f)
attempt("eval", lambda: eval("len('ab')"))
del d[k]
attempt("gone", lambda: len("abc"))
d["len"] = orig["len"]
attempt("back", lambda: len("abc"))
gl = globals()
k = Named("made_up"); gl[k] = "found in the globals"
attempt("a global", lambda: made_up)
attempt("a global, at the top", lambda: eval("made_up"))
del gl[k]
attempt("a global, gone", lambda: made_up)
