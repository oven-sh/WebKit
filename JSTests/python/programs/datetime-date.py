# datetime.date, and what isocalendar() gives
import os, time
os.environ["TZ"] = "America/New_York"; time.tzset()
import _datetime, datetime, pickle, copy, operator, sys
from datetime import date, timedelta as td
import re; address = re.compile('0x[0-9a-f]+')
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
    print(label.encode("ascii", "backslashreplace").decode(), "->", address.sub("0x", ascii(r)))
class I(int): pass
class Index:
    def __index__(self): return 3
    def __repr__(self): return "Index()"
class D(date): pass
class DN(date):
    def __new__(cls, y, m, d, extra=None):
        r = date.__new__(cls, y, m, d); r.extra = extra; return r
class DX(date):
    def __new__(cls, *a): return "not a date"
print("===== making one")
values = [0, 1, -1, 2, 12, 13, 28, 29, 30, 31, 32, 99, 9999, 10000, 2**31 - 1, 2**31, -2**31, -2**31 - 1, 2**63, 10**30, True, False, 1.0, 1.5, None, "1", b"1", [], I(5), Index()]
for v in values:
    attempt("year %r" % (v,), lambda: date(v, 1, 1))
    attempt("month %r" % (v,), lambda: date(2000, v, 1))
    attempt("day %r" % (v,), lambda: date(2000, 1, v))
for y in (1, 4, 100, 400, 1900, 2000, 2023, 2024, 2100, 9999):
    attempt("the ends of the months of %d" % y, lambda: [[attempt("   %d-%d-%d" % (y, m, d), lambda: date(y, m, d)) for d in (28, 29, 30, 31, 32)] for m in (1, 2, 4, 12)] and None)
for label, f in {"none": lambda: date(), "one": lambda: date(2000), "two": lambda: date(2000, 1), "four": lambda: date(2000, 1, 1, 1), "keywords": lambda: date(year=2000, month=1, day=2), "some keywords": lambda: date(2000, day=2, month=1), "unknown": lambda: date(2000, 1, 1, hour=1),
                 "unknown in place of one": lambda: date(2000, 1, days=1), "twice": lambda: date(2000, 1, 1, year=1), "twice of three": lambda: date(2000, 1, year=1), "a bad one and one missing": lambda: date("x"), "a bad second and none third": lambda: date(1, "x"),
                 "a bad one and too many": lambda: date("x", 1, 1, 1), "a bad one and an unknown": lambda: date("x", 1, zzz=1), "missing by keyword": lambda: date(month=1, day=1), "missing the second": lambda: date(2000, day=1), "out of range and one missing": lambda: date(2**40),
                 "float and one missing": lambda: date(1.5)}.items():
    attempt(label, f)
print("===== what pickle calls it with")
for s in (b"\x07\xd0\x01\x02", b"\x07\xd0\x00\x02", b"\x07\xd0\x0c\x02", b"\x07\xd0\x0d\x02", b"\x00\x00\x01\x00", b"\xff\xff\x0c\xff", b"\x07\xd0\x01", b"\x07\xd0\x01\x02\x03", b"", bytearray(b"\x07\xd0\x01\x02"),
          "\x07\xd0\x01\x02", "\x07\xd0\x00\x02", "\x07\u0100\x01\x02", "\x07\U0001f600\x01\x02", "\x07\xd0\x01", "\x07\xd0\x0d\u0100", "\x07\xd0\x01\u20ac", type("B", (bytes,), {})(b"\x07\xd0\x01\x02"), type("S", (str,), {})("\x07\xd0\x01\x02")):
    attempt("state %r" % (s,), lambda: (lambda d: (d.year, d.month, d.day, repr(d)))(date(s)))
    attempt("   of a derived class", lambda: type(D(s)).__name__)
    attempt("   and a keyword", lambda: date(s, month=1))
    attempt("   by keyword", lambda: date(year=s))
