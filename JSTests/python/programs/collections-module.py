# The module _collections: deque, defaultdict, and what Counter and namedtuple() are helped by.
import _collections
import sys
from _collections import deque, defaultdict, _deque_iterator, _deque_reverse_iterator, _tuplegetter, _count_elements


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


CLASSES = [deque, defaultdict, _deque_iterator, _deque_reverse_iterator, _tuplegetter]
print("---- what there is")
t("the module", lambda: (_collections.__name__, _collections.__package__, _collections.__loader__.__name__, sorted(n for n in vars(_collections) if not n.startswith("__") and n != "OrderedDict"), _collections.__doc__))
for c in CLASSES:
    t(c.__name__, lambda: (c.__module__, c.__qualname__, [b.__name__ for b in c.__mro__], sorted(vars(c)), c.__text_signature__, c.__basicsize__, c.__itemsize__, c.__dictoffset__, c.__weakrefoffset__ > 0, bool(c.__flags__ & (1 << 10)), bool(c.__flags__ & (1 << 8)), bool(c.__flags__ & (1 << 5)), bool(c.__flags__ & (1 << 6)), repr(c)))
    t("its doc", lambda: c.__doc__ if c is not _tuplegetter else type(vars(c)["__doc__"]).__name__)
    for name in sorted(vars(c)):
        if name not in ("__doc__", "__new__", "__hash__"):
            t("%s.%s" % (c.__name__, name), lambda: (type(vars(c)[name]).__name__, getattr(vars(c)[name], "__text_signature__", None), getattr(vars(c)[name], "__doc__", None)))
t("_count_elements", lambda: (type(_count_elements).__name__, _count_elements.__text_signature__, _count_elements.__doc__, _count_elements.__module__))

print("---- deque")
D = lambda *a, **k: deque(*a, **k)
t("made", lambda: (D(), D("abc"), D([1, 2]), D(range(3)), D("abc", 2), D("abc", 0), D("abc", None), D("abc", maxlen=5), D(iterable="ab"), D(maxlen=3), D(iterable="abcd", maxlen=2), D(D("ab")), D({1: 2}), D(iter("ab")), D("abc", True)))
for args, kwargs in (((5,), {}), ((None,), {}), (("ab", -1), {}), (("ab", 1.5), {}), (("ab", "x"), {}), (("ab", 2 ** 63), {}), (("ab", 2 ** 63 - 1), {}), (("ab", 1, 2), {}), (("ab",), {"other": 1}), (("ab",), {"iterable": "c"}), ((), {"maxlen": -1}), (("ab", [1]), {})):
    t("deque(*%r, **%r)" % (args, kwargs), lambda: deque(*args, **kwargs))
t("maxlen", lambda: (D().maxlen, D("a", 3).maxlen, D("a", 0).maxlen, D("a", None).maxlen, attempt(setattr, D(), "maxlen", 1), attempt(delattr, D(), "maxlen")))
t("both ends", lambda: [(d.append(1), d.appendleft(0), d.append(2), d, d.pop(), d.popleft(), d, d.pop(), d, attempt(d.pop), attempt(d.popleft)) for d in [D()]])
t("with a limit", lambda: [(d.append(4), list(d), d.appendleft(0), list(d), d.extend("xy"), list(d), d.extendleft("pq"), list(d)) for d in [D([1, 2, 3], 3)]])
t("with room for nothing", lambda: [(d.append(1), d.appendleft(1), d.extend("ab"), d.extendleft("ab"), list(d), len(d), attempt(d.insert, 0, 1), attempt(d.pop)) for d in [D("abc", 0)]])
t("extend", lambda: [(d.extend("cd"), list(d), d.extendleft("xy"), list(d), d.extend(d), list(d), d.extendleft(d), list(d), d.extend([]), d.extend(iter("z")), len(d)) for d in [D("ab")]])
t("extend with what fails", lambda: [(attempt(d.extend, 5), attempt(d.extendleft, None), attempt(d.extend, (1 / x for x in (1, 0, 2))), list(d), attempt(d.extendleft, (1 / x for x in (2, 0))), list(d)) for d in [D("a")]])
t("what is gone through when there is room for nothing", lambda: [(D(maxlen=0).extend(seen.append(x) for x in "abc"), seen) for seen in [[]]])
t("len and truth", lambda: (len(D()), len(D("abc")), bool(D()), bool(D("a")), len(D(range(1000)))))
t("items", lambda: [(d[0], d[1], d[-1], d[-4], d[3], attempt(lambda: d[4]), attempt(lambda: d[-5]), d[True]) for d in [D("abcd")]])
for key in (slice(1, 2), slice(None), "a", 1.5, None, (1,), 2 ** 63, -2 ** 63 - 1, 2 ** 100):
    t("d[%r]" % (key,), lambda: (attempt(lambda: D("abc")[key]), attempt(D("abc").__getitem__, key), attempt(D("abc").__setitem__, key, 1), attempt(D("abc").__delitem__, key), attempt(lambda: exec("d[k] = 1", {"d": D("abc"), "k": key})), attempt(lambda: exec("del d[k]", {"d": D("abc"), "k": key}))))


