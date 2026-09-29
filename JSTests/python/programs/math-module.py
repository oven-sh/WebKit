# The module math. What a function comes to is compared to the last bit.
import math
import sys
from fractions import Fraction


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


inf, nan = float("inf"), float("nan")
print("---- what there is")
t("the module", lambda: (math.__name__, math.__package__, math.__loader__.__name__, math.__doc__, sorted(n for n in vars(math) if not n.startswith("__"))))
for name in sorted(n for n in vars(math) if not n.startswith("__")):
    x = getattr(math, name)
    if isinstance(x, float):
        t(name, lambda: (x, x.hex() if x == x else math.copysign(1, x)))
    else:
        t(name, lambda: (type(x).__name__, x.__text_signature__, x.__doc__, x.__module__))


class Float:
    def __init__(self, v): self.v = v
    def __float__(self): return self.v


class Index:
    def __init__(self, v): self.v = v
    def __index__(self): return self.v


class F(float):
    pass


class I(int):
    pass


class Raises:
    def __float__(self): raise ValueError("no float")
    def __index__(self): raise ValueError("no index")
    def __ceil__(self): raise ValueError("no ceil")
    def __floor__(self): raise ValueError("no floor")
    def __trunc__(self): raise ValueError("no trunc")


# Numbers that are made the same way every time, of every size that a float can be
state = 12345


