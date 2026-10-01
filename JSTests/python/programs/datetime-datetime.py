# datetime.datetime
import os, time as _time
os.environ["TZ"] = "America/New_York"; _time.tzset()
import _datetime, datetime, pickle, copy, operator, sys, warnings, hashlib
from datetime import datetime as dt, date, time, timedelta as td, timezone as tz, tzinfo
import re; address = re.compile('0x[0-9a-f]+')
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
    print(label.encode("ascii", "backslashreplace").decode(), "->", address.sub("0x", ascii(r)))
def quiet(op, a, b):
    try: return op(a, b)
    except TypeError: return "T"
class I(int): pass
class Index:
    def __index__(self): return 3
    def __repr__(self): return "Index()"
class D(dt): pass
class DN(dt):
    def __new__(cls, *a, **k):
        r = dt.__new__(cls, *a, **k); r.extra = sorted(k); return r
class DX(dt):
    made = False
    def __new__(cls, *a, **k):
        if not cls.made: cls.made = True; return dt.__new__(cls, *a, **k)
        return "not a datetime"
log = []
class Z(tzinfo):
    def __init__(self, offset=None, dst=None, name=None): self.o, self.d, self.n = offset, dst, name
    def give(self, what, when):
        log.append((type(when).__name__, getattr(when, "fold", None)))
        if isinstance(what, BaseException): raise what
        if callable(what): return what(when)
        return what
    def utcoffset(self, when): return self.give(self.o, when)
    def dst(self, when): return self.give(self.d, when)
    def tzname(self, when): return self.give(self.n, when)
    def __repr__(self): return "Z()"
print("===== making one")
values = [0, 1, -1, 12, 13, 23, 24, 31, 32, 59, 60, 9999, 10000, 999999, 1000000, 2**31 - 1, 2**31, -2**31 - 1, 10**30, True, False, 1.0, None, "1", [], I(5), Index()]
names = ("year", "month", "day", "hour", "minute", "second", "microsecond", "fold")
for v in values:
    for n in names:
        attempt("%s %r" % (n, v), lambda: dt(**{"year": 2000, "month": 1, "day": 1, n: v}))
for v in (None, tz.utc, Z(), 5, "UTC", td(0), tzinfo(), tz, False):
    attempt("tzinfo %s" % address.sub("0x", repr(v)), lambda: dt(2000, 1, 1, tzinfo=v).tzinfo is v)
for label, f in {"none": lambda: dt(), "one": lambda: dt(2000), "two": lambda: dt(2000, 1), "three": lambda: dt(2000, 1, 2), "eight": lambda: dt(2000, 1, 2, 3, 4, 5, 6, tz.utc), "fold by position": lambda: dt(2000, 1, 2, 3, 4, 5, 6, None, 1), "ten": lambda: dt(2000, 1, 2, 3, 4, 5, 6, None, 1, 1), "unknown": lambda: dt(2000, 1, 1, hours=1),
                 "twice": lambda: dt(2000, 1, 1, year=1), "a bad one and one missing": lambda: dt("x"), "a bad one and too many by position": lambda: dt("x", 1, 2, 3, 4, 5, 6, None, 1), "a bad one and too many": lambda: dt("x", 1, 2, 3, 4, 5, 6, None, 1, 1), "a bad one and an unknown": lambda: dt("x", 1, 1, zzz=1),
                 "a bad date and a bad time": lambda: dt(2000, 13, 1, 24), "a bad time and a bad tzinfo": lambda: dt(2000, 1, 1, 24, tzinfo=5), "missing by keyword": lambda: dt(month=1, day=1), "30 February": lambda: dt(2000, 2, 30), "29 February": lambda: (dt(2000, 2, 29), dt(2004, 2, 29))}.items():
    attempt(label, f)
