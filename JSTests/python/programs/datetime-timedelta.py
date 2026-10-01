# datetime.timedelta
import _datetime, datetime, pickle, copy, operator, fractions, sys, hashlib
from datetime import timedelta as td
import re; address = re.compile('0x[0-9a-f]+')
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
    line = "%s -> %s" % (label, address.sub("0x", ascii(r)))
    if together is None or len(sys.argv) > 1: print(line)
    if together is not None: together.append(line)
# What comes of a great many tries is printed only if there is an argument. Otherwise it is what they come to, so many at a time.
together = None
def come_to(label):
    print(label, len(together), hashlib.md5("\n".join(together).encode()).hexdigest())
    together.clear()
print(datetime.timedelta is _datetime.timedelta)
class I(int): pass
class IM(int):
    def __mul__(self, o): return "IM.__mul__"
    def __rmul__(self, o): return "IM.__rmul__"
class F(float): pass
class FR(float):
    def as_integer_ratio(self): return self.r
class Index:
    def __index__(self): return 3
    def __repr__(self): return "Index()"
class T(td): pass
class TN(td):
    def __new__(cls, *a, **k):
        r = td.__new__(cls, *a, **k); r.extra = "x"; return r
print("===== making one")
values = [0, 1, -1, 2, 7, 59, 60, 61, 999, 1000, 1001, 3599, 3600, 86399, 86400, 86401, 999999, 1000000, 10**9, -10**9, 10**12, 2**31, 2**63, 2**64, 10**30, True, False, 0.0, -0.0, 0.5, 1.5, -0.5, 2.5, 1e-7, 5e-7, 1.5e-6, 2.5e-6, 0.1, 1 / 3, 1e9, 1e15, 1e16, 1e30, 1e300,
          float("inf"), float("-inf"), float("nan"), I(5), IM(5), F(1.5), None, "1", b"1", [], 1j, fractions.Fraction(1, 2), Index()]
for name in ("days", "seconds", "microseconds", "milliseconds", "minutes", "hours", "weeks"):
    for v in values:
        attempt("%s=%r" % (name, v), lambda: (lambda t: (t.days, t.seconds, t.microseconds))(td(**{name: v})))
attempt("none", lambda: td())
attempt("positional", lambda: td(1, 2, 3, 4, 5, 6, 7))
attempt("too many", lambda: td(1, 2, 3, 4, 5, 6, 7, 8))
attempt("unknown keyword", lambda: td(years=1))
attempt("twice", lambda: td(1, days=1))
attempt("keyword that is near", lambda: td(day=1))
attempt("the same one for nothing", lambda: (td() is td(0), td(0) is td(seconds=0), T() is T(), td(1) is td(1), td(1) - td(1) is td()))
for a in ((0.5, 0.5), (1.5, 0.5), (0.25, 0.25), (2.5, 0), (3.5, 0), (-0.5, 0), (-1.5, 0), (0.5, 1), (0.5, 2), (1e-6, 1e-6), (0.4, 0.1), (0.3, 0.2), (0.7, 0.8)):
    attempt("halves %r" % (a,), lambda: td(microseconds=a[0], milliseconds=a[1] / 1000))
    attempt("   and seconds", lambda: td(microseconds=a[0], seconds=a[1] / 1000000))
for a in (dict(days=1.5, hours=1.5, minutes=1.5, seconds=1.5, milliseconds=1.5, microseconds=1.5, weeks=1.5), dict(days=0.1, hours=0.1, minutes=0.1, seconds=0.1, milliseconds=0.1, microseconds=0.1, weeks=0.1), dict(days=-0.1, hours=0.1, minutes=-0.1, seconds=0.1),
          dict(days=999999999, hours=23, minutes=59, seconds=59, microseconds=999999), dict(days=999999999, hours=24), dict(days=-999999999), dict(days=-999999999, microseconds=-1), dict(weeks=142857142, days=5), dict(weeks=142857142, days=6), dict(days=10**9), dict(days=-10**9),
          dict(seconds=86400 * 999999999 + 86399), dict(seconds=86400 * 10**9), dict(microseconds=86400 * 10**6 * 10**9 - 1), dict(microseconds=86400 * 10**6 * 10**9), dict(microseconds=-86400 * 10**6 * 999999999), dict(microseconds=-86400 * 10**6 * 999999999 - 1)):
    attempt("together %r" % (sorted(a.items()),), lambda: repr(td(**a)))