class Index:
    def __init__(self, v): self.v = v
    def __index__(self): return self.v


t("with __index__", lambda: [(d[Index(1)], d.__getitem__(Index(-1)), d.__setitem__(Index(0), "z"), d.__delitem__(Index(1)), list(d), d.rotate(Index(1)), list(d), d.insert(Index(1), "i"), list(d), list(d * Index(2))) for d in [D("abcd")]])
t("set and delete", lambda: [(d.__setitem__(0, "A"), d.__setitem__(-1, "D"), list(d), d.__delitem__(1), list(d), d.__delitem__(-1), list(d), attempt(d.__setitem__, 5, 1), attempt(d.__delitem__, 5), attempt(d.__delitem__, -3)) for d in [D("abcd")]])
t("in", lambda: ("a" in D("abc"), "z" in D("abc"), 1 in D(), None in D([None]), 1.0 in D([1])))
t("count and index", lambda: [(d.count("a"), d.count("z"), d.count(1), d.index("a"), d.index("b"), d.index("a", 1), d.index("a", -2), d.index("a", 1, 4), attempt(d.index, "a", 1, 3), attempt(d.index, "z"), attempt(d.index, "a", 10), d.index("a", -100), d.index("a", 0, 100), attempt(d.index, "a", 3, 1), d.index("a", 2 ** 100 * -1), attempt(d.index, "a", 2 ** 100)) for d in [D("abcab")]])
for args in ((), ("a", "x"), ("a", None), ("a", 0, None), ("a", 1.5), ("a", 0, 1, 2), ("a", Index(1))):
    t("index%r" % (tuple("Index" if isinstance(a, Index) else a for a in args),), lambda: D("abcab").index(*args))