print("===== shown")
samples = [date(1, 1, 1), date(9999, 12, 31), date(2000, 2, 29), date(1970, 1, 1), date(1969, 12, 31), date(2024, 12, 30), date(2021, 1, 3), date(999, 9, 9), date(99, 1, 1), date(2026, 9, 30), D(2000, 1, 2), DN(2000, 1, 2, "x")]
for d in samples:
    attempt(repr(d), lambda: (str(d), d.isoformat(), d.ctime(), d.year, d.month, d.day, d.toordinal(), d.weekday(), d.isoweekday(), format(d), "%s" % d, bool(d)))
    attempt("   timetuple", lambda: (d.timetuple(), type(d.timetuple()).__name__, tuple(d.timetuple())))
    attempt("   isocalendar", lambda: (d.isocalendar(), tuple(d.isocalendar()), d.isocalendar().year, d.isocalendar().week, d.isocalendar().weekday))
    attempt("   reduce", lambda: (d.__reduce__(), d.__reduce_ex__(2), d.__reduce_ex__(4)))
    attempt("   pickle", lambda: [(lambda r: (r == d, type(r) is type(d)))(pickle.loads(pickle.dumps(d, p))) for p in range(6)])
    attempt("   pickled", lambda: [pickle.dumps(d, p) for p in (0, 2, 4)] if type(d) is date else None)
    attempt("   copy", lambda: (copy.copy(d) == d, copy.deepcopy(d) == d, copy.copy(d) is d, copy.replace(d, day=5)))
    attempt("   hash", lambda: (hash(d) == hash(date(d.year, d.month, d.day)), hash(d) == hash(d.__reduce__()[1][0])))
    attempt("   and back", lambda: (date.fromordinal(d.toordinal()) == d, date.fromisoformat(d.isoformat()) == d, date.fromisocalendar(*d.isocalendar()) == d))
print("===== every day of some years")
import hashlib
for y in (1, 2, 4, 5, 100, 101, 400, 401, 1582, 1900, 1999, 2000, 2001, 2004, 2015, 2016, 2020, 2021, 2026, 2100, 9998, 9999):
    h = hashlib.md5()
    d = date(y, 1, 1)
    while d.year == y:
        h.update(repr((d, d.toordinal(), d.weekday(), tuple(d.isocalendar()), d.ctime(), tuple(d.timetuple()), date.fromordinal(d.toordinal()) == d, date.fromisocalendar(*d.isocalendar()) == d)).encode())
        if d == date.max: break
        d += td(1)
    print(y, h.hexdigest())
print("===== strftime")
for f in ("", "%Y-%m-%d", "%y", "%Y", "%G", "%C", "%F", "%D", "%j", "%U", "%W", "%V", "%u", "%w", "%a %A %b %B", "%H:%M:%S", "%f", "%z", "%Z", "%:z", "%%", "%", "%%%", "%Y%", "abc", "%e", "%x", "%c", "%p", "%I", "caf\xe9 %Y", "\u20ac%d", "\U0001f600%m", "%Y\0%m", "%:", "%:y", "%::z", "%fz%f", "%%f", "%%%f",
          "%Q", "%E", "%O", "%Ey", "%-d", "%5Y", "\ud800%Y"):
    for d in (date(2024, 2, 29), date(1, 1, 1), date(99, 12, 31), date(999, 6, 15), date(1000, 1, 1), date(9999, 12, 31)):
        attempt("%r of %s" % (f, d), lambda: d.strftime(f))
    attempt("   format()", lambda: format(date(2024, 2, 29), f))
    attempt("   in an f-string", lambda: "{:{}}".format(date(2024, 2, 29), f))
for label, f in {"none": lambda: date(1, 1, 1).strftime(), "two": lambda: date(1, 1, 1).strftime("a", "b"), "keyword": lambda: date(2000, 1, 1).strftime(format="%Y"), "unknown": lambda: date(1, 1, 1).strftime(fmt="%Y"), "int": lambda: date(1, 1, 1).strftime(5), "None": lambda: date(1, 1, 1).strftime(None),
                 "bytes": lambda: date(1, 1, 1).strftime(b"%Y"), "a derived str": lambda: date(2000, 1, 1).strftime(type("S", (str,), {})("%Y")), "__format__ none": lambda: date(1, 1, 1).__format__(), "__format__ int": lambda: date(1, 1, 1).__format__(5), "__format__ two": lambda: date(1, 1, 1).__format__("a", "b"),
                 "__format__ keyword": lambda: date(1, 1, 1).__format__(format=""), "format(d, None)": lambda: format(date(1, 1, 1), None)}.items():
    attempt(label, f)