def bits(n):
    global state
    out = 0
    for _ in range((n + 30) // 31):
        state = (state * 1103515245 + 12345) % 2 ** 31
        out = out << 31 | state
    return out >> ((n + 30) // 31 * 31 - n)


def some_float(low=-1074, high=1023):
    return (1 - 2 * bits(1)) * (1 + bits(52) / 2 ** 52) * 2.0 ** (low + bits(20) % (high - low + 1))


def digest(values):
    "A number that depends on every character of every one of them"
    h = 0
    for v in values:
        for c in v if isinstance(v, str) else repr(v):
            h = (h * 1000003 + ord(c)) % (2 ** 61 - 1)
    return h


SPECIAL = (0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 2.0, -2.0, 1.5, -1.5, 0.1, 3.0, 10.0, 100.0, 170.5, 171.7, 200.5, -170.5, -200.5, 709.0, 710.0, -745.0, -746.0, 1e-300, 5e-324, -5e-324, 2.2250738585072014e-308, 1e-20, 1e-21, -1e-21, 1e16, 1e22, 1e300, 1.7976931348623157e308, -1.7976931348623157e308, inf, -inf, nan, math.pi, -math.pi, math.pi / 2, math.e, 0.9999999999999999, 1.0000000000000002, -0.9999999999999999, 2.5, 3.5, -2.5, 4503599627370496.5, 9007199254740993.0)
ONE = ("acos", "acosh", "asin", "asinh", "atan", "atanh", "cbrt", "cos", "cosh", "degrees", "erf", "erfc", "exp", "exp2", "expm1", "fabs", "gamma", "lgamma", "log", "log10", "log1p", "log2", "radians", "sin", "sinh", "sqrt", "tan", "tanh", "ulp", "ceil", "floor", "trunc", "frexp", "modf", "isfinite", "isinf", "isnan")
print("---- of one number")
for name in ONE:
    f = getattr(math, name)
    t(name, lambda: [attempt(f, x) for x in SPECIAL])
    t(name + " of what is not a float", lambda: [attempt(f, x) for x in (0, 1, -1, 2, True, False, 10 ** 20, 10 ** 400, -10 ** 400, Float(0.5), Index(1), F(0.5), I(1), Fraction(1, 2), "a", None, 1j, [], b"1", Raises(), Float("a"), Index("a"))])
    t(name + " called wrongly", lambda: [attempt(f), attempt(f, 1, 2, 3), attempt(lambda: f(x=1))])
for name, low, high in (("acos", -60, 0), ("acosh", 0, 1023), ("asin", -60, 0), ("asinh", -1074, 1023), ("atan", -1074, 1023), ("atanh", -60, -1), ("cbrt", -1074, 1023), ("cos", -60, 60), ("cosh", -60, 9), ("degrees", -1074, 1000), ("erf", -60, 4), ("erfc", -60, 5), ("exp", -60, 9), ("exp2", -60, 10), ("expm1", -60, 9), ("gamma", -70, 8), ("lgamma", -70, 1000), ("log", -1074, 1023), ("log10", -1074, 1023), ("log1p", -60, 1023), ("log2", -1074, 1023), ("radians", -1074, 1023), ("sin", -60, 60), ("sinh", -60, 9), ("sqrt", -1074, 1023), ("tan", -60, 60), ("tanh", -60, 6), ("ulp", -1074, 1023), ("frexp", -1074, 1023), ("modf", -60, 60), ("ceil", -5, 70), ("floor", -5, 70), ("trunc", -5, 70)):
    f = getattr(math, name)
    for part in range(4):
        t("%s at random %d" % (name, part), lambda: digest(attempt(f, some_float(low, high)) for _ in range(500)))
t("gamma and lgamma near the whole numbers", lambda: digest((attempt(math.gamma, n + d), attempt(math.lgamma, n + d)) for n in range(-180, 180) for d in (0.0, 1e-15, -1e-15, 0.5, 1e-9, 0.25, 0.999999)))

print("---- of two")
TWO = ("atan2", "copysign", "fmod", "pow", "remainder", "nextafter", "hypot", "log", "isclose")
PAIRS = [(x, y) for x in (0.0, -0.0, 1.0, -1.0, 0.5, 2.0, -2.0, 3.0, -3.0, 2.5, 1e308, -1e308, 5e-324, 1e-300, inf, -inf, nan) for y in (0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 2.0, -2.0, 3.0, -3.0, 1e308, 5e-324, 1023.0, 1024.0, -1075.0, inf, -inf, nan)]
for name in TWO:
    f = getattr(math, name)
    t(name, lambda: [attempt(f, x, y) for x, y in PAIRS])
    t(name + " of what is not a float", lambda: [attempt(f, *a) for a in ((1, 2), (True, 2), (10 ** 400, 1), (1, 10 ** 400), (Float(2.0), Index(3)), (F(2.0), I(3)), ("a", 1), (1, "a"), (None, 1), (1, None), (1j, 1), (Raises(), 1), (1, Raises()))])
    t(name + " called wrongly", lambda: [attempt(f), attempt(f, 1) if name not in ("hypot", "log") else None, attempt(f, 1, 2, 3) if name != "hypot" else None, attempt(lambda: f(x=1, y=2))])
    for part in range(4):
        t("%s at random %d" % (name, part), lambda: digest(attempt(f, some_float(-200, 200), some_float(-200, 200)) for _ in range(500)))
t("pow at random, where it is neither too large nor too small", lambda: digest(attempt(math.pow, abs(some_float(-8, 8)), some_float(-4, 6)) for _ in range(3000)))
t("remainder halfway", lambda: [math.remainder(x * 0.5, 1.0) for x in range(-9, 10)] + [math.remainder(x, 2.0) for x in (1.0, 3.0, 5.0, -1.0, -3.0)] + [math.remainder(5e-324 * k, 5e-324 * 2) for k in range(6)])
t("ldexp", lambda: [attempt(math.ldexp, x, i) for x in (0.0, -0.0, 1.0, -1.5, 5e-324, 1e308, inf, -inf, nan) for i in (0, 1, -1, 1023, 1024, -1074, -1075, 2000, -2000, 2 ** 31, -2 ** 31 - 1, 2 ** 63, -2 ** 63 - 1, 10 ** 30, -10 ** 30, True, I(3))])
t("ldexp of what will not do", lambda: [attempt(math.ldexp, *a) for a in ((), (1,), (1, 2, 3), (1, 1.0), (1, "a"), (1, None), (1, Index(2)), ("a", 1), (Raises(), 1), (10 ** 400, 1))])
t("ldexp at random", lambda: digest(math.ldexp(some_float(-100, 100), bits(12) - 2048) if True else 0 for _ in range(0)) or digest(attempt(math.ldexp, some_float(), bits(12) - 2048) for _ in range(2000)))
t("fma", lambda: [attempt(math.fma, *a) for a in ((2.0, 3.0, 4.0), (0.1, 10.0, -1.0), (1e308, 10.0, -inf), (1e308, 10.0, 0.0), (inf, 0.0, 1.0), (0.0, inf, nan), (inf, 1.0, -inf), (nan, 1.0, 1.0), (1.0, nan, 1.0), (1.0, 1.0, nan), (inf, 1.0, 1.0), (-0.0, 0.0, -0.0), (0.0, 0.0, -0.0), (5e-324, 0.5, 0.0), (1, 2, 3), (), (1, 2), (1, 2, 3, 4), ("a", 1, 1), (1, "a", 1), (1, 1, "a"))])
t("fma at random", lambda: digest(attempt(math.fma, some_float(-300, 300), some_float(-300, 300), some_float(-600, 600)) for _ in range(3000)))
t("nextafter by steps", lambda: [attempt(math.nextafter, x, y, steps=s) for x, y in ((1.0, 2.0), (1.0, 0.0), (0.0, 1.0), (0.0, -1.0), (-0.0, 0.0), (0.0, -0.0), (5e-324, -5e-324), (1.0, 1.0), (1.0, inf), (1.7976931348623157e308, inf), (inf, 0.0), (nan, 1.0), (1.0, nan), (-1.0, 1.0), (1e-320, -1e-320)) for s in (None, 0, 1, 2, 3, 10, 2 ** 52, 2 ** 62, 2 ** 63, 2 ** 64 - 1, 2 ** 64, 10 ** 30, True, Index(2))])
t("nextafter of what will not do", lambda: [attempt(math.nextafter, 1.0, 2.0, steps=s) for s in (-1, -10 ** 30, 1.5, "a", [], Raises())] + [attempt(math.nextafter, 1.0, 2.0, 3), attempt(lambda: math.nextafter(1.0, 2.0, other=1))])
t("isclose", lambda: [attempt(math.isclose, *a, **k) for a, k in (((1.0, 1.0 + 1e-10), {}), ((1.0, 1.0 + 1e-8), {}), ((1.0, 1.1), {"rel_tol": 0.1}), ((1.0, 1.1), {"rel_tol": 0.09}), ((0.0, 1e-10), {}), ((0.0, 1e-10), {"abs_tol": 1e-9}), ((inf, inf), {}), ((inf, -inf), {}), ((inf, 1e308), {"rel_tol": 1e300}), ((nan, nan), {}), ((1.0, nan), {"abs_tol": inf}), ((1, 1), {"rel_tol": -1}), ((1, 2), {"abs_tol": -1}), ((1, 1), {"rel_tol": nan}), ((1, 2), {"rel_tol": nan}), ((1, 2), {"rel_tol": "a"}), ((1, 2), {"abs_tol": None}), ((1, 2, 3), {}), ((1, 2), {"other": 1}), ((), {"a": 1, "b": 1}), ((1,), {"b": 1}), ((1, 2), {"rel_tol": Float(2.0), "abs_tol": Index(0)}))])

print("---- of many")
t("hypot", lambda: [attempt(math.hypot, *a) for a in ((), (3,), (-3,), (3, 4), (3.0, 4.0, 12.0), (1e308, 1e308), (1e308, 1e308, 1e308, 1e308), (5e-324, 5e-324), (5e-324,) * 3, (1e-320, 1e-320, 1e-321), (inf, nan), (nan, inf), (nan, 1), (-inf, 1), (0.0, -0.0), (1,) * 20, (1e-200, 1e200), (True, False), (Float(3.0), Index(4)), ("a",), (1, "a"), (10 ** 400,), (None,))] + [attempt(lambda: math.hypot(x=1))])
for n in (2, 3, 5, 16, 17, 100):
    t("hypot of %d at random" % n, lambda: digest(math.hypot(*[some_float(-30, 30) for _ in range(n)]) for _ in range(300)))
    t("hypot of %d of very different sizes" % n, lambda: digest(math.hypot(*[some_float(-1074, 1000) for _ in range(n)]) for _ in range(300)))
t("dist", lambda: [attempt(math.dist, *a) for a in (((), ()), ((1,), (4,)), ((1, 2), (4, 6)), ([1, 2], [4, 6]), (iter([1, 2]), (4, 6)), ((1.5, 2.5, 3.5), (0.5, 0.25, 0.125)), ((1, 2), (1,)), ((1,), (1, 2)), ((inf,), (inf,)), ((inf, 1), (1, nan)), ((nan,), (1,)), ((1e308,), (-1e308,)), (5, (1,)), ((1,), 5), (("a",), (1,)), ((1,), ("a",)), ((10 ** 400,), (1,)), ("ab", "cd"), (), ((1,),), ((1,), (2,), (3,)), ({1: 2}, {4: 5}), ((True,), (False,)))])
t("dist at random", lambda: digest(math.dist([some_float(-30, 30) for _ in range(n)], [some_float(-30, 30) for _ in range(n)]) for n in (1, 2, 3, 7, 16, 17, 40) for _ in range(150)))
t("fsum", lambda: [attempt(math.fsum, a) for a in ([], [1], [0.1] * 10, [1e100, 1.0, -1e100, 1e-100, 1e50, -1.0, -1e50], [1e308, 1e308, -1e308], [1e308, -1e308, 1e308], [1e16, 1.0, 1e-16], [1e-16, 1, 1e16], [inf], [inf, -inf], [inf, inf], [nan], [nan, inf], [inf, nan, -inf], [1, inf], [-inf, 1e308, 1e308], [2.0 ** 53, -0.5, -2.0 ** -54], [2.0 ** 53, 1.0, 2.0 ** -100], [2.0 ** 53 + 10.0, 1.0, 2.0 ** -100], [-0.0], [-0.0, -0.0], [0.0, -0.0], (1, 2, 3), iter([1.5, 2.5]), [True, False], [10 ** 20, 0.5], [10 ** 400], ["a"], [1, "a"], [None], 5, None, "ab", [Float(1.5), Index(2), F(0.25), I(3)], [Raises()], {1.5: 0}, range(100))] + [attempt(math.fsum), attempt(math.fsum, [], []), attempt(lambda: math.fsum(seq=[]))])
for n in (2, 10, 33, 100, 1000):
    t("fsum of %d at random" % n, lambda: digest(math.fsum(some_float(-500, 500) for _ in range(n)) for _ in range(60)))
t("fsum of what cancels", lambda: digest(math.fsum(v for x in [[some_float(-300, 300) for _ in range(40)]] for v in x + [-y for y in x[:39]]) for _ in range(100)))
t("prod", lambda: [attempt(math.prod, *a, **k) for a, k in ((([],), {}), (([],), {"start": 5}), (([],), {"start": "a"}), (([1, 2, 3],), {}), (([1.5, 2, 3],), {}), (([2, 1.5, 3],), {}), (([2 ** 40, 2 ** 40],), {}), (([2 ** 62, 2],), {}), (([2 ** 62, 2, -1],), {}), (([-2 ** 63, -1],), {}), (([3037000500, 3037000500],), {}), (([10 ** 30, 2],), {}), (([1.5, 10 ** 30],), {}), (([1.5, 10 ** 400],), {}), (([1e308, 10],), {}), (([1e308, 10.0, 0.0],), {}), (([nan, 1],), {}), (([True, 2],), {}), (([2, True],), {}), (([1.5, True],), {}), (([Fraction(1, 2), 4],), {}), (([2, Fraction(1, 2)],), {}), (([1.5, Fraction(1, 2)],), {}), (([1j, 2],), {}), (([[1], 2],), {}), ((["a", 3],), {}), ((["a", "b"],), {}), (([2],), {"start": 1.5}), (([2],), {"start": [1]}), (([1.5],), {"start": I(2)}), (([I(2), F(1.5)],), {}), ((5,), {}), ((None,), {}), ((), {}), (([], 1), {}), (([],), {"other": 1}), ((range(1, 30),), {}), ((iter([1, 2]),), {}), (({2: 0, 3: 0},), {}), (([Raises()],), {}))])
t("prod at random", lambda: (digest(math.prod(some_float(-20, 20) for _ in range(20)) for _ in range(300)), digest(math.prod(bits(20) - 2 ** 19 for _ in range(n)) for n in range(1, 40)), digest(math.prod([some_float(-5, 5) if bits(1) else bits(10) for _ in range(12)]) for _ in range(300))))
t("sumprod", lambda: [attempt(math.sumprod, *a) for a in (([], []), ([1, 2], [3, 4]), ([1.5, 2.5], [3.5, 4.5]), ([1, 2.5], [3.5, 4]), ([1.5, 2], [True, False]), ([True, False], [1.5, 2.5]), ([True], [True]), ([1], [1, 2]), ([1, 2], [1]), ([], [1]), ([2 ** 62, 2 ** 62], [2, 2]), ([2 ** 31, 2 ** 31], [2 ** 31, 2 ** 31]), ([2 ** 62], [1] ), ([10 ** 30], [2]), ([1, 10 ** 30, 1.5], [2, 2, 2]), ([1.5, 10 ** 400], [1.0, 1.0]), ([10 ** 400, 1.5], [1.0, 1.0]), ([1e308, 1e308], [10.0, -10.0]), ([1e308, -1e308], [10.0, 10.0]), ([inf, 1.0], [1.0, 1.0]), ([inf, -inf], [1.0, 1.0]), ([nan], [1.0]), ([inf], [0.0]), ([0.1] * 10, [1] * 10), ([0.1] * 10, [1.0] * 10), ([1e100, 1.0, -1e100], [1.0, 1.0, 1.0]), ([Fraction(1, 2), 1], [4, 1.5]), ([1, Fraction(1, 2)], [1.5, 4]), ([1j], [2]), (["a"], [3]), (["a"], ["b"]), ([[1]], [2]), ([F(1.5)], [2.0]), ([I(2)], [3]), ([I(2)], [1.5]), (5, []), ([], 5), (None, None), (iter([1, 2]), iter([3, 4])), ((1, 2), [3, 4]), ({1: 0}, {2: 0}), ("ab", [1, 2]), ([Raises()], [1]), (), ([],), ([], [], []))])
t("sumprod at random", lambda: (digest(math.sumprod([some_float(-40, 40) for _ in range(n)], [some_float(-40, 40) for _ in range(n)]) for n in (1, 2, 5, 20, 100) for _ in range(100)), digest(math.sumprod([bits(40) - 2 ** 39 for _ in range(n)], [bits(30) - 2 ** 29 for _ in range(n)]) for n in range(1, 60)), digest(math.sumprod([some_float(-9, 9) if bits(1) else bits(9) for _ in range(15)], [some_float(-9, 9) if bits(1) else bits(9) for _ in range(15)]) for _ in range(300))))


def stops_and_goes_on():
    seen = []

    def g(name, n):
        for i in range(n):
            seen.append((name, i))
            yield i
    return attempt(math.sumprod, g("p", 2), g("q", 3)), seen


t("what is asked for, and in what order", stops_and_goes_on)

print("---- whole numbers")
INTS = (0, 1, -1, 2, -2, 6, 12, -12, 18, 2 ** 31, 2 ** 32, 2 ** 63, 2 ** 64, -2 ** 64, 10 ** 30, 2 ** 100 * 3 ** 20, 2 ** 90 * 5 ** 10, True, False, I(12), Index(18))
for f in (math.gcd, math.lcm):
    t(f.__name__, lambda: [f(), f(0), f(5), f(-5), f(I(-5)), type(f(I(5))).__name__, type(f(True)).__name__, f(Index(-7))] + [f(a, b) for a in INTS for b in INTS] + [f(12, 18, 24), f(12, 18, 24, 7), f(0, 0, 0), f(1, 10 ** 50, 3), f(0, 5, 0), f(*range(1, 30)), f(2 ** 200, 2 ** 150 * 3, 2 ** 100 * 9)])
    t(f.__name__ + " of what will not do", lambda: [attempt(f, *a) for a in ((1.5,), (1, 1.5), ("a",), (1, "a"), (None,), (1, 2, None), (1, 1, "a"), (0, 0, "a"), (Raises(),), (1, Raises()), (Fraction(1, 2),), (1.0, 2.0))] + [attempt(lambda: f(x=1))])
    t(f.__name__ + " at random", lambda: digest(f(bits(n) * bits(9), bits(m) * bits(9)) for n in (5, 30, 62, 64, 100, 300) for m in (5, 30, 62, 64, 100, 300) for _ in range(12)))
t("isqrt", lambda: [math.isqrt(n) for n in (0, 1, 2, 3, 4, 8, 9, 15, 16, 17, 99, 100, 2 ** 31, 2 ** 32 - 1, 2 ** 32, 2 ** 62, 2 ** 63 - 1, 2 ** 63, 2 ** 64 - 1, 2 ** 64, 2 ** 64 + 1, 2 ** 65, 10 ** 30, 10 ** 100, 10 ** 100 - 1, (10 ** 50) ** 2, (10 ** 50) ** 2 - 1, (10 ** 50) ** 2 + 1, True, I(17), Index(17))] + [type(math.isqrt(I(16))).__name__, type(math.isqrt(True)).__name__])
t("isqrt is what it should be", lambda: all(r * r <= n < (r + 1) * (r + 1) for k in range(1, 700, 7) for n in [bits(k)] for r in [math.isqrt(n)]) and digest(math.isqrt(bits(k)) for k in range(1, 3000, 13)))
t("isqrt about the squares", lambda: all(math.isqrt(s * s + d) == s - (d < 0) for k in (10, 31, 32, 33, 63, 64, 65, 200, 1000) for s in [bits(k) | 1 << (k - 1)] for d in (-1, 0, 1)))
t("isqrt of what will not do", lambda: [attempt(math.isqrt, *a) for a in ((), (-1,), (-10 ** 30,), (1.0,), ("a",), (None,), (1, 2), (Raises(),), (Fraction(4, 1),))] + [attempt(lambda: math.isqrt(n=1))])
t("factorial", lambda: [math.factorial(n) for n in list(range(0, 30)) + [50, 100, True, I(5), Index(5)]] + [type(math.factorial(I(3))).__name__])
t("factorial of more", lambda: digest(math.factorial(n) for n in (127, 128, 129, 255, 256, 257, 500, 1000, 1023, 1024, 2000, 5000)))
t("factorial of what will not do", lambda: [attempt(math.factorial, *a) for a in ((), (-1,), (-10 ** 30,), (2 ** 63,), (10 ** 30,), (1.0,), (5.5,), ("a",), (None,), (1, 2), (Raises(),))] + [attempt(lambda: math.factorial(n=1))])
for f in (math.comb, math.perm):
    t(f.__name__, lambda: [[f(n, k) for k in range(0, n + 3)] for n in range(0, 12)])
    t(f.__name__ + " of more", lambda: digest(f(n, k) for n in (20, 34, 61, 62, 63, 66, 67, 68, 100, 116, 117, 127, 128, 129, 200, 1000, 3329022, 3329023, 4294967296, 4294967297, 2 ** 62, 2 ** 63 - 1) for k in (0, 1, 2, 3, 5, 8, 13, 14, 16, 20, 21, 33, 34, 35, 60) if k <= n))
    t(f.__name__ + " of very large numbers", lambda: [f(2 ** 63, 0), f(2 ** 63, 1), f(2 ** 63, 2), f(10 ** 30, 3), f(10 ** 30, 10 ** 30 + 1), f(2 ** 64, 5), f(I(10), I(3)), f(True, True), f(Index(10), Index(3)), type(f(I(5), 1)).__name__, type(f(I(5), 0)).__name__])
    t(f.__name__ + " of what will not do", lambda: [attempt(f, *a) for a in ((), (5, 2, 1), (-1, 1), (1, -1), (-1, -1), (-10 ** 30, 1), (1, -10 ** 30), (1.0, 1), (1, 1.0), ("a", 1), (1, "a"), (None, 1), (Raises(), 1), (1, Raises()), (10 ** 30, 10 ** 25), (2 ** 64, 2 ** 63))] + [attempt(lambda: f(n=5, k=2))])
t("comb from the other end", lambda: [math.comb(10 ** 30, 10 ** 30), math.comb(10 ** 30, 10 ** 30 - 1), math.comb(10 ** 30, 10 ** 30 - 2), math.comb(2 ** 63, 2 ** 63 - 2), math.comb(100, 98), math.comb(2 ** 62, 2 ** 62 - 3)])
t("perm of one", lambda: [attempt(math.perm, *a) for a in ((5,), (5, None), (0,), (20,), (25,), (-1,), (1.0,), ("a",), (2 ** 63,), (I(5),), (True,))])

print("---- the logarithm of an int that is too large to be a float")
for f in (math.log, math.log2, math.log10):
    t(f.__name__, lambda: [f(n) for n in (1, 2, 10, 2 ** 52, 2 ** 53 + 1, 2 ** 1023, 2 ** 1024 - 1, 2 ** 1024 - 2 ** 970, 2 ** 1024 - 2 ** 970 - 1, 2 ** 1024, 2 ** 1024 + 1, 10 ** 308, 10 ** 309, 10 ** 1000, 2 ** 10000, 2 ** 10000 - 1, 3 ** 5000, 10 ** 10000, True, I(10 ** 400))] + [attempt(f, n) for n in (0, -1, -10 ** 400, False, I(0))])
    t(f.__name__ + " at random", lambda: digest(f(bits(k) | 1 << (k - 1)) for k in range(1000, 4000, 17)))
t("log to a base", lambda: [attempt(math.log, *a) for a in ((8, 2), (100, 10), (10 ** 400, 10), (10 ** 400, 10 ** 200), (2, 10 ** 400), (8.0, 2.0), (1, 1), (2, 1), (2, 1.0), (0, 2), (2, 0), (2, -1), (-1, 2), (2.0, 0.0), (0.0, 2.0), (inf, 2), (2, inf), (nan, 2), (2, nan), (2, None), (2, "a"), ("a", 2), (2, 0.5), (1e-300, 1e300))])

print("---- __ceil__, __floor__ and __trunc__")


class Rounds:
    def __ceil__(self): return "ceil"
    def __floor__(self): return "floor"
    def __trunc__(self): return "trunc"


class OnInstance:
    pass


o = OnInstance()
o.__ceil__ = o.__floor__ = o.__trunc__ = lambda: "not looked for here"


class NotCallable:
    __ceil__ = __floor__ = __trunc__ = 5


class IsNone(float):
    __ceil__ = __floor__ = __trunc__ = None


class FloatRounds(float):
    def __ceil__(self): return "derived ceil"
    def __floor__(self): return "derived floor"
    def __trunc__(self): return "derived trunc"


for f in (math.ceil, math.floor, math.trunc):
    t(f.__name__, lambda: [attempt(f, x) for x in (Rounds(), o, NotCallable(), IsNone(1.5), FloatRounds(1.5), F(1.5), F(-1.5), Float(1.5), Index(3), I(3), True, 5, 10 ** 30, Fraction(7, 2), Fraction(-7, 2), 1e300, -1e300, 2.0 ** 62, 2.0 ** 63, -2.0 ** 63, 2.0 ** 64)] + [type(f(x)).__name__ for x in (1.5, F(1.5), 5, True, I(5), 1e30)])
