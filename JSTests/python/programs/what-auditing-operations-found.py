# What was found to be wrong by trying every operator between every two of a hundred values, and the built-in functions of them: audits/operations.py.


def t(label, f):
    try:
        r = f()
    except BaseException as e:
        r = type(e).__name__ + ": " + str(e)
    print(label, "=>", r)


# ---- A frozenset is not changed
f = frozenset({1, 2})
g = f
g -= {1}
print(sorted(f), sorted(g), type(g).__name__)
for source in ("g |= {3}", "g &= {1}", "g ^= {1, 5}", "g -= frozenset({2})"):
    g = f
    exec(source)
    print(source, sorted(f), sorted(g), g is f)


class Fs(frozenset):
    pass


class St(set):
    pass


h = k = Fs({1, 2})
k -= {1}
print(sorted(h), sorted(k), type(k).__name__)
h = k = St({1, 2})
k -= {1}
print(sorted(h), sorted(k), type(k).__name__, h is k)
print([type(x).__name__ for x in (St({1}) | {2}, Fs({1}) | {2}, St({1}).copy(), Fs({1}).copy(), {1} | Fs({2}), frozenset({1}) | St({2}))], frozenset({1}).copy() is not None, (z := frozenset({1})).copy() is z)
t("a set in a set", lambda: ({1} in {frozenset({1})}, set() in frozenset(), {1} in {1}, St({1}) in {frozenset({1})}))
t("removing a set", lambda: (s := {frozenset({1}), 2}, s.remove({1}), s.discard({5}), s)[3])
t("removing a set that is not there", lambda: {1}.remove({2}))
t("a list in a set", lambda: [] in {1})

# ---- sum()
t("floats", lambda: (sum([0.1] * 10), sum([1e100, 1.0, -1e100]), sum([0.1] * 10, 0.0), sum([1, 0.1, 0.2, 0.3]), sum([0.1, 0.2, 0.3, 1]), sum([0.1] * 10, True), sum([1e308, 1e308, -1e308]), sum([-0.0, -0.0], -0.0), sum([float("inf"), 1.0])))
t("complex", lambda: (sum([0.1 + 0.1j] * 10), sum([0.1j] * 10, 0j), sum([1, 0.1, 0.1j, 0.2]), sum([0.1] * 10 + [1j])))
t("what it starts from", lambda: [sum([[1], [2]], []), sum([(1,)], ()), sum([], "x") if 0 else None, sum([], None), sum(range(5), 10), sum([2 ** 62, 2 ** 62, 2 ** 62])])
for start in ("", "a", b"", bytearray(b"")):
    t("sum from %r" % (start,), lambda: sum([], start))
    t("sum of what cannot be gone through, from %r" % (start,), lambda: sum(0, start))
t("too large for a float", lambda: sum([0.5, 10 ** 400]))

# ---- pow() with three arguments


class P:
    def __repr__(self):
        return type(self).__name__ + "()"

    def __pow__(self, other, modulus=None):
        return ("pow", other, modulus)

    def __rpow__(self, other, modulus=None):
        return ("rpow", other, modulus)


class Q(P):
    def __rpow__(self, other, modulus=None):
        return ("Q.rpow", type(other).__name__, modulus)


class OnlyTwo:
    def __pow__(self, other):
        return "two"


for a, b, c in ((2, 10, 7), (2, -1, 7), (2, 10, -7), (2, 10, 0), (2, 10, 1), (0, 0, 5), (True, 2, 3), (2.0, 2, 3), (2, 2.0, 3), (2, 2, 3.0), (2j, 2, 3), (2, 2j, 3), (2, 2, 3j), (None, 2, 3), (2, None, 3), (2, 2, "x"), ("a", 2, 3), (P(), 2, 3), (2, P(), 3), (2, 2, P()), (P(), P(), 3),
                (P(), Q(), 3), (Q(), P(), 3), (OnlyTwo(), 2, 3), (2.0, P(), 3), (2j, P(), 3), (P(), 2.0, None), (2, 2, None), (10 ** 30, 10 ** 30, 10 ** 9 + 7), (3, -5, 10 ** 9 + 7), (4, -1, 6)):
    t("pow(%r, %r, %r)" % tuple(x if not isinstance(x, (P, OnlyTwo)) else type(x).__name__ for x in (a, b, c)), lambda: pow(a, b, c))
t("a negative float to a power that is not whole", lambda: ((-8.0) ** (1 / 3) != 0, (-1e308) ** 1.5))

