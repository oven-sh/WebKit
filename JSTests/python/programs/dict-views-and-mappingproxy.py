def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
def s(x): return sorted(x, key=repr) if isinstance(x, (set, frozenset)) else x
d = {1: "a", 2: "b", 3: "c"}
e = {2: "b", 3: "x", 4: "d"}
k, v, i = d.keys(), d.values(), d.items()
show("types", lambda: (type(k).__name__, type(v).__name__, type(i).__name__, len(k), len(v), len(i)))
show("repr", lambda: (repr(k), repr(v), repr(i), str(k)))
show("reversed", lambda: (list(reversed(k)), list(reversed(v)), list(reversed(i)), type(reversed(v)).__name__, type(reversed(i)).__name__))
show("contains", lambda: (1 in k, 9 in k, (1, "a") in i, (1, "b") in i, (9, "a") in i, 1 in i, (1, "a", 2) in i, [1, "a"] in i, "a" in v, "z" in v))
show("contains unhashable", lambda: [] in k)
show("contains unhashable in items", lambda: ([], 1) in i)
show("mapping", lambda: (type(k.mapping).__name__, k.mapping == d, v.mapping[1], i.mapping is i.mapping, dict(k.mapping) is d))
show("mapping read only", lambda: setattr(k, "mapping", 1))
show("live", lambda: (lambda x: (lambda kk: (x.__setitem__(2, 2), list(kk), len(kk)))(x.keys()))({1: 1}))
for name, op in [("&", lambda a, b: a & b), ("|", lambda a, b: a | b), ("-", lambda a, b: a - b), ("^", lambda a, b: a ^ b)]:
    show("keys " + name + " keys", lambda: (s(op(k, e.keys())), type(op(k, e.keys())).__name__))
    show("items " + name + " items", lambda: s(op(i, e.items())))
    show("keys " + name + " set", lambda: s(op(k, {2, 9})))
    show("set " + name + " keys", lambda: (s(op({2, 9}, k)), type(op({2, 9}, k)).__name__))
    show("frozenset " + name + " keys", lambda: (s(op(frozenset({2, 9}), k)), type(op(frozenset({2, 9}), k)).__name__))
    show("keys " + name + " list", lambda: s(op(k, [2, 9, 9])))
    show("list " + name + " keys", lambda: s(op([2, 9, 9], k)))
    show("keys " + name + " str", lambda: s(op({"a": 1}.keys(), "ab")))
    show("keys " + name + " generator", lambda: s(op(k, (x for x in (2, 9)))))
    show("keys " + name + " dict", lambda: s(op(k, e)))
    show("keys " + name + " items", lambda: s(op(k, i)))
    show("keys " + name + " int", lambda: op(k, 1))
    show("int " + name + " keys", lambda: op(1, k))
    show("keys " + name + " unhashable", lambda: op(k, [[]]))
    show("values " + name + " values", lambda: op(v, v))
    show("keys " + name + " empty", lambda: s(op(k, ())))
    show("empty " + name + " keys", lambda: s(op({}.keys(), k)))
show("items with unhashable values", lambda: {1: []}.items() & {1: []}.items())
show("items with unhashable values or", lambda: {1: []}.items() | set())
show("items xor with unhashable values", lambda: {1: []}.items() ^ {1: []}.items())
show("items xor with unhashable values that differ", lambda: {1: []}.items() ^ {1: [1]}.items())
show("methods", lambda: (s(k.__and__({2})), s(k.__rand__({2})), s(k.__sub__({2})), s(k.__rsub__({2, 9})), s(k.__or__({9})), s(k.__ror__({9})), s(k.__xor__({2, 9})), s(k.__rxor__({2, 9}))))
show("in place", lambda: (lambda x: (x, type(x).__name__))(inplace()))
def inplace():
    x = d.keys(); x |= {9}; return s(x)
show("in place", inplace)
for name, op in [("==", lambda a, b: a == b), ("!=", lambda a, b: a != b), ("<", lambda a, b: a < b), ("<=", lambda a, b: a <= b), (">", lambda a, b: a > b), (">=", lambda a, b: a >= b)]:
    show("compare " + name, lambda: [op(k, o) for o in (d.keys(), {1, 2, 3}, {1, 2}, {1, 2, 3, 4}, frozenset({1, 2, 3}), {7, 8, 9}, e.keys(), i)])
    show("compare reflected " + name, lambda: [op(o, k) for o in ({1, 2, 3}, {1, 2}, {1, 2, 3, 4}, frozenset({1, 2, 3}))])
    show("compare items " + name, lambda: [op(i, o) for o in (d.items(), {(1, "a"), (2, "b"), (3, "c")}, {(1, "a")}, e.items())])
    show("compare with a list " + name, lambda: op(k, [1, 2, 3]))
    show("compare values " + name, lambda: op(v, d.values()))