print("===== shown")
samples = [td(), td(1), td(-1), td(2), td(-2), td(0, 1), td(0, 0, 1), td(0, -1), td(0, 0, -1), td(1, 1, 1), td(-1, 1, 1), td(100, 3661, 5), td(0, 86399, 999999), td.min, td.max, td.resolution, td(999999999), td(hours=25), td(minutes=61), td(seconds=59.9999999), T(1, 2, 3), T(), TN(5)]
for t in samples:
    attempt(repr(t), lambda: (str(t), t.days, t.seconds, t.microseconds, t.total_seconds(), bool(t), format(t), "%s" % t, type(t).__name__))
    attempt("   signs", lambda: (-t, +t, abs(t), type(-t).__name__, type(+t).__name__, type(abs(t)).__name__))
    attempt("   reduce", lambda: (t.__reduce__(), t.__reduce_ex__(2), t.__reduce_ex__(4)))
    attempt("   pickle", lambda: [(lambda r: (r == t, type(r) is type(t)))(pickle.loads(pickle.dumps(t, p))) for p in range(6)])
    attempt("   pickled", lambda: pickle.dumps(t, 2) if type(t) is td else None)
    attempt("   copy", lambda: (copy.copy(t) == t, copy.deepcopy(t) == t, copy.copy(t) is t))
    attempt("   hash", lambda: (hash(t) == hash((t.days, t.seconds, t.microseconds)), hash(t) == hash(td(t.days, t.seconds, t.microseconds))))
print("===== arithmetic")
others = [td(), td(1), td(-1), td(0, 1), td(0, 0, 1), td(0, 0, 7), td(3, 4, 5), td.max, td.min, T(2), 0, 1, -1, 2, 3, 7, 10**6, 10**20, -10**20, True, False, 0.0, 0.5, -0.5, 1.5, 1 / 3, 1e-9, 1e9, 1e300, float("inf"), float("nan"), I(3), IM(3), F(0.5), None, "a", 1j, fractions.Fraction(1, 2),
          Index(), [], datetime.date(2000, 1, 1), datetime.datetime(2000, 1, 1), datetime.time(1)]
lefts = [td(), td(1), td(-1), td(0, 1), td(0, 0, 1), td(0, 0, 3), td(0, 0, 5), td(7, 8, 9), td(-7, 8, 9), td.max, td.min, T(1, 2, 3)]
ops = [("+", operator.add), ("-", operator.sub), ("*", operator.mul), ("/", operator.truediv), ("//", operator.floordiv), ("%", operator.mod), ("divmod", divmod), ("**", operator.pow), ("@", operator.matmul), ("<<", operator.lshift), ("&", operator.and_)]
together = []
for name, op in ops:
    for a in lefts:
        for b in others:
            attempt("%r %s %r" % (a, name, b), lambda: (lambda r: (r, type(r).__name__))(op(a, b)))
            if not isinstance(b, td): attempt("   %r %s %r" % (b, name, a), lambda: (lambda r: (r, type(r).__name__))(op(b, a)))
        come_to("%r %s" % (a, name))
together = None
print("===== the methods themselves")
for m in ("__add__", "__radd__", "__sub__", "__rsub__", "__mul__", "__rmul__", "__truediv__", "__rtruediv__", "__floordiv__", "__rfloordiv__", "__mod__", "__rmod__", "__divmod__", "__rdivmod__"):
    for b in (td(0, 3), 2, 0.5, None, "a"):
        attempt("td(0, 7).%s(%r)" % (m, b), lambda: getattr(td(0, 7), m)(b))
    attempt("   no argument", lambda: getattr(td(0, 7), m)())
    attempt("   of the class, on an int", lambda: getattr(td, m)(5, td(1)))