class DT(date):
    def timetuple(self): return self.t
    def strftime(self, f): return "strftime(%r)" % f
    def __str__(self): return "DT.__str__"
    def isoformat(self): return "DT.isoformat"
for t in ((2000, 1, 1, 0, 0, 0, 0, 1, -1), (5, 1, 1, 0, 0, 0, 0, 1, -1), (-5, 1, 1, 0, 0, 0, 0, 1, -1), (), None, "abc", (1,), ("x", 1, 1, 0, 0, 0, 0, 1, -1), [2000, 1, 1, 0, 0, 0, 0, 1, -1], (10**30, 1, 1, 0, 0, 0, 0, 1, -1), time.struct_time((500, 1, 1, 0, 0, 0, 0, 1, -1))):
    d = DT(2000, 1, 1); d.t = t
    attempt("timetuple() gives %r" % (t,), lambda: [date.strftime(d, f) for f in ("%Y", "%G", "%C", "%F", "x")])
attempt("what format() asks", lambda: (format(DT(2000, 1, 1)), format(DT(2000, 1, 1), "%Y"), str(DT(2000, 1, 1)), date.__str__(DT(2000, 1, 1)), repr(DT(2000, 1, 1))))
print("===== fromisoformat")
texts = ["2024-02-29", "20240229", "2024-W09-4", "2024W094", "2024-W09", "2024W09", "2023-02-29", "2024-13-01", "2024-00-01", "2024-01-00", "2024-01-32", "0001-01-01", "9999-12-31", "0000-01-01", "10000-01-01", "2024-1-01", "2024-01-1", "24-01-01", "2024/01/01", "2024-0101", "202401-01",
         "2024-01-01 ", " 2024-01-01", "2024-01-01T00", "", "2024", "2024-01", "202401", "2024-001", "2024001", "2024-W00-1", "2024-W53-1", "2020-W53-1", "2020-W53-7", "2020-W54-1", "2024-W01-0", "2024-W01-8", "2024-W1-1", "2024-w01-1", "2024-W01-", "2024-W011", "2024W01-1", "2024-W01-1x",
         "0001-W01-1", "9999-W52-5", "9999-W52-6", "0000-W01-1", "\uff12\uff10\uff12\uff14-01-01", "2024-01-0\u0661", "2024\u201301\u201301", "2024-01-01\0", "2024-01\0001", "\ud8002024-01", "2024-01-\ud800", "+2024-01-1", "-024-01-01", "2024-+1-01", "\uff12\uff10\uff12\uff140101", "202\u00e9", "2024-01-\xe9"]
for t in texts:
    attempt("fromisoformat(%a)" % t, lambda: date.fromisoformat(t))
for v in (None, 5, b"2024-01-01", [], type("S", (str,), {})("2024-01-01")):
    attempt("fromisoformat(%r)" % (v,), lambda: date.fromisoformat(v))
attempt("none", lambda: date.fromisoformat())
attempt("two", lambda: date.fromisoformat("a", "b"))
attempt("keyword", lambda: date.fromisoformat(date_string="2024-01-01"))
print("===== the other ways of making one")
for v in (1, 2, 0, -1, 365, 366, 730120, 3652059, 3652060, 2**31 - 1, 2**31, -2**31 - 1, 10**30, True, 1.0, 1.5, None, "1", I(5), Index()):
    attempt("fromordinal(%r)" % (v,), lambda: date.fromordinal(v))