t("insert", lambda: [[(d.insert(i, "X"), "".join(d))[1] for d in [D("abcd")]][0] for i in (0, 1, 2, 3, 4, 5, 100, -1, -2, -3, -4, -5, -100)])
t("insert into one that is full", lambda: [(attempt(d.insert, 1, "X"), list(d), d.pop(), d.insert(1, "X"), list(d)) for d in [D("abc", 3)]])
t("remove", lambda: [(d.remove("b"), list(d), d.remove("a"), list(d), attempt(d.remove, "z"), d.remove("a"), d.remove("c"), list(d), attempt(d.remove, "a")) for d in [D("abca")]])
t("rotate", lambda: [[(d.rotate(n), "".join(d))[1] for d in [D("abcde")]][0] for n in (0, 1, 2, 3, 4, 5, 6, 7, -1, -2, -5, -6, 100, -100, 2 ** 62, -2 ** 62)])
t("rotate()", lambda: [(d.rotate(), "".join(d), attempt(d.rotate, "a"), attempt(d.rotate, 1.5), attempt(d.rotate, None), attempt(d.rotate, 2 ** 63), attempt(d.rotate, 1, 2), attempt(lambda: d.rotate(n=1))) for d in [D("abc")]])
t("rotate of little", lambda: [(d.rotate(3), list(d)) for d in (D(), D("a"), D("ab"))])
t("reverse", lambda: [(d.reverse(), "".join(d))[1] for d in (D(), D("a"), D("ab"), D("abc"), D("abcd"), D("abcde", 5))])
t("reversed", lambda: (list(reversed(D("abc"))), list(reversed(D())), type(reversed(D())).__name__, list(D("abc").__reversed__())))
t("clear", lambda: [(d.clear(), list(d), len(d), d.maxlen, d.append(1), list(d), d.clear(), d.clear()) for d in [D("abc", 5)]])
t("copy", lambda: [(c, c is d, c == d, c.maxlen, d.append("z"), list(c), d.__copy__(), type(c).__name__) for d in [D("abc", 5)] for c in [d.copy()]])
t("+", lambda: (D("ab") + D("cd"), D("ab", 3) + D("cd"), D("ab") + D("cd", 1), (D("ab", 3) + D("cd")).maxlen, D() + D(), attempt(lambda: D("ab") + "cd"), attempt(lambda: D("ab") + ["c"]), attempt(lambda: ["c"] + D("ab")), attempt(lambda: D("a") + None)))
t("+=", lambda: [(d.__iadd__("cd") is d, list(d), d.__iadd__(d) is d, list(d), attempt(d.__iadd__, 5)) for d in [D("ab")]])
t("*", lambda: (D("ab") * 2, D("ab") * 0, D("ab") * -1, D("ab") * 1, 3 * D("a"), D() * 5, D("ab", 3) * 2, D("ab", 3) * 5, D("a", 3) * 10, (D("ab", 3) * 2).maxlen, D("abc") * True, attempt(lambda: D("a") * "x"), attempt(lambda: D("a") * 1.5), attempt(lambda: D("a") * D("a")), attempt(lambda: D("ab") * 2 ** 63), attempt(lambda: D("ab") * (2 ** 63 - 1))))
t("by way of the methods", lambda: (attempt(D("a").__mul__, "x"), attempt(D("a").__rmul__, 1.5), attempt(D("a").__imul__, None), attempt(D("a").__add__, "x"), attempt(D("a").__mul__, 2 ** 63), attempt(lambda: "x" * D("a")), attempt(lambda: 1.5 * D("a")), attempt(lambda: exec("d *= 'x'", {"d": D("a")})), attempt(lambda: exec("d += 5", {"d": D("a")})),
                                   attempt(lambda: D("a") - D("a")), attempt(lambda: D("a") @ 2)))
t("*=", lambda: [(d.__imul__(2) is d, list(d), d.__imul__(1) is d, d.__imul__(0) is d, list(d)) for d in [D("ab")]] + [[(d.__imul__(100), len(d), d.maxlen) for d in [D("a", 70)]], [(d.__imul__(200), len(d)) for d in [D("a")]], [(d.__imul__(50), len(d), "".join(d)[-4:]) for d in [D("abc", 100)]]])
t("repr", lambda: (repr(D()), repr(D("ab")), repr(D("ab", 5)), repr(D(maxlen=0)), str(D([1, "a"])), repr(D([D("a")]))))
t("of itself", lambda: [(d.append(d), repr(d), d.append([d]), repr(d)) for d in [D("a")]])
t("it cannot be hashed", lambda: (attempt(hash, D()), deque.__hash__, attempt(lambda: {D(): 1})))
ORDER = (D(), D("a"), D("ab"), D("b"), D("ab", 5), D([1]), D([1.0]))
for op in ("__eq__", "__ne__", "__lt__", "__le__", "__gt__", "__ge__"):
    t(op, lambda: [[getattr(a, op)(b) for b in ORDER[:5]] for a in ORDER[:5]])
    t(op + " with something else", lambda: (getattr(D("a"), op)(["a"]), getattr(D("a"), op)("a"), getattr(D("a"), op)(None), attempt(getattr(deque, op), 5, D())))
t("compared", lambda: (D([1]) == D([1.0]), D("a") == ["a"], D("a") != ["a"], attempt(lambda: D("a") < ["a"]), attempt(lambda: D([1]) < D(["a"])), D([1, "a"]) < D([2, 1]), [d == d for d in [D([float("nan")])]]))


class Eq:
    def __init__(self, act): self.act = act
    def __eq__(self, other): return self.act()


for name, args in (("count", ()), ("index", ()), ("remove", ()), ("__contains__", ())):
    t(name + " while it is changed", lambda: [(attempt(getattr(d, name), Eq(lambda: d.append(1) or False)), len(d)) for d in [D("abc")]])
    t(name + " with what cannot be compared", lambda: [(attempt(getattr(d, name), Eq(lambda: 1 / 0)), len(d)) for d in [D("abc")]])
    t(name + " while it is cleared", lambda: [(attempt(getattr(d, name), Eq(lambda: d.clear() or True)), len(d)) for d in [D("abc")]])
