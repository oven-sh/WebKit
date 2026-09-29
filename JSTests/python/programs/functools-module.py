# The module _functools, which functools takes what it can from: partial, Placeholder, reduce, cmp_to_key, and what lru_cache makes.
import _functools
import collections
import copy
import functools
import inspect
import pickle
import weakref


def show(e):
    return type(e).__name__ + ": " + str(e)


def attempt(f, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return show(e)


def t(label, f):
    r = attempt(f)
    print(label, "=>", r if isinstance(r, str) else ascii(r))


def capture(*a, **k):
    return a, k


print("---- what there is")
names = sorted(n for n in vars(_functools) if not n.startswith("__"))
t("the module", lambda: (_functools.__name__, _functools.__package__, _functools.__loader__.__name__, _functools.__doc__, names))
for name in ("reduce", "cmp_to_key"):
    x = getattr(_functools, name)
    t(name, lambda: (type(x).__name__, x.__text_signature__, x.__doc__, x.__module__, x.__qualname__))
t("functools takes them", lambda: [(n, getattr(functools, n, None) is getattr(_functools, n)) for n in names])
K = type(functools.cmp_to_key(len))
for T in (_functools.partial, _functools._lru_cache_wrapper, _functools._PlaceholderType, K):
    t(T.__name__, lambda: (T.__name__, T.__module__, T.__qualname__, repr(T), [b.__name__ for b in T.__mro__], [(n, type(v).__name__) for n, v in sorted(vars(T).items())], T.__doc__, T.__basicsize__, T.__flags__ & 0xFFFFF, T.__dictoffset__ != 0, T.__weakrefoffset__ != 0, T.__text_signature__, attempt(setattr, T, "x", 1), attempt(lambda: type("S", (T,), {}).__name__)))

print("---- Placeholder")
P = functools.Placeholder
t("Placeholder", lambda: (repr(P), str(P), type(P).__name__, type(P)() is P, attempt(type(P), 1), attempt(type(P), x=1), P.__reduce__(), copy.copy(P) is P, copy.deepcopy(P) is P, [pickle.loads(pickle.dumps(P, p)) is P for p in range(pickle.HIGHEST_PROTOCOL + 1)], bool(P), attempt(setattr, P, "x", 1), attempt(hash, P) != 0, P == P, attempt(weakref.ref, P), attempt(type(P).__new__, type(P)) is P, attempt(object.__new__, type(P))))

print("---- partial")
partial = functools.partial
t("what it calls with", lambda: (partial(capture)(), partial(capture, 1)(), partial(capture, 1)(2), partial(capture, 1, 2)(3, 4), partial(capture, a=1)(), partial(capture, a=1)(b=2), partial(capture, a=1)(a=2), partial(capture, 1, a=1)(2, b=2), partial(capture, *range(20))(*range(3)), partial(capture, **{"k%d" % i: i for i in range(12)})(z=1)))
t("what it has", lambda: [(p.func is capture, p.args, p.keywords, p.__dict__, type(p.args).__name__, type(p.keywords).__name__) for p in (partial(capture), partial(capture, 1, 2), partial(capture, a=1), partial(capture, 1, a=2))])
t("which cannot be set", lambda: [attempt(setattr, partial(capture), n, v) for n, v in (("func", len), ("args", ()), ("keywords", {}))] + [attempt(delattr, partial(capture), n) for n in ("func", "args", "keywords", "__dict__")])
t("and what can", lambda: [(setattr(p, "x", 1), p.x, p.__dict__, setattr(p, "__dict__", {"y": 2}), p.y, attempt(getattr, p, "x"), attempt(setattr, p, "__dict__", 5), attempt(setattr, p, "__doc__", "d"), p.__dict__) for p in [partial(capture)]])
t("how it is made", lambda: (attempt(partial), attempt(partial, 5), attempt(partial, None), attempt(partial, func=len), attempt(partial, "a", 1), type(partial(len)).__name__, type(partial(int)).__name__, type(partial(partial(len))).__name__, attempt(partial.__new__, partial), attempt(partial.__new__, partial, len)("ab"), attempt(partial.__new__, int, len)))
t("the keywords are its own", lambda: [(p.keywords is d, d.update(b=2), p.keywords, p.keywords.update(c=3), p()) for d in [{"a": 1}] for p in [partial(capture, **d)]])
t("what is called can change what it is given", lambda: [(p(), p(), p.keywords) for p in [partial(lambda **k: (k.update(x=1), sorted(k))[1], a=1)]])
t("of a partial", lambda: [(q.func is capture, q.args, q.keywords, q(5, c=6)) for p in [partial(capture, 1, a=2)] for q in (partial(p), partial(p, 3), partial(p, b=4), partial(p, 3, a=9))])
t("of a partial that has been given something else", lambda: [(setattr(p, "x", 1), q.func is p, q.args, q.keywords, q(5))[1:] for p in [partial(capture, 1)] for _ in [setattr(p, "x", 1)] for q in [partial(p, 2)]])


class Sub(partial):
    pass


t("of a class derived from it", lambda: [(type(q).__name__, q.func is capture, q.args) for q in (Sub(capture, 1), Sub(partial(capture, 1), 2), partial(Sub(capture, 1), 2), Sub(Sub(capture, 1), 2))])
t("Placeholder in it", lambda: (partial(capture, P, 2)(1), partial(capture, P, 2)(1, 3), partial(capture, P, P, 3)(1, 2), partial(capture, 1, P, 3)(2, 4), partial(capture, P, 2, a=1)(1, b=2), attempt(partial(capture, P, 2)), attempt(partial(capture, P, P, 3), 1), attempt(partial(capture, P, 2), a=1), attempt(partial, capture, P), attempt(partial, capture, 1, P), attempt(partial, capture, P, P), attempt(partial, capture, a=P), attempt(partial, capture, 1, a=P)))
t("Placeholder in a partial of a partial", lambda: [(q.args, attempt(q, "x", "y", "z")) for p in [partial(capture, P, 2, P, 4)] for q in (partial(p), partial(p, 1), partial(p, 1, 3), partial(p, 1, 3, 5), partial(p, P, 3), partial(p, 1, P, 5), partial(p, P, P, 5))])
t("how it is shown", lambda: (repr(partial(len)), repr(partial(len, 1)), repr(partial(len, 1, "a")), repr(partial(len, a=1)), repr(partial(len, 1, a=2, b="c")), repr(partial(partial(len, 1), 2)), repr(Sub(len, 1)), repr(partial(len, P, 1)), str(partial(len))))
t("shown when it is in itself", lambda: [(p.__setstate__((p, (), {}, {})), repr(p))[1] for p in [partial(len)]] + [(p.__setstate__((len, (p,), {}, {})), repr(p))[1] for p in [partial(len)]] + [(p.__setstate__((len, (), {"a": p}, {})), repr(p))[1] for p in [partial(len)]])
t("shown when something will not be", lambda: [attempt(repr, p) for r in [type("R", (), {"__repr__": lambda s: 1 / 0, "__call__": lambda s: 1})()] for p in (partial(r), partial(len, r), partial(len, a=r))])
t("keywords that are not strs", lambda: [(p.__setstate__((len, (), {1: 2}, None)), repr(p), attempt(p))[1:] for p in [partial(capture)]])
t("reduced", lambda: [(r[0].__name__, r[1][0] is capture, r[2][0] is capture, r[2][1:]) for p in (partial(capture), partial(capture, 1, a=2), Sub(capture, 1)) for r in [p.__reduce__()]] + [[(setattr(p, "x", 1), p.__reduce__()[2][3])[1] for p in [partial(capture)]]])
t("__setstate__", lambda: [(attempt(p.__setstate__, s), p.func is capture or p.func.__name__, p.args, p.keywords, p.__dict__) for s in ((len, (1,), {"a": 2}, {"x": 3}), (len, (1,), None, None), (len, (), {}, {}), (len, [1], {}, {}), (5, (), {}, {}), (len, (), [], {}), (len, (), {}, []), (len, (), {}), (len, (), {}, {}, 1), [len, (), {}, {}], None, 5, (), (len, (P,), {}, {}), (len, (P, 1), {}, {}),
                                                                                                                                (len, type("T", (tuple,), {})((1,)), type("D", (dict,), {})(a=1), type("D", (dict,), {})(x=1))) for p in [partial(capture)]])
t("what __setstate__ keeps", lambda: [(p.__setstate__((len, a, k, d)), p.args is a, p.keywords is k, p.__dict__ is d, type(p.args).__name__, type(p.keywords).__name__)[1:] for a, k, d in (((1,), {"a": 1}, {"x": 1}), (type("T", (tuple,), {})((1,)), type("D", (dict,), {})(a=1), type("D", (dict,), {})(x=1))) for p in [partial(capture)]])
t("copied and pickled", lambda: [(repr(copy.copy(p)), repr(copy.deepcopy(p)), [repr(pickle.loads(pickle.dumps(p, n))) for n in range(pickle.HIGHEST_PROTOCOL + 1)], copy.copy(p).args is p.args, copy.deepcopy(p).args == p.args) for p in (partial(len), partial(len, [1], a=[2]), partial(len, P, 1))])
t("as an attribute of a class", lambda: [(type(vars(C)["m"]).__name__, type(C.m).__name__, type(C().m).__name__, C.m(1), C().m(1)[0][0], C().m(1)[0][2:], type(C().m(1)[0][1]).__name__, C().m.__self__.__class__.__name__, C().m.__func__ is vars(C)["m"]) for C in [type("C", (), {"m": partial(capture, 0)})]])
t("__get__", lambda: [(p.__get__(None, int) is p, p.__get__(None) is p, type(p.__get__(5)).__name__, p.__get__(5)(), attempt(p.__get__), attempt(p.__get__, None, None), attempt(p.__get__, 1, 2, 3)) for p in [partial(capture, 0)]])
t("what else", lambda: (callable(partial(len)), attempt(hash, partial(len)) != 0, partial(len) == partial(len), weakref.ref(p := partial(len))() is p, partial[int], attempt(lambda: partial(len)[int]), str(inspect.signature(partial(lambda a, b, c=1: 0, 1))), partial(len).__class__.__name__, attempt(getattr, partial(len), "__name__"), attempt(getattr, partial(len), "__wrapped__"), partial(len).__doc__ == partial.__doc__))
t("what it calls raises", lambda: (attempt(partial(len)), attempt(partial(len, 5)), attempt(partial(int, "a")), attempt(partial(lambda: 1 / 0)), attempt(partial(capture, a=1), **{"a": 2}), attempt(partial(lambda a: a, a=1), 2)))
t("very many deep", lambda: (lambda p: (p.func is capture, len(p.args)))(functools.reduce(lambda p, i: partial(p, i), range(2000), partial(capture))))

print("---- reduce")
reduce = functools.reduce
t("reduce", lambda: (reduce(lambda a, b: a + b, [1, 2, 3]), reduce(lambda a, b: a + b, [1, 2, 3], 10), reduce(lambda a, b: a + b, [], 10), reduce(lambda a, b: a + b, [5]), reduce(lambda a, b: (a, b), "abc"), reduce(lambda a, b: (a, b), iter("abc"), None), reduce(lambda a, b: a + b, [1, 2], initial=10), reduce(capture, [1, 2, 3]), reduce(lambda a, b: a * b, range(1, 6)), reduce(lambda a, b: a + b, {1: 0, 2: 0}), reduce(max, [3, 1, 4])))
t("what goes wrong", lambda: (attempt(reduce), attempt(reduce, len), attempt(reduce, len, [], 1, 2), attempt(reduce, lambda a, b: a, []), attempt(reduce, lambda a, b: a, 5), attempt(reduce, lambda a, b: a, None), attempt(reduce, 5, [1, 2]), attempt(reduce, 5, [1]), attempt(reduce, 5, []), attempt(reduce, lambda a, b: 1 / 0, [1, 2]), attempt(reduce, lambda a: a, [1, 2]), attempt(reduce, function=len, iterable=[]), attempt(reduce, len, iterable=[]), attempt(reduce, len, [], x=1),
                                  attempt(reduce, lambda a, b: a, type("I", (), {"__iter__": lambda s: 1 / 0})()), attempt(reduce, lambda a, b: a, type("I", (), {"__iter__": lambda s: s, "__next__": lambda s: 1 / 0})()), attempt(reduce, lambda a, b: a, type("I", (), {"__iter__": lambda s: 5})()), attempt(reduce, lambda a, b: a, type("I", (), {"__getitem__": lambda s, i: [1, 2][i]})())))
t("the arguments are not kept from one time to the next", lambda: [(reduce(lambda *a: (kept.append(a), a[0] + a[1])[1], [1, 2, 3, 4]), kept) for kept in [[]]])

print("---- cmp_to_key")
ck = functools.cmp_to_key


def cmp(a, b):
    calls.append((a, b))
    return (a > b) - (a < b)


calls = []
key = ck(cmp)
t("what it makes", lambda: (type(key).__name__, type(key(1)).__name__, type(key) is type(key(1)), key(1).obj, attempt(getattr, key, "obj"), repr(key).split(" at ")[0], repr(key(1)).split(" at ")[0], key.__text_signature__, key(1).__text_signature__, callable(key(1)), type(key(1)(2)).__name__, key(1)(2).obj))
t("compared", lambda: [(calls.clear(), [attempt(op, key(a), key(b)) for op in (lambda x, y: x < y, lambda x, y: x <= y, lambda x, y: x == y, lambda x, y: x != y, lambda x, y: x > y, lambda x, y: x >= y)], calls[:])[1:] for a, b in ((1, 2), (2, 1), (1, 1))])
t("with what is not one", lambda: [attempt(op, key(1), other) for other in (1, None, "a", object()) for op in (lambda x, y: x < y, lambda x, y: x == y, lambda x, y: y < x, lambda x, y: y == x, lambda x, y: x != y)])
t("with one that has nothing in it", lambda: (attempt(lambda: key < key(1)), attempt(lambda: key(1) < key), attempt(lambda: key == key)))
t("with one of another function", lambda: (ck(lambda a, b: -1)(1) < ck(lambda a, b: 1)(1), ck(lambda a, b: 1)(1) < ck(lambda a, b: -1)(1)))
t("what the function can give", lambda: [[attempt(op, ck(lambda a, b: r)(1), ck(lambda a, b: r)(2)) for op in (lambda x, y: x < y, lambda x, y: x == y, lambda x, y: x > y)] for r in (-1, 0, 1, -5.5, 0.0, True, False, None, "a", 2 ** 70, [], 1j)])
t("what the function does", lambda: (attempt(lambda: ck(lambda a, b: 1 / 0)(1) < ck(len)(2)), attempt(lambda: ck(lambda a: 0)(1) < ck(len)(2)), attempt(lambda: ck(5)(1) < ck(5)(2)), attempt(lambda: ck(None)(1) == ck(None)(2))))
t("how it is made and called", lambda: (attempt(ck), attempt(ck, 1, 2), type(ck(mycmp=cmp)).__name__, attempt(ck, cmp=cmp), attempt(key), attempt(key, 1, 2), key(obj=5).obj, attempt(key, x=5), attempt(key, 1, obj=2), attempt(K), attempt(K, cmp)))
t("what it has", lambda: (attempt(hash, key(1)), attempt(setattr, key(1), "obj", 5), attempt(setattr, key(1), "x", 5), attempt(delattr, key(1), "obj"), attempt(getattr, key(1), "__dict__"), attempt(getattr, key(1), "cmp"), attempt(weakref.ref, key(1)), attempt(copy.copy, key(1)), attempt(pickle.dumps, key(1)), K.__hash__))
t("obj can be set", lambda: [(setattr(k, "obj", 9), k.obj, k < key(10), delattr(k, "obj"), attempt(getattr, k, "obj"), attempt(lambda: k < key(10))) for k in [key(1)]])
t("sorting by it", lambda: (sorted([3, 1, 2], key=ck(lambda a, b: a - b)), sorted([3, 1, 2], key=ck(lambda a, b: b - a)), sorted("bca", key=ck(lambda a, b: (a > b) - (a < b))), max([3, 1, 2], key=ck(lambda a, b: b - a)), attempt(sorted, [3, 1, 2], key=ck(lambda a, b: 1 / 0))))

print("---- what lru_cache makes")
Info = collections.namedtuple("Info", "hits misses maxsize currsize")
W = _functools._lru_cache_wrapper
t("how it is made", lambda: (attempt(W), attempt(W, len), attempt(W, len, 1), attempt(W, len, 1, False), type(W(len, 1, False, Info)).__name__, attempt(W, 5, 1, False, Info), attempt(W, len, "a", False, Info), attempt(W, len, 1.5, False, Info), attempt(W, len, 2 ** 70, False, Info), type(W(len, None, False, Info)).__name__, W(len, -5, False, Info).cache_info(), W(len, True, [], None).__class__.__name__, type(W(user_function=len, maxsize=1, typed=0, cache_info_type=Info)).__name__, attempt(W, len, 1, False, Info, 5), attempt(W, len, 1, False, Info, x=5),
                             W(len, type("I", (), {"__index__": lambda s: 3})(), False, Info).cache_info(), attempt(W, len, 1, type("B", (), {"__bool__": lambda s: 1 / 0})(), Info)))


def counted(maxsize, typed=False):
    seen = []

    @functools.lru_cache(maxsize, typed)
    def f(*a, **k):
        seen.append((a, k))
        return len(seen)
    return f, seen


for maxsize in (None, 0, 1, 2, 3, 128):
    f, seen = counted(maxsize)
    t("maxsize %r" % (maxsize,), lambda: ([f(x) for x in (1, 2, 1, 3, 1, 2, 4, 1, 1, 5, 2)], tuple(f.cache_info()), len(seen), f.cache_parameters(), f.cache_clear(), tuple(f.cache_info()), f(1), f(1), tuple(f.cache_info())))
f, seen = counted(8)
t("what counts as the same", lambda: ([f(*a, **k) for a, k in (((1,), {}), ((1.0,), {}), ((True,), {}), (("1",), {}), (((1,),), {}), ((), {"a": 1}), ((), {"a": 1.0}), ((1,), {"a": 1}), ((), {"a": 1, "b": 2}), ((), {"b": 2, "a": 1}), ((), {}), ((None,), {}), ((1, 2), {}), (((1, 2),), {}))], tuple(f.cache_info())))
f, seen = counted(32, True)
t("and when it goes by the class as well", lambda: ([f(*a, **k) for a, k in (((1,), {}), ((1.0,), {}), ((True,), {}), ((1,), {}), ((), {"a": 1}), ((), {"a": 1.0}), ((), {"a": 1}), (((1,),), {}), (((1.0,),), {}))], tuple(f.cache_info())))
t("what cannot be hashed", lambda: [(attempt(f, []), attempt(f, a=[]), attempt(f, {}), attempt(f, (1, [])), tuple(f.cache_info())) for m in (None, 0, 2) for f, _ in [counted(m)]])


class H:
    def __init__(self, v): self.v = v
    def __hash__(self):
        log.append(("hash", self.v))
        return hash(self.v)
    def __eq__(self, other):
        log.append(("eq", self.v, other.v))
        return self.v == other.v


log = []
for maxsize in (None, 0, 1, 2):
    f, seen = counted(maxsize)
    log.clear()
    t("what is asked of the arguments, maxsize %r" % (maxsize,), lambda: ([f(H(x)) for x in (1, 1, 2, 3, 1)], log[:], tuple(f.cache_info())))
t("what raises is not kept", lambda: [(attempt(f, 0), attempt(f, 0), f(1), f(1), tuple(f.cache_info())) for m in (None, 0, 2) for f in [functools.lru_cache(m)(lambda x: 1 / x)]])
t("what calls itself", lambda: [(fib(30), tuple(fib.cache_info())) for m in (None, 2, 3, 128) for fib in [functools.lru_cache(m)(lambda n: n if n < 2 else fib(n - 1) + fib(n - 2))]])
t("what clears it meanwhile", lambda: [(f(1), f(2), f(1), tuple(f.cache_info())) for m in (None, 1, 2) for f in [functools.lru_cache(m)(lambda x: (f.cache_clear(), x)[1])]])
t("what fills it meanwhile", lambda: [(f(3), tuple(f.cache_info()), f(3), f(0), tuple(f.cache_info())) for m in (None, 1, 2, 3) for f in [functools.lru_cache(m)(lambda x: x and (f(x - 1), f(x - 1), x)[2])]])
t("what puts the same thing there meanwhile", lambda: [(f(1, True), tuple(f.cache_info())) for m in (None, 1, 2) for f in [functools.lru_cache(m)(lambda x, again=False: (again and f(x, True) if False else 0, x)[1])]])


@functools.lru_cache(2)
def documented(a, b=1):
    "the doc"
    return a


t("what it has", lambda: (type(documented).__name__, documented.__name__, documented.__qualname__, documented.__doc__, documented.__module__, documented.__wrapped__.__name__, sorted(documented.__dict__), repr(documented).split(" at ")[0], str(inspect.signature(documented)), callable(documented), attempt(hash, documented) != 0, weakref.ref(documented)() is documented, attempt(setattr, documented, "x", 1), documented.x))
t("copied and pickled", lambda: (copy.copy(documented) is documented, copy.deepcopy(documented) is documented, documented.__reduce__(), [pickle.loads(pickle.dumps(documented, p)) is documented for p in range(pickle.HIGHEST_PROTOCOL + 1)], attempt(documented.__copy__) is documented, attempt(documented.__copy__, 1, 2) is documented, attempt(documented.__deepcopy__) is documented, attempt(documented.__deepcopy__, {}) is documented, attempt(documented.__copy__, x=1), attempt(W(len, 1, False, Info).__reduce__)))
t("how its methods are called", lambda: (attempt(documented.cache_info, 1), attempt(documented.cache_clear, 1), attempt(documented.cache_info, x=1), attempt(W.cache_info), attempt(W.cache_info, 5), attempt(W.cache_clear, None)))
t("what cache_info is made by", lambda: (W(len, 2, False, capture).cache_info(), W(len, None, False, capture).cache_info(), attempt(W(len, 2, False, None).cache_info), attempt(W(len, 2, False, lambda: 0).cache_info), attempt(W(len, 2, False, lambda *a: 1 / 0).cache_info)))


class WithCached:
    @functools.lru_cache(4)
    def m(self, x): return (type(self).__name__, x)

    @functools.cache
    def n(self): return "n"

    @staticmethod
    @functools.lru_cache(4)
    def s(x): return x

    @classmethod
    @functools.lru_cache(4)
    def c(cls, x): return (cls.__name__, x)


t("as an attribute of a class", lambda: (WithCached().m(1), type(WithCached.m).__name__, type(WithCached().m).__name__, WithCached().n(), WithCached.s(2), WithCached().s(2), WithCached.c(3), WithCached().c(3), tuple(WithCached.m.cache_info())[:2], WithCached().m.__func__ is vars(WithCached)["m"], attempt(WithCached.m), attempt(WithCached.m, 1)))
t("__get__", lambda: (documented.__get__(None, int) is documented, type(documented.__get__(5)).__name__, documented.__get__(5)(), attempt(documented.__get__), attempt(documented.__get__, None, None)))
t("a class derived from it", lambda: [(type(w).__name__, w("ab"), w("ab"), tuple(w.cache_info()), w.__dict__) for S in [type("S", (W,), {})] for w in [S(len, 2, False, Info)]])
t("a great many", lambda: [(sum(f(i % 700) for i in range(5000)), tuple(f.cache_info())) for m in (None, 1, 100, 699, 700, 701) for f in [functools.lru_cache(m)(lambda x: x)]])

print("---- what is written over them in functools")
t("partialmethod", lambda: [(C().m(2), C().k(2), C.m(C(), 2)) for C in [type("C", (), {"f": lambda self, *a, **k: (a, k), "m": functools.partialmethod(lambda self, *a, **k: (a, k), 1), "k": functools.partialmethod(lambda self, *a, **k: (a, k), x=1)})]])
t("wraps", lambda: [(g.__name__, g.__doc__, g.__wrapped__ is documented.__wrapped__) for g in [functools.wraps(documented.__wrapped__)(lambda *a: 0)]])
t("total_ordering", lambda: [(C(1) < C(2), C(1) <= C(1), C(2) > C(1), C(2) >= C(3)) for C in [functools.total_ordering(type("C", (), {"__init__": lambda s, v: setattr(s, "v", v), "__eq__": lambda s, o: s.v == o.v, "__lt__": lambda s, o: s.v < o.v}))]])
t("singledispatch", lambda: [(f(1), f("a"), f(1.5), f([1]), f(True)) for f in [functools.singledispatch(lambda x: "any")] for _ in [(f.register(int, lambda x: "int"), f.register(str, lambda x: "str"), f.register(list, lambda x: "list"))]])
t("cached_property", lambda: [(c.p, c.p, c.n, vars(c)) for C in [type("C", (), {"n": 0, "p": functools.cached_property(lambda s: (setattr(s, "n", s.n + 1), s.n)[1])})] for _ in [C.p.__set_name__(C, "p")] for c in [C()]])
