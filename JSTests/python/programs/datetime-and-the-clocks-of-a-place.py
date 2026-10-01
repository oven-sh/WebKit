# datetime and the clocks of a place: timestamps, local time, and times that come twice or not at all
import os, time as _time, sys, hashlib, warnings
import datetime
from datetime import datetime as dt, date, time, timedelta as td, timezone as tz, tzinfo
import re; address = re.compile('0x[0-9a-f]+')
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
    print(label, "->", address.sub("0x", ascii(r)))
class I(int): pass
class F(float): pass
class Index:
    def __index__(self): return 3
    def __repr__(self): return "Index()"
def zone(name):
    os.environ["TZ"] = name; _time.tzset()
def show(d): return (d.isoformat(), d.fold, d.tzname(), d.utcoffset())
for name in ("America/New_York", "Europe/London", "Australia/Lord_Howe", "Asia/Kolkata", "UTC", "Pacific/Kiritimati", "America/St_Johns", "EST5EDT", "XXX-3:30", "Africa/Casablanca"):
    zone(name)
    print("=====", name)
    # Each hour of the days on which clocks in one place or another are changed, and of some on which none are
    for start in (946684800, 1699142400, 1710032400 - 86400, 1711846800 - 43200, 1729990800 - 43200, 1696082400 - 43200, 1712412000 - 43200, 0, -86400, 86400 * 365 * 30, 1e9):
        lines = []
        for k in range(0, 48 * 2):
            t = start + k * 1800
            d = dt.fromtimestamp(t)
            a = d.astimezone()
            lines.append((t, d.isoformat(), d.fold, d.timestamp() == t, show(a), a.timestamp() == t, dt.fromtimestamp(t, tz.utc).astimezone().isoformat(), date.fromtimestamp(t).isoformat(), d.astimezone(tz.utc).isoformat()))
        h = hashlib.md5(repr(lines).encode()).hexdigest()
        print(int(start), h, [l[1:3] for l in lines if l[2]][:4])
        if len(sys.argv) > 1:
            for l in lines: print("  ", l)
    # Every half hour by the clock, with each fold, whether or not there is such a time
    for day in (dt(2023, 11, 5), dt(2024, 3, 10), dt(2024, 3, 31), dt(2024, 10, 27), dt(2023, 10, 1), dt(2024, 4, 7), dt(2000, 1, 1), dt(1970, 1, 1), dt(2037, 6, 15)):
        lines = []
        for k in range(0, 10):
            for fold in (0, 1):
                d = (day + td(minutes=30 * k)).replace(fold=fold)
                lines.append((d.isoformat(), fold, d.timestamp(), show(d.astimezone()), show(d.astimezone(tz.utc)), dt.fromtimestamp(d.timestamp()).isoformat()))
        print(day.date(), hashlib.md5(repr(lines).encode()).hexdigest(), [l[2] for l in lines[2:8]])
        if len(sys.argv) > 1:
            for l in lines: print("  ", l)
zone("America/New_York")
print("===== fromtimestamp")
values = [0, 1, -1, 0.5, -0.5, 1.5, 0.000001, 0.0000005, 0.0000015, 0.0000025, -0.0000005, -0.0000015, 0.9999995, 0.9999994, 1.9999995, -0.9999995, 1e-7, 1e9, 1e9 + 0.1, 1234567890.123456, 1234567890.1234565, 1234567890.1234575, 2**31, 2**31 - 0.5, 2**32, 253402318799, 253402318799.9, 253402318799.9999999, 253402318800, 253402300799, 253402300800,
          -62135579038, -62135579039, -62135596800, -62135596801, -62135510400, 1e11, 1e12, 1e13, 1e17, 1e18, 1e19, 1e20, -1e19, 1e300, 2**62, 2**63 - 1, 2**63, -2**63, -2**63 - 1, 2**64, 10**30, float("inf"), float("-inf"), float("nan"), True, False, None, "1", b"1", [], 1j, I(5), F(1.5), Index()]
for v in values:
    attempt("fromtimestamp(%r)" % (v,), lambda: dt.fromtimestamp(v))
    attempt("   in UTC", lambda: dt.fromtimestamp(v, tz.utc))
    attempt("   an hour on", lambda: dt.fromtimestamp(v, tz(td(hours=1))))
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        attempt("   utcfromtimestamp", lambda: dt.utcfromtimestamp(v))
for label, f in {"none": lambda: dt.fromtimestamp(), "three": lambda: dt.fromtimestamp(0, None, 1), "keywords": lambda: dt.fromtimestamp(timestamp=0, tz=tz.utc), "unknown": lambda: dt.fromtimestamp(0, tzinfo=tz.utc), "a bad zone": lambda: dt.fromtimestamp(0, 5), "a bad zone and a bad time": lambda: dt.fromtimestamp("x", 5), "tz None": lambda: dt.fromtimestamp(0, None)}.items():
    attempt("fromtimestamp: " + label, f)
print("===== what is deprecated")
for label, f in {"utcnow": lambda: type(dt.utcnow()).__name__, "utcfromtimestamp": lambda: dt.utcfromtimestamp(0), "utcfromtimestamp: keyword": lambda: dt.utcfromtimestamp(timestamp=0), "utcnow: an argument": lambda: dt.utcnow(1)}.items():
    with warnings.catch_warnings(record=True) as w:
        warnings.simplefilter("always")
        attempt(label, f)
        print("  ", [(x.category.__name__, str(x.message), os.path.basename(x.filename), x.lineno > 0) for x in w])
    with warnings.catch_warnings():
        warnings.simplefilter("error")
        attempt("   as an error", f)
