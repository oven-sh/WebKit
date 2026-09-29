# The module cmath. Every function, of every pair out of a good many numbers that are large, small, near to where something changes, infinite or not numbers at all, to the last bit.
import binascii
import cmath
import math
import random
import warnings

warnings.simplefilter("ignore")


def attempt(f, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)


def t(label, f):
    r = attempt(f)
    print(label, "=>", r if isinstance(r, str) else ascii(r))


def bits(r):
    "All of a result, the sign of a nought and of a NaN included"
    if isinstance(r, complex):
        return (r.real.hex() if r.real == r.real else "nan", r.imag.hex() if r.imag == r.imag else "nan")
    if isinstance(r, float):
        return r.hex() if r == r else "nan"
    if isinstance(r, tuple):
        return tuple(bits(x) for x in r)
    return r


print("---- what there is")
names = sorted(n for n in vars(cmath) if not n.startswith("__"))
t("the module", lambda: (cmath.__name__, cmath.__package__, cmath.__loader__.__name__, cmath.__doc__, names))
for name in names:
    x = getattr(cmath, name)
    t(name, lambda: (type(x).__name__, x.__text_signature__, x.__doc__, x.__module__) if callable(x) else (type(x).__name__, bits(x), repr(x), math.copysign(1, x.imag if isinstance(x, complex) else x)))

ONE = "acos acosh asin asinh atan atanh cos cosh exp log log10 sin sinh sqrt tan tanh phase polar isfinite isinf isnan".split()
inf, nan = float("inf"), float("nan")
SPECIAL = [0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 2.0, -2.0, inf, -inf, nan, 1e-308, -1e-308, 5e-324, -5e-324, 2.2250738585072014e-308, 1.7976931348623157e308, -1.7976931348623157e308, 8.98846567431158e307, 4.4942328371557893e307, 1.3407807929942596e154, 1e154, 1e-154, 1.4916681462400413e-154,
           709.0, 709.78, 710.0, 710.5, 711.0, -709.0, -745.0, -746.0, 1e16, 1e-16, 0.71, 1.73, 0.7071067811865476, 1.0000000000000002, 0.9999999999999999, math.pi, math.pi / 2, math.pi / 4, 3 * math.pi / 4, math.e, 1e5, 1e-5, 6.0, 0.3, 100.0, 354.0, 355.0, 22.0, 1e300, 1e-300]
print("---- of everything with everything:", len(SPECIAL) ** 2, "numbers")
for name in ONE:
    f = getattr(cmath, name)
    results = [bits(attempt(f, complex(a, b))) for a in SPECIAL for b in SPECIAL]
    t(name, lambda: (binascii.crc32(repr(results).encode()), sum(isinstance(r, str) for r in results), sorted({r for r in results if isinstance(r, str)}), results[:3], results[500:503]))
results = [bits(attempt(cmath.rect, a, b)) for a in SPECIAL for b in SPECIAL]
t("rect", lambda: (binascii.crc32(repr(results).encode()), sum(isinstance(r, str) for r in results), sorted({r for r in results if isinstance(r, str)}), results[:3], results[500:503]))
few = SPECIAL[:14] + [1e300, 1e-300, 10.0]
results = [bits(attempt(cmath.log, complex(a, b), complex(c, d))) for a in few for b in few[:8] for c in few for d in few[:8]]
t("log to a base", lambda: (binascii.crc32(repr(results).encode()), sum(isinstance(r, str) for r in results), sorted({r for r in results if isinstance(r, str)})))

print("---- at random")
rng = random.Random(2718)
for scale in (1.0, 1e-3, 30.0, 700.0, 1e10, 1e150, 1e300, 1e-150, 1e-300):
    zs = [complex(rng.uniform(-scale, scale), rng.uniform(-scale, scale)) for _ in range(400)]
    t("about %g" % scale, lambda: [(name, binascii.crc32(repr([bits(attempt(getattr(cmath, name), z)) for z in zs]).encode())) for name in ONE[:18]] + [binascii.crc32(repr([bits(attempt(cmath.rect, z.real, z.imag)) for z in zs]).encode())])

print("---- what they are given")


class C:
    def __init__(self, v): self.v = v
    def __complex__(self): return self.v() if callable(self.v) else self.v