print("===== what pickle calls it with")
for s in (b"\x07\xd0\x01\x02\x03\x04\x05\x00\x00\x06", b"\x07\xd0\x81\x02\x03\x04\x05\x00\x00\x06", b"\x07\xd0\x8c\x02\x03\x04\x05\x00\x00\x06", b"\x07\xd0\x8d\x02\x03\x04\x05\x00\x00\x06", b"\x07\xd0\x00\x02\x03\x04\x05\x00\x00\x06", b"\x07\xd0\x0d\x02\x03\x04\x05\x00\x00\x06", b"\xff\xff\x0c\xff\xff\xff\xff\xff\xff\xff",
          b"\x07\xd0\x01\x02\x03\x04\x05\x00\x00", b"\x07\xd0\x01\x02\x03\x04\x05\x00\x00\x06\x07", b"\x07\xd0\x01\x02", "\x07\xd0\x01\x02\x03\x04\x05\x00\x00\x06", "\x07\xd0\x81\x02\x03\x04\x05\x00\x00\x06", "\x07\u0100\x01\x02\x03\x04\x05\x00\x00\x06", "\x07\xd0\u0101\x02\x03\x04\x05\x00\x00\x06", "\x07\xd0\x01\x02\x03\x04\x05\x00\x00"):
    attempt("state %r" % (s,), lambda: (lambda d: (d.year, d.month, d.day, d.hour, d.minute, d.second, d.microsecond, d.fold, d.tzinfo, repr(d)))(dt(s)))
    for z in (None, tz.utc, 5, Z()):
        attempt("   with %r" % (z,), lambda: (lambda d: (d.year, d.fold, d.tzinfo is z))(dt(s, z)))
    attempt("   with three", lambda: dt(s, None, 1))
    attempt("   of a derived class", lambda: type(D(s)).__name__)
print("===== shown")
zones = [None, tz.utc, tz(td(hours=5, minutes=30)), tz(td(hours=-5), "EST"), tz(td(seconds=1)), tz(td(microseconds=-1)), Z(), Z(td(hours=1), td(hours=1), "Z%"), Z(td(hours=1), td(0), None)]
for z in zones:
    for d in (dt(2024, 2, 29, tzinfo=z), dt(2024, 2, 29, 1, tzinfo=z), dt(2024, 2, 29, 1, 2, tzinfo=z), dt(2024, 2, 29, 1, 2, 3, tzinfo=z), dt(2024, 2, 29, 1, 2, 3, 4, tzinfo=z), dt(1, 1, 1, tzinfo=z), dt(9999, 12, 31, 23, 59, 59, 999999, tzinfo=z), dt(999, 9, 9, 9, 9, 9, 9000, tzinfo=z), dt(2024, 2, 29, 1, 2, 3, 4, tzinfo=z, fold=1),
              dt(2024, 2, 29, fold=1, tzinfo=z), D(2000, 1, 2, 3, tzinfo=z), DN(2000, 1, 2, 3, 4, 5, tzinfo=z, fold=1)):
        attempt(repr(d), lambda: (str(d), d.isoformat(), d.ctime(), d.year, d.month, d.day, d.hour, d.minute, d.second, d.microsecond, d.fold, d.tzinfo is z, format(d), d.utcoffset(), d.dst(), d.tzname()))
        attempt("   parts", lambda: (d.date(), d.time(), d.timetz(), type(d.date()).__name__, type(d.time()).__name__, d.toordinal(), d.weekday(), d.isoweekday(), d.isocalendar()))
        attempt("   tuples", lambda: (tuple(d.timetuple()), tuple(d.utctimetuple())))
        attempt("   timespec", lambda: [d.isoformat(" ", s) for s in ("auto", "hours", "minutes", "seconds", "milliseconds", "microseconds")])
        attempt("   strftime", lambda: d.strftime("%Y-%m-%d %H:%M:%S.%f|%z|%:z|%Z|%j|%a|%G-%V-%u|%C|%F|%%"))
        attempt("   reduce", lambda: (d.__reduce__()[1][0], len(d.__reduce__()[1]), [d.__reduce_ex__(p)[1][0] for p in range(6)]))
        attempt("   pickle", lambda: [(lambda r: (r == d, type(r) is type(d), r.fold))(pickle.loads(pickle.dumps(d, p))) for p in range(6)])
        attempt("   copy", lambda: (copy.copy(d) == d, copy.deepcopy(d) == d, copy.copy(d) is d, copy.replace(d, hour=5)))
        attempt("   and back", lambda: (dt.fromisoformat(d.isoformat()) == d, dt.combine(d.date(), d.timetz()) == d, dt.fromisoformat(str(d)) == d))
        attempt("   timestamp", lambda: d.timestamp() if z is not None and d.utcoffset() is not None else None)
