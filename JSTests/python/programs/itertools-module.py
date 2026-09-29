# The module itertools
import itertools
import sys
from itertools import *
from itertools import _grouper, _tee, _tee_dataobject


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


def take(n, it):
    "As many as that of what it gives, and how it ends if it does."
    out = []
    for i in range(n):
        try:
            out.append(next(it))
        except BaseException as e:
            out.append(show(e))
            break
    return out


L = lambda it: take(60, it)
CLASSES = [accumulate, batched, chain, combinations, combinations_with_replacement, compress, count, cycle, dropwhile, filterfalse, groupby, islice, pairwise, permutations, product, repeat, starmap, takewhile, zip_longest, _grouper, _tee, _tee_dataobject]

print("---- what there is")
t("the module", lambda: (itertools.__name__, itertools.__package__, itertools.__loader__.__name__, itertools.__spec__.origin, sorted(n for n in vars(itertools) if not n.startswith("__")), itertools.__doc__))
for c in CLASSES:
    t(c.__name__, lambda: (c.__module__, c.__qualname__, [b.__name__ for b in c.__mro__], sorted(vars(c)), c.__text_signature__, c.__basicsize__, c.__itemsize__, c.__dictoffset__, c.__weakrefoffset__, bool(c.__flags__ & (1 << 10)), bool(c.__flags__ & (1 << 8)), type(c).__name__, repr(c)))
    t("its doc", lambda: c.__doc__)
    for name in sorted(vars(c)):
        if name not in ("__doc__", "__new__"):
            t("%s.%s" % (c.__name__, name), lambda: (type(vars(c)[name]).__name__, getattr(vars(c)[name], "__text_signature__", None), getattr(vars(c)[name], "__doc__", None)))
t("tee", lambda: (type(tee).__name__, tee.__module__, tee.__qualname__, tee.__text_signature__, tee.__doc__, tee.__self__ is itertools))

print("---- accumulate")
t("sums", lambda: (L(accumulate([1, 2, 3, 4])), L(accumulate([])), L(accumulate([5])), L(accumulate("abc")), L(accumulate([[1], [2]])), L(accumulate([1.5, 2])), L(accumulate([1, 2], None))))
t("another function", lambda: (L(accumulate([1, 2, 3, 4], lambda a, b: a * b)), L(accumulate([3, 1, 4], max)), L(accumulate([1, 2], func=min)), L(accumulate(iterable=[1, 2], func=None))))
t("initial", lambda: (L(accumulate([1, 2], initial=10)), L(accumulate([], initial=10)), L(accumulate([1, 2], initial=None)), L(accumulate([1], initial=0)), L(accumulate([1], lambda a, b: (a, b), initial=())), L(accumulate([[2]], initial=[1]))))
t("what cannot be added", lambda: (L(accumulate([1, "a", 2])), L(accumulate([1, 2], 5)), L(accumulate([1, 2], lambda a: a))))
t("it goes on after that", lambda: [(take(3, it), take(3, it)) for it in [accumulate([1, "a", 2, 3])]])

print("---- batched")
t("batches", lambda: (L(batched("ABCDEFG", 3)), L(batched("ABCDEF", 3)), L(batched("", 3)), L(batched("AB", 5)), L(batched("ABC", 1)), L(batched(range(5), n=2)), L(batched(iterable="abc", n=2))))
t("strict", lambda: (L(batched("ABCDEFG", 3, strict=True)), L(batched("ABCDEF", 3, strict=True)), L(batched("", 3, strict=True)), L(batched("ABCD", 3, strict=0)), L(batched("ABCD", 3, strict="yes"))))
t("after the end", lambda: [(L(it), L(it)) for it in [batched("ABCD", 3, strict=True)]])
# What is large and is not refused out of hand, CPython sets about making room for.
for n in (0, -1, 1.5, "a", None, True, 2 ** 63, 2 ** 63 - 1):
    t("batched('ab', %r)" % (n,), lambda: L(batched("ab", n)))