t("remove, when what is found changes it", lambda: [(attempt(d.remove, Eq(lambda: d.rotate(1) or True)), list(d)) for d in [D("abc")]])
t("__sizeof__", lambda: [d.__sizeof__() - deque.__basicsize__ for d in (D(), D("a"), D(range(31)), D(range(32)), D(range(33)), D(range(63)), D(range(64)), D(range(65)), D(range(128)), D(range(1000)))])
t("__sizeof__ goes by where it began", lambda: [(d.__sizeof__() - deque.__basicsize__) for d in ([(x, [x.append(i) for i in range(33)])[0] for x in [D()]] + [(x, [x.appendleft(i) for i in range(33)])[0] for x in [D()]] + [(x, x.extendleft(range(64)))[0] for x in [D()]] + [(x, x.extendleft(range(65)))[0] for x in [D()]])])
t("__reduce__", lambda: [(r[0].__name__, r[1], r[2], list(r[3]), len(r)) for r in (D("ab").__reduce__(), D("ab", 5).__reduce__(), D().__reduce__(), D(maxlen=0).__reduce__())])
t("deque[int]", lambda: (repr(deque[int]), deque[int].__origin__ is deque))
t("weakly referred to", lambda: [__import__("_weakref").ref(d)() is d for d in [D()]])
t("no attributes", lambda: (attempt(setattr, D(), "x", 1), attempt(lambda: D().__dict__)))
t("in a pattern", lambda: [("sequence", a, rest) if True else None for d in [D("abc")] for a, *rest in [d]])


def matches(x):
    match x:
        case [a, b]:
            return ("two", a, b)
        case [a, *rest]:
            return ("more", a, rest)
        case {}:
            return "a mapping"
        case _:
            return "nothing"


t("match", lambda: (matches(D("ab")), matches(D("abc")), matches(D()), matches(defaultdict(int))))
t("made again", lambda: [(d.__init__("xy"), list(d), d.maxlen, d.__init__(), list(d), d.__init__("abc", 2), list(d), attempt(d.__init__, "abc", -1), list(d), d.maxlen, attempt(d.__init__, 5), list(d), d.maxlen) for d in [D("abc", 5)]])
t("__new__", lambda: (deque.__new__(deque), deque.__new__(deque, 1, 2, 3, x=4), attempt(deque.__new__), attempt(deque.__new__, int), attempt(object.__new__, deque), len(deque.__new__(deque)), deque.__new__(deque).maxlen))

print("---- what goes through one")
t("iter", lambda: [(type(it).__name__, iter(it) is it, it.__length_hint__(), next(it), it.__length_hint__(), list(it), it.__length_hint__(), attempt(next, it)) for it in [iter(D("abc"))]])
t("reversed", lambda: [(type(it).__name__, iter(it) is it, it.__length_hint__(), next(it), it.__length_hint__(), list(it), it.__length_hint__(), attempt(next, it)) for it in [reversed(D("abc"))]])
for make in (iter, reversed):
    for label, act in (("append", lambda d: d.append(1)), ("pop", lambda d: d.pop()), ("rotate", lambda d: d.rotate(1)), ("rotate by nothing", lambda d: d.rotate(0)), ("clear", lambda d: d.clear()), ("reverse", lambda d: d.reverse()), ("set an item", lambda d: d.__setitem__(0, "z")), ("extend with nothing", lambda d: d.extend("")), ("*= 1", lambda d: d.__imul__(1)),
                       ("delete an item", lambda d: d.__delitem__(0)), ("insert", lambda d: d.insert(1, "i")), ("pop and append", lambda d: d.append(d.pop()))):
        t("%s, and then %s" % (make.__name__, label), lambda: [(next(it), act(d) and None, attempt(next, it), attempt(next, it), it.__length_hint__()) for d in [D("abcd")] for it in [make(d)]])
    t(make.__name__ + " that is over, and then it is changed", lambda: [(list(it), d.append(1), attempt(next, it)) for d in [D("ab")] for it in [make(d)]])
    t(make.__name__ + " of an empty one, and then it is changed", lambda: [(d.append(1), attempt(next, it)) for d in [D()] for it in [make(d)]])
    t(make.__name__ + ".__reduce__", lambda: [(r[0].__name__, r[1], next(it), it.__reduce__()[1], list(it), it.__reduce__()[1]) for d in [D("abc")] for it in [make(d)] for r in [it.__reduce__()]])
