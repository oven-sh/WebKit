# collections.OrderedDict
import collections, _collections, pickle, copy, operator, sys, json, inspect, types, hashlib
from collections import OrderedDict as OD
import re; address = re.compile('0x[0-9a-f]+')
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
        c = e.__context__
        if c is not None: r += " <- %s: %s" % (type(c).__name__, c)
    print(label, "->", address.sub("0x", ascii(r)))
print(OD is _collections.OrderedDict)
def abc(): return OD([("a", 1), ("b", 2), ("c", 3)])
class S(OD): pass
log = []
class L(OD):
    "One that says what is asked of it."
    def __setitem__(self, k, v): log.append(("set", k, v)); OD.__setitem__(self, k, v)
    def __getitem__(self, k): log.append(("get", k)); return OD.__getitem__(self, k)
    def __delitem__(self, k): log.append(("del", k)); OD.__delitem__(self, k)
    def __contains__(self, k): log.append(("in", k)); return OD.__contains__(self, k)
    def __iter__(self): log.append(("iter",)); return OD.__iter__(self)
    def keys(self): log.append(("keys",)); return OD.keys(self)
    def items(self): log.append(("items",)); return OD.items(self)
    def values(self): log.append(("values",)); return OD.values(self)
    def __missing__(self, k): log.append(("missing", k)); return "missing"
def logged(f):
    log.clear(); r = f(); return r, list(log)
class K:
    "A key that says when it is hashed and compared."
    def __init__(self, n, h=None): self.n = n; self.h = n if h is None else h
    def __hash__(self): log.append(("hash", self.n)); return self.h
    def __eq__(self, o): log.append(("eq", self.n, getattr(o, "n", o))); return isinstance(o, K) and self.n == o.n
    def __repr__(self): return "K(%r)" % self.n
print("===== making one")
for label, f in {"none": lambda: OD(), "pairs": lambda: OD([("a", 1), ("b", 2)]), "a dict": lambda: OD({"a": 1, "b": 2}), "keywords": lambda: OD(a=1, b=2), "both": lambda: OD([("a", 1)], b=2, a=3), "another": lambda: OD(abc()), "two": lambda: OD([], []), "three": lambda: OD(1, 2, 3), "an int": lambda: OD(5), "None": lambda: OD(None),
                 "a str": lambda: OD("ab"), "strs of two": lambda: OD(["ab", "cd"]), "a pair too short": lambda: OD([("a",)]), "an empty pair": lambda: OD([()]), "a pair too long": lambda: OD([("a", 1, 2)]), "not pairs": lambda: OD([1]), "a generator": lambda: OD((c, i) for i, c in enumerate("abc")), "lists": lambda: OD([["a", 1]]),
                 "sets of two": lambda: len(OD([{1, 2}])), "unhashable": lambda: OD([([], 1)]), "self by keyword": lambda: OD(self=1), "other by keyword": lambda: OD(other=1), "keys and getitem": lambda: OD(type("M", (), {"keys": lambda s: "ab", "__getitem__": lambda s, k: k.upper()})()),
                 "items only": lambda: OD(type("M", (), {"items": lambda s: [("a", 1)]})()), "keys not callable": lambda: OD(type("M", (), {"keys": 5})()), "keys raises": lambda: OD(type("M", (), {"keys": property(lambda s: 1 / 0)})()), "keys gives an int": lambda: OD(type("M", (), {"keys": lambda s: 5})()),
                 "getitem raises": lambda: OD(type("M", (), {"keys": lambda s: "a", "__getitem__": lambda s, k: 1 / 0})()), "a mappingproxy": lambda: OD(types.MappingProxyType({"a": 1})), "a Counter": lambda: OD(collections.Counter("aab")), "a defaultdict": lambda: OD(collections.defaultdict(int, a=1)),
                 "__init__ again": lambda: (lambda d: (d.__init__([("z", 1)], a=5), d)[1])(abc()), "__init__ with two": lambda: abc().__init__(1, 2), "__new__ alone": lambda: (lambda d: (d, len(d), list(d), d.popitem.__name__))(OD.__new__(OD)), "dict.__new__": lambda: type(dict.__new__(OD)).__name__, "object.__new__": lambda: object.__new__(OD),
                 "dict.__init__": lambda: (lambda d: (dict.__init__(d, a=1), len(d), list(d), dict.keys(d)))(OD())}.items():
    attempt(label, f)