class F:
    def __init__(self, v): self.v = v
    def __float__(self): return self.v


class I:
    def __init__(self, v): self.v = v
    def __index__(self): return self.v


THINGS = (1, -1, 0, True, 2 ** 70, 2 ** 2000, 1.5, C(1 + 2j), C(1.5), C("a"), C(lambda: 1 / 0), C(type("S", (complex,), {})(3j)), F(2.5), F("a"), I(3), I(2 ** 2000), type("S", (complex,), {})(1 + 1j), type("S", (float,), {})(0.5), type("S", (int,), {})(2), "1", b"1", None, [], object)
for name in ONE:
    f = getattr(cmath, name)
    t(name, lambda: [bits(attempt(f, v)) for v in THINGS] + [attempt(f), attempt(f, 1, 2, 3), attempt(f, z=1)])
t("rect", lambda: [bits(attempt(cmath.rect, a, b)) for a, b in ((1, 0), (1, 1), (2 ** 70, 0), (2 ** 2000, 0), (0, 2 ** 2000), (F(1.5), F(0.5)), (I(2), I(0)), (1j, 0), (0, 1j), ("1", 0), (0, None), (C(1j), 0), (True, False))] + [attempt(cmath.rect), attempt(cmath.rect, 1), attempt(cmath.rect, 1, 2, 3), attempt(cmath.rect, r=1, phi=0)])
t("log", lambda: [bits(attempt(cmath.log, *a)) for a in ((1,), (0,), (0j,), (-0.0,), (10, 10), (8, 2), (1, 1), (2, 1), (2, 0), (0, 2), (0, 0), (1, 0), (2, None), (2, "a"), (2, C(2j)), (2, F(2.0)), (C(lambda: 1 / 0), 2), (2, C(lambda: 1 / 0)), (0, C(lambda: 1 / 0)), (inf, 2), (2, inf), (nan, 2), (2, nan), (1e308 + 1e308j, 1e-308))] + [attempt(cmath.log), attempt(cmath.log, 1, 2, 3), attempt(cmath.log, z=1), attempt(cmath.log, 1, base=2)])

print("---- isclose")
t("isclose", lambda: [attempt(cmath.isclose, *a, **k) for a, k in (((1, 1), {}), ((1, 1 + 1e-10), {}), ((1, 1 + 1e-8), {}), ((1j, 1j + 1e-10), {}), ((1, 2), {}), ((1, 2), {"rel_tol": 0.5}), ((1, 2), {"rel_tol": 0.49}), ((1, 2), {"abs_tol": 1}), ((1, 2), {"abs_tol": 0.99}), ((0, 1e-10), {}), ((0, 1e-10), {"abs_tol": 1e-9}), ((inf, inf), {}), ((inf, -inf), {}), ((complex(inf, 1), complex(inf, 1)), {}), ((complex(inf, 1), complex(inf, 2)), {}),
                                                                   ((inf, 1e308), {"rel_tol": 1e308}), ((nan, nan), {}), ((nan, 1), {"abs_tol": inf}), ((1, 2), {"abs_tol": inf}), ((1, 2), {"rel_tol": inf}), ((1, 2), {"rel_tol": nan}), ((1, 1), {"rel_tol": nan}), ((1, 2), {"rel_tol": -1}), ((1, 2), {"abs_tol": -1}), ((1, 1), {"rel_tol": -1}), ((1, 2), {"rel_tol": -0.0}), ((1e308 + 1e308j, -1e308 - 1e308j), {}), ((1e308, 1e308j), {"rel_tol": 2}),
                                                                   ((1,), {}), ((), {}), ((1, 2, 3), {}), ((1, 2), {"x": 1}), ((), {"a": 1, "b": 1}), ((1,), {"b": 1}), ((1, 2), {"rel_tol": "a"}), ((1, 2), {"abs_tol": None}), ((1, 2), {"rel_tol": 1j}), (("a", 1), {}), ((1, None), {}), ((1, 2), {"rel_tol": F(1.0)}), ((1, 2), {"rel_tol": I(1)}), ((C(1j), C(1j)), {}), ((1, 2), {"rel_tol": 2 ** 2000}))])