print("---- chain")
t("chains", lambda: (L(chain()), L(chain("ab")), L(chain("ab", [1], (), {2: 3})), L(chain([], [], [])), L(chain.from_iterable(["ab", "c"])), L(chain.from_iterable([])), L(chain.from_iterable("ab")), L(chain(chain("a"), chain("b")))))
t("what cannot be gone through", lambda: (L(chain("a", 5, "b")), L(chain(5)), L(chain.from_iterable([[1], 5, [2]])), attempt(chain.from_iterable, 5), L(chain.from_iterable(5 for i in range(2)))))
t("it is over after that", lambda: [(L(it), L(it)) for it in [chain("a", 5, "b")]])
t("lazily", lambda: [(take(3, it), seen) for seen in [[]] for it in [chain.from_iterable((seen.append(i), [i, i])[1] for i in count())]])
t("chain[int]", lambda: (repr(chain[int]), chain[int].__origin__ is chain, chain[int, str].__args__, attempt(chain.__class_getitem__), attempt(chain.__class_getitem__, 1, 2)))

print("---- combinations and the like")
for f in (combinations, combinations_with_replacement, permutations):
    for pool in ("", "A", "AB", "ABC", "ABCD", range(4)):
        for r in (0, 1, 2, 3, 4, 5):
            t("%s(%r, %d)" % (f.__name__, pool, r), lambda: L(f(pool, r)))
    t("by name", lambda: (L(f(iterable="AB", r=1)), L(f("AB", r=2))))
    for r in (-1, 1.5, "a", None, True, 2 ** 63, 2 ** 63 - 1):
        t("%s('AB', %r)" % (f.__name__, r), lambda: L(f("AB", r)))
    t("__sizeof__", lambda: [f(p, r).__sizeof__() - f.__basicsize__ for p, r in (("", 0), ("AB", 1), ("ABC", 2), ("ABCDE", 5), ("AB", 7))])
    t("after the end", lambda: [(len(L(it)), L(it), L(it)) for it in [f("ABC", 2)]])
    t("each is another tuple", lambda: [(a, b, a is b) for it in [f("ABC", 2)] for a in [next(it)] for b in [next(it)]])
t("permutations(x)", lambda: (L(permutations("ABC")), L(permutations("")), L(permutations("AB", None)), L(permutations([1]))))
t("what cannot be gone through", lambda: [attempt(f, 5, 1) for f in (combinations, combinations_with_replacement, permutations)])

print("---- product")
t("products", lambda: (L(product()), L(product("AB")), L(product("AB", "xy")), L(product("AB", "", "xy")), L(product("A", "B", "C")), L(product(range(2), repeat=3)), L(product("AB", "x", repeat=2)), L(product("AB", repeat=0)), L(product(repeat=3)), L(product(repeat=0)), L(product("AB", repeat=1)), L(product([], repeat=2))))
for repeat_ in (-1, 1.5, "a", None, True, 2 ** 63, 2 ** 62, 2 ** 63 - 1):
    t("product('AB', repeat=%r)" % (repeat_,), lambda: len(L(product("AB", repeat=repeat_))))
t("other keywords", lambda: (attempt(lambda: product("AB", other=1)), attempt(lambda: product("AB", repeat=1, other=1)), attempt(lambda: product("AB", other=1, repeat=1)), attempt(lambda: product(**{"repeat": 2, "x": 1}))))
t("__sizeof__", lambda: [p.__sizeof__() - product.__basicsize__ for p in (product(), product("AB"), product("AB", "C"), product("AB", repeat=5), product("AB", repeat=0))])
t("what cannot be gone through", lambda: (attempt(product, 5), attempt(product, "a", 5), L(product(5, repeat=0))))
t("all of it is read at once", lambda: [(product(iter_logging(seen, "ab"), iter_logging(seen, "c")) and None, seen) for seen in [[]]])


def iter_logging(seen, items):
    for x in items:
        seen.append(x)
        yield x


t("all of it is read at once", lambda: [(product(iter_logging(seen, "ab"), iter_logging(seen, "c")) and None, seen) for seen in [[]]])
t("after the end", lambda: [(len(L(it)), L(it)) for it in [product("AB", "xy")]])