print("===== shown")
for d in (OD(), abc(), OD(a=OD(b=1)), S(), S(a=1), OD([(1, "x"), ((1, 2), None)]), OD({"a": [1, 2]})):
    attempt(repr(d), lambda: (str(d), format(d), len(d), bool(d), list(d), list(d.keys()), list(d.values()), list(d.items()), list(reversed(d)), list(reversed(d.keys())), list(reversed(d.values())), list(reversed(d.items()))))
    attempt("   views", lambda: (repr(d.keys()), repr(d.values()), repr(d.items()), len(d.keys()), type(d.keys()).__name__, type(d.values()).__name__, type(d.items()).__name__, type(iter(d)).__name__, type(iter(d.items())).__name__, type(reversed(d.values())).__name__))
def recursive():
    d = OD(); d["a"] = d; return repr(d)
attempt("in itself", recursive)
def recursive_values():
    d = OD(); d["a"] = d.values(); return repr(d)
attempt("its values in itself", recursive_values)
def recursive_items():
    d = OD(); d["a"] = d.items(); return repr(d), repr(d.items())
attempt("its items in itself", recursive_items)
attempt("a repr that raises", lambda: repr(OD(a=type("R", (), {"__repr__": lambda s: 1 / 0})())))
attempt("the name of a derived class", lambda: (repr(type("m.X", (OD,), {})(a=1)), repr(type("X", (OD,), {"__module__": "mod"})())))
print("===== the order")
def steps(*ops):
    d = abc(); out = []
    for op in ops:
        try: r = op(d)
        except Exception as e: r = "%s: %s" % (type(e).__name__, e)
        out.append((r, list(d.items())))
    return out
attempt("set again", lambda: steps(lambda d: d.__setitem__("a", 9)))
attempt("delete and set", lambda: steps(lambda d: d.__delitem__("a"), lambda d: d.__setitem__("a", 9)))
for k in ("a", "b", "c", "z", 5, None, [], ("a",)):
    for last in (True, False, 0, 1, None, "x", [], 2):
        attempt("move_to_end(%r, %r)" % (k, last), lambda: steps(lambda d: d.move_to_end(k, last)))
attempt("move_to_end: default", lambda: steps(lambda d: d.move_to_end("a")))
attempt("move_to_end: keywords", lambda: steps(lambda d: d.move_to_end(key="a", last=False)))
attempt("move_to_end: none", lambda: abc().move_to_end())
attempt("move_to_end: three", lambda: abc().move_to_end("a", True, 1))
attempt("move_to_end: unknown", lambda: abc().move_to_end("a", first=True))
attempt("move_to_end: of nothing", lambda: OD().move_to_end("a"))
attempt("move_to_end: last that cannot say", lambda: abc().move_to_end("a", type("B", (), {"__bool__": lambda s: 1 / 0})()))
attempt("move_to_end: of one", lambda: (lambda d: (d.move_to_end("a"), d.move_to_end("a", False), d)[2])(OD(a=1)))
attempt("many moves", lambda: (lambda d: ([d.move_to_end(i % 7, i % 3 == 0) for i in range(100)], list(d))[1])(OD.fromkeys(range(7))))
for last in (True, False, 0, 1, None, "x"):
    attempt("popitem(%r)" % (last,), lambda: steps(lambda d: d.popitem(last), lambda d: d.popitem(last), lambda d: d.popitem(last), lambda d: d.popitem(last)))
attempt("popitem: keyword", lambda: abc().popitem(last=False))
attempt("popitem: two", lambda: abc().popitem(True, 1))
attempt("popitem: unknown", lambda: abc().popitem(first=True))
for a in (("a",), ("z",), ("z", None), ("z", 5), ("a", 5), ([],), ([], 5), (), ("a", 1, 2)):
    attempt("pop%r" % (a,), lambda: steps(lambda d: d.pop(*a)))
attempt("pop: keywords", lambda: (abc().pop(key="a"), abc().pop("z", default=7), abc().pop(key="z", default=None)))
attempt("pop: unknown", lambda: abc().pop("a", d=1))
for a in (("a",), ("z",), ("z", 5), ("a", 5), ([],), (), ("a", 1, 2)):
    attempt("setdefault%r" % (a,), lambda: steps(lambda d: d.setdefault(*a)))
