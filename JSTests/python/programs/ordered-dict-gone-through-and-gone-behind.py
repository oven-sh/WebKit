# collections.OrderedDict: what goes through one, its views, and what is done behind its back
import collections, _collections, pickle, copy, operator, sys, inspect, types, itertools
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
def abc(): return OD([("a", 1), ("b", 2), ("c", 3)])
makers = {"iter": iter, "reversed": reversed, "keys": lambda d: iter(d.keys()), "values": lambda d: iter(d.values()), "items": lambda d: iter(d.items()), "reversed keys": lambda d: reversed(d.keys()), "reversed values": lambda d: reversed(d.values()), "reversed items": lambda d: reversed(d.items())}
print("===== changed while it is gone through")
changes = {"set another": lambda d: d.__setitem__("z", 1), "set the same": lambda d: d.__setitem__("a", 9), "set what is to come": lambda d: d.__setitem__("b", 9), "del what has been": lambda d: d.__delitem__("a" if "a" in d else "c"), "del what is to come": lambda d: d.__delitem__("b"), "del and set": lambda d: (d.__delitem__("b"), d.__setitem__("b", 1)),
           "del one and set another": lambda d: (d.__delitem__("b"), d.__setitem__("z", 1)), "move": lambda d: d.move_to_end("b"), "move what is there already": lambda d: d.move_to_end("c"), "move to the front what is there": lambda d: d.move_to_end("a", False), "clear": lambda d: d.clear(), "clear and fill": lambda d: (d.clear(), d.update(a=1, b=2, c=3)),
           "popitem": lambda d: d.popitem(), "pop": lambda d: d.pop("b"), "pop what is not there": lambda d: d.pop("zz", None), "setdefault there": lambda d: d.setdefault("a"), "setdefault not there": lambda d: d.setdefault("z"), "update with nothing": lambda d: d.update(), "copy": lambda d: d.copy(), "nothing": lambda d: None,
           "dict.__setitem__ another": lambda d: dict.__setitem__(d, "z", 1), "dict.__setitem__ the same": lambda d: dict.__setitem__(d, "b", 9), "dict.__delitem__ to come": lambda d: dict.__delitem__(d, "b"), "dict.clear": lambda d: dict.clear(d), "dict.pop": lambda d: dict.pop(d, "b"), "dict.popitem": lambda d: dict.popitem(d)}
for how, make in makers.items():
    for what, change in changes.items():
        def go():
            d = abc(); it = make(d); out = [next(it)]; change(d)
            for _ in range(5):
                try: out.append(next(it))
                except StopIteration: out.append("stop")
                except Exception as e: out.append("%s: %s" % (type(e).__name__, e))
            return out
        attempt("%s, %s" % (how, what), go)
print("===== an iterator")
for how, make in makers.items():
    it = make(abc())
    attempt(how, lambda: (type(it).__name__, iter(it) is it, next(it), list(it), list(it), next(it, "no more")))
    attempt("   of nothing", lambda: (list(make(OD())), next(make(OD()), "no more")))
    attempt("   reduce", lambda: (lambda i: (i.__reduce__(), next(i), i.__reduce__(), list(i), i.__reduce__()))(make(abc())))
    attempt("   pickle", lambda: [(lambda i: (next(i), list(pickle.loads(pickle.dumps(i, p))), list(i)))(make(abc())) for p in (0, 2, 5)])
    attempt("   copy", lambda: (lambda i: (next(i), type(copy.copy(i)).__name__, list(copy.copy(i)), list(i)))(make(abc())))
    attempt("   length hint", lambda: operator.length_hint(make(abc()), -1))
    attempt("   reduce after a change", lambda: (lambda d: (lambda i: (next(i), d.__setitem__("z", 1), i.__reduce__()))(make(d)))(abc()))