attempt("pickled", lambda: [pickle.dumps(dt(2024, 2, 29, 1, 2, 3, 4, tzinfo=tz(td(hours=1), "n"), fold=1), p) for p in (0, 2, 4)])
for v in ("T", " ", "", "ab", "\xe9", "\u20ac", "\U0001f600", "\ud800", "\0", "0", "-", 5, None, b"T", type("S", (str,), {})("x"), "\U0001f600\U0001f600"):
    attempt("isoformat(%a)" % (v,), lambda: dt(2000, 1, 2, 3, 4, 5).isoformat(v))
    attempt("   and back", lambda: dt.fromisoformat(dt(2000, 1, 2, 3, 4, 5).isoformat(v)))
for label, f in {"three": lambda: dt(1, 1, 1).isoformat("T", "auto", 1), "keywords": lambda: dt(1, 1, 1).isoformat(sep="x", timespec="hours"), "timespec alone": lambda: dt(1, 1, 1).isoformat(timespec="minutes"), "unknown keyword": lambda: dt(1, 1, 1).isoformat(spec="hours"), "unknown": lambda: dt(1, 1, 1).isoformat("T", "days"),
                 "timespec an int": lambda: dt(1, 1, 1).isoformat("T", 5), "timespec None": lambda: dt(1, 1, 1).isoformat("T", None), "a null": lambda: dt(1, 1, 1).isoformat("T", "a\0"), "both wrong": lambda: dt(1, 1, 1).isoformat(5, 5), "__reduce_ex__ none": lambda: dt(1, 1, 1).__reduce_ex__(), "__reduce_ex__ str": lambda: dt(1, 1, 1).__reduce_ex__("2")}.items():
    attempt("isoformat: " + label, f)
class DS(dt):
    def isoformat(self, sep="T"): return "isoformat(%r)" % sep
attempt("what str() asks", lambda: (str(DS(1, 1, 1)), format(DS(1, 1, 1)), dt.__str__(DS(1, 1, 1))))
print("===== fromisoformat")
dates = ["2024-02-29", "20240229", "2024-W09-4", "2024W094", "2024-W09", "2024W09", "2023-02-29", "0001-01-01", "9999-12-31", "2024-13-01", "2024-1-1", "2024-001"]
seps = ["T", " ", "t", "-", "0", ":", "+", "Z", "\xe9", "\u20ac", "\U0001f600", "\ud800", "\udc00", "", "TT"]
times = ["00", "01:02", "0102", "01:02:03", "010203", "01:02:03.5", "01:02:03.123456789", "01:02:03,5", "24", "24:00", "24:00:00.000000", "24:00:01", "25", "01:60", "01Z", "01+01", "01:02:03.5+01:30", "01:02-01:30:15.5", "01+24", "01z", "", "1", "01:", "01:02:03.", "01:02:03+", "0000", "000000", "0", "00000"]
h = hashlib.md5(); shown = 0
for a in dates:
    for s in seps:
        for b in times:
            try: r = repr(dt.fromisoformat(a + s + b))
            except Exception as e: r = "%s: %s" % (type(e).__name__, e)
            line = "%a -> %s" % (a + s + b, ascii(r))
            if len(sys.argv) > 1 or (a in dates[:6] and s in ("T", "-", "0", "") ): print(line)
            h.update(line.encode())