# ---- Which of what is wrong is said
t("issubclass", lambda: issubclass(0, 0))
t("issubclass of a class", lambda: issubclass(int, 0))
t("isinstance", lambda: isinstance(0, 0))


class Duck:
    def __init__(self, *bases):
        self.__bases__ = bases


a = Duck()
b = Duck(a)
c = Duck(Duck(), b)
t("whatever has __bases__ is a class", lambda: (issubclass(c, a), issubclass(a, c), issubclass(b, b), issubclass(c, (int, a)), issubclass(int, a) if 0 else None))


class Instance:
    __class__ = c


t("and an instance of one", lambda: (isinstance(Instance(), a), isinstance(0, a)))
for x, base in ((0, 0.0), (0, 1), (0, 37), (0, 10), ("10", 1), ("10", 2), ("10", 2 ** 70), (b"10", 8), (1.5, 10), ("z", 36), ("10", "x"), (None, 10)):
    t("int(%r, %r)" % (x, base), lambda: int(x, base))
for arguments in ((0, 0), ("a", 0), ("a", None), ("a", "ascii", 0), (b"a", "ascii"), (0, "ascii"), ("a",)):
    t("bytes%r" % (arguments,), lambda: bytes(*arguments))
    t("bytearray%r" % (arguments,), lambda: bytearray(*arguments))
t("ord", lambda: (ord(b"a"), ord(bytearray(b"\xff")), ord("\U0001d54f")))
for x in (b"", b"ab", bytearray(b""), "", "ab", 1, None):
    t("ord(%r)" % (x,), lambda: ord(x))
t("nothing, in an encoding that there is none of", lambda: (str(b"", "no such"), b"".decode("no such"), str(bytearray(b""), "no such", "nor this")))
t("something", lambda: str(b"a", "no such"))
for value in (b"", b"a", [], [1], {}, {"a": 1}, range(1), bytearray(b""), (), (1,), "", "a", 1, None, memoryview(b"")):
    shown = repr(value).partition(" at ")[0]
    t("'' %% %s" % shown, lambda: "" % value)
    t("b'' %% %s" % shown, lambda: b"" % value)
    t("'%%(a)s' %% %s" % shown, lambda: "%(a)s" % value)
for value in (-2 ** 63, -2 ** 63 - 1, 2 ** 63 - 1, 2 ** 63):
    t("times %d" % value, lambda: ("" * value, [] * value, () * value, b"" * value))
    t("item %d" % value, lambda: "abc"[value])
    t("slice %d" % value, lambda: ("abc"[value:], "abc"[:value], [1, 2][value:value]))
for value, specification in ((1j, "05"), (1j, "=5"), (1j, "0=5"), (1j, "x"), (1j, "0x"), (1j, "j"), (1j, "<5"), (1j, "0<5"), (1j, "5"), (1 + 2j, ".1f"), (1j, "s"), (1.5, "j"), (1, "j"), (True, "j"), ("a", "j")):
    t("format(%r, %r)" % (value, specification), lambda: format(value, specification))