for c in (_deque_iterator, _deque_reverse_iterator):
    t(c.__name__, lambda: (list(c(D("abcd"))), list(c(D("abcd"), 0)), list(c(D("abcd"), 1)), list(c(D("abcd"), 3)), list(c(D("abcd"), 4)), list(c(D("abcd"), 100)), list(c(D("abcd"), -1)), list(c(D())), list(c(D(), 5)), list(c(D("ab"), True))))
    for args, kwargs in (((), {}), ((5,), {}), (("ab",), {}), ((None,), {}), ((D(), "a"), {}), ((D(), 1.5), {}), ((D(), None), {}), ((D(), 1, 2), {}), ((D(), 2 ** 63), {}), ((D("ab"),), {"other": 1}), ((), {"deque": D()})):
        t("%s(*%r, **%r)" % (c.__name__, args, kwargs), lambda: list(c(*args, **kwargs)))
    t("it cannot be derived from", lambda: type("X", (c,), {}))
t("across blocks", lambda: [(list(d) == list(range(300)), list(reversed(d)) == list(range(299, -1, -1)), sum(1 for x in d), [d[i] for i in (0, 31, 32, 63, 64, 65, 127, 128, 150, 298, 299, -1, -150, -300)]) for d in [D(range(300))]])

print("---- derived from deque")


class Sub(deque):
    pass


class Odd(deque):
    def __init__(self, *args, **kwargs):
        calls.append((args and list(args[0]), args[1:], kwargs))
        super().__init__(*args)


class Wrong(deque):
    def __new__(cls, *args): return "not a deque" if args else super().__new__(cls)


calls = []
t("Sub", lambda: [(repr(s), type(s.copy()).__name__, type(s + s).__name__, type(s * 2).__name__, type(s.__copy__()).__name__, s == D("ab"), D("ab") == s, setattr(s, "x", 1), s.x, s.__reduce__()[2], type(s.__reduce__()[0]).__name__, s.__reduce__()[0].__name__) for s in [Sub("ab")]])
t("with a limit", lambda: [(repr(s), s.copy().maxlen, repr(s + s), repr(s * 3)) for s in [Sub("ab", 3)]])
t("how it is copied", lambda: [(calls.clear(), o.copy() and None, calls[:], calls.clear(), Odd("ab", 5).copy() and None, calls) for o in [Odd("ab")]])
t("a class that gives something else", lambda: [(attempt(w.copy), attempt(lambda: w + w), attempt(lambda: w * 2)) for w in [Wrong()]])
t("its own __getitem__", lambda: [(s[1:2], s["a"], s[2 ** 100]) for s in [type("G", (deque,), {"__getitem__": lambda self, k: ("got", k)})("abc")]])
t("without one", lambda: [(attempt(lambda: s[1:2]), attempt(lambda: s[2 ** 100]), s[1]) for s in [Sub("abc")]])
t("its own __iter__", lambda: [(repr(s), list(s), s == D("abc"), s.copy(), len(s)) for s in [type("I", (deque,), {"__iter__": lambda self: iter("xy")})("abc")]])

print("---- at random, against a list")
state = [24681357]


def random(n):
    v = state[0]
    v ^= (v << 13) & 0xFFFFFFFF
    v ^= v >> 17
    v ^= (v << 5) & 0xFFFFFFFF
    state[0] = v
    return v % n