print("all of them come to", h.hexdigest())
for t in ("", "2024", "2024-02", "202402", "2024-02-2", "2024-02-29T", "2024-02-29 ", "2024-02-29T1", "2024-12-31T24:00", "9999-12-31T24:00", "2024-02-29T24:00", "2024-02-28T24:00", "2023-02-28T24:00", "2024-02-30T24:00", "2024-13-01T24:00", "2024-00-01T24:00", "2024-01-00T24:00", "2024-01-32T24:00:01", "2024-01-31T24:00:00.000001",
          "2024-W01-0000", "2024-W01-1000", "2024-W01-1T00", "2024-W01-100", "2024-W01-10", "2024-W01-", "2024-W01T00", "2024W01T00", "2024W011T00", "2024W0100", "2024W01100", "2024W010000", "2024W0110000", "2024W01000000", "2024W011000000", "2024W0", "2024W", "2024-W0", "2024-W",
          "\ud8002024-02-29", "2024-02-29\ud800\ud800", "2024-02-29\ud80001\ud800", "2024-02\ud80029", "2024022\ud800", "2024W09\ud80001", "20240229\ud80001", "2024-02-29T01:02\0", "2024-02-29\x0001", "\uff12\uff10\uff12\uff14-02-29", "2024-02-29T\uff10\uff11", " 2024-02-29", "2024-02-29T01 ", "2024-02-29T01:02:03.123456+01:00:00.000001",
          "2024-02-29T01:02:03+00:00", "2024-02-29T01:02:03-00:00", "2024-02-29T01:02:03+00:00:00.000000", "2024-02-29T01:02:03Z", "2024-02-29T01:02:03 Z", "20240229T010203Z", "20240229T010203.5-0500", "2024-02-29T010203", "20240229T01:02:03"):
    attempt("fromisoformat(%a)" % t, lambda: (lambda r: (r, r.tzinfo is tz.utc))(dt.fromisoformat(t)))
for v in (None, 5, b"2024-01-01", [], type("S", (str,), {})("2024-01-01T01")):
    attempt("fromisoformat(%r)" % (v,), lambda: dt.fromisoformat(v))
print("===== combine")
for a in ((date(2000, 1, 2), time(3, 4)), (date(2000, 1, 2), time(3, 4, tzinfo=tz.utc)), (date(2000, 1, 2), time(3, 4, tzinfo=tz.utc), None), (date(2000, 1, 2), time(3, 4), tz.utc), (date(2000, 1, 2), time(3, 4, fold=1)), (dt(2000, 1, 2, 9, tzinfo=tz.utc), time(3, 4)), (date(2000, 1, 2), time(3, 4), 5), (date(2000, 1, 2), dt(2000, 1, 1)),
          (date(2000, 1, 2), 5), (5, time()), (None, None), (time(), date(1, 1, 1)), (date(2000, 1, 2),), (), (date(1, 1, 1), time(), None, 1), ("x", "y")):
    attempt("combine%r" % (a,), lambda: dt.combine(*a))
attempt("combine: keywords", lambda: dt.combine(date=date(1, 1, 1), time=time(1), tzinfo=tz.utc))
attempt("combine: unknown", lambda: dt.combine(date(1, 1, 1), time(1), tz=tz.utc))
print("===== strptime")
for a in (("2024-02-29 01:02:03", "%Y-%m-%d %H:%M:%S"), ("2024-02-29 01:02:03.5", "%Y-%m-%d %H:%M:%S.%f"), ("2024 +0130", "%Y %z"), ("2024 Z", "%Y %z"), ("2024 UTC", "%Y %Z"), ("2024 -01:30:15.5", "%Y %z"), ("2023-02-29", "%Y-%m-%d"), ("x", "%Y"), ("2024x", "%Y"), ("", ""), (5, "%Y"), ("1", None), ("24", "%y"), ("2024 60", "%Y %j"), ("2024 09 4", "%G %V %u")):
    attempt("strptime%r" % (a,), lambda: dt.strptime(*a))