print("===== now")
def near(a, b): return abs(a - b) < td(seconds=5)
attempt("now", lambda: (near(dt.now(), dt.fromtimestamp(_time.time())), dt.now().tzinfo, near(dt.now(tz.utc), dt.fromtimestamp(_time.time(), tz.utc)), dt.now(tz.utc).tzinfo is tz.utc, near(dt.now(tz=tz(td(hours=5))), dt.now(tz.utc)), dt.now(None).tzinfo, near(dt.today(), dt.now())))
attempt("now: a bad zone", lambda: dt.now(5))
attempt("now: two", lambda: dt.now(None, 1))
attempt("now: unknown", lambda: dt.now(tzinfo=None))
attempt("goes on", lambda: dt.now() <= dt.now() <= dt.now())
print("===== timestamp")
for d in (dt(1970, 1, 1), dt(1970, 1, 1, tzinfo=tz.utc), dt(1969, 12, 31, 19), dt(2000, 1, 1, 0, 0, 0, 1), dt(2000, 1, 1, 0, 0, 0, 999999), dt(1, 1, 1), dt(1, 1, 2), dt(1, 1, 1, tzinfo=tz.utc), dt(9999, 12, 31, 23, 59, 59, 999999), dt(9999, 12, 31, 23, 59, 59, 999999, tzinfo=tz.utc), dt(9999, 12, 31), dt(1900, 1, 1), dt(1883, 11, 18, 12), dt(2038, 1, 19, 3, 14, 8),
          dt(1, 1, 1, tzinfo=tz(td(hours=23))), dt(9999, 12, 31, 23, tzinfo=tz(td(hours=-23))), dt(2000, 1, 1, tzinfo=tz(td(microseconds=1)))):
    attempt("timestamp of %r" % d, d.timestamp)
    attempt("   astimezone()", lambda: show(d.astimezone()))
    attempt("   astimezone(utc)", lambda: show(d.astimezone(tz.utc)))
    attempt("   astimezone(+14)", lambda: show(d.astimezone(tz(td(hours=14)))))
    attempt("   astimezone(-14)", lambda: show(d.astimezone(tz(td(hours=-14)))))
print("===== astimezone")
for label, f in {"a bad zone": lambda: dt(2000, 1, 1).astimezone(5), "two": lambda: dt(2000, 1, 1).astimezone(None, 1), "keyword": lambda: show(dt(2000, 1, 1).astimezone(tz=tz.utc)), "unknown": lambda: dt(2000, 1, 1).astimezone(tzinfo=tz.utc), "None": lambda: show(dt(2000, 6, 1).astimezone(None)),
                 "to its own": lambda: (lambda d: d.astimezone(tz.utc) is d)(dt(2000, 1, 1, tzinfo=tz.utc)), "to one that is equal": lambda: (lambda d: d.astimezone(tz(td(0), "x")) is d)(dt(2000, 1, 1, tzinfo=tz.utc)), "the zone it gives": lambda: (lambda z: (type(z).__name__, repr(z)))(dt(2000, 6, 1).astimezone().tzinfo),
                 "in winter": lambda: repr(dt(2000, 1, 1).astimezone().tzinfo), "keeps the microsecond": lambda: dt(2000, 1, 1, 1, 2, 3, 4).astimezone(tz.utc).microsecond, "the fold goes": lambda: dt(2023, 11, 5, 1, 30, fold=1).astimezone(tz.utc).fold}.items():
    attempt(label, f)
class Z(tzinfo):
    def __init__(self, o, f=None): self.o = o; self.f = f
    def utcoffset(self, w): return self.o
    def dst(self, w): return None
    def tzname(self, w): return "Z"
    def fromutc(self, w):
        if self.f is None: return ("fromutc", w.isoformat(), w.tzinfo is self, type(w).__name__)
        return self.f(w)
attempt("what fromutc() is given", lambda: dt(2000, 1, 1, 12, tzinfo=tz(td(hours=1))).astimezone(Z(td(0))))
attempt("   of a naive one", lambda: dt(2000, 1, 1, 12).astimezone(Z(td(0))))
attempt("   raises", lambda: dt(2000, 1, 1, 12).astimezone(Z(td(0), lambda w: 1 / 0)))
attempt("from one that says nothing", lambda: show(dt(2000, 1, 1, 12, tzinfo=Z(None)).astimezone(tz.utc)))
attempt("   to itself", lambda: (lambda z: dt(2000, 1, 1, 12, tzinfo=z).astimezone(z).hour)(Z(None)))
attempt("now, with it", lambda: dt.now(Z(td(0)))[2:])
attempt("fromtimestamp, with it", lambda: dt.fromtimestamp(0, Z(td(0))))
print("===== timetuple")
class DST(tzinfo):
    def __init__(self, d): self.d = d
    def utcoffset(self, w): return td(hours=1)
    def dst(self, w): return self.d
for v in (None, td(0), td(hours=1), td(microseconds=1), td(hours=-1)):
    attempt("dst() gives %r" % (v,), lambda: dt(2000, 1, 1, tzinfo=DST(v)).timetuple().tm_isdst)
attempt("naive", lambda: (dt(2000, 6, 1).timetuple().tm_isdst, dt(2000, 6, 1).timetuple().tm_zone, dt(2000, 6, 1).timetuple().tm_gmtoff, dt(2000, 6, 1).utctimetuple().tm_isdst))
attempt("utctimetuple at the ends", lambda: [attempt("   %s" % n, f) for n, f in {"before the first": lambda: tuple(dt(1, 1, 1, tzinfo=tz(td(hours=1))).utctimetuple()), "after the last": lambda: tuple(dt(9999, 12, 31, 23, 30, tzinfo=tz(td(hours=-1))).utctimetuple())}.items()] and None)
print("done")