print("---- compress")
t("compress", lambda: (L(compress("ABCDEF", [1, 0, 1, 0, 1, 1])), L(compress("ABC", [])), L(compress("", [1])), L(compress("ABC", [1])), L(compress("A", [1, 1, 1])), L(compress("ABC", ["", "x", None])), L(compress(data="AB", selectors=[0, 1])), L(compress("ABC", count()))))
t("which is asked first", lambda: [(L(compress(iter_logging(seen, "ab"), iter_logging(seen, [0, 1]))), seen) for seen in [[]]])
t("what cannot be gone through", lambda: (attempt(compress, 5, []), attempt(compress, [], 5), attempt(compress, 5, 6)))


class Truth:
    def __bool__(self): raise KeyError("from __bool__")


t("a selector that cannot be told", lambda: [(L(it), L(it)) for it in [compress("ABC", [0, Truth(), 1])]])

print("---- count")
t("counts", lambda: (take(3, count()), take(3, count(5)), take(3, count(5, 2)), take(3, count(0, -1)), take(3, count(1.5)), take(3, count(1, 0.5)), take(3, count(step=3)), take(3, count(start=2)), take(3, count(0, 0)), take(3, count(True)), take(3, count(1, True)), take(3, count(1j)), take(3, count(-3))))
t("large", lambda: (take(3, count(2 ** 63 - 2)), take(3, count(2 ** 63)), take(3, count(-2 ** 63)), take(3, count(2 ** 100, 2 ** 100)), take(3, count(2 ** 63 - 1)), take(2, count(0, 2 ** 63)), take(3, count(-2 ** 63 - 1))))
for c in (count(), count(5), count(5, 1), count(5, 2), count(1.5), count(1, 1.0), count(1.0, 1), count(2 ** 63), count(2 ** 63 - 1), count(0, 0), count(True), count(1, True), count(0, -1), count(2 ** 100, 2 ** 100), count(-1)):
    t("repr", lambda: (repr(c), next(c), repr(c)))
t("repr as it gets there", lambda: [(repr(c), next(c), repr(c), next(c), repr(c), next(c), repr(c)) for c in [count(2 ** 63 - 3)]])


class Num:
    def __init__(self, v): self.v = v
    def __index__(self): return self.v
    def __add__(self, other): return Num(self.v + getattr(other, "v", other))
    __radd__ = __add__
    def __repr__(self): return "Num(%d)" % self.v


class I(int):
    def __add__(self, other): return "added"
    def __repr__(self): return "I(%d)" % int(self)


t("of other kinds of number", lambda: (take(3, count(Num(1))), take(3, count(1, Num(2))), repr(count(Num(1), Num(2))), take(3, count(I(5))), repr(count(I(5))), take(3, count(I(5), 2)), take(3, count(0, I(1))), repr(count(0, I(1))), repr(count(0, I(2)))))
for args in (("a",), (None,), ([],), (1, "a"), (1, None), ("a", "b"), (b"1",)):
    t("count%r" % (args,), lambda: count(*args))

print("---- cycle")
t("cycles", lambda: (take(7, cycle("abc")), take(3, cycle("")), take(3, cycle("a")), take(5, cycle(iter("ab"))), take(4, cycle(range(2)))))
t("what is kept is what it gave", lambda: [(take(5, cycle(iter_logging(seen, "ab"))), seen) for seen in [[]]])


def fails_after(items, error=ValueError("from the iterable")):
    yield from items
    raise error


t("what fails part of the way", lambda: [(take(4, it), take(4, it)) for it in [cycle(fails_after("ab"))]])

print("---- dropwhile, takewhile and filterfalse")
small = lambda x: x < 3
t("dropwhile", lambda: (L(dropwhile(small, [1, 2, 3, 1, 5])), L(dropwhile(small, [])), L(dropwhile(small, [1, 2])), L(dropwhile(small, [5, 1])), L(dropwhile(bool, [1, 0, 1])), L(dropwhile(lambda x: "", "ab"))))
t("takewhile", lambda: (L(takewhile(small, [1, 2, 3, 1, 5])), L(takewhile(small, [])), L(takewhile(small, [1, 2])), L(takewhile(small, [5, 1])), L(takewhile(bool, [1, 0, 1]))))
t("filterfalse", lambda: (L(filterfalse(small, [1, 2, 3, 1, 5])), L(filterfalse(None, [0, 1, "", "a", None])), L(filterfalse(bool, [0, 1, "", "a"])), L(filterfalse(small, [])), L(filterfalse(lambda x: [], "ab"))))
t("what it is asked of", lambda: [([L(f(lambda x: seen.append(x) or x < 2, [1, 2, 1, 3])) for f in (dropwhile, takewhile, filterfalse)], seen) for seen in [[]]])
t("takewhile has taken one too many", lambda: [(L(takewhile(small, it)), L(it)) for it in [iter([1, 5, 2, 6])]])
t("takewhile is over", lambda: [(L(w), L(w), L(it)) for it in [iter([1, 5, 2, 6])] for w in [takewhile(small, it)]])
for f in (dropwhile, takewhile, filterfalse):
    t(f.__name__ + " with what fails", lambda: ([(take(3, it), take(3, it)) for it in [f(lambda x: 1 / x, [1, 0, 2, 3])]], [(take(3, it), take(3, it)) for it in [f(lambda x: Truth(), [1, 2])]], L(f(5, [1])), L(f(small, fails_after([1, 5]))), attempt(f, small, 5), attempt(f, None, 5)))