attempt("setdefault: keywords", lambda: steps(lambda d: d.setdefault(key="z", default=7)))
attempt("clear", lambda: steps(lambda d: d.clear(), lambda d: d.__setitem__("q", 1), lambda d: d.popitem()))
attempt("clear: an argument", lambda: abc().clear(1))
attempt("del what is not there", lambda: steps(lambda d: d.__delitem__("z"), lambda d: d.__delitem__([])))
attempt("get, in, len", lambda: (abc().get("a"), abc().get("z"), abc().get("z", 5), "a" in abc(), "z" in abc(), abc()["a"], len(abc())))
attempt("getitem of what is not there", lambda: abc()["z"])
print("===== update, and |")
for label, a, k in (("a dict", ({"c": 9, "d": 4},), {}), ("pairs", ([("d", 4), ("a", 9)],), {}), ("keywords", (), {"d": 4, "a": 9}), ("both", ({"d": 4},), {"e": 5}), ("another", (OD(z=1, a=2),), {}), ("none", (), {}), ("two", ({}, {}), {}), ("an int", (5,), {}), ("None", (None,), {}), ("bad pairs", ([(1, 2, 3)],), {}), ("part way", ([("x", 1), ("y",)],), {}),
                    ("itself", "self", {}), ("self by keyword", (), {"self": 1})):
    attempt("update: " + label, lambda: steps(lambda d: d.update(*((d,) if a == "self" else a), **k)))
for b in ({"c": 9, "d": 4}, OD(d=4, a=9), S(d=4), [("d", 4)], 5, None, collections.Counter("dd"), types.MappingProxyType({"d": 4}), collections.UserDict(d=4), {}):
    attempt("| %r" % (b,), lambda: (lambda r: (r, type(r).__name__))(abc() | b))
    attempt("   the other way", lambda: (lambda r: (r, type(r).__name__))(b | abc()))
    attempt("   |=", lambda: (lambda d, same: (d.__ior__(b) is d, d))(abc(), None))
    attempt("   S |", lambda: (lambda r: (r, type(r).__name__))(S(a=1) | b))
    attempt("   | S", lambda: (lambda r: (r, type(r).__name__))(b | S(a=1)))
attempt("__or__ itself", lambda: (abc().__or__(5), abc().__ror__(5), abc().__or__({"z": 1}), abc().__ror__({"z": 1}), OD.__or__({"z": 1}, abc())))
attempt("__or__ of neither", lambda: OD.__or__({}, {}))
print("===== copies")
for d in (OD(), abc(), S(a=1, b=2)):
    attempt("copy of %r" % d, lambda: (lambda c: (c, type(c).__name__, c == d, c is d))(d.copy()))
    attempt("   copy.copy", lambda: (lambda c: (c, type(c).__name__))(copy.copy(d)))
    attempt("   deepcopy", lambda: (lambda c: (c, type(c).__name__))(copy.deepcopy(d)))
    attempt("   reduce", lambda: (lambda r: (r[0].__name__, r[1], r[2], r[3], type(r[4]).__name__, list(r[4])))(d.__reduce__()))
    attempt("   reduce_ex", lambda: [(lambda r: (r[0].__name__, r[1], r[2], r[3], list(r[4])))(d.__reduce_ex__(p)) for p in (0, 2, 4)])
    attempt("   pickle", lambda: [(lambda r: (r == d, type(r) is type(d), list(r)))(pickle.loads(pickle.dumps(d, p))) for p in range(6)])
    attempt("   pickled", lambda: [pickle.dumps(d, p) for p in (0, 2, 4)] if type(d) is OD else None)
def with_attributes():
    d = abc(); d.x = 1; d.y = [2]; c = copy.copy(d); p = pickle.loads(pickle.dumps(d)); return vars(d), d.__reduce__()[2], vars(c), vars(p), vars(d.copy()), c.y is d.y, copy.deepcopy(d).y is d.y
attempt("with attributes", with_attributes)
attempt("moved, and then copied", lambda: (lambda d: (d.move_to_end("a"), d.copy(), dict(d), copy.copy(d), pickle.loads(pickle.dumps(d)), {**d}, list(d.items()), json.dumps(d), (lambda **k: list(k))(**d), OD(d), repr(d)))(abc()))
attempt("copy: an argument", lambda: abc().copy(1))
class NoArgs(OD):
    def __init__(self, required): OD.__init__(self)