I = type(iter(OD()))
attempt("the class", lambda: (I, I.__name__, I.__qualname__, I.__module__, I.__mro__, sorted(vars(I)), bool(I.__flags__ & (1 << 10)), I.__doc__))
attempt("make one", lambda: I())
attempt("derive", lambda: type("X", (I,), {}))
attempt("attribute", lambda: setattr(iter(OD()), "x", 1))
attempt("__next__ of something else", lambda: I.__next__(5))
attempt("__next__ with an argument", lambda: iter(abc()).__next__(1))
attempt("keeps the dict", lambda: (lambda i: list(i))(iter(OD(a=1))))
attempt("many at once", lambda: (lambda d: list(zip(d, reversed(d), d.values(), d.items())))(abc()))
attempt("a large one", lambda: (lambda d: (sum(d), sum(reversed(d)), sum(d.values()), len(list(d.items())), next(iter(d)), next(reversed(d))))(OD.fromkeys(range(100000), 1)))
def churn():
    d = OD(); out = []
    for i in range(20000):
        d[i] = i
        if i % 3 == 0: d.move_to_end(i // 2 if i // 2 in d else i, i % 2 == 0)
        if i % 5 == 0 and len(d) > 3: d.popitem(i % 10 == 0)
        if i % 7 == 0 and (i - 3) in d: del d[i - 3]
    return len(d), list(d)[:8], list(reversed(d))[:8], sum(d), list(d) == list(reversed(list(reversed(d)))), [k for k, v in d.items() if k != v]
attempt("a good deal of coming and going", churn)
print("===== the views")
d = abc()
for name, v, base in (("keys", d.keys(), type({}.keys())), ("values", d.values(), type({}.values())), ("items", d.items(), type({}.items()))):
    V = type(v)
    attempt(name, lambda: (V, V.__name__, V.__qualname__, V.__module__, V.__mro__, V.__base__ is base, sorted(vars(V)), bool(V.__flags__ & (1 << 10)), V.__doc__, isinstance(v, base), isinstance(v, collections.abc.MappingView)))
    attempt("   abc", lambda: (isinstance(v, collections.abc.KeysView), isinstance(v, collections.abc.ValuesView), isinstance(v, collections.abc.ItemsView), isinstance(v, collections.abc.Set), isinstance(v, collections.abc.Reversible)))
    attempt("   one", lambda: (repr(v), len(v), list(v), list(reversed(v)), v.mapping, type(v.mapping).__name__, v.mapping["a"]))
    attempt("   make one", lambda: V())
    attempt("   make one of a dict", lambda: V({}))
    attempt("   derive", lambda: type("X", (V,), {}))
    attempt("   attribute", lambda: setattr(v, "x", 1))
    attempt("   hash", lambda: hash(v) == hash(v))
    attempt("   pickle", lambda: pickle.dumps(v))
    attempt("   copy", lambda: copy.copy(v))
    attempt("   follows the dict", lambda: (lambda e: (lambda w: (list(w), e.__setitem__("z", 1), e.move_to_end("a"), list(w), len(w)))(getattr(e, name)()))(abc()))
    attempt("   __iter__ of the base", lambda: (type(base.__iter__(v)).__name__, list(base.__iter__(v))))
    attempt("   __reversed__ of the base", lambda: list(base.__reversed__(v)))
    attempt("   __iter__ on the view of a dict", lambda: V.__iter__(getattr({"a": 1}, name)()))
moved = abc(); moved.move_to_end("a")
attempt("the base goes by the dict", lambda: (list(dict.keys(moved)), list(dict.items(moved)), list(dict.values(moved)), list(dict.__iter__(moved)), list(dict.__reversed__(moved)), dict.__repr__(moved), dict.copy(moved), type(dict.copy(moved)).__name__, dict.__eq__(moved, abc())))
sets = [{"a", "z"}, {"a"}, set(), ["a", "a", "z"], "ab", {"a": 1}.keys(), OD(a=1, z=2).keys(), frozenset("a"), 5, None, {("a", 1), ("z", 2)}, OD(a=1).items(), {"a": 1}.items(), iter("ab")]
for name, op in (("&", operator.and_), ("|", operator.or_), ("-", operator.sub), ("^", operator.xor), ("==", operator.eq), ("!=", operator.ne), ("<", operator.lt), ("<=", operator.le), (">", operator.gt), (">=", operator.ge)):
    for o in sets:
        for vn in ("keys", "items", "values"):
            def go(swap=False):
                v = getattr(abc(), vn)(); x = iter("ab") if type(o) is type(iter("")) else o
                r = op(x, v) if swap else op(v, x)
                return (sorted(r, key=repr), type(r).__name__) if isinstance(r, (set, frozenset)) else r
            attempt("%s %s %s" % (vn, name, address.sub("0x", repr(o))), go)
            attempt("   the other way", lambda: go(True))
for o in sets:
    attempt("isdisjoint(%s)" % address.sub("0x", repr(o)), lambda: (abc().keys().isdisjoint(iter("ab") if type(o) is type(iter("")) else o), abc().items().isdisjoint(iter("ab") if type(o) is type(iter("")) else o)))
attempt("values has none", lambda: abc().values().isdisjoint([]))
for x in ("a", "z", 1, ("a", 1), ("a", 2), ("z", 1), ("a",), ("a", 1, 2), [], ([], 1), ("a", []), None):
    attempt("%r in" % (x,), lambda: (x in abc().keys(), x in abc().items(), x in abc().values()))
print("===== behind its back")
# Not all that can be done, by a long way: CPython goes round for ever after some of it, and goes down after some.
def behind(*ops):
    d = abc(); out = []
    for op in ops:
        try: r = op(d)
        except Exception as e: r = "%s: %s" % (type(e).__name__, e)
        out.append(r)
    for label, f in (("len", len), ("list", list), ("items", lambda d: list(d.items())), ("values", lambda d: list(d.values())), ("repr", repr), ("dict keys", lambda d: list(dict.keys(d))), ("copy", lambda d: d.copy()), ("==", lambda d: d == d)):
        try: r = f(d)
        except Exception as e: r = "%s: %s" % (type(e).__name__, e)
        out.append((label, r))
    return out
for label, ops in {"dict.__setitem__ another": [lambda d: dict.__setitem__(d, "z", 1)], "dict.__setitem__ the same": [lambda d: dict.__setitem__(d, "a", 9)], "dict.__delitem__": [lambda d: dict.__delitem__(d, "b")], "dict.__delitem__ the first": [lambda d: dict.__delitem__(d, "a")], "dict.__delitem__ the last": [lambda d: dict.__delitem__(d, "c")],
                   "dict.pop": [lambda d: dict.pop(d, "b")], "dict.update": [lambda d: dict.update(d, z=1)], "dict.setdefault": [lambda d: dict.setdefault(d, "z", 1)], "dict.__ior__": [lambda d: dict.__ior__(d, {"z": 1}) is d],
                   "set behind, then set": [lambda d: dict.__setitem__(d, "z", 1), lambda d: d.__setitem__("z", 5)], "set behind, then deleted": [lambda d: dict.__setitem__(d, "z", 1), lambda d: d.__delitem__("z")],
                   "deleted behind, then deleted": [lambda d: dict.__delitem__(d, "b"), lambda d: d.__delitem__("b")], "deleted behind, then popped": [lambda d: dict.__delitem__(d, "b"), lambda d: d.pop("b")], "deleted behind, then popped with a default": [lambda d: dict.__delitem__(d, "b"), lambda d: d.pop("b", "default")],
                   "deleted behind, then moved": [lambda d: dict.__delitem__(d, "b"), lambda d: d.move_to_end("b")], "cleared behind, then cleared": [lambda d: dict.clear(d), lambda d: d.clear()],
                   "set behind, many": [lambda d: [dict.__setitem__(d, i, i) for i in range(50)] and None], "set behind, many, then set": [lambda d: [dict.__setitem__(d, i, i) for i in range(50)] and None, lambda d: d.__setitem__("q", 1)]}.items():
    attempt(label, lambda: behind(*ops))
print("===== as other things take it")
m = abc(); m.move_to_end("a")
for label, f in {"dict()": lambda: dict(m), "{**}": lambda: {**m}, "f(**)": lambda: (lambda **k: list(k.items()))(**m), "list": lambda: list(m), "tuple": lambda: tuple(m), "sorted": lambda: sorted(m), "set": lambda: sorted(set(m)), "enumerate": lambda: list(enumerate(m)), "zip": lambda: list(zip(m, m.values())), "max": lambda: max(m), "join": lambda: "".join(m),
                 "unpack": lambda: (lambda a, b, c: (a, b, c))(*m), "star": lambda: [*m], "dict comprehension": lambda: {k: v for k, v in m.items()}, "update of a dict": lambda: (lambda d: (d.update(m), d)[1])({}), "dict |": lambda: {} | m, "| dict": lambda: m | {}, "format_map": lambda: "{a}{b}".format_map(m), "format(**)": lambda: "{a}{b}".format(**m),
                 "%": lambda: "%(a)s%(b)s" % m, "ChainMap": lambda: list(collections.ChainMap(m)), "Counter": lambda: list(collections.Counter(m).items()), "type()": lambda: [n for n in vars(type("X", (), m)) if len(n) == 1], "mappingproxy": lambda: list(types.MappingProxyType(m)), "match": lambda: match(m), "isinstance": lambda: (isinstance(m, dict), isinstance(m, collections.abc.MutableMapping), isinstance(m, collections.abc.Reversible), issubclass(OD, dict)),
                 "vars() of what has it for a __dict__": lambda: has_dict(), "exec": lambda: in_exec(), "eval with it for locals": lambda: eval("a + b", {}, m), "SimpleNamespace": lambda: repr(types.SimpleNamespace(**m)), "operator.itemgetter": lambda: operator.itemgetter("a", "c")(m), "copy of its dict order": lambda: list(dict.copy(m)), "pprint": lambda: __import__("pprint").pformat(m), "reprlib": lambda: __import__("reprlib").repr(m)}.items():
    attempt(label, f)
def match(x):
    match x:
        case {"a": a, **rest}: return a, rest, type(rest).__name__
def has_dict():
    class C: pass
    c = C(); d = OD(x=1); c.__dict__ = d; c.y = 2
    return c.x, c.y, type(vars(c)).__name__, vars(c) is d, len(d), sorted(dict.keys(d))
def in_exec():
    d = OD(); exec("p = 1; q = 2", d); return type(d).__name__, "p" in d, d["q"], len([k for k in dict.keys(d) if len(k) == 1])
attempt("match", lambda: match(m)); attempt("as a __dict__", has_dict); attempt("exec", in_exec)
print("===== the class")
attempt("the class", lambda: (OD, OD.__name__, OD.__qualname__, OD.__module__, OD.__mro__, OD.__doc__, OD.__text_signature__, OD.__basicsize__, OD.__itemsize__, OD.__dictoffset__, OD.__weakrefoffset__, hex(OD.__flags__), sorted(vars(OD))))
for n in sorted(vars(OD)):
    v = vars(OD)[n]
    attempt("   " + n, lambda: (type(v).__name__, getattr(v, "__doc__", None), getattr(v, "__text_signature__", None), getattr(v, "__qualname__", None)))
attempt("signature", lambda: str(inspect.signature(OD)))
attempt("attributes", lambda: (lambda d: (setattr(d, "x", 1), d.x, vars(d), d.__dict__, delattr(d, "x"), vars(d), list(d)))(abc()))
attempt("__dict__ set", lambda: (lambda d: (setattr(d, "__dict__", {"q": 1}), d.q, list(d)))(abc()))
attempt("__dict__ set to an int", lambda: setattr(abc(), "__dict__", 5))
attempt("__dict__ deleted", lambda: (lambda d: (setattr(d, "x", 1), delattr(d, "__dict__"), vars(d)))(abc()))
attempt("weak reference", lambda: (lambda d: __import__("weakref").ref(d)() is d)(abc()))
attempt("generic", lambda: (OD[str, int], OD[str, int].__origin__ is OD, type(OD[str, int]).__name__))
attempt("slots", lambda: (lambda X: (X.__dictoffset__ != 0, X(a=1)))(type("X", (OD,), {"__slots__": ()})))
attempt("with another base", lambda: [type("X", b, {}).__mro__ for b in ((OD, dict), (OD, collections.abc.Mapping), (collections.Counter, OD), (OD, collections.defaultdict))])
attempt("dict first", lambda: type("X", (dict, OD), {}))
attempt("__class__", lambda: (lambda d: (setattr(d, "__class__", type("Y", (OD,), {})), type(d).__name__))(type("X", (OD,), {})(a=1)))
attempt("__class__ of a plain one", lambda: setattr(abc(), "__class__", dict))
for n in ("__setitem__", "__delitem__", "__iter__", "__reversed__", "__repr__", "__eq__", "__init__", "__reduce__", "__sizeof__", "clear", "copy", "items", "keys", "values", "move_to_end", "pop", "popitem", "setdefault", "update", "__or__", "__ior__"):
    attempt("OD.%s of a dict" % n, lambda: getattr(OD, n)({}, *(("a",) * {"__setitem__": 2, "__delitem__": 1, "__eq__": 1, "move_to_end": 1, "pop": 1, "setdefault": 1, "__or__": 1, "__ior__": 1}.get(n, 0))))
    attempt("   of nothing", lambda: getattr(OD, n)())
    attempt("   too many", lambda: getattr(abc(), n)(1, 2, 3, 4))
    attempt("   a keyword", lambda: getattr(abc(), n)(zzz=1))
print("done")