wrong = []
for maxlen in (None, 0, 1, 5, 64, 100, 200):
    d, model, sizes = deque(maxlen=maxlen), [], 0

    def trim(left):
        if maxlen is not None:
            while len(model) > maxlen:
                model.pop(0 if left else -1)

    for step in range(6000):
        what, x = random(22), random(1000)
        if what < 4:
            d.append(x); model.append(x); trim(True)
        elif what < 8:
            d.appendleft(x); model.insert(0, x); trim(False)
        elif what == 8 and model:
            if d.pop() != model.pop(): wrong.append((step, "pop"))
        elif what == 9 and model:
            if d.popleft() != model.pop(0): wrong.append((step, "popleft"))
        elif what == 10:
            n = random(400) - 200
            d.rotate(n)
            if model:
                k = n % len(model)
                model[:] = model[-k:] + model[:-k] if k else model
        elif what == 11:
            d.reverse(); model.reverse()
        elif what == 12:
            more = [random(9) for i in range(random(90))]
            d.extend(more)
            for m in more:
                model.append(m); trim(True)
        elif what == 13:
            more = [random(9) for i in range(random(90))]
            d.extendleft(more)
            for m in more:
                model.insert(0, m); trim(False)
        elif what == 14 and model:
            i = random(len(model))
            d[i] = x; model[i] = x
        elif what == 15 and model:
            i = random(len(model)) - random(2) * len(model)
            del d[i]; del model[i]
        elif what == 16 and (maxlen is None or len(model) < maxlen):
            i = random(len(model) + 3) - random(2) * len(model)
            d.insert(i, x); model.insert(i, x)
        elif what == 17 and model:
            v = model[random(len(model))]
            if d.index(v) != model.index(v) or d.count(v) != model.count(v): wrong.append((step, "index"))
            d.remove(v); model.remove(v)
        elif what == 18 and random(40) == 0:
            d.clear(); model.clear()
        elif what == 19 and model:
            i = random(len(model)) - random(2) * len(model)
            if d[i] != model[i]: wrong.append((step, "item", i))
        elif what == 20 and random(10) == 0:
            c = d.copy()
            if list(c) != model or c.maxlen != maxlen: wrong.append((step, "copy"))
        elif what == 21 and random(30) == 0 and len(model) < 300:
            n = random(4)
            d *= n
            grown = model * n
            model[:] = grown if maxlen is None else grown[-maxlen:] if maxlen else []
        if len(d) != len(model) or (step % 37 == 0 and (list(d) != model or list(reversed(d)) != model[::-1])):
            wrong.append((step, what, "differs"))
            break
        sizes = (sizes * 31 + d.__sizeof__()) % 1000000007
    print("maxlen", maxlen, len(d), list(d) == model, sizes, wrong[:3])

print("---- defaultdict")
t("made", lambda: (defaultdict(), defaultdict(int), defaultdict(None), defaultdict(list, {1: 2}), defaultdict(int, [(1, 2)]), defaultdict(int, a=1), defaultdict(int, {1: 2}, a=1), defaultdict(None, {1: 2}), defaultdict(default_factory=1), defaultdict(int, default_factory=1)))
for args in ((5,), ("a",), ([],), (int, 5), (int, {}, {}), (int, [1])):
    t("defaultdict%r" % (tuple(getattr(a, "__name__", a) for a in args),), lambda: defaultdict(*args))
t("what is not there", lambda: [(d[1], d["a"], d, d[1], len(d), 2 in d, d.get(2), d.get(2, "x"), d, d.pop(1), d.setdefault(3), d) for d in [defaultdict(int)]])
t("lists", lambda: [(d["a"].append(1), d["a"].append(2), d["b"].append(3), d) for d in [defaultdict(list)]])
t("with none", lambda: [(attempt(lambda: d[1]), attempt(d.__missing__, 1), attempt(lambda: d[(1, 2)]), d) for d in [defaultdict()]])

def _catch(f):
    try:
        f()
    except Exception as e:
        return e