t("None is only for filterfalse", lambda: (L(dropwhile(None, [1])), L(takewhile(None, [1]))))

print("---- groupby")
G = lambda it: [(k, list(g)) for k, g in it]
t("groups", lambda: (G(groupby("AAAABBBCCDAA")), G(groupby("")), G(groupby("A")), G(groupby([1, 1.0, True, 2])), G(groupby("aAbB", str.lower)), G(groupby(range(6), lambda x: x // 2)), G(groupby("ab", None)), G(groupby("ab", key=None)), G(groupby(iterable="aab", key=ord))))
t("a group is over when the next is asked for", lambda: [(k1, k2, list(g1), list(g2)) for it in [groupby("AABB")] for k1, g1 in [next(it)] for k2, g2 in [next(it)]])
t("part of a group", lambda: [(k1, next(g1), [(k, list(g)) for k, g in it], list(g1)) for it in [groupby("AAABBC")] for k1, g1 in [next(it)]])
t("groups that are not gone through", lambda: [k for k, g in groupby("AAABBC")])
t("what a group is", lambda: [(type(g).__name__, iter(g) is g, type(it).__name__, iter(it) is it) for it in [groupby("A")] for k, g in [next(it)]])
t("with what fails", lambda: (attempt(G, groupby([1, 0], lambda x: 1 / x)), attempt(G, groupby(fails_after("aab"))), attempt(groupby, 5), attempt(G, groupby("ab", 5)), [(take(2, it) and None, [k for k, g in it]) for it in [groupby([1, 0, 2], lambda x: 1 / x)]]))


class Eq:
    def __init__(self, v): self.v = v
    def __eq__(self, other): raise KeyError("from __eq__")
    def __repr__(self): return "Eq(%r)" % self.v


t("keys that cannot be compared", lambda: (attempt(G, groupby([Eq(1), Eq(2)])), [(k, take(3, g)) for it in [groupby([Eq(1), Eq(2)])] for k, g in [next(it)]]))
same = Eq(1)
t("the same key is not compared", lambda: G(groupby([same, same, same])))
t("_grouper", lambda: [(L(_grouper(it, "A")), L(_grouper(it, "B")), L(_grouper(it, "Z")), L(it)[:1] and None) for it in [groupby("AABB")]])
for args in ((), (1,), (1, 2), (groupby("a"),), (groupby("a"), 1, 2), ("a", "a"), (None, None)):
    t("_grouper%r" % (tuple("groupby" if isinstance(a, groupby) else a for a in args),), lambda: type(_grouper(*args)).__name__)

print("---- islice")
for args in ((0,), (2,), (5,), (100,), (None,), (0, 0), (1, 3), (1, None), (None, 3), (None, None), (3, 1), (2, 2), (0, 10, 2), (1, 10, 3), (None, None, 2), (None, None, None), (0, None, 1), (1, 2, 100), (10, 20), (0, 5, 2 ** 63 - 1), (2 ** 63 - 1,), (0, 2 ** 63 - 1, 2 ** 63 - 1), (True, 3), (1, 6, True)):
    t("islice('ABCDEFG', *%r)" % (args,), lambda: L(islice("ABCDEFG", *args)))
for args in ((-1,), (1.5,), ("a",), (2 ** 63,), (-1, 2), (0, -1), (0, -2), (1.5, 2), ("a", 2), (0, "a"), (0, 1.5), (0, 5, 0), (0, 5, -1), (0, 5, 1.5), (0, 5, "a"), (2 ** 63, 5), (0, 2 ** 63), (0, 5, 2 ** 63), (-2 ** 63,), (None, -1), (-1, None), ([],), (0, [])):
    t("islice('AB', *%r)" % (args,), lambda: L(islice("AB", *args)))
t("how much it takes", lambda: [(L(islice(it, *args)), L(it)) for args in ((2,), (0,), (1, 3), (0, 4, 2), (0, 5, 2), (2, 2), (3, 1), (1, None, 3), (10,), (5, 10)) for it in [iter("ABCDEFG")]])
t("after the end", lambda: [(L(s), L(s), L(it)) for it in [iter("ABCDEFG")] for s in [islice(it, 2)]])
t("with __index__", lambda: (L(islice("ABCDEF", Num(2))), L(islice("ABCDEF", Num(1), Num(5), Num(2)))))
t("what fails", lambda: (attempt(islice, 5, 1), L(islice(fails_after("ab"), 5)), [(L(s), L(s)) for s in [islice(fails_after("ab"), 5)]]))

print("---- pairwise")
t("pairs", lambda: (L(pairwise("ABCD")), L(pairwise("")), L(pairwise("A")), L(pairwise("AB")), L(pairwise(range(4))), L(pairwise(pairwise("ABC")))))
t("after the end", lambda: [(L(p), L(p), L(it)) for it in [iter("ABC")] for p in [pairwise(it)]])
t("what fails", lambda: (attempt(pairwise, 5), [(L(p), L(p)) for p in [pairwise(fails_after("ab"))]], [(L(p), L(p)) for p in [pairwise(fails_after(""))]], [(L(p), L(p)) for p in [pairwise(fails_after("a"))]]))

print("---- repeat")
t("repeats", lambda: (take(3, repeat("a")), L(repeat("a", 3)), L(repeat("a", 0)), L(repeat("a", -1)), L(repeat("a", -5)), L(repeat("a", times=2)), L(repeat(object="a", times=1)), take(2, repeat(object="a")), L(repeat(None, 2)), L(repeat("a", True)), L(repeat("a", Num(2)))))
for r in (repeat("a"), repeat("a", 3), repeat("a", 0), repeat("a", -1), repeat([1], 2), repeat("a", times=-5), repeat(None), repeat("a", 2 ** 63 - 1)):
    t("repr", lambda: (repr(r), attempt(r.__length_hint__), take(1, r), repr(r), attempt(r.__length_hint__)))
t("the same each time", lambda: [a is b is x for x in [[]] for it in [repeat(x)] for a in [next(it)] for b in [next(it)]])
for args, kwargs in (((), {}), (("a", "b"), {}), (("a", 1.5), {}), (("a", None), {}), (("a", 2 ** 63), {}), (("a", 1, 2), {}), (("a",), {"other": 1}), (("a",), {"object": "b"}), ((), {"times": 1}), (("a", 1), {"times": 1}), (("a",), {"times": "x"})):
    t("repeat(*%r, **%r)" % (args, kwargs), lambda: L(repeat(*args, **kwargs)))
t("list() asks how long", lambda: (list(repeat(1, 3)), len(list(repeat(1, 1000)))))

print("---- starmap")
t("starmap", lambda: (L(starmap(pow, [(2, 5), (3, 2)])), L(starmap(pow, [])), L(starmap(lambda *a: a, ["ab", [1], (), {1: 2}, range(2)])), L(starmap(lambda: "none", [()])), L(starmap(max, [[1, 2], iter([3, 4])]))))
t("what fails", lambda: ([(L(s), L(s)) for s in [starmap(pow, [(2, 5), 5, (3, 2)])]], L(starmap(pow, [(1,)])), L(starmap(5, [(1,)])), attempt(starmap, pow, 5), L(starmap(lambda x: 1 / x, [(1,), (0,), (2,)])), L(starmap(pow, fails_after([(2, 2)])))))


class T(tuple):
    def __iter__(self): return iter(("from", "__iter__"))


t("a class derived from tuple is gone through", lambda: L(starmap(lambda *a: a, [T((1, 2))])))

print("---- tee")
t("tee", lambda: ([L(x) for x in tee("abc")], [L(x) for x in tee("abc", 3)], tee("abc", 0), [L(x) for x in tee("abc", 1)], [L(x) for x in tee("", 2)], len(tee("a", 10)), [type(x).__name__ for x in tee("a")]))
t("each at its own pace", lambda: [(next(a), next(a), next(b), L(a), L(b)) for a, b in [tee("abcd")]])
t("it is read once", lambda: [([L(x) for x in tee(iter_logging(seen, "abc"), 3)], seen) for seen in [[]]])
t("more than a link's worth", lambda: [(sum(a), take(2, b), sum(b), L(a), L(b)) for a, b in [tee(range(200))]])
t("of a tee", lambda: [(next(a), [L(x) for x in tee(a)], L(a), L(b)) for a, b in [tee("abcd")]])
t("__copy__", lambda: [(next(a), L(a.__copy__()), L(a), type(a.__copy__()).__name__, attempt(a.__copy__, 1)) for a, b in [tee("abcd")]])
t("_tee", lambda: (L(_tee("abc")), [(next(a), L(_tee(a)), L(a)) for a in [_tee("abc")]], attempt(_tee, 5), attempt(_tee), attempt(_tee, "a", "b"), attempt(lambda: _tee(iterable="a"))))
for n in (-1, 1.5, "a", None, True, 2 ** 63, 2 ** 63 - 1):
    t("tee('ab', %r)" % (n,), lambda: len(tee("ab", n)))
t("what fails", lambda: (attempt(tee, 5), attempt(tee, 5, 0), attempt(tee), attempt(tee, "a", 1, 2), attempt(lambda: tee("a", n=2)), [(L(a), L(b), L(a)) for a, b in [tee(fails_after("ab"))]]))


class Reenters:
    def __init__(self): self.other = None
    def __iter__(self): return self
    def __next__(self): return next(self.other)


t("come back into", lambda: [(setattr(r, "other", b), attempt(next, a), attempt(next, b)) for r in [Reenters()] for a, b in [tee(r)]])
t("weakly referred to", lambda: [__import__("_weakref").ref(a)() is a for a, b in [tee("a")]])
t("_tee_dataobject", lambda: (type(_tee_dataobject("a", [], None)).__name__, type(_tee_dataobject(5, [1, 2], None)).__name__, type(_tee_dataobject("a", [0] * 57, None)).__name__, type(_tee_dataobject("a", [0] * 57, _tee_dataobject("a", [], None))).__name__))
for args in ((), ("a",), ("a", []), ("a", [], None, 1), ("a", (), None), ("a", 5, None), ("a", [0] * 58, None), ("a", [], 5), ("a", [1], _tee_dataobject("a", [], None)), ("a", [0] * 57, 5), ("a", [0] * 57, "x")):
    t("_tee_dataobject%s" % ascii(tuple(len(a) if isinstance(a, list) else type(a).__name__ if isinstance(a, _tee_dataobject) else a for a in args)), lambda: type(_tee_dataobject(*args)).__name__)

print("---- zip_longest")
t("zip_longest", lambda: (L(zip_longest()), L(zip_longest("ab")), L(zip_longest("ab", "xyz")), L(zip_longest("abc", "x", "")), L(zip_longest("", "")), L(zip_longest("ab", "xyz", fillvalue="-")), L(zip_longest("a", "xy", fillvalue=None)), L(zip_longest(fillvalue=1)), L(zip_longest("ab", [], fillvalue=[]))))
t("what has run out is not asked again", lambda: [(L(zip_longest(iter_logging(seen, "a"), iter_logging(seen, "xyz"))), seen) for seen in [[]]])
t("other keywords", lambda: (attempt(lambda: zip_longest("a", other=1)), attempt(lambda: zip_longest("a", fillvalue=1, other=1)), attempt(lambda: zip_longest("a", **{"fillvalue": 1, "x": 2}))))
t("what fails", lambda: (attempt(zip_longest, 5), attempt(zip_longest, "a", 5), [(L(z), L(z)) for z in [zip_longest(fails_after("a"), "xyz")]], [(L(z), L(z)) for z in [zip_longest("xyz", fails_after("a"))]]))
t("after the end", lambda: [(L(z), L(z)) for z in [zip_longest("ab", "x")]])

print("---- the wrong arguments")
VALUES = ("ab", 2, None, small)
for c in CLASSES[:19] + [tee, chain.from_iterable]:
    name = getattr(c, "__qualname__", c.__name__)
    for n in range(6):
        t("%s of %d" % (name, n), lambda: type(c(*(["ab"] * n))).__name__)
    t(name + " by a name that it has not", lambda: (attempt(lambda: type(c(zzz=1)).__name__), attempt(lambda: type(c("ab", zzz=1)).__name__), attempt(lambda: type(c("ab", "ab", zzz=1)).__name__), attempt(lambda: type(c("ab", 2, zzz=1)).__name__)))
    t(name + " of each kind", lambda: [attempt(lambda: type(c(a, b)).__name__) for a in VALUES for b in VALUES])
for c in CLASSES:
    t(c.__name__ + ".__new__", lambda: (attempt(c.__new__), attempt(c.__new__, int), attempt(c.__new__, 5), attempt(c.__new__, object), attempt(object.__new__, c)))
    t(c.__name__ + ".__next__ of something else", lambda: (attempt(c.__next__, 5), attempt(c.__next__), attempt(c.__iter__, 5)) if hasattr(c, "__next__") else None)

print("---- derived from")
for c, args in ((accumulate, ([1, 2],)), (batched, ("abc", 2)), (chain, ("ab", "c")), (combinations, ("abc", 2)), (combinations_with_replacement, ("ab", 2)), (compress, ("ab", [1, 1])), (count, (5,)), (cycle, ("ab",)), (dropwhile, (small, [1, 5])), (filterfalse, (small, [1, 5])), (groupby, ("aab",)), (islice, ("abc", 2)),
                (pairwise, ("abc",)), (permutations, ("ab",)), (product, ("ab", "c")), (repeat, ("a", 2)), (starmap, (pow, [(2, 3)])), (takewhile, (small, [1, 5])), (zip_longest, ("ab", "c"))):
    D = type("D", (c,), {})
    t("derived from " + c.__name__, lambda: [(type(x).__name__, isinstance(x, c), iter(x) is x, setattr(x, "attribute", 1), x.attribute, [v if not isinstance(v, tuple) or not isinstance(v[-1], _grouper) else v[0] for v in take(3, x)]) for x in [D(*args)]])
    K = type("K", (c,), {"__init__": lambda self, *a, **k: None})
    t("with an __init__ of its own, and a keyword", lambda: type(K(*args, keyword=1)).__name__)
    t("without", lambda: type(D(*args, keyword=1)).__name__)
    N = type("N", (c,), {"__next__": lambda self: "its own"})
    t("with a __next__ of its own", lambda: (next(N(*args)), take(2, iter(N(*args))), take(2, zip(N(*args)))))
for c in (_grouper, _tee, _tee_dataobject):
    t(c.__name__ + " cannot be", lambda: type("D", (c,), {}))
t("chain.from_iterable of a derived class", lambda: type(type("D", (chain,), {}).from_iterable(["a"])).__name__)
t("repr of a derived class", lambda: (repr(type("D", (count,), {})(3)), repr(type("D", (repeat,), {})("a", 2)), repr(type("D", (count,), {})(1.5, 2))))
t("they are not to be changed", lambda: (attempt(setattr, count, "x", 1), attempt(setattr, count(), "x", 1), attempt(delattr, chain, "from_iterable")))
t("nor pickled or copied", lambda: [attempt(x.__reduce__)[:40] if isinstance(attempt(x.__reduce__), str) else "reduced" for x in (count(), chain(), repeat(1), cycle("a"), islice("a", 1))])

print("---- what the one inside says as it ends")


def returns(items, value):
    yield from items
    return value


for label, make in (("accumulate", lambda it: accumulate(it)), ("batched", lambda it: batched(it, 2)), ("chain", lambda it: chain(it)), ("compress data", lambda it: compress(it, repeat(1))), ("compress selectors", lambda it: compress(repeat(1), it)), ("cycle", cycle), ("dropwhile", lambda it: dropwhile(small, it)), ("filterfalse", lambda it: filterfalse(small, it)),
                    ("groupby", groupby), ("islice", lambda it: islice(it, 5)), ("islice from 3", lambda it: islice(it, 3, 5)), ("pairwise", pairwise), ("starmap", lambda it: starmap(max, it)), ("takewhile", lambda it: takewhile(small, it)), ("tee", lambda it: tee(it)[0]), ("zip_longest", lambda it: zip_longest(it))):
    def go():
        it = make(returns([], "the value"))
        try:
            return next(it)
        except StopIteration as e:
            return e.args
    t(label, go)

print("---- at random, against what the documentation says that they are")
state = [987654321]


def random(n):
    v = state[0]
    v ^= (v << 13) & 0xFFFFFFFF
    v ^= v >> 17
    v ^= (v << 5) & 0xFFFFFFFF
    state[0] = v
    return v % n


def py_islice(iterable, *args):
    s = slice(*args)
    start, stop, step = s.start or 0, s.stop, s.step or 1
    for i, x in enumerate(iterable):
        if stop is not None and i >= stop:
            return
        if i >= start and (i - start) % step == 0:
            yield x


def py_combinations(pool, r):
    for indices in py_permutations(range(len(pool)), r):
        if sorted(indices) == list(indices):
            yield tuple(pool[i] for i in indices)


def py_permutations(pool, r):
    pool = tuple(pool)
    for indices in py_product(*[range(len(pool))] * r):
        if len(set(indices)) == r:
            yield tuple(pool[i] for i in indices)


def py_cwr(pool, r):
    for indices in py_product(*[range(len(pool))] * r):
        if sorted(indices) == list(indices):
            yield tuple(pool[i] for i in indices)


def py_product(*pools):
    result = [[]]
    for pool in pools:
        result = [x + [y] for x in result for y in pool]
    for prod in result:
        yield tuple(prod)


def py_groupby(items, key):
    out = []
    for x in items:
        if out and out[-1][0] == key(x):
            out[-1][1].append(x)
        else:
            out.append((key(x), [x]))
    return out


wrong = []
for trial in range(400):
    items = [random(4) for i in range(random(9))]
    a, b, c = random(10), random(10), random(3) + 1
    checks = (
        ("islice", list(islice(items, a, b, c)), list(py_islice(items, a, b, c))),
        ("islice to", list(islice(items, a)), items[:a]),
        ("groupby", [(k, list(g)) for k, g in groupby(items, lambda x: x // 2)], py_groupby(items, lambda x: x // 2)),
        ("pairwise", list(pairwise(items)), list(zip(items, items[1:]))),
        ("batched", list(batched(items, c)), [tuple(items[i:i + c]) for i in range(0, len(items), c)]),
        ("accumulate", list(accumulate(items)), [sum(items[:i + 1]) for i in range(len(items))]),
        ("compress", list(compress(items, items[::-1])), [x for x, s in zip(items, items[::-1]) if s]),
        ("dropwhile then takewhile", list(takewhile(small, items)) + list(dropwhile(small, items)), items),
        ("filterfalse", list(filterfalse(small, items)), [x for x in items if not small(x)]),
        ("zip_longest", list(zip_longest(items, items[:a], fillvalue="-")), [(x, items[i] if i < min(a, len(items)) else "-") for i, x in enumerate(items)]),
        ("chain", list(chain(items, items[:a], [])), items + items[:a]),
        ("tee", [list(x) for x in tee(items, c)], [items] * c),
        ("cycle", take(a, cycle(items)) if items else [], [items[i % len(items)] for i in range(a)] if items else []),
    )
    small_pool = items[:4]
    r = random(4)
    checks += (
        ("combinations", list(combinations(small_pool, r)), list(py_combinations(small_pool, r))),
        ("permutations", list(permutations(small_pool, r)), list(py_permutations(small_pool, r))),
        ("combinations_with_replacement", list(combinations_with_replacement(small_pool, r)), list(py_cwr(small_pool, r))),
        ("product", list(product(small_pool, items[:2], repeat=random(2) + 1 if False else 1)), list(py_product(small_pool, items[:2]))),
    )
    wrong += [(name, items, a, b, c, r) for name, got, want in checks if got != want]
print("wrong", wrong[:3])
