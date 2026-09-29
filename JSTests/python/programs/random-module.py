# The module _random, which is the Mersenne Twister, and random, which is written in Python over it. From the same seed come the same numbers.
import _random
import random


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


def digest(values):
    h = 0
    for v in values:
        for c in repr(v):
            h = (h * 1000003 + ord(c)) % (2 ** 61 - 1)
    return h


R = _random.Random
print("---- what there is")
t("the module", lambda: (_random.__name__, _random.__package__, _random.__loader__.__name__, _random.__doc__, sorted(n for n in vars(_random) if not n.startswith("__"))))
t("Random", lambda: (R.__name__, R.__module__, R.__qualname__, [b.__name__ for b in R.__mro__], sorted(vars(R)), R.__doc__, R.__text_signature__, R.__basicsize__, R.__flags__ & 0x7FFF, repr(R), attempt(setattr, R, "x", 1), attempt(delattr, R, "x")))
for name in sorted(vars(R)):
    x = vars(R)[name]
    t("Random." + name, lambda: (type(x).__name__, getattr(x, "__text_signature__", None), x.__doc__ if not isinstance(x, str) else None))


class Index:
    def __init__(self, v): self.v = v
    def __index__(self): return self.v


class I(int):
    def __abs__(self): return "not asked"
    def __hash__(self): return 5


class Hashes:
    def __init__(self, v): self.v = v
    def __hash__(self): return self.v


class NoHash:
    __hash__ = None


print("---- from a seed")
# Not a str or bytes, which are taken for what they hash to, and that is different every time that CPython is started.
for seed in (0, 1, -1, 2, 42, 2 ** 31, 2 ** 32 - 1, 2 ** 32, 2 ** 32 + 1, 2 ** 63, 2 ** 64 - 1, 2 ** 64, 2 ** 64 + 1, -2 ** 64, 10 ** 30, 2 ** 96, 2 ** 20000 + 7, 2 ** (32 * 624), 2 ** (32 * 625) + 1, True, False, I(7), I(-7), 1.5, 0.0, -0.0, (1, 2), (), frozenset(), Hashes(7), Hashes(-7), Hashes(0), Hashes(2 ** 60), 1j, float("inf")):
    t("seed %s" % ("of %d bits" % seed.bit_length() if type(seed) is int and seed.bit_length() > 200 else ascii(seed) if type(seed) not in (Hashes, I) else "%s(%d)" % (type(seed).__name__, seed if isinstance(seed, int) else seed.v)), lambda: [(r.random(), r.random(), r.getrandbits(32), r.getrandbits(64), r.getrandbits(7), digest(r.getstate())) for r in [R(seed)]])
t("the same by seed()", lambda: [(a.seed(s), a.getstate() == R(s).getstate())[1] for a in [R()] for s in (0, 5, -5, 10 ** 40, (1, 2), 1.5)])
t("without its sign", lambda: [R(n).getstate() == R(-n).getstate() for n in (1, 2 ** 32, 10 ** 30)])
t("what cannot be a seed", lambda: [attempt(R, x) for x in ([], {}, NoHash(), set())] + [attempt(R().seed, x) for x in ([], NoHash())])
t("with none, each is different", lambda: (R().random() != R().random(), R(None).getstate() != R(None).getstate(), [(r.seed(), r.random() != 0.8444218515250481)[1] for r in [R(0)]], [(r.seed(None), 0 <= r.random() < 1)[1] for r in [R(0)]]))
t("how it is made", lambda: [attempt(R, *a, **k) if not isinstance(attempt(R, *a, **k), R) else "made" for a, k in (((), {}), ((1,), {}), ((1, 2), {}), ((), {"seed": 1}), ((), {"x": 1}), ((1,), {"x": 1}), ((None,), {}))])
t("made again", lambda: [(r.random(), r.__init__(5), r.random(), attempt(r.__init__, 1, 2), attempt(lambda: r.__init__(x=1)), attempt(r.__init__, [])) for r in [R(5)]])
t("__new__ alone", lambda: [(r.getstate()[:3], r.getstate()[-1], r.random(), r.getrandbits(8)) for r in [R.__new__(R)]] + [attempt(R.__new__), attempt(R.__new__, int), type(R.__new__(R, "anything", at="all")).__name__])