t("the key of the error", lambda: (_catch(lambda: defaultdict()[(1, 2)]).args, _catch(lambda: defaultdict()[1]).args, _catch(lambda: defaultdict().__missing__("k")).args))
t("__missing__", lambda: [(d.__missing__("k"), dict(d), d.__missing__("k"), attempt(d.__missing__), attempt(d.__missing__, 1, 2), attempt(d.__missing__, [])) for d in [defaultdict(lambda: "made")]])
t("a factory that fails", lambda: [(attempt(lambda: d[1]), dict(d)) for d in [defaultdict(lambda: 1 / 0)]])
t("a factory that takes something", lambda: attempt(lambda: defaultdict(lambda x: x)[1]))
t("a factory that puts something there", lambda: [(d[1], dict(d)) for d in [defaultdict()] if not setattr(d, "default_factory", lambda: d.__setitem__(1, "put there") or "returned")])
t("default_factory", lambda: [(d.default_factory, setattr(d, "default_factory", list), d.default_factory, d[1], setattr(d, "default_factory", None), attempt(lambda: d[2]), setattr(d, "default_factory", 5), attempt(lambda: d[2]), delattr(d, "default_factory"), d.default_factory, repr(d), attempt(lambda: d[2]), attempt(delattr, d, "default_factory")) for d in [defaultdict(int)]])
t("repr", lambda: (repr(defaultdict()), repr(defaultdict(int)), repr(defaultdict(list, {1: [2]})), repr(defaultdict(None, a=1)), str(defaultdict(int))))
t("of itself", lambda: [(d.__setitem__(1, d), repr(d)) for d in [defaultdict(int)]])
t("whose factory is a method of it", lambda: [(setattr(d, "default_factory", d.copy), repr(d).replace(hex(id(d)), "ADDRESS")[:70]) for d in [defaultdict()]])
t("copy", lambda: [(c, c is d, c.default_factory, type(c).__name__, d.__copy__(), c[5], d) for d in [defaultdict(int, {1: 2})] for c in [d.copy()]])
t("|", lambda: (defaultdict(int, {1: 2}) | {3: 4}, {3: 4} | defaultdict(int, {1: 2}), defaultdict(int, {1: 2}) | defaultdict(list, {1: 5}), (defaultdict(int) | {}).default_factory, ({} | defaultdict(list)).default_factory, (defaultdict(int) | defaultdict(list)).default_factory, attempt(lambda: defaultdict(int) | [(1, 2)]), attempt(lambda: [(1, 2)] | defaultdict(int)),
                defaultdict(int).__or__(5), defaultdict(int).__ror__(5), defaultdict(int, a=1).__ror__({"b": 2})))
t("|=", lambda: [(d.__ior__({3: 4}) is d, d, d.__ior__([(5, 6)]) is d, d, d.default_factory) for d in [defaultdict(int, {1: 2})]])
t("__reduce__", lambda: [(r[0].__name__, r[1], r[2], r[3], list(r[4]), len(r)) for r in (defaultdict(int, {1: 2}).__reduce__(), defaultdict().__reduce__(), defaultdict(None, a=1).__reduce__())])
t("it is a dict", lambda: (isinstance(defaultdict(), dict), defaultdict(int, {1: 2}) == {1: 2}, {1: 2} == defaultdict(list, {1: 2}), dict(defaultdict(int, a=1)), list(defaultdict(int, a=1).items()), attempt(hash, defaultdict()), attempt(setattr, defaultdict(), "x", 1), repr(defaultdict[int, str])))
t("made again", lambda: [(d.__init__(list), d, d.default_factory, d.__init__(), d.default_factory, d.__init__(None, {3: 4}), d, attempt(d.__init__, 5), d.default_factory) for d in [defaultdict(int, {1: 2})]])


class DD(defaultdict):
    pass


class DM(defaultdict):
    def __missing__(self, key): return "its own"


class DI(defaultdict):
    def __init__(self, tag): super().__init__(int); self.tag = tag


t("derived from", lambda: [(repr(d), d[1], type(d.copy()).__name__, type(d | {}).__name__, type({} | d).__name__, setattr(d, "x", 1), d.x, d.__reduce__()[0].__name__) for d in [DD(int)]])
t("its own __missing__", lambda: [(d[1], d) for d in [DM(int)]])
t("made otherwise", lambda: [(d[1], attempt(d.copy), attempt(lambda: d | {})) for d in [DI("t")]])

print("---- _count_elements")
t("counts", lambda: [(_count_elements(m, "abracadabra"), m, _count_elements(m, "ab"), m, _count_elements(m, []), _count_elements(m, iter("z")), m) for m in [{}]])
t("what is there already", lambda: [(_count_elements(m, "aab"), m) for m in [{"a": 10, "b": 1.5, "c": 0}]])
t("what cannot be added to", lambda: [(attempt(_count_elements, m, "ba"), m) for m in [{"a": "x"}]])
t("what cannot be hashed", lambda: [(attempt(_count_elements, m, ["a", [], "b"]), m) for m in [{}]])


class Logs(dict):
    def get(self, key, default=None):
        log.append(("get", key, default))
        return super().get(key, default)

    def __setitem__(self, key, value):
        log.append(("set", key, value))
        super().__setitem__(key, value)


class OnlyGet(dict):
    def get(self, key, default=None): return 100


class Mapping:
    def __init__(self): self.d = {}
    def get(self, key, default): return self.d.get(key, default)
    def __setitem__(self, key, value): self.d[key] = value