attempt("fromordinal: none", lambda: date.fromordinal())
attempt("fromordinal: two", lambda: date.fromordinal(1, 2))
attempt("fromordinal: keyword", lambda: date.fromordinal(ordinal=1))
for a in ((2024, 1, 1), (2024, 52, 7), (2024, 53, 1), (2020, 53, 7), (2020, 54, 1), (2024, 0, 1), (2024, -1, 1), (2024, 1, 0), (2024, 1, 8), (0, 1, 1), (10000, 1, 1), (1, 1, 1), (9999, 52, 5), (9999, 52, 6), (9999, 52, 7), (2**31, 1, 1), (2024, 2**31, 1), (2024, 1, 2**31), (-2**31 - 1, 1, 1), (10**30, 1, 1),
          (2024.0, 1, 1), ("2024", 1, 1), (None, 1, 1), (2024, None, 1), (2024, 1, None), (True, True, True), (I(2024), I(1), I(1)), (Index(), Index(), Index())):
    attempt("fromisocalendar%r" % (a,), lambda: date.fromisocalendar(*a))
for label, f in {"none": lambda: date.fromisocalendar(), "two": lambda: date.fromisocalendar(2024, 1), "four": lambda: date.fromisocalendar(2024, 1, 1, 1), "keywords": lambda: date.fromisocalendar(year=2024, week=1, day=1), "unknown": lambda: date.fromisocalendar(2024, 1, weekday=1), "a bad one and one missing": lambda: date.fromisocalendar("x"), "too large and one missing": lambda: date.fromisocalendar(2**40), "a bad one and too many": lambda: date.fromisocalendar("x", 1, 1, 1),
                 "a bad one and an unknown": lambda: date.fromisocalendar("x", 1, zzz=1)}.items():
    attempt("fromisocalendar: " + label, f)
for v in (0, 1, -1, 86399, 86400, 18000, 17999, 1e9, 1.5, -1.5, 0.999999, -0.000001, 2**31, 2**32, 253402318799, 253402318800, -62135579038, -62135596800, 1e11, 1e12, 1e20, -1e20, 2**63, -2**63, 2**64, 10**30, float("inf"), float("nan"), True, None, "1", I(5), Index(), [], 1j):
    attempt("fromtimestamp(%r)" % (v,), lambda: date.fromtimestamp(v))
attempt("fromtimestamp: none", lambda: date.fromtimestamp())
attempt("fromtimestamp: two", lambda: date.fromtimestamp(1, 2))
attempt("fromtimestamp: keyword", lambda: date.fromtimestamp(timestamp=1))
attempt("today", lambda: (type(date.today()).__name__, date.today() == date.fromtimestamp(time.time()), type(D.today()).__name__))
attempt("today: an argument", lambda: date.today(1))
for a in (("2024-02-29", "%Y-%m-%d"), ("29/02/24", "%d/%m/%y"), ("2024-02-30", "%Y-%m-%d"), ("2024", "%Y"), ("x", "%Y"), ("2024-02-29 12", "%Y-%m-%d %H"), ("", ""), (5, "%Y"), ("2024", 5), (None, None), (b"2024", "%Y")):
    attempt("strptime%r" % (a,), lambda: date.strptime(*a))
attempt("strptime: one", lambda: date.strptime("2024"))
attempt("strptime: three", lambda: date.strptime("2024", "%Y", 1))
attempt("strptime: keyword", lambda: date.strptime("2024", format="%Y"))
print("===== of a derived class")
for label, f in {"fromordinal": lambda c: c.fromordinal(730120), "fromisoformat": lambda c: c.fromisoformat("2000-01-02"), "fromisocalendar": lambda c: c.fromisocalendar(2000, 1, 1), "fromtimestamp": lambda c: c.fromtimestamp(0), "today": lambda c: c.today(), "replace": lambda c: c(2000, 1, 2).replace(day=3),
                 "+": lambda c: c(2000, 1, 2) + td(1), "-": lambda c: c(2000, 1, 2) - td(1), "r+": lambda c: td(1) + c(2000, 1, 2), "- a date": lambda c: c(2000, 1, 2) - date(2000, 1, 1), "strptime": lambda c: c.strptime("2000", "%Y"), "copy.replace": lambda c: copy.replace(c(2000, 1, 2), day=3)}.items():
    for c in (D, DN, DX):
        attempt("%s %s" % (c.__name__, label), lambda: (lambda r: (type(r).__name__, getattr(r, "extra", "no extra")))(f(c)))