print("---- a good many")
t("random()", lambda: [digest(r.random() for _ in range(5000)) for r in [R(12345)]])
t("past where the words run out", lambda: [(digest(r.getrandbits(32) for _ in range(623)), r.getstate()[-1], r.getrandbits(32), r.getstate()[-1], r.getrandbits(32), r.getstate()[-1], digest(r.getstate())) for r in [R(1)]])
t("getrandbits", lambda: [[r.getrandbits(k) for k in (0, 1, 2, 7, 8, 31, 32, 33, 63, 64, 65, 95, 96, 97, 127, 128, 129, 1000)] for r in [R(99)]])
t("it has no more bits than that", lambda: [all(r.getrandbits(k).bit_length() <= k for k in range(200) for _ in range(20)) for r in [R(3)]])
t("of every size", lambda: [digest(r.getrandbits(k) for k in range(0, 3000, 7)) for r in [R(4)]])
t("what it is", lambda: [type(R(1).getrandbits(k)).__name__ for k in (0, 1, 31, 32, 64, 100)] + [R(1).getrandbits(True), R(1).getrandbits(Index(8))])
t("getrandbits of what will not do", lambda: [attempt(R(1).getrandbits, *a) for a in ((), (-1,), (-2 ** 70,), (1.5,), ("a",), (None,), (1, 2), (2 ** 64,), (2 ** 64 - 1,), (2 ** 63,), (2 ** 62,), (2 ** 100,))] + [attempt(lambda: R(1).getrandbits(k=1))])

print("---- the state")
t("getstate", lambda: [(type(s).__name__, len(s), s[:4], s[-3:], {type(x).__name__ for x in s}, max(s[:-1]) < 2 ** 32, min(s) >= 0) for s in [R(7).getstate()]] + [attempt(R().getstate, 1)])
t("setstate", lambda: [(a.random(), b.setstate(a.getstate()), a.random() == b.random(), [a.getrandbits(50) == b.getrandbits(50) for _ in range(5)]) for a in [R(1)] for b in [R(2)]])
GOOD = R(7).getstate()
t("setstate of what will not do", lambda: [attempt(R().setstate, *a) for a in ((), (5,), (None,), ([],), (list(GOOD),), ((),), (GOOD[:-1],), (GOOD + (1,),), (GOOD[:-1] + (-1,),), (GOOD[:-1] + (625,),), (GOOD[:-1] + (624,),), (GOOD[:-1] + (0,),), (GOOD[:-1] + ("a",),), (GOOD[:-1] + (1.5,),), (GOOD[:-1] + (2 ** 63,),), (GOOD[:-1] + (Index(3),),), (("a",) + GOOD[1:],), ((1.5,) + GOOD[1:],), ((None,) + GOOD[1:],), ((-1,) + GOOD[1:],), ((2 ** 64,) + GOOD[1:],), ((Index(3),) + GOOD[1:],), (GOOD, 1))])
t("what has more than 32 bits is cut down", lambda: [(r.setstate((2 ** 32 + 5, 2 ** 63, True) + GOOD[3:]), r.getstate()[:4]) for r in [R()]])
t("nothing is changed by what will not do", lambda: [(attempt(r.setstate, GOOD[:-2] + ("a", 5)), attempt(r.setstate, GOOD[:-1] + (700,)), r.getstate() == R(1).getstate()) for r in [R(1)]])
t("a tuple of a derived class", lambda: [(r.setstate(type("T", (tuple,), {})(GOOD)), r.getstate() == GOOD) for r in [R()]])