log = []
t("a dict with its own get and __setitem__", lambda: [(_count_elements(m, "aba"), m, log) for m in [Logs()]])
t("with its own get", lambda: [(_count_elements(m, "aba"), m) for m in [OnlyGet()]])
t("derived from dict and nothing more", lambda: [(_count_elements(m, "aba"), m) for m in [type("P", (dict,), {})()]])
t("no dict at all", lambda: [(_count_elements(m, "aba"), m.d) for m in [Mapping()]])
t("a defaultdict", lambda: [(_count_elements(m, "aba"), dict(m)) for m in [defaultdict(lambda: 50)]])
for args in ((), ({},), ({}, 5), (5, "a"), (None, "a"), ({}, "a", 1), ([], "a"), (object(), "")):
    t("_count_elements%s" % ascii(tuple(type(a).__name__ if type(a) is object else a for a in args)), lambda: _count_elements(*args))
t("which fails first", lambda: attempt(_count_elements, 5, 5))

print("---- _tuplegetter")
g = _tuplegetter(1, "the doc")
t("what it is", lambda: (repr(g), g.__doc__, g.__get__(("a", "b", "c")), g.__get__(("a", "b"), tuple), attempt(g.__get__, None), attempt(g.__get__, None, None), g.__get__(None, tuple) is g, g.__reduce__()[0].__name__, g.__reduce__()[1], repr(_tuplegetter(0, None)), repr(_tuplegetter(-1, [1]))))
t("of the wrong thing", lambda: (attempt(g.__get__, ["a", "b"]), attempt(g.__get__, 5), attempt(g.__get__, "ab"), attempt(g.__get__, ("a",)), attempt(g.__get__, ()), attempt(_tuplegetter(-1, "").__get__, ("a",)), attempt(_tuplegetter(2 ** 62, "").__get__, ("a",)), attempt(g.__get__), attempt(g.__get__, (), tuple, 1)))
t("it cannot be set", lambda: (attempt(g.__set__, ("a", "b"), 1), attempt(g.__delete__, ("a", "b")), attempt(g.__set__, 5, 1), attempt(g.__set__, ()), attempt(g.__delete__)))
t("its doc can", lambda: [(setattr(x, "__doc__", "another"), x.__doc__, repr(x), setattr(x, "__doc__", None), x.__doc__, delattr(x, "__doc__"), x.__doc__, repr(x), attempt(delattr, x, "__doc__")) for x in [_tuplegetter(0, "d")]])
for args, kwargs in (((), {}), ((1,), {}), ((1, 2, 3), {}), (("a", "d"), {}), ((1.5, "d"), {}), ((None, "d"), {}), ((2 ** 63, "d"), {}), ((True, "d"), {}), ((Index(2), "d"), {}), ((1, "d"), {"x": 1}), ((), {"index": 1, "doc": "d"})):
    t("_tuplegetter(*%s, **%r)" % (ascii(tuple("Index" if isinstance(a, Index) else a for a in args)), kwargs), lambda: repr(_tuplegetter(*args, **kwargs)))


class P(tuple):
    x = _tuplegetter(0, "x")
    y = _tuplegetter(1, "y")


t("in a class", lambda: [(p.x, p.y, P.x is vars(P)["x"], attempt(setattr, p, "x", 1), attempt(delattr, p, "x"), P.x.__doc__, attempt(lambda: P((1,)).y)) for p in [P((1, 2))]])
t("it cannot be derived from, nor given attributes", lambda: (attempt(type, "X", (_tuplegetter,), {}), attempt(setattr, g, "x", 1)))

print("---- what is made of them")
import collections
t("namedtuple", lambda: [(p, p.x, p._replace(x=5), p._asdict(), N._fields, N.__doc__, N.x.__doc__, type(vars(N)["x"]).__name__, N._make([7, 8]), repr(N(x=1, y=2)), attempt(N, 1), attempt(setattr, p, "x", 1)) for N in [collections.namedtuple("N", "x y")] for p in [N(1, 2)]])
t("Counter", lambda: [(c, c.most_common(2), c["z"], c + c, c - collections.Counter("ab"), sorted(c.elements()), c.total()) for c in [collections.Counter("abracadabra")]])
t("collections", lambda: (collections.deque is deque, collections.defaultdict is defaultdict, isinstance(deque(), collections.abc.MutableSequence), isinstance(defaultdict(), collections.abc.MutableMapping), collections.ChainMap({1: 2}, {3: 4})[3], list(collections.OrderedDict(a=1, b=2).items())))