print("===== of a derived class")
plus1 = tz(td(hours=1)); varies = Z(td(hours=1), td(hours=1))
for label, f in {"fromisoformat": lambda c: c.fromisoformat("2000-01-02T03"), "combine": lambda c: c.combine(date(2000, 1, 2), time(3)), "combine with a fold": lambda c: c.combine(date(2000, 1, 2), time(3, fold=1)), "fromtimestamp": lambda c: c.fromtimestamp(0), "fromtimestamp in a zone": lambda c: c.fromtimestamp(0, tz.utc),
                 "fromtimestamp in a fold": lambda c: c.fromtimestamp(1699164000), "now": lambda c: c.now(), "now in a zone": lambda c: c.now(tz.utc), "today": lambda c: c.today(), "fromordinal": lambda c: c.fromordinal(730120), "fromisocalendar": lambda c: c.fromisocalendar(2000, 1, 1), "strptime": lambda c: c.strptime("2000", "%Y"),
                 "replace": lambda c: c(2000, 1, 2).replace(day=3), "replace fold": lambda c: c(2000, 1, 2).replace(fold=1), "+": lambda c: c(2000, 1, 2) + td(1), "r+": lambda c: td(1) + c(2000, 1, 2), "-": lambda c: c(2000, 1, 2) - td(1), "+ with a fold": lambda c: c(2000, 1, 2, fold=1) + td(1),
                 "astimezone": lambda c: c(2000, 1, 2, tzinfo=tz.utc).astimezone(tz(td(hours=1))), "astimezone, naive": lambda c: c(2000, 1, 2).astimezone(tz.utc), "astimezone to the same": lambda c: c(2000, 1, 2, tzinfo=tz.utc).astimezone(tz.utc), "utctimetuple": lambda c: c(2000, 1, 2, tzinfo=tz(td(hours=1))).utctimetuple(),
                 "date": lambda c: c(2000, 1, 2).date(), "fromutc": lambda c: plus1.fromutc(c(2000, 1, 2, tzinfo=plus1)), "fromutc of tzinfo": lambda c: varies.fromutc(c(2000, 1, 2, tzinfo=varies))}.items():
    for c in (D, DN, DX):
        # CPython takes whatever the class makes for a datetime, and goes down if it is not.
        if c is DX and label in ("astimezone", "astimezone, naive", "utctimetuple", "fromutc of tzinfo"): continue
        DX.made = False
        attempt("%s %s" % (c.__name__, label), lambda: (lambda r: (type(r).__name__, getattr(r, "extra", "no extra"), getattr(r, "fold", None)))(f(c)))
print("===== replace")
d = dt(2024, 2, 29, 1, 2, 3, 4, tzinfo=tz.utc, fold=1)
for k in ({}, {"year": 2020}, {"year": 2023}, {"month": 3}, {"day": 30}, {"hour": 5}, {"hour": 24}, {"minute": 60}, {"second": -1}, {"microsecond": 10**6}, {"tzinfo": None}, {"tzinfo": 5}, {"fold": 0}, {"fold": 2}, {"year": 2**31}, {"year": None}, {"year": 1.0}, {"hour": True}, {"hour": Index()}, {"days": 1}, {"year": 10**30}):
    attempt("replace(%r)" % (sorted(k.items(), key=str),), lambda: d.replace(**k))
    attempt("   __replace__", lambda: d.__replace__(**k))