attempt("copy of a class that wants an argument", lambda: NoArgs(1).copy())
attempt("| of a class that takes one", lambda: type(NoArgs(1) | {}).__name__)
for a in (("abc",), ("abc", 0), ([],), (5,), (), ("ab", 1, 2), ({"x": 1, "y": 2},), (OD(b=1, a=2),), ({3, }, None), ([[]],)):
    attempt("fromkeys%r" % (a,), lambda: (lambda r: (r, type(r).__name__))(OD.fromkeys(*a)))
    attempt("   of S", lambda: (lambda r: (r, type(r).__name__))(S.fromkeys(*a)))
attempt("fromkeys: keywords", lambda: OD.fromkeys(iterable="ab", value=1))
attempt("fromkeys: unknown", lambda: OD.fromkeys("ab", v=1))
attempt("fromkeys: on an instance", lambda: abc().fromkeys("xy"))
attempt("fromkeys: a class that makes something else", lambda: type("X", (OD,), {"__new__": lambda c: {}}).fromkeys("ab"))
attempt("fromkeys: a class that makes a list", lambda: type("X", (OD,), {"__new__": lambda c: []}).fromkeys("ab"))
print("===== compared")
pairs = [(abc(), abc()), (abc(), OD(c=3, b=2, a=1)), (abc(), {"a": 1, "b": 2, "c": 3}), (abc(), {"c": 3, "b": 2, "a": 1}), (abc(), OD(a=1, b=2)), (abc(), OD(a=1, b=2, c=4)), (OD(), OD()), (OD(), {}), (abc(), S(a=1, b=2, c=3)), (abc(), S(c=3, b=2, a=1)), (S(a=1), S(a=1)), (abc(), [("a", 1)]), (abc(), None), (abc(), 5),
         (abc(), collections.UserDict(abc())), (abc(), collections.Counter(a=1, b=2, c=3)), (abc(), types.MappingProxyType(abc())), (OD({1: 1, 2: 2}), OD({1.0: 1, 2.0: 2})), (OD({1: 1, 2: 2}), OD({2: 2, 1: 1.0}))]
for a, b in pairs:
    attempt("%r and %r" % (a, b), lambda: (a == b, a != b, b == a, b != a, a.__eq__(b), a.__ne__(b)))
    for name, op in (("<", operator.lt), ("<=", operator.le), (">", operator.gt), (">=", operator.ge)):
        attempt("   " + name, lambda: op(a, b))
attempt("the methods", lambda: (abc().__lt__(abc()), abc().__le__({}), abc().__gt__(5), abc().__ge__(abc())))
attempt("hash", lambda: hash(abc()))
attempt("__hash__", lambda: (OD.__hash__, S.__hash__))
print("===== what is asked of a key")
attempt("set", lambda: logged(lambda: OD().__setitem__(K(1), 1)))
def one(): d = OD(); d[K(1)] = 1; d[K(2)] = 2; return d
for label, f in {"set the same": lambda d: d.__setitem__(K(1), 5), "set another": lambda d: d.__setitem__(K(3), 5), "get": lambda d: d[K(1)], "in": lambda d: K(1) in d, "del": lambda d: d.__delitem__(K(1)), "pop": lambda d: d.pop(K(1)), "pop what is not there": lambda d: d.pop(K(3), None), "popitem": lambda d: d.popitem(),
                 "popitem first": lambda d: d.popitem(False), "move_to_end": lambda d: d.move_to_end(K(1)), "move_to_end of the last": lambda d: d.move_to_end(K(2)), "setdefault there": lambda d: d.setdefault(K(1), 5), "setdefault not there": lambda d: d.setdefault(K(3), 5), "list": lambda d: list(d), "items": lambda d: list(d.items()),
                 "values": lambda d: list(d.values()), "reversed": lambda d: list(reversed(d)), "copy": lambda d: d.copy(), "==": lambda d: d == one.saved, "repr": lambda d: repr(d), "update": lambda d: d.update(one.saved), "dict()": lambda d: dict(d), "clear": lambda d: d.clear(), "fromkeys": lambda d: OD.fromkeys(d), "len": lambda d: len(d)}.items():
    d = one(); one.saved = one()
    attempt(label, lambda: logged(lambda: f(d)))