print("===== replace")
d = date(2024, 2, 29)
for k in ({}, {"year": 2020}, {"year": 2023}, {"month": 3}, {"day": 1}, {"day": 30}, {"year": 0}, {"year": 10000}, {"month": 13}, {"year": 2**31}, {"year": 10**30}, {"year": None}, {"year": 1.0}, {"year": "1"}, {"year": True}, {"year": Index()}, {"hour": 1}, {"year": 2020, "month": 1, "day": 1}, {"days": 1}):
    attempt("replace(%r)" % (sorted(k.items()),), lambda: d.replace(**k))
    attempt("   __replace__", lambda: d.__replace__(**k))
attempt("positional", lambda: d.replace(2020, 1, 1))
attempt("too many", lambda: d.replace(2020, 1, 1, 1))
attempt("twice", lambda: d.replace(2020, year=1))
attempt("__replace__: positional", lambda: d.__replace__(2020, 1, 1))
attempt("__replace__: too many", lambda: d.__replace__(2020, 1, 1, 1))
print("===== arithmetic")
others = [td(), td(1), td(-1), td(0, 86399), td(0, 86399, 999999), td(1, 1), td(-1, 86399), td(365), td(366), td(3652058), td(-3652058), td(3652059), td.max, td.min, date(1, 1, 1), date(2024, 2, 29), date(9999, 12, 31), D(2000, 1, 1), datetime.datetime(2000, 1, 1), datetime.time(1), 0, 1, 1.5, None, "a", True]
for a in (date(1, 1, 1), date(1, 1, 2), date(2024, 2, 28), date(2024, 2, 29), date(2024, 3, 1), date(2023, 12, 31), date(9999, 12, 30), date(9999, 12, 31), D(2000, 1, 1)):
    for b in others:
        for name, op in (("+", operator.add), ("-", operator.sub), ("*", operator.mul)):
            attempt("%r %s %r" % (a, name, b), lambda: (lambda r: (r, type(r).__name__))(op(a, b)))
            attempt("   the other way", lambda: (lambda r: (r, type(r).__name__))(op(b, a)))
for m in ("__add__", "__radd__", "__sub__", "__rsub__"):
    for b in (td(1), date(2000, 1, 1), datetime.datetime(2000, 1, 1), 1, None):
        attempt("date(2000, 1, 5).%s(%r)" % (m, b), lambda: getattr(date(2000, 1, 5), m)(b))
        attempt("   of date, on a datetime", lambda: getattr(date, m)(datetime.datetime(2000, 1, 5), b))
print("===== compared")
cmps = [("==", operator.eq), ("!=", operator.ne), ("<", operator.lt), ("<=", operator.le), (">", operator.gt), (">=", operator.ge)]
for name, op in cmps:
    for a in (date(2000, 1, 1), date(2000, 1, 2), date(1999, 12, 31), date(256, 1, 1), date(255, 12, 31), D(2000, 1, 1)):
        attempt("%r %s" % (a, name), lambda: [op(a, b) for b in (date(2000, 1, 1), date(2000, 1, 2), date(2000, 2, 1), date(2001, 1, 1), date(1999, 12, 31), date(256, 1, 1), date(255, 12, 31), D(2000, 1, 1), date.min, date.max)])
        for b in (datetime.datetime(2000, 1, 1), datetime.datetime(2000, 1, 1, 1), 0, None, "2000-01-01", (2000, 1, 1), td(1), datetime.time(0), 730120):
            attempt("   %r" % (b,), lambda: op(a, b))
            attempt("   the other way", lambda: op(b, a))
            attempt("   the method of date", lambda: getattr(date, "__%s__" % op.__name__)(a, b))
attempt("the method of date, on a datetime", lambda: (date.__eq__(datetime.datetime(2000, 1, 1, 5), date(2000, 1, 1)), date.__lt__(datetime.datetime(2000, 1, 1, 5), date(2000, 1, 2)), date.__hash__(datetime.datetime(2000, 1, 1, 5)) == hash(date(2000, 1, 1))))
attempt("methods of date, on a datetime", lambda: (date.__repr__(datetime.datetime(2000, 1, 2, 5)), date.isoformat(datetime.datetime(2000, 1, 2, 5)), date.ctime(datetime.datetime(2000, 1, 2, 5)), date.__reduce__(datetime.datetime(2000, 1, 2, 5)), date.timetuple(datetime.datetime(2000, 1, 2, 5)),
    date.replace(datetime.datetime(2000, 1, 2, 5), day=3), date.__str__(datetime.datetime(2000, 1, 2, 5)), date.strftime(datetime.datetime(2000, 1, 2, 5, 6, 7, 8), "%H %f")))