print("===== as_integer_ratio")
for r in ((1, 2), [1, 2], (1,), (1, 2, 3), None, ("a", "b"), (1.5, 2), (1, 0), (I(1), I(2)), (IM(1), IM(2)), (True, True), (10**30, 10**29)):
    f = FR(0.5); f.r = r
    attempt("ratio %r" % (r,), lambda: td(0, 10) * f)
    attempt("   divided", lambda: td(0, 10) / f)
print("===== nearest")
for us in range(-12, 13):
    attempt("%d us" % us, lambda: [(td(0, 0, us) / d).microseconds + (td(0, 0, us) / d).days * 86400 * 10**6 + (td(0, 0, us) / d).seconds * 10**6 for d in (1, 2, 3, 4, 5, 8, -1, -2, -3, -4, -8)])
    attempt("   times", lambda: [(lambda t: t.days * 86400 * 10**6 + t.seconds * 10**6 + t.microseconds)(td(0, 0, us) * f) for f in (0.5, 0.25, 1.5, 2.5, -0.5, -1.5, 0.1, 0.3)])
print("===== compared")
cmps = [("==", operator.eq), ("!=", operator.ne), ("<", operator.lt), ("<=", operator.le), (">", operator.gt), (">=", operator.ge)]
for name, op in cmps:
    for a in (td(), td(1), td(0, 1), td(0, 0, 1), td(-1), T(1)):
        attempt("%r %s" % (a, name), lambda: [op(a, b) for b in (td(), td(1), td(0, 1), td(0, 0, 1), td(-1), T(1), td.max, td.min)])
        for b in (0, 1, None, "a", 86400.0, datetime.date(1, 1, 1), (1, 0, 0)):
            attempt("   %r" % (b,), lambda: op(a, b))
            attempt("   the other way", lambda: op(b, a))
attempt("sorted", lambda: sorted([td(1), td(-1), td(0, 5), td(), td(0, 0, 1), T(0, 3)]))
attempt("in a set", lambda: len({td(1), td(hours=24), T(1), td(0, 86400), td(2)}))
attempt("sum", lambda: sum([td(1), td(2)], td()))
attempt("sum from 0", lambda: sum([td(1), td(2)]))
print("===== attributes")
t = td(1, 2, 3)
for n in ("days", "seconds", "microseconds"):
    attempt("set " + n, lambda: setattr(t, n, 5))
    attempt("delete " + n, lambda: delattr(t, n))
    attempt("descriptor " + n, lambda: (type(vars(td)[n]).__name__, vars(td)[n].__doc__, vars(td)[n].__get__(t), vars(td)[n].__name__, vars(td)[n].__objclass__ is td))
    attempt("   of something else", lambda: vars(td)[n].__get__(5))
attempt("another", lambda: setattr(t, "x", 1))
attempt("of a derived class", lambda: (lambda u: (setattr(u, "x", 1), u.x, vars(u)))(T(1)))
attempt("min, max, resolution", lambda: (td.min, td.max, td.resolution, T.min, type(T.min).__name__))
attempt("set on the class", lambda: setattr(td, "x", 1))
attempt("weak reference", lambda: __import__("weakref").ref(t))
attempt("__new__ of object", lambda: object.__new__(td))
attempt("__new__ with another class", lambda: td.__new__(int))
attempt("__new__ with no class", lambda: td.__new__())
attempt("__new__ with a derived class", lambda: type(td.__new__(T, 1)).__name__)
attempt("__init__", lambda: td(1).__init__(5, 6, 7))
attempt("sizeof", lambda: sys.getsizeof(td(1)) > 0)
attempt("int, float, index", lambda: [attempt("   " + f.__name__, lambda: f(td(1))) for f in (int, float, operator.index, complex, round, len, iter)])
attempt("TN keeps", lambda: (TN(1).extra, hasattr(TN(1) + TN(1), "extra"), hasattr(-TN(1), "extra"), hasattr(TN(1) * 2, "extra")))
print("done")