def collide(): d = OD(); [d.__setitem__(K(i, 1), i) for i in range(4)]; return d
for label, f in {"get": lambda d: d[K(3, 1)], "del the second": lambda d: (d.__delitem__(K(1, 1)), list(d)), "del and get": lambda d: (d.__delitem__(K(1, 1)), d[K(3, 1)], list(d.items())), "move": lambda d: (d.move_to_end(K(0, 1)), list(d)), "pop": lambda d: (d.pop(K(2, 1)), list(d)), "list": lambda d: list(d)}.items():
    d = collide()
    attempt("hashes that collide: " + label, lambda: logged(lambda: f(d)))
class BadHash:
    def __hash__(self): raise ZeroDivisionError("hash")
class BadEq:
    def __init__(self): self.bad = False
    def __hash__(self): return 1
    def __eq__(self, o):
        if self.bad: raise ZeroDivisionError("eq")
        return self is o
for label, f in {"set": lambda d: d.__setitem__(BadHash(), 1), "del": lambda d: d.__delitem__(BadHash()), "pop": lambda d: d.pop(BadHash()), "pop with a default": lambda d: d.pop(BadHash(), 1), "move_to_end": lambda d: d.move_to_end(BadHash()), "setdefault": lambda d: d.setdefault(BadHash()), "in": lambda d: BadHash() in d, "get": lambda d: d.get(BadHash())}.items():
    attempt("a hash that raises: " + label, lambda: f(abc()))
def badeq(f):
    a, b = BadEq(), BadEq(); d = OD([(a, 1)]); a.bad = b.bad = True
    try: return f(d, b)
    finally: a.bad = b.bad = False; print("      left with", len(d), len(list(d)))
for label, f in {"set": lambda d, b: d.__setitem__(b, 1), "del": lambda d, b: d.__delitem__(b), "pop": lambda d, b: d.pop(b), "pop with a default": lambda d, b: d.pop(b, 1), "move_to_end": lambda d, b: d.move_to_end(b), "setdefault": lambda d, b: d.setdefault(b), "in": lambda d, b: b in d}.items():
    attempt("an __eq__ that raises: " + label, lambda: badeq(f))
print("===== what is asked of a derived class")
def l(): d = L(); OD.update(d, [("a", 1), ("b", 2)]); log.clear(); return d
for label, f in {"L(pairs)": lambda d: L([("x", 1)]), "L(dict)": lambda d: L({"x": 1}), "L(keywords)": lambda d: L(x=1), "L(L)": lambda d: L(d), "OD(L)": lambda d: OD(d), "dict(L)": lambda d: dict(d), "update(L)": lambda d: OD().update(d), "update": lambda d: d.update(x=1), "setdefault there": lambda d: d.setdefault("a", 5),
                 "setdefault not there": lambda d: d.setdefault("x", 5), "pop": lambda d: d.pop("a"), "pop what is not there": lambda d: d.pop("x", None), "popitem": lambda d: d.popitem(), "copy": lambda d: d.copy(), "move_to_end": lambda d: d.move_to_end("a"), "clear": lambda d: d.clear(), "repr": lambda d: repr(d), "==": lambda d: d == OD(a=1, b=2),
                 "== a dict": lambda d: d == {"a": 1, "b": 2}, "reduce": lambda d: list(d.__reduce__()[4]), "fromkeys": lambda d: L.fromkeys("x"), "|": lambda d: d | {"x": 1}, "r|": lambda d: {"x": 1} | d, "|=": lambda d: d.__ior__({"x": 1}), "list": lambda d: list(d), "reversed": lambda d: list(reversed(d)), "get": lambda d: d.get("a"),
                 "missing": lambda d: d["zz"], "get of what is missing": lambda d: d.get("zz"), "**": lambda d: (lambda **k: k)(**d), "{**}": lambda d: {**d}, "json": lambda d: json.dumps(d), "copy.copy": lambda d: copy.copy(d), "pickle": lambda d: pickle.loads(pickle.dumps(d)), "len": lambda d: len(d), "in": lambda d: "a" in d,
                 "sorted": lambda d: sorted(d), "keys()": lambda d: list(OD.keys(d)), "items()": lambda d: list(OD.items(d)), "values()": lambda d: list(OD.values(d))}.items():
    d = l()
    attempt(label, lambda: logged(lambda: f(d)))
print("done with the first part")