attempt("positional", lambda: d.replace(2020, 1, 2, 3, 4, 5, 6, None))
attempt("fold by position", lambda: d.replace(2020, 1, 2, 3, 4, 5, 6, None, 0))
print("===== arithmetic")
others = [td(), td(1), td(-1), td(0, 1), td(0, 0, 1), td(0, -1), td(0, 0, -1), td(0, 86399, 999999), td(365, 3661, 7), td(3652058), td(-3652058), td(3652058, 86399, 999999), td(3652059), td.max, td.min, 0, 1.5, None, "a", date(2000, 1, 1), time(1)]
for a in (dt(1, 1, 1), dt(1, 1, 1, 0, 0, 0, 1), dt(2024, 2, 28, 23, 59, 59, 999999), dt(2024, 2, 29), dt(2023, 12, 31, 23, 59, 59), dt(9999, 12, 31, 23, 59, 59, 999999), dt(9999, 12, 31), dt(2000, 1, 1, tzinfo=tz.utc), dt(2000, 1, 1, fold=1), D(2000, 1, 1)):
    for b in others:
        for name, op in (("+", operator.add), ("-", operator.sub)):
            attempt("%r %s %r" % (a, name, b), lambda: (lambda r: (r, type(r).__name__))(op(a, b)))
            attempt("   the other way", lambda: (lambda r: (r, type(r).__name__))(op(b, a)))
same = Z(td(hours=1))
whens = [dt(2000, 1, 1), dt(2000, 1, 1, 0, 0, 0, 1), dt(1999, 12, 31, 23, 59, 59, 999999), dt(1, 1, 1), dt(9999, 12, 31, 23, 59, 59, 999999), dt(2000, 1, 1, fold=1), dt(2000, 1, 1, tzinfo=tz.utc), dt(2000, 1, 1, 1, tzinfo=tz(td(hours=1))), dt(1999, 12, 31, 19, tzinfo=tz(td(hours=-5))), dt(2000, 1, 1, tzinfo=tz(td(0), "x")),
         dt(2000, 1, 1, tzinfo=same), dt(2000, 1, 2, tzinfo=same), dt(2000, 1, 1, tzinfo=Z(td(hours=1))), dt(2000, 1, 1, tzinfo=Z()), dt(1, 1, 1, tzinfo=tz(td(hours=23))), dt(9999, 12, 31, 23, tzinfo=tz(td(hours=-23))), dt(2000, 1, 1, 0, 0, 0, 5, tzinfo=tz(td(microseconds=5))), D(2000, 1, 1), D(2000, 1, 1, tzinfo=tz.utc)]
for a in whens:
    attempt("%r -" % a, lambda: [quiet(operator.sub, a, b) for b in whens])
print("===== compared")
cmps = [("==", operator.eq), ("!=", operator.ne), ("<", operator.lt), ("<=", operator.le), (">", operator.gt), (">=", operator.ge)]
for name, op in cmps:
    for a in whens:
        attempt("%r %s" % (a, name), lambda: [quiet(op, a, b) for b in whens])
    for a in (whens[0], whens[6]):
        for b in (date(2000, 1, 1), date(2000, 1, 2), 0, None, "2000-01-01", td(1), time(0), 946684800):
            attempt("   %r %s %r" % (a, name, b), lambda: op(a, b))
            attempt("   the other way", lambda: op(b, a))
attempt("hashes that are to be equal", lambda: [[hash(a) == hash(b) for b in whens if quiet(operator.eq, a, b) is True] for a in whens])
attempt("hash of a naive one", lambda: (hash(dt(2000, 1, 2, 3, 4, 5, 6)) == hash(dt(2000, 1, 2, 3, 4, 5, 6).__reduce__()[1][0]), hash(dt(2000, 1, 1, fold=1)) == hash(dt(2000, 1, 1))))
attempt("hash with an offset", lambda: hash(dt(2000, 1, 1, 5, tzinfo=tz(td(hours=5)))) == hash(td(days=730120)))
print("===== what a tzinfo gives")
bad = [None, td(0), td(hours=23, minutes=59, seconds=59, microseconds=999999), td(hours=24), td(hours=-24), td(hours=-24, microseconds=1), td(seconds=30), 0, 1.5, "x", ZeroDivisionError("raised")]
for v in bad:
    z = Z(offset=v, dst=v)
    d = dt(2000, 1, 1, 12, tzinfo=z)
    for label, f in {"utcoffset": d.utcoffset, "dst": d.dst, "isoformat": d.isoformat, "str": lambda: str(d), "%z": lambda: d.strftime("%z"), "hash": lambda: hash(d) == hash(d), "==": lambda: d == dt(2000, 1, 1, 12, tzinfo=tz.utc), "<": lambda: d < dt(2000, 1, 1, 12, tzinfo=tz.utc), "== naive": lambda: d == dt(2000, 1, 1, 12),
                     "< naive": lambda: d < dt(2000, 1, 1, 13), "-": lambda: d - dt(2000, 1, 1, tzinfo=tz.utc), "- naive": lambda: d - dt(2000, 1, 1), "timetuple": lambda: tuple(d.timetuple()), "utctimetuple": lambda: tuple(d.utctimetuple()), "timestamp": d.timestamp, "astimezone": lambda: d.astimezone(tz.utc), "fromutc": lambda: z.fromutc(d),
                     "now": lambda: type(dt.now(z)).__name__, "fromtimestamp": lambda: dt.fromtimestamp(0, z)}.items():
        attempt("%r: %s" % (v, label), f)