# ---- Instances of classes derived from the built-in ones
class S(str): pass
class I(int): pass
class F(float): pass
class B(bytes): pass
class C(complex): pass
class T(tuple): pass
class L(list): pass
class D(dict): pass
class St(set): pass
class Fs(frozenset): pass
class Ba(bytearray): pass
things = [("S", S("ms")), ("I", I(5)), ("F", F(1.5)), ("B", B(b"ab")), ("C", C(1 + 2j)), ("T", T((1, 2))), ("L", L([1, 2])), ("D", D({1: 2})), ("St", St({1})), ("Fs", Fs({1})), ("Ba", Ba(b"ab"))]
ops = [("iter", lambda x: list(iter(x))), ("for", lambda x: [c for c in x]), ("len", len), ("contains", lambda x: (x[0] if hasattr(x, "__getitem__") and not isinstance(x, dict) else 1) in x), ("getitem", lambda x: x[0]), ("slice", lambda x: x[0:1]), ("reversed", lambda x: list(reversed(x))),
       ("hash", lambda x: type(hash(x)).__name__), ("repr", repr), ("str", str), ("bool", bool), ("eq", lambda x: x == x), ("lt", lambda x: x < x), ("add", lambda x: x + x), ("mul", lambda x: x * 2), ("rmul", lambda x: 2 * x), ("mod", lambda x: x % 2), ("star", lambda x: [*x]), ("unpack", lambda x: (lambda a, *b: (a, b))(*x)),
       ("tuple", tuple), ("list", list), ("set", lambda x: sorted(set(x))), ("sorted", sorted), ("min", min), ("max", max), ("sum", lambda x: sum(x)), ("enumerate", lambda x: list(enumerate(x))), ("zip", lambda x: list(zip(x, x))), ("map", lambda x: list(map(repr, x))), ("join", lambda x: "-".join(x)), ("in list", lambda x: x in [x]),
       ("dict key", lambda x: {x: 1}[x]), ("format", lambda x: format(x)), ("f-string", lambda x: f"{x}|{x!r}"), ("percent", lambda x: "%s|%r" % (x, x)), ("int", int), ("float", float), ("index", lambda x: [1, 2, 3, 4, 5, 6][x]), ("neg", lambda x: -x), ("abs", abs), ("copy", lambda x: x.copy()), ("iter next", lambda x: next(iter(x))),
       ("match seq", lambda x: (lambda: [1 for _ in [0]])() and __import__("sys") and seq(x)), ("any", any), ("all", all), ("bytes", bytes), ("str.upper", lambda x: x.upper()), ("split", lambda x: x.split()), ("dunder iter", lambda x: list(type(x).__mro__[1].__iter__(x))), ("dunder len", lambda x: type(x).__mro__[1].__len__(x)), ("dunder contains", lambda x: type(x).__mro__[1].__contains__(x, 1)),
       ("dunder getitem", lambda x: type(x).__mro__[1].__getitem__(x, 0)), ("dunder hash", lambda x: type(type(x).__mro__[1].__hash__(x)).__name__), ("dunder repr", lambda x: type(x).__mro__[1].__repr__(x)), ("dunder str", lambda x: type(x).__mro__[1].__str__(x)), ("dunder eq", lambda x: type(x).__mro__[1].__eq__(x, x)), ("dunder add", lambda x: type(x).__mro__[1].__add__(x, x)), ("dunder mul", lambda x: type(x).__mro__[1].__mul__(x, 2)),
       ("dunder bool", lambda x: type(x).__mro__[1].__bool__(x)), ("dunder reversed", lambda x: list(type(x).__mro__[1].__reversed__(x))), ("dunder format", lambda x: type(x).__mro__[1].__format__(x, "")), ("dunder getnewargs", lambda x: x.__getnewargs__()), ("dunder sizeof", lambda x: type(x.__sizeof__()).__name__), ("dunder dir", lambda x: len(dir(x)) > 10), ("set attr", lambda x: (setattr(x, "z", 1), x.z)[1]), ("vars", vars)]
def seq(x):
    match x:
        case [a, *b]: return ("seq", a, b)
        case {1: v}: return ("map", v)
        case str(v): return ("str", v)
        case int(v): return ("int", v)
        case _: return "other"
for on, f in ops:
    for tn, x in things:
        try: r = repr(f(x))
        except Exception as e: r = type(e).__name__ + ": " + str(e)
        print(on, tn, "=>", r)


class MyStr(str):
    pass


class MyInt(int):
    pass


class MyFloat(float):
    pass


m = MyStr("real")
t("a name that is of a class derived from str", lambda: (hasattr(1, m), getattr(1, m), getattr(1, MyStr("nope"), 5), (o := type("O", (), {})(), setattr(o, m, 3), o.real, delattr(o, m), hasattr(o, m))[2::2]))
t("how to format", lambda: (format(5, MyStr(">3")), format("a", MyStr(">3")), format(1.5, MyStr(".2f")), format(1j, MyStr(".1f")), (5).__format__(MyStr("x"))))
t("an encoding", lambda: (str(b"a", MyStr("ascii")), bytes("a", MyStr("ascii")), str(b"\xff", MyStr("ascii"), MyStr("replace"))))
t("what is left as it was is what was given", lambda: [type(x).__name__ for x in (m % (), m.format(), format(m, "0"), format(m, "2"), format(m, ""), format(m, "9"), MyStr("%s") % (1,), MyStr("{}").format(1), f"{m}", f"{m:1}", m + "", m * 1, m[:], str(m))])
for value in (MyInt(5), MyFloat(1.5), m, True):
    t("what it is that cannot be formatted so: %r" % (value,), lambda: format(value, "!!"))
    t("nor so: %r" % (value,), lambda: format(value, "j"))
t("%d of one", lambda: "%d" % m)
t("comparing", lambda: (m < MyStr("s"), m <= "real", "a" < m, m == MyStr("real"), m != "x", str.__eq__(m, m), str.__lt__(m, "z"), sorted([MyStr("b"), "a", MyStr("c")]), max(MyStr("b"), "a"), m in ["real"], MyStr("ea") in m, MyStr("ea") in "real", "ea" in m))
