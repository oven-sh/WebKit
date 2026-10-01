# A dict that is changed while it is gone through or taken from, what is said when it cannot be made of something, how often a key is asked for its hash, and keys of a class derived from str.
def attempt(label, f):
    try: print(label, "=>", repr(f()))
    except BaseException as e: print(label, "=>", type(e).__name__, e, getattr(e, "__notes__", ""))
print("---- changed while it is gone through")
def through(make, view, change, most=10):
    d = make(); out = []
    it = iter(view(d))
    try:
        for x in it:
            out.append(x); change(d, x)
            if len(out) >= most: out.append("..."); break
    except RuntimeError as e:
        out.append("RuntimeError: %s" % e)
        out.append(("then", list(it)))
    return out
def delAndAdd(d, x):
    k = next(iter(d)); v = d.pop(k); d[k] = v
def delAndAddOther(d, x):
    d.pop(next(iter(d))); d[len(d) + 100 + id(x) % 7] = 0
views = {"keys": lambda d: d, "keys()": lambda d: d.keys(), "values()": lambda d: d.values(), "items()": lambda d: d.items(), "reversed": lambda d: reversed(d), "reversed items": lambda d: reversed(d.items())}
for name, view in views.items():
    for label, make in (("one", lambda: {0: 0}), ("three", lambda: {0: 0, 1: 1, 2: 2}), ("strs", lambda: {"a": 1, "b": 2})):
        attempt("%s of %s, taken out and put back" % (name, label), lambda: through(make, view, delAndAdd))
    attempt("%s, one added" % name, lambda: through(lambda: {0: 0, 1: 1}, view, lambda d, x: d.__setitem__(len(d) + 5, 0)))
    attempt("%s, one taken out" % name, lambda: through(lambda: {0: 0, 1: 1, 2: 2}, view, lambda d, x: d.pop(2, None)))
    attempt("%s, a value set" % name, lambda: through(lambda: {0: 0, 1: 1}, view, lambda d, x: d.__setitem__(1, 9)))
    attempt("%s, cleared" % name, lambda: through(lambda: {0: 0, 1: 1}, view, lambda d, x: d.clear()))
    attempt("%s, cleared and filled" % name, lambda: through(lambda: {0: 0, 1: 1}, view, lambda d, x: (d.clear(), d.update({5: 5, 6: 6}))))
class O: pass
def instance():
    o = O(); o.a = 1; o.b = 2; return o.__dict__
def afterClear():
    d = {}
    for i in range(1000): d[f"k{i}"] = i
    for i in range(1, 1000): del d[f"k{i}"]
    its = (reversed(d), reversed(d.keys()), reversed(d.values()), reversed(d.items()), iter(d), iter(d.items()))
    d.clear(); d["k0"] = 0
    return [list(it) for it in its]
attempt("cleared and restored before it is begun", afterClear)
def small():
    d = {"a": 1, "b": 2}
    its = (reversed(d), iter(d))
    d.clear(); d["x"] = 1; d["y"] = 2
    return [list(it) for it in its]
attempt("the same, of a small one", small)
print("---- update")
class X:
    def __hash__(self): return 0
    def __eq__(self, o):
        other.clear()
        return False
other = dict([(i, 0) for i in range(1, 1337)]); other[X()] = 0
attempt("what is being taken from is cleared", lambda: {X(): 0, 1: 1}.update(other))
def badgen():
    yield "key"
    raise TypeError("oops")
class BadIter:
    def __iter__(self): raise TypeError("from __iter__")
class BadLen:
    def __iter__(self): return iter((1, 2, 3))
for label, arg in (("objects", [object()]), ("a generator that raises", [badgen()]), ("__iter__ raises", [BadIter()]), ("three", [BadLen()]), ("one", [(1,)]), ("a str of two", ["ab"]), ("a str of three", ["abc"]), ("an int", [5]), ("second", [(1, 2), 5]), ("ValueError", [(i for i in [1, int("x") if False else 2, 3])]), ("not iterable", 5), ("None", None), ("unhashable", [([], 1)])):
    attempt("dict(%s)" % label, lambda: dict(arg))
    attempt("update(%s)" % label, lambda: {}.update(arg))
print("---- how often the hash is asked for")
class Hashed:
    def __init__(self): self.hash_count = 0; self.eq_count = 0
    def __hash__(self): self.hash_count += 1; return 42
    def __eq__(self, other): self.eq_count += 1; return id(self) == id(other)
def counts(do):
    a, b = Hashed(), Hashed()
    y = {a: 5}
    a.hash_count = 0
    do(y, b)
    return a.hash_count, b.hash_count, a.eq_count + b.eq_count
for label, do in {"setdefault": lambda y, b: y.setdefault(b, []), "get": lambda y, b: y.get(b), "in": lambda y, b: b in y, "set": lambda y, b: y.__setitem__(b, 1), "pop": lambda y, b: y.pop(b, None), "getitem": lambda y, b: y.get(b) or y.__contains__(b), "update": lambda y, b: y.update({b: 1}), "|": lambda y, b: y | {b: 1}, "copy": lambda y, b: y.copy(), "dict()": lambda y, b: dict(y), "fromkeys": lambda y, b: dict.fromkeys([b]), "set.add": lambda y, b: set(y).add(b), "set()": lambda y, b: set(y), "frozenset()": lambda y, b: frozenset(y), "set |": lambda y, b: set(y) | {b}, "in keys": lambda y, b: b in y.keys(), "defaultdict": lambda y, b: __import__("collections").defaultdict(list, y)[b], "Counter": lambda y, b: __import__("collections").Counter([b, b])}.items():
    attempt(label, lambda: counts(do))
print("---- a key of a class derived from str")
class S(str): pass
class H(str):
    def __hash__(self): return 1
    def __eq__(self, o): return False
class Foo:
    def __init__(self, msg): self.msg = msg
f = Foo("123")
attempt("got", lambda: (f.__dict__[S("msg")], f.__dict__.get(S("msg")), S("msg") in f.__dict__, getattr(f, S("msg"))))
attempt("one that is like nothing", lambda: (f.__dict__.get(H("msg")), H("msg") in f.__dict__))
def setIt():
    o = O(); o.attr = 1
    o.__dict__[S("attr")] = 2
    return o.attr, len(o.__dict__), list(o.__dict__)
attempt("set", setIt)
def delIt():
    o = O(); o.attr = 1
    del o.__dict__[S("attr")]
    return hasattr(o, "attr"), o.__dict__
attempt("deleted", delIt)
def newKey():
    o = O(); o.__dict__[S("attr")] = 2
    return o.attr, len(o.__dict__)
attempt("new", newKey)
attempt("in an ordinary dict", lambda: ({"a": 1}[S("a")], {S("a"): 1}["a"], {"a": 1, S("a"): 2}, S("a") in {"a"}, {"a": 1}.get(H("a"))))
import types
m = types.ModuleType("m"); m.x = 5
attempt("in a module's", lambda: (m.__dict__[S("x")], vars(m).get(S("x"))))
attempt("in a class's", lambda: (Foo.__dict__[S("__init__")].__name__, S("__init__") in Foo.__dict__))
attempt("in globals()", lambda: globals()[S("f")] is f)