print("---- classes derived from it")


class D(R):
    pass


class WithInit(R):
    def __init__(self, a, b=2): self.a, self.b = a, b


class WithNew(R):
    def __new__(cls, *a, **k): return super().__new__(cls)


t("derived", lambda: (D(5).random() == R(5).random(), attempt(lambda: D(x=1)), attempt(D, 1, 2), [(d.__dict__, setattr(d, "y", 1), d.y) for d in [D(1)]], type(D(1)).__mro__[1] is R))
t("with __init__() of its own", lambda: [(w.a, w.b, w.getstate()[-1], w.random()) for w in [WithInit(1, b=3)]])
t("with __new__() of its own, it may be given anything by name", lambda: [type(attempt(lambda: WithNew(x=1))).__name__, attempt(WithNew, 1, 2), WithNew(5).random() == R(5).random()])
t("what it has", lambda: (attempt(setattr, R(), "x", 1), attempt(getattr, R(), "__dict__"), attempt(hash, R()) is not None, R(1) == R(1), attempt(lambda: R(1) < R(1)), repr(R()).split(" at ")[0]))

print("---- random")
random.seed(2024)
t("the functions", lambda: (random.random(), random.uniform(1, 10), random.randint(1, 6), random.randrange(0, 100, 7), random.choice("abcdef"), random.choices("abc", k=5), random.choices("abc", weights=[1, 2, 3], k=5), random.sample(range(100), 5), random.getrandbits(40), random.randbytes(8), random.triangular(0, 10, 3)))
t("shuffle", lambda: [(random.shuffle(x), x)[1] for x in [list(range(20))]])
t("the distributions", lambda: (random.gauss(0, 1), random.normalvariate(0, 1), random.lognormvariate(0, 1), random.expovariate(1.5), random.vonmisesvariate(0, 2), random.gammavariate(2, 3), random.gammavariate(0.5, 3), random.gammavariate(1, 3), random.betavariate(2, 3), random.paretovariate(2), random.weibullvariate(1, 2), random.binomialvariate(10, 0.3), random.binomialvariate(1000, 0.4), random.binomialvariate(100, 0.99)))
t("a good many of each", lambda: [digest(f() for _ in range(400)) for f in (random.random, lambda: random.randint(1, 10 ** 30), lambda: random.randrange(7), lambda: random.gauss(0, 1), lambda: random.gammavariate(3.5, 2), lambda: random.betavariate(0.5, 0.5), lambda: random.vonmisesvariate(1, 4), lambda: random.binomialvariate(50, 0.5), lambda: random.sample(range(50), 7), lambda: random.choices(range(9), cum_weights=range(1, 10), k=3))])
# A str or bytes is put through hashlib.sha512() first, and hashlib is for whatever the engine is part of to supply.
for seed in (0, 1, 1.5, 10 ** 50, -5):
    t("seed(%r)" % (seed,), lambda: (random.seed(seed), random.random(), random.randint(0, 10 ** 20)))
t("seed, version 1", lambda: [(random.seed(s, version=1), random.random())[1] for s in ("a string", b"bytes", 5, "")])
t("seed of what will not do", lambda: [attempt(random.seed, s) for s in ([], {}, (1, 2), object)])
t("getstate and setstate", lambda: [(random.random(), random.setstate(s), random.random(), s[0], len(s[1]), s[2]) for _ in [random.seed(9)] for s in [random.getstate()]])
t("an instance of its own", lambda: [(r.random(), r.randint(1, 100), r.gauss(0, 1), r.gauss(0, 1), type(r).__mro__[1].__name__) for r in [random.Random(77)]])
t("SystemRandom", lambda: [(0 <= s.random() < 1, s.getrandbits(100).bit_length() <= 100, 1 <= s.randint(1, 6) <= 6, len(s.randbytes(5)), attempt(s.getstate), s.seed(1)) for s in [random.SystemRandom()]])