class NoCall(tzinfo):
    utcoffset = None; dst = 5; tzname = None; fromutc = None
attempt("not callable", lambda: [attempt("   " + n, f) for n, f in {"utcoffset": dt(1, 1, 1, tzinfo=NoCall()).utcoffset, "dst": dt(1, 1, 1, tzinfo=NoCall()).dst, "tzname": dt(1, 1, 1, tzinfo=NoCall()).tzname, "now": lambda: dt.now(NoCall())}.items()] and None)
log.clear()
attempt("what it is given", lambda: (dt(2000, 1, 1, tzinfo=Z(td(0), td(0), "n"), fold=1).utcoffset(), hash(dt(2000, 1, 1, tzinfo=Z(td(0)), fold=1)) is None, D(2000, 1, 1, tzinfo=Z(td(0)), fold=1) == dt(2000, 1, 1, tzinfo=tz.utc), list(log)))
print("===== a time that comes twice, or not at all")
byfold = Z(lambda w: td(hours=-4) if not w.fold else td(hours=-5), lambda w: td(hours=1) if not w.fold else td(0), lambda w: "EDT" if not w.fold else "EST")
for f1 in (0, 1):
    a = dt(2000, 1, 1, 12, tzinfo=byfold, fold=f1)
    for b in (dt(2000, 1, 1, 16, tzinfo=tz.utc), dt(2000, 1, 1, 17, tzinfo=tz.utc), dt(2000, 1, 1, 12, tzinfo=byfold), dt(2000, 1, 1, 12, tzinfo=byfold, fold=1), dt(2000, 1, 1, 12, tzinfo=Z(byfold.o), fold=f1), dt(2000, 1, 1, 12)):
        attempt("fold %d and %r" % (f1, b), lambda: (a == b, a != b, b == a, quiet(operator.lt, a, b), quiet(operator.le, a, b), quiet(operator.sub, a, b), hash(a) == hash(b)))
print("===== fromutc")
for o, s in ((td(hours=1), td(0)), (td(hours=1), td(hours=1)), (td(hours=-5), td(hours=1)), (td(hours=1), None), (None, td(0)), (td(hours=2), lambda w: td(hours=1) if w.hour < 12 else td(0)), (td(hours=2), lambda w: td(hours=1) if w.hour < 12 else None), (td(hours=23), td(hours=-23))):
    z = Z(o, s)
    for d in (dt(2000, 1, 1, 6, tzinfo=z), dt(2000, 1, 1, 11, tzinfo=z), dt(2000, 1, 1, 18, tzinfo=z), dt(1, 1, 1, tzinfo=z), dt(9999, 12, 31, 23, tzinfo=z)):
        attempt("fromutc(%s) with %r" % (d.isoformat()[:16], (o, s if not callable(s) else "varies")), lambda: z.fromutc(d).replace(tzinfo=None))
for v in (dt(2000, 1, 1), dt(2000, 1, 1, tzinfo=tz.utc), date(2000, 1, 1), None, 5):
    attempt("fromutc(%r)" % (v,), lambda: Z(td(0), td(0)).fromutc(v))
print("done")