attempt("sorted", lambda: sorted([date(2000, 1, 2), date(1999, 1, 1), D(2000, 1, 1), date.max, date.min]))
attempt("in a set", lambda: len({date(2000, 1, 1), D(2000, 1, 1), date(2000, 1, 2), datetime.datetime(2000, 1, 1)}))
print("===== attributes")
d = date(2000, 1, 2)
for n in ("year", "month", "day"):
    attempt("set " + n, lambda: setattr(d, n, 5))
    attempt("delete " + n, lambda: delattr(d, n))
    attempt("descriptor " + n, lambda: (type(vars(date)[n]).__name__, vars(date)[n].__doc__, vars(date)[n].__get__(d), vars(date)[n].__name__))
    attempt("   of something else", lambda: vars(date)[n].__get__(5))
attempt("another", lambda: setattr(d, "x", 1))
attempt("min, max, resolution", lambda: (date.min, date.max, date.resolution, D.min, type(D.min).__name__))
attempt("__new__ of object", lambda: object.__new__(date))
attempt("date.__new__(datetime)", lambda: date.__new__(datetime.datetime, 2000, 1, 1))
attempt("date.__new__(int)", lambda: date.__new__(int, 2000, 1, 1))
attempt("date.__new__(D)", lambda: type(date.__new__(D, 2000, 1, 1)).__name__)
attempt("weak reference", lambda: __import__("weakref").ref(d))
print("===== IsoCalendarDate")
C = type(d.isocalendar())
c = d.isocalendar()
attempt("the class", lambda: (C, C.__name__, C.__qualname__, C.__module__, C.__mro__, C.__doc__, C.__text_signature__, bool(C.__flags__ & (1 << 10)), bool(C.__flags__ & (1 << 8)), hasattr(datetime, "IsoCalendarDate"), hasattr(_datetime, "IsoCalendarDate")))
attempt("one", lambda: (c, repr(c), str(c), tuple(c), len(c), c[0], c[-1], c[1:], c == (1999, 52, 7), c < (2000,), list(c), c + (1,), c * 2, c.count(52), c.index(7), isinstance(c, tuple)))
attempt("unpacked", lambda: (lambda y, w, wd: (y, w, wd))(*c))
attempt("reduce", lambda: (c.__reduce__(), c.__reduce_ex__(2), pickle.loads(pickle.dumps(c)), type(pickle.loads(pickle.dumps(c))).__name__, copy.copy(c), type(copy.copy(c)).__name__, type(copy.deepcopy(c)).__name__))
for label, f in {"made": lambda: C(1, 2, 3), "keywords": lambda: C(year=1, week=2, weekday=3), "none": lambda: C(), "two": lambda: C(1, 2), "four": lambda: C(1, 2, 3, 4), "unknown": lambda: C(1, 2, day=3), "a str": lambda: C("a", 2, 3), "a float": lambda: C(1.0, 2, 3), "too large": lambda: C(2**31, 2, 3), "huge": lambda: C(10**30, 2, 3),
                 "any ints": lambda: C(-5, 100, 0), "True": lambda: C(True, True, True), "Index": lambda: C(Index(), 1, 1), "a tuple": lambda: C((1, 2, 3)), "derive": lambda: type("X", (C,), {}), "set": lambda: setattr(c, "year", 1), "another": lambda: setattr(c, "x", 1), "set on the class": lambda: setattr(C, "x", 1),
                 "tuple.__new__": lambda: tuple.__new__(C, (1, 2)), "match": lambda: C.__match_args__}.items():
    attempt(label, f)
for n in ("year", "week", "weekday"):
    attempt("descriptor " + n, lambda: (type(vars(C)[n]).__name__, vars(C)[n].__doc__, vars(C)[n].__get__(c)))
print("done")
