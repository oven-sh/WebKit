# The module _operator, which operator takes everything from that it can.
import _operator
import collections
import copy
import glob
import operator
import pickle
import re
import warnings

warnings.simplefilter("ignore")


def show(e):
    return type(e).__name__ + ": " + str(e)


def attempt(f, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return show(e)


def t(label, f):
    r = attempt(f)
    # Where things are in memory is not part of it.
    print(label, "=>", re.sub(r" at 0x[0-9a-f]+", " at 0x", r if isinstance(r, str) else ascii(r)))


print("---- what there is")
names = sorted(n for n in vars(_operator) if not n.startswith("__"))
t("the module", lambda: (_operator.__name__, _operator.__package__, _operator.__loader__.__name__, _operator.__doc__, names))
for name in names:
    x = getattr(_operator, name)
    t(name, lambda: (type(x).__name__, x.__text_signature__, x.__doc__, x.__module__, x.__name__, x.__qualname__, getattr(x, "__self__", None) is _operator))
t("operator takes them", lambda: [n for n in names if getattr(operator, n, None) is not getattr(_operator, n)])
t("and calls them other things too", lambda: [(n, getattr(operator, n).__name__) for n in sorted(vars(operator)) if n.startswith("__") and n != "__loader__" and callable(getattr(operator, n)) and hasattr(getattr(operator, n), "__name__")])
for T in (_operator.itemgetter, _operator.attrgetter, _operator.methodcaller):
    t(T.__name__, lambda: (T.__name__, T.__module__, T.__qualname__, repr(T), [b.__name__ for b in T.__mro__], sorted(vars(T)), [(n, type(v).__name__) for n, v in sorted(vars(T).items())], T.__basicsize__, T.__flags__ & 0xFFFFF, T.__text_signature__, attempt(setattr, T, "x", 1), attempt(lambda: type("S", (T,), {}))))

print("---- it is not a method of what it is an attribute of")


class Holder:
    add = operator.add
    get = operator.itemgetter(0)
    attr = operator.attrgetter("x")
    call = operator.methodcaller("upper")
    x = 5


t("as an attribute of a class", lambda: (Holder().add(1, 2), Holder.add(1, 2), Holder().get("ab"), attempt(Holder().attr, Holder()), Holder().call("ab")))
t("which glob goes by", lambda: (glob._StringGlobber.concat_path is operator.add, type(glob._StringGlobber("/", False).concat_path).__name__))

print("---- what stands for an operator")


class N:
    "It says what is asked of it"
    def __init__(self, name="n"): self.name = name
    def __repr__(self): return self.name


for method in ("add radd iadd sub rsub isub mul rmul imul matmul rmatmul imatmul truediv rtruediv itruediv floordiv rfloordiv ifloordiv mod rmod imod pow rpow ipow lshift rlshift ilshift rshift rrshift irshift "
               "and rand iand or ror ior xor rxor ixor lt le eq ne gt ge getitem contains").split():
    setattr(N, "__%s__" % method, (lambda method: lambda self, *a: (method, self.name) + a)(method))
for method in "neg pos abs invert".split():
    setattr(N, "__%s__" % method, (lambda method: lambda self: (method, self.name))(method))
TWO = "add sub mul matmul truediv floordiv mod pow lshift rshift and_ or_ xor iadd isub imul imatmul itruediv ifloordiv imod ipow ilshift irshift iand ior ixor lt le eq ne gt ge getitem concat iconcat".split()
for name in TWO:
    f = getattr(operator, name)
    t(name, lambda: (f(N("a"), N("b")), attempt(f, N("a"), 5), attempt(f, 5, N("b")), attempt(f, 7, 2), attempt(f, 7.5, 2), attempt(f, "ab", "c"), attempt(f, [1], [2]), attempt(f, (1,), (2,)), attempt(f, [1], 2), attempt(f, 2, [1]), attempt(f, {1}, {2}), attempt(f, None, None), attempt(f, object(), 1).replace("object", "object")))
    t(name + ", how it is called", lambda: (attempt(f), attempt(f, 1), attempt(f, 1, 2, 3), attempt(f, a=1, b=2), attempt(f, 1, b=2)))
for name in "neg pos abs inv invert not_ truth index is_none is_not_none".split():
    f = getattr(operator, name)
    t(name, lambda: [attempt(f, v) for v in (N("a"), 5, -5, 0, 2 ** 70, 1.5, -0.0, True, None, "a", "", [], [0], 1j, object)])
    t(name + ", how it is called", lambda: (attempt(f), attempt(f, 1, 2), attempt(f, a=1)))
t("is_ and is_not", lambda: [(operator.is_(a, b), operator.is_not(a, b)) for a, b in ((None, None), (1, 1), (1, 1.0), ([], []), ("a", "a"), (True, 1), (N, N))])
t("in place, what is changed and what is not", lambda: [(f.__name__, r is a, a, r) for f, a, b in ((operator.iadd, [1], [2]), (operator.iadd, (1,), (2,)), (operator.imul, [1], 2), (operator.ior, {1}, {2}), (operator.iconcat, [1], [2]), (operator.iconcat, [1], (2,)), (operator.iand, {1, 2}, {2}), (operator.iadd, bytearray(b"a"), b"b"), (operator.ior, {1: 1}, {2: 2})) for r in [f(a, b)]])

print("---- index")


class Ix:
    def __init__(self, v): self.v = v
    def __index__(self): return self.v


class IntSub(int):
    def __index__(self): return int(self) + 1


class IntSub2(int):
    pass


with warnings.catch_warnings(record=True) as caught:
    warnings.simplefilter("always")
    t("index", lambda: [(attempt(operator.index, v), type(attempt(operator.index, v)).__name__) for v in (Ix(5), Ix(2 ** 70), Ix(True), Ix(IntSub2(6)), Ix(1.5), Ix("a"), Ix(None), IntSub(7), IntSub2(8), True, 1.5, "a", None)])
    t("what it warns of", lambda: sorted({(w.category.__name__, str(w.message)) for w in caught}))

print("---- sequences")


class Seq:
    def __init__(self, *v): self.v = v
    def __getitem__(self, i): return self.v[i]
    def __len__(self): return len(self.v)


class SeqAdd(Seq):
    def __add__(self, other): return ("add", self.v, getattr(other, "v", other))
    def __radd__(self, other): return ("radd", self.v, getattr(other, "v", other))
    def __iadd__(self, other): return ("iadd", self.v, getattr(other, "v", other))


class ListAdd(list):
    def __add__(self, other): return "ListAdd.__add__"


class ListIAdd(list):
    def __iadd__(self, other): return "ListIAdd.__iadd__"


things = {"list": [1], "tuple": (1,), "str": "a", "bytes": b"a", "bytearray": bytearray(b"a"), "deque": collections.deque([1]), "range": range(1), "dict": {1: 1}, "set": {1}, "int": 1, "None": None, "Seq": Seq(1), "SeqAdd": SeqAdd(1), "ListAdd": ListAdd([1]), "ListIAdd": ListIAdd([1]), "N": N("n"), "memoryview": memoryview(b"a")}
for name in ("concat", "iconcat"):
    f = getattr(operator, name)
    for a_name in things:
        t("%s of %s" % (name, a_name), lambda: [(b_name, attempt(f, copy.copy(things[a_name]) if a_name not in ("memoryview", "N", "Seq", "SeqAdd") else things[a_name], things[b_name])) for b_name in things if b_name not in ("N",)])


class Eq:
    def __init__(self, v): self.v = v
    def __eq__(self, other):
        log.append(self.v)
        return self.v == other


class Raises:
    def __eq__(self, other): raise KeyError("eq")


class NoIter:
    pass


class BadIter:
    def __iter__(self): return 5


class IterRaises:
    def __iter__(self): raise KeyError("iter")


class NextRaises:
    def __iter__(self): return self
    def __next__(self): raise KeyError("next")


class Contains:
    def __contains__(self, x): return x


nan = float("nan")
log = []
for name in ("contains", "countOf", "indexOf"):
    f = getattr(operator, name)
    t(name, lambda: [attempt(f, a, b) for a, b in (([1, 2, 1], 1), ([1, 2, 1], 3), ((1, 2), 2), ("abca", "a"), ("abca", "bc"), ("abc", 1), (b"aba", 97), (b"aba", b"a"), ({1: 2}, 1), ({1: 2}, 2), ({1, 2}, 2), (range(5), 3), (iter([1, 2, 3]), 2), (Seq(1, 2, 1), 1), ([nan], nan),
                                                   (NoIter(), 1), (BadIter(), 1), (IterRaises(), 1), (NextRaises(), 1), (5, 1), (None, None), ([Raises()], 1), ([1, Raises()], 1), (Contains(), 5), (Contains(), 0), (Contains(), []), ((x for x in (1, 2, 1)), 1), (collections.deque([1, 2, 1]), 1))])
    t(name + ", how it is called", lambda: (attempt(f), attempt(f, 1), attempt(f, [], 2, 3), attempt(f, a=[], b=2)))
    log.clear()
    t(name + ", which are asked", lambda: (f([Eq(1), Eq(2), Eq(3), Eq(2)], 2), log[:]))
t("getitem, setitem, delitem", lambda: [(attempt(operator.getitem, c, k), attempt(operator.setitem, c, k, "v"), c if not isinstance(c, str) else c, attempt(operator.delitem, c, k), c) for c, k in (([1, 2], 0), ([1, 2], 5), ([1, 2, 3], slice(0, 2)), ({1: 2}, 1), ({1: 2}, 3), ((1, 2), 0), ("ab", 0), (5, 0), (None, 0), (bytearray(b"ab"), 0))])
t("how they are called", lambda: (attempt(operator.setitem, [], 1), attempt(operator.setitem, [1], 0, 1, 2), attempt(operator.delitem, []), attempt(operator.setitem, a=[1], b=0, c=1)))

print("---- length_hint")


class Hint:
    def __init__(self, v): self.v = v
    def __length_hint__(self): return self.v() if callable(self.v) else self.v


class Len:
    def __init__(self, v): self.v = v
    def __len__(self): return self.v() if callable(self.v) else self.v


t("length_hint", lambda: [attempt(operator.length_hint, *a) for a in (([1, 2],), (iter([1, 2]),), (iter([]),), (5,), (5, 7), (None, 3), (Hint(4),), (Hint(NotImplemented),), (Hint(NotImplemented), 9), (Hint(-1),), (Hint("a"),), (Hint(1.5),), (Hint(2 ** 70),), (Hint(True),), (Hint(lambda: 1 / 0),), (Hint(lambda: [][0]),),
                                                                      (Hint(lambda: int("a")),), (Len(3),), (Len(-1),), (Len(lambda: 1 / 0),), (Len(lambda: len(5)), 8), (Hint(lambda: len(5)), 8), (5, "a"), (5, 1.5), (5, None), (5, -3), (5, 2 ** 70), (5, Ix(6)), (), (1, 2, 3), ((x for x in ()), 2), (range(10),), ({1: 1}.keys(),), (zip([1], [2]),), (map(len, []), 4), (reversed([1, 2, 3]),), (iter("abc"),), (iter(range(7)),), (iter({1, 2}),), (enumerate([1]), 5))])
t("how it is called", lambda: (attempt(operator.length_hint, obj=[]), attempt(operator.length_hint, [], default=1)))

print("---- _compare_digest")
t("_compare_digest", lambda: [attempt(_operator._compare_digest, a, b) for a, b in (("a", "a"), ("a", "b"), ("a", "ab"), ("", ""), ("", "a"), (b"a", b"a"), (b"a", b"b"), (b"", b""), (b"abc", bytearray(b"abc")), (bytearray(b"abc"), memoryview(b"abc")), (b"a", "a"), ("a", b"a"), ("\xe9", "\xe9"), ("a", "\xe9"), ("Ā", "a"),
                                                                                    (1, 1), (None, None), (1, b"a"), (b"a", 1), ("a", 1), (1, "a"), ([], []), (type("S", (str,), {})("a"), "a"), (type("B", (bytes,), {})(b"a"), b"a"), (memoryview(b"abcd").cast("B", (2, 2)), b"abcd"), (b"abcd", memoryview(b"abcd").cast("B", (2, 2))), (memoryview(b"abcd")[::2], b"ac"), (memoryview(b"abcd").cast("H"), b"abcd"), ("a" * 1000, "a" * 1000), ("a" * 1000, "a" * 999 + "b"))])
t("how it is called", lambda: (attempt(_operator._compare_digest), attempt(_operator._compare_digest, "a"), attempt(_operator._compare_digest, "a", "a", "a"), attempt(_operator._compare_digest, a="a", b="a")))

print("---- call")
t("call", lambda: (operator.call(len, "ab"), operator.call(dict, a=1), operator.call(lambda *a, **k: (a, k), 1, 2, x=3), operator.call(int), attempt(operator.call), attempt(operator.call, 5), attempt(operator.call, obj=len), attempt(operator.call, len), attempt(operator.call, len, x=1), operator.call(operator.call, operator.call, len, "abc")))

print("---- itemgetter")
ig = operator.itemgetter
t("what it gets", lambda: (ig(0)("ab"), ig(1)([1, 2]), ig(-1)((1, 2)), ig(0, 1)("ab"), ig(1, 0, 1)("ab"), ig("k")({"k": 5}), ig(slice(1, 3))("abcd"), ig(0)((5,)), ig(2 ** 70)({2 ** 70: 1}), ig(True)((1, 2)), ig(None)({None: 1}), ig((1, 2))({(1, 2): 3}), ig(Ix(1))("ab"), ig(0)(type("T", (tuple,), {"__getitem__": lambda s, i: "mine"})((1,)))))
t("what it does not", lambda: (attempt(ig(5), "ab"), attempt(ig(5), (1,)), attempt(ig("k"), {}), attempt(ig(0), 5), attempt(ig(0), None), attempt(ig(0, 5), "ab"), attempt(ig(-5), (1,)), attempt(ig(0), {})))
t("how it is made", lambda: (attempt(ig), attempt(ig, item=1), attempt(ig, 1, x=2), type(ig(1, 2, 3)).__name__, attempt(ig.__new__, ig), attempt(ig.__new__, ig, 0)("ab"), attempt(ig.__new__, int, 0)))
t("how it is called", lambda: (attempt(ig(0)), attempt(ig(0), "a", "b"), attempt(ig(0), obj="a"), attempt(ig(0), "a", x=1), attempt(ig(0).__call__, "ab"), attempt(ig(0).__call__), attempt(ig(0).__call__, obj="a")))
t("how it is shown", lambda: (repr(ig(0)), repr(ig(0, 1)), repr(ig("a")), repr(ig((1, 2))), repr(ig(slice(1, 2))), str(ig(0)), repr(ig(ig(0))), [(l.append(ig(l)), repr(l[0]))[1] for l in [[]]], attempt(repr, ig(type("R", (), {"__repr__": lambda s: 1 / 0})()))))
t("what it has", lambda: (ig(0).__text_signature__, attempt(getattr, ig(0), "__dict__"), attempt(setattr, ig(0), "x", 1), attempt(setattr, ig(0), "__text_signature__", "x"), attempt(hash, ig(0)) != 0, ig(0) == ig(0), type(attempt(getattr, ig(0), "__vectorcalloffset__")).__name__, attempt(getattr, ig(0), "item"), callable(ig(0)), attempt(__import__("weakref").ref, ig(0))))
t("reduced, copied and pickled", lambda: [(g.__reduce__()[0].__name__, g.__reduce__()[1:], repr(copy.copy(g)), repr(copy.deepcopy(g)), [repr(pickle.loads(pickle.dumps(g, p))) for p in range(pickle.HIGHEST_PROTOCOL + 1)]) for g in (ig(0), ig(0, 1), ig("a", "b", "c"), ig((1, 2)))])

print("---- attrgetter")
ag = operator.attrgetter


class O:
    pass


o = O()
o.a, o.b = 1, O()
o.b.c, o.b.d = 2, O()
o.b.d.e = 3
t("what it gets", lambda: (ag("a")(o), ag("b.c")(o), ag("b.d.e")(o), ag("a", "b.c")(o), ag("a", "a", "b.d.e")(o), ag("real")(5), ag("__class__.__name__")(5), ag(type("S", (str,), {})("a"))(o)))
t("what it does not", lambda: (attempt(ag("x"), o), attempt(ag("a.x"), o), attempt(ag("b.x.y"), o), attempt(ag("a", "x"), o), attempt(ag(""), o), attempt(ag("."), o), attempt(ag("a."), o), attempt(ag(".a"), o), attempt(ag("a..b"), o), attempt(ag("\ud800"), o), attempt(ag("a b"), o)))
t("how it is made", lambda: (attempt(ag), attempt(ag, 1), attempt(ag, "a", 1), attempt(ag, None), attempt(ag, b"a"), attempt(ag, attr="a"), attempt(ag, "a", x=1), attempt(ag, ("a", "b")), attempt(ag.__new__, ag), attempt(ag.__new__, ag, "a")(o)))
t("how it is called", lambda: (attempt(ag("a")), attempt(ag("a"), o, o), attempt(ag("a"), obj=o), attempt(ag("a").__call__, o), attempt(ag("a").__call__, obj=o)))
t("how it is shown", lambda: (repr(ag("a")), repr(ag("a", "b")), repr(ag("a.b")), repr(ag("a.b", "c.d.e")), repr(ag("")), repr(ag(".")), repr(ag("a'b")), repr(ag("\xe9Ā"))))
t("what it has", lambda: (ag("a").__text_signature__, attempt(getattr, ag("a"), "__dict__"), attempt(setattr, ag("a"), "x", 1), ag("a") == ag("a"), callable(ag("a"))))
t("reduced, copied and pickled", lambda: [(g.__reduce__()[0].__name__, g.__reduce__()[1:], repr(copy.copy(g)), repr(copy.deepcopy(g)), [repr(pickle.loads(pickle.dumps(g, p))) for p in range(pickle.HIGHEST_PROTOCOL + 1)]) for g in (ag("a"), ag("a", "b"), ag("a.b.c"), ag("a.b", "c"))])
t("what is asked of what, and in what order", lambda: [(ag("x.y", "z")(w), w.log) for w in [type("W", (), {"log": [], "__getattr__": lambda s, n: (s.log.append(n), s)[1]})()]][0][1])

print("---- methodcaller")
mc = operator.methodcaller


class M:
    def m(self, *a, **k): return (a, k)


t("what it calls", lambda: (mc("upper")("ab"), mc("split", "b")("abc"), mc("m")(M()), mc("m", 1, 2)(M()), mc("m", x=1)(M()), mc("m", 1, x=2, y=3)(M()), mc("m", *range(10))(M()), mc("m", *range(10), **{"k%d" % i: i for i in range(10)})(M()), mc("m", name="n", self="s")(M()), mc("encode", encoding="ascii")("a"), mc(type("S", (str,), {})("upper"))("a")))
t("what it does not", lambda: (attempt(mc("x"), "a"), attempt(mc("upper", 1), "a"), attempt(mc("upper", x=1), "a"), attempt(mc("real"), 5), attempt(mc(""), 5), attempt(mc("a.b"), 5)))
t("how it is made", lambda: (attempt(mc), attempt(mc, 1), attempt(mc, None), attempt(mc, b"a"), attempt(mc, name="a"), attempt(mc.__new__, mc), attempt(mc.__new__, mc, "upper")("a")))
t("how it is called", lambda: (attempt(mc("upper")), attempt(mc("upper"), "a", "b"), attempt(mc("upper"), obj="a"), attempt(mc("upper"), "a", x=1), attempt(mc("upper").__call__, "a"), attempt(mc("m", *range(10))), attempt(mc("m", *range(10)), M(), M()), attempt(mc("m", *range(10)), obj=M())))
t("how it is shown", lambda: (repr(mc("a")), repr(mc("a", 1)), repr(mc("a", 1, "b")), repr(mc("a", x=1)), repr(mc("a", 1, x=2, y="z")), repr(mc("a", mc("b"))), [(l.append(mc("a", l)), repr(l[0]))[1] for l in [[]]], attempt(repr, mc("a", type("R", (), {"__repr__": lambda s: 1 / 0})())), attempt(repr, mc("a", x=type("R", (), {"__repr__": lambda s: 1 / 0})()))))
t("what it has", lambda: (mc("a").__text_signature__, attempt(getattr, mc("a"), "__dict__"), attempt(setattr, mc("a"), "x", 1), mc("a") == mc("a"), callable(mc("a"))))
t("reduced", lambda: [(r[0].__name__ if isinstance(r[0], type) else (type(r[0]).__name__, r[0].func.__name__, r[0].args, r[0].keywords), r[1:]) for g in (mc("a"), mc("a", 1, 2), mc("a", x=1), mc("a", 1, x=2)) for r in [g.__reduce__()]])
t("copied and pickled", lambda: [(repr(copy.copy(g)), repr(copy.deepcopy(g)), [attempt(lambda: repr(pickle.loads(pickle.dumps(g, p)))) for p in range(pickle.HIGHEST_PROTOCOL + 1)]) for g in (mc("a"), mc("a", 1, 2), mc("a", x=1), mc("a", 1, x=2))])
t("what it was given can be changed afterwards, if it can be changed", lambda: [(g(M()), l.append(2), g(M())) for l in [[1]] for g in [mc("m", l, k=l)]])