show("compare methods", lambda: (k.__eq__({1, 2, 3}), k.__eq__([1, 2, 3]), k.__lt__({1, 2, 3, 4}), k.__ge__(1), v.__eq__(v), v.__eq__(d.values())))
show("isdisjoint", lambda: [k.isdisjoint(o) for o in ({9}, {1}, [9], [1], (), "ab", k, e.keys(), {}.keys(), (x for x in (9,)), {9: 1})])
show("isdisjoint items", lambda: [i.isdisjoint(o) for o in ({(1, "a")}, {(1, "z")}, [(1, "a")], i, e.items())])
show("isdisjoint of empty with itself", lambda: (lambda x: x.isdisjoint(x))({}.keys()))
show("isdisjoint wrong", lambda: k.isdisjoint(1))
show("isdisjoint nothing", lambda: k.isdisjoint())
show("values has none", lambda: v.isdisjoint)
show("hash", lambda: [attempt(lambda: hash(x)) is not None for x in (k, i)] + [type(hash(v)).__name__])
def attempt(f):
    try: return f()
    except Exception as ex: return type(ex).__name__ + ": " + str(ex)
show("hash", lambda: [attempt(lambda: hash(x)) for x in (k, i)] + [type(hash(v)).__name__])
show("cannot be made", lambda: [attempt(lambda: type(x)()) for x in (k, v, i)])
show("cannot be derived from", lambda: [attempt(lambda: type("X", (type(x),), {})) for x in (k, v, i)])
class D(dict):
    def __iter__(s): return iter(["overridden"])
    def __contains__(s, x): return "overridden"
show("of a derived class", lambda: (list(D(a=1).keys()), "a" in D(a=1).keys(), s(D(a=1).keys() | {1}), s(D(a=1).keys() & {"a"})))
class H:
    def __init__(s, n): s.n = n
    def __hash__(s): log.append("hash " + str(s.n)); return s.n
    def __eq__(s, o): log.append("eq " + str(s.n)); return s.n == getattr(o, "n", None)
log = []

# ---- mappingproxy
mp = type(int.__dict__)
p = mp(d)
show("proxy", lambda: (repr(p), str(p), len(p), p[1], 1 in p, 9 in p, list(p), p.get(1), p.get(9), p.get(9, "z"), list(p.keys()), list(p.values()), list(p.items()), p.copy(), type(p.copy()).__name__))
show("proxy reversed", lambda: list(reversed(p)))
show("proxy missing", lambda: p[9])
show("proxy set", lambda: p.__setitem__)
show("proxy assign", lambda: exec("p[1] = 2"))
show("proxy delete", lambda: exec("del p[1]"))
show("proxy or", lambda: (p | {9: 9}, {9: 9} | p, p | p, p | mp({8: 8}), type(p | p).__name__))
show("proxy or wrong", lambda: p | 1)
show("proxy or wrong reflected", lambda: 1 | p)
show("proxy or list", lambda: p | [(1, 2)])
show("proxy ior", lambda: exec("q = p\nq |= {1: 1}"))
show("proxy ior method", lambda: p.__ior__({}))
show("proxy or methods", lambda: (p.__or__({9: 9}), p.__ror__({9: 9}), p.__or__(1)))
for name, op in [("==", lambda a, b: a == b), ("!=", lambda a, b: a != b), ("<", lambda a, b: a < b), (">=", lambda a, b: a >= b)]:
    show("proxy compare " + name, lambda: [op(p, o) for o in (d, dict(d), mp(d), mp(dict(d)), {}, 1)])
    show("proxy compare reflected " + name, lambda: [op(o, p) for o in (d, {}, 1)])
show("proxy compare methods", lambda: (p.__eq__(d), p.__eq__(1), p.__ne__(d), p.__lt__(d), p.__eq__(mp(d))))
show("proxy hash", lambda: hash(p))
class HM:
    def __getitem__(s, k): return k
    def __hash__(s): return 42
    def __len__(s): return 7
    def __repr__(s): return "<HM>"
    def __str__(s): return "HM as str"
    def get(s, k, default): return ("got", k, default)
show("proxy of anything", lambda: (hash(mp(HM())), len(mp(HM())), mp(HM())["x"], repr(mp(HM())), str(mp(HM())), mp(HM()).get(1), mp(HM()).get(1, 2)))
show("proxy of anything lacks", lambda: mp(HM()).keys())
show("proxy of anything reversed", lambda: reversed(mp(HM())))
show("proxy of anything iter", lambda: next(iter(mp(HM()))))
show("proxy of anything contains", lambda: 3 in mp(HM()))
for bad in ([], (), 1, "s", None, {1}):
    show("proxy of " + repr(bad), lambda: mp(bad))
show("proxy of nothing", lambda: mp())
show("proxy get wrong", lambda: p.get())
show("proxy get wrong 2", lambda: p.get(1, 2, 3))
show("proxy live", lambda: (lambda x: (lambda q: (x.__setitem__(2, 2), dict(q)))(mp(x)))({1: 1}))
show("proxy of a class", lambda: (type(int.__dict__).__name__, "real" in int.__dict__, type(vars(H)).__name__, sorted(vars(H))[:3], vars(H) == vars(H), isinstance(vars(H) | {}, dict)))
show("proxy generic", lambda: mp[int, str])
show("proxy cannot be derived from", lambda: type("X", (mp,), {}))
show("proxy attributes", lambda: setattr(p, "x", 1))
