# datetime.time, datetime.tzinfo and datetime.timezone
import _datetime, datetime, pickle, copy, operator, sys
from datetime import time, timedelta as td, timezone as tz, tzinfo
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
class T(time): pass
class TN(time):
    def __new__(cls, *a, **k):
        r = time.__new__(cls, *a, **k); r.extra = sorted(k); return r
class TX(time):
    def __new__(cls, *a, **k): return "not a time"
log = []
class Z(tzinfo):
    "A tzinfo that gives what it was told to."
    def __init__(self, offset=None, dst=None, name=None): self.o, self.d, self.n = offset, dst, name
    def give(self, what, dt):
        log.append((type(dt).__name__, getattr(dt, "fold", None)))
        if isinstance(what, BaseException): raise what
        return what
    def utcoffset(self, dt): return self.give(self.o, dt)
    def dst(self, dt): return self.give(self.d, dt)
    def tzname(self, dt): return self.give(self.n, dt)
    def __repr__(self): return "Z(%r, %r, %r)" % (self.o, self.d, self.n)
class TD(td): pass
print("===== making one")
values = [0, 1, -1, 23, 24, 59, 60, 61, 999999, 1000000, 2**31 - 1, 2**31, -2**31 - 1, 10**30, True, False, 1.0, 1.5, None, "1", b"1", [], I(5), Index()]
for v in values:
    for n in ("hour", "minute", "second", "microsecond", "fold"):
        attempt("%s %r" % (n, v), lambda: time(**{n: v}))
for v in (None, tz.utc, tz(td(hours=1)), Z(), 5, "UTC", td(0), tzinfo(), tzinfo, tz, object(), False, 0):
    attempt("tzinfo %s" % address.sub("0x", repr(v)), lambda: time(1, tzinfo=v).tzinfo is v)
for label, f in {"none": lambda: time(), "positional": lambda: time(1, 2, 3, 4, tz.utc), "fold by position": lambda: time(1, 2, 3, 4, None, 1), "seven": lambda: time(1, 2, 3, 4, None, 1, 1), "unknown": lambda: time(1, hours=1), "twice": lambda: time(1, hour=1), "a bad one and too many": lambda: time("x", 2, 3, 4, None, 1),
                 "a bad one and an unknown": lambda: time("x", zzz=1), "bad fold and an unknown": lambda: time(fold="x", zzz=1), "out of range, and a bad tzinfo": lambda: time(24, tzinfo=5), "bad fold value and a bad tzinfo": lambda: time(fold=2, tzinfo=5), "too many with keywords": lambda: time(1, 2, 3, 4, None, fold=1, a=1)}.items():
    attempt(label, f)
print("===== what pickle calls it with")
for s in (b"\x01\x02\x03\x04\x05\x06", b"\x17\x3b\x3b\x0f\x42\x3f", b"\x18\x00\x00\x00\x00\x00", b"\x81\x02\x03\x04\x05\x06", b"\x97\x00\x00\x00\x00\x00", b"\x98\x00\x00\x00\x00\x00", b"\x00\xff\xff\xff\xff\xff", b"\x01\x02\x03\x04\x05", b"\x01\x02\x03\x04\x05\x06\x07", b"",
          "\x01\x02\x03\x04\x05\x06", "\x81\x02\x03\x04\x05\x06", "\x18\x02\x03\x04\x05\x06", "\x01\u0100\x03\x04\x05\x06", "\u0101\x02\x03\x04\x05\x06", "\u0118\x02\x03\x04\x05\x06", "\x01\x02\x03\x04\x05", bytearray(b"\x01\x02\x03\x04\x05\x06")):
    attempt("state %r" % (s,), lambda: (lambda t: (t.hour, t.minute, t.second, t.microsecond, t.fold, t.tzinfo, repr(t)))(time(s)))
    for z in (None, tz.utc, 5, "x", Z()):
        attempt("   with %r" % (z,), lambda: (lambda t: (t.hour, t.fold, t.tzinfo is z))(time(s, z)))
    attempt("   with three", lambda: time(s, None, 1))
    attempt("   and a keyword", lambda: time(s, fold=1))
    attempt("   of a derived class", lambda: type(T(s)).__name__)
print("===== shown")
zones = [None, tz.utc, tz(td(hours=5, minutes=30)), tz(td(hours=-5)), tz(td(seconds=1)), tz(td(microseconds=1)), tz(td(hours=-23, minutes=-59, seconds=-59, microseconds=-999999)), tz(td(hours=1), "name"), tz(td(0), "zero"), Z(), Z(td(hours=1), td(hours=1), "Z"), Z(td(minutes=-90), None, "a%b")]
for z in zones:
    for t in (time(tzinfo=z), time(1, tzinfo=z), time(1, 2, tzinfo=z), time(1, 2, 3, tzinfo=z), time(1, 2, 3, 4, tzinfo=z), time(23, 59, 59, 999999, tzinfo=z), time(0, 0, 0, 1000, tzinfo=z), time(1, 2, 3, 4, tzinfo=z, fold=1), time(12, fold=1, tzinfo=z), T(1, 2, tzinfo=z), TN(1, 2, 3, tzinfo=z, fold=1)):
        attempt(repr(t), lambda: (str(t), t.isoformat(), t.hour, t.minute, t.second, t.microsecond, t.fold, t.tzinfo is z, format(t), bool(t), t.utcoffset(), t.dst(), t.tzname()))
        attempt("   timespec", lambda: [t.isoformat(s) for s in ("auto", "hours", "minutes", "seconds", "milliseconds", "microseconds")])
        attempt("   strftime", lambda: t.strftime("%H:%M:%S.%f|%z|%:z|%Z|%Y-%m-%d|%j|%a|%p|%I|%%"))
        attempt("   reduce", lambda: (t.__reduce__()[1][0], len(t.__reduce__()[1]), [t.__reduce_ex__(p)[1][0] for p in range(6)]))
        attempt("   pickle", lambda: [(lambda r: (r == t, type(r) is type(t), r.fold, r.tzinfo == t.tzinfo if not isinstance(z, Z) else type(r.tzinfo).__name__))(pickle.loads(pickle.dumps(t, p))) for p in range(6)])
        attempt("   copy", lambda: (copy.copy(t) == t, copy.deepcopy(t) == t, copy.copy(t) is t, copy.replace(t, hour=5)))
        attempt("   and back", lambda: time.fromisoformat(t.isoformat()) == t)
attempt("pickled", lambda: [pickle.dumps(time(1, 2, 3, 4, tzinfo=tz(td(hours=1), "n"), fold=1), p) for p in (0, 2, 4)])
for label, f in {"two": lambda: time().isoformat("auto", 1), "keyword": lambda: time(1).isoformat(timespec="hours"), "unknown keyword": lambda: time().isoformat(spec="hours"), "unknown": lambda: time().isoformat("days"), "empty": lambda: time().isoformat(""), "int": lambda: time().isoformat(5), "None": lambda: time().isoformat(None),
                 "bytes": lambda: time().isoformat(b"hours"), "a null": lambda: time().isoformat("hours\0"), "a surrogate": lambda: time().isoformat("\ud800"), "capitals": lambda: time().isoformat("Hours"), "a derived str": lambda: time(1).isoformat(type("S", (str,), {})("hours")), "not ASCII": lambda: time().isoformat("\xe9")}.items():
    attempt("isoformat: " + label, f)
for label, f in {"none": lambda: time().strftime(), "int": lambda: time().strftime(5), "keyword": lambda: time(1).strftime(format="%H"), "two": lambda: time().strftime("a", "b"), "__format__": lambda: (format(time(1, 2), ""), format(time(1, 2), "%M"), time(1).__format__("%H")), "__format__ int": lambda: time().__format__(5),
                 "__reduce_ex__ none": lambda: time().__reduce_ex__(), "__reduce_ex__ str": lambda: time().__reduce_ex__("2"), "__reduce_ex__ two": lambda: time().__reduce_ex__(1, 2), "__reduce_ex__ huge": lambda: time().__reduce_ex__(2**40), "__reduce_ex__ negative": lambda: time(fold=1).__reduce_ex__(-1), "__reduce__ one": lambda: time().__reduce__(1),
                 "__reduce_ex__ keyword": lambda: time().__reduce_ex__(protocol=2)}.items():
    attempt(label, f)
print("===== what a tzinfo gives")
bad = [None, td(0), td(hours=23, minutes=59, seconds=59, microseconds=999999), td(hours=24), td(hours=-24), td(hours=-24, microseconds=1), td(days=1, seconds=1), td(days=-2), td(seconds=30), td(microseconds=-1), TD(hours=1), 0, 60, 1.5, "x", (), ZeroDivisionError("raised"), True]
for v in bad:
    for which in ("utcoffset", "dst"):
        z = Z(**{("offset" if which == "utcoffset" else "dst"): v})
        attempt("%s gives %r" % (which, v), lambda: getattr(time(1, tzinfo=z), which)())
    z = Z(offset=v)
    attempt("   isoformat", lambda: time(1, tzinfo=z).isoformat())
    attempt("   str", lambda: str(time(1, tzinfo=z)))
    attempt("   %z", lambda: time(1, tzinfo=z).strftime("%z %:z"))
    attempt("   hash", lambda: hash(time(1, tzinfo=z)) == hash(time(1, tzinfo=z)))
    attempt("   ==", lambda: time(1, tzinfo=z) == time(1, tzinfo=tz.utc))
    attempt("   <", lambda: time(1, tzinfo=z) < time(1, tzinfo=tz.utc))
    attempt("   == a naive one", lambda: time(1, tzinfo=z) == time(1))
    attempt("   < a naive one", lambda: time(1, tzinfo=z) < time(2))
    attempt("   bool", lambda: bool(time(0, tzinfo=z)))
for v in (None, "name", "", "a%b", "%", "%%", "%Y", "\xe9", "\u20ac", "\U0001f600", "\ud800", "a\0b", 5, b"x", (), ZeroDivisionError("raised"), type("S", (str,), {})("derived"), type("S", (str,), {"replace": lambda s, a, b: 5})("x"), type("S", (str,), {"replace": lambda s, a, b: "replaced"})("x"), type("S", (str,), {"replace": None})("x")):
    z = Z(name=v)
    attempt("tzname gives %r" % (v,), lambda: time(1, tzinfo=z).tzname())
    attempt("   %Z", lambda: time(1, tzinfo=z).strftime("[%Z]"))
    attempt("   twice", lambda: time(1, tzinfo=z).strftime("%Z%Z"))
log.clear()
attempt("what it is given", lambda: (time(1, tzinfo=Z(td(0), td(0), "n"), fold=1).utcoffset(), time(1, tzinfo=Z(td(0), td(0), "n")).dst(), time(1, tzinfo=Z(td(0), td(0), "n")).tzname(), hash(time(1, tzinfo=Z(td(0)), fold=1)) is None, list(log)))
log.clear()
attempt("asked once for each in strftime", lambda: (time(1, tzinfo=Z(td(0), td(0), "n")).strftime("%z%z%:z%:z%Z%Z"), len(log)))
print("===== fromisoformat")
texts = ["00", "01", "23", "24", "25", "0", "1", "", "T", "T01", "t01", "TT01", "01:02", "0102", "01:02:03", "010203", "01:0203", "0102:03", "01:02:03.4", "01:02:03.45", "01:02:03.456", "01:02:03.4567", "01:02:03.45678", "01:02:03.456789", "01:02:03.4567891", "01:02:03.456789123456789", "01:02:03,456", "010203.456", "010203,456789",
         "01:02:03.", "01:02:03,", "01:02.5", "01.5", "01:02:03.x", "01:02:03.4x", "01:02:03.456789x", "01:02:03:04", "01:02:", "01:", ":01", "01::02", "1:02", "01:2", "01:02:3", "60", "01:60", "01:02:60", "24:00", "24:00:00", "24:00:00.000000", "24:00:00.000001", "24:01", "24:00:01", "2400", "240000",
         "01Z", "01:02Z", "01:02:03Z", "01:02:03.456Z", "01z", "01ZZ", "01Z0", "Z", "01+00", "01+00:00", "01-00:00", "01+0000", "01+01", "01+01:30", "01+0130", "01-01:30", "01+01:30:15", "01+013015", "01+01:30:15.5", "01+01:30:15.123456", "01+01:30:15.1234567", "01-01:30:15.123456", "01+01:30:15,5", "01+24", "01+24:00", "01+23:59", "01+23:59:59.999999",
         "01-23:59:59.999999", "01-24:00", "01+25", "01+1", "01+", "01-", "01+01:", "01+01:3", "01+01:60", "01+01:30:60", "01+01:30Z", "01+01+01", "01+01-01", "01 +01", "01+ 01", "01+00:00:00.000001", "01-00:00:00.000001", "01+00:00:00.000000", "24:00+01:00", "24Z",
         "\uff10\uff11", "01:02:03\0", "01\0", "01:02:03.4\0005", "\ud80001", "01\ud800", "01:02\u2236", "01\u221201", "01:02:03.\u0664", " 01", "01 ", "01:02:03 Z", "1e1", "+1", "-1", "0x", "T24:00", "T0102Z", "T01:02:03.5+01:00"]
for t in texts:
    attempt("fromisoformat(%a)" % t, lambda: (lambda r: (r, r.tzinfo is tz.utc))(time.fromisoformat(t)))
for v in (None, 5, b"01", [], type("S", (str,), {})("01:02")):
    attempt("fromisoformat(%r)" % (v,), lambda: time.fromisoformat(v))
attempt("none", lambda: time.fromisoformat())
for a in (("01:02:03", "%H:%M:%S"), ("1", "%H"), ("25", "%H"), ("01 +0100", "%H %z"), ("2024 01", "%Y %H"), ("", ""), (5, "%H"), ("1", None)):
    attempt("strptime%r" % (a,), lambda: time.strptime(*a))
print("===== of a derived class")
for label, f in {"fromisoformat": lambda c: c.fromisoformat("01:02"), "fromisoformat with a zone": lambda c: c.fromisoformat("01:02+01:00"), "replace": lambda c: c(1, 2).replace(hour=3), "replace fold": lambda c: c(1, 2).replace(fold=1), "replace of one with a fold": lambda c: c(1, 2, fold=1).replace(hour=3),
                 "strptime": lambda c: c.strptime("1", "%H"), "copy.replace": lambda c: copy.replace(c(1, 2), hour=3)}.items():
    for c in (T, TN, TX):
        attempt("%s %s" % (c.__name__, label), lambda: (lambda r: (type(r).__name__, getattr(r, "extra", "no extra"), getattr(r, "fold", None)))(f(c)))
print("===== replace")
t = time(1, 2, 3, 4, tzinfo=tz.utc, fold=1)
for k in ({}, {"hour": 5}, {"minute": 5}, {"second": 5}, {"microsecond": 5}, {"tzinfo": None}, {"tzinfo": tz(td(hours=1))}, {"fold": 0}, {"fold": 2}, {"hour": 24}, {"hour": -1}, {"hour": 2**31}, {"hour": 10**30}, {"hour": None}, {"hour": 1.0}, {"hour": "1"}, {"hour": True}, {"hour": Index()}, {"tzinfo": 5}, {"fold": None}, {"year": 1}, {"fold": True}):
    attempt("replace(%r)" % (sorted(k.items(), key=str),), lambda: t.replace(**k))
    attempt("   __replace__", lambda: t.__replace__(**k))
attempt("positional", lambda: t.replace(5, 6, 7, 8, None))
attempt("fold by position", lambda: t.replace(5, 6, 7, 8, None, 0))
attempt("twice", lambda: t.replace(5, hour=1))
print("===== compared")
cmps = [("==", operator.eq), ("!=", operator.ne), ("<", operator.lt), ("<=", operator.le), (">", operator.gt), (">=", operator.ge)]
same = Z(td(hours=1))
times = [time(), time(1), time(1, 0, 0, 1), time(0, 59, 59, 999999), time(1, fold=1), time(1, tzinfo=tz.utc), time(2, tzinfo=tz(td(hours=1))), time(0, tzinfo=tz(td(hours=-1))), time(1, tzinfo=tz(td(0), "x")), time(1, tzinfo=same), time(2, tzinfo=same), time(1, tzinfo=Z(td(hours=1))), time(1, tzinfo=Z()), time(0, tzinfo=tz(td(hours=23))),
         time(23, tzinfo=tz(td(hours=-23))), time(1, 0, 0, 5, tzinfo=tz(td(microseconds=5))), time(1, tzinfo=tz(td(seconds=-1, microseconds=1))), T(1), T(1, tzinfo=tz.utc)]
for name, op in cmps:
    for a in times:
        attempt("%r %s" % (a, name), lambda: [attemptless(op, a, b) for b in times])
        if a is times[1] or a is times[5]:
            for b in (0, None, "01:00", (1, 0), td(hours=1), datetime.date(1, 1, 1), datetime.datetime(1, 1, 1, 1), 3600):
                attempt("   %r" % (b,), lambda: op(a, b))
                attempt("   the other way", lambda: op(b, a))
def attemptless(op, a, b):
    try: return op(a, b)
    except TypeError as e: return "T"
for name, op in cmps:
    for a in times:
        attempt("%r %s" % (a, name), lambda: [attemptless(op, a, b) for b in times])
attempt("hashes that are to be equal", lambda: [[hash(a) == hash(b) for b in times if attemptless(operator.eq, a, b) is True] for a in times])
attempt("hash of a naive one", lambda: (hash(time(1, 2, 3, 4)) == hash(time(1, 2, 3, 4).__reduce__()[1][0]), hash(time(1, fold=1)) == hash(time(1))))
attempt("hash with an offset", lambda: (hash(time(5, tzinfo=tz(td(hours=5)))) == hash(td(0)), hash(time(1, tzinfo=tz(td(hours=5)))) == hash(td(hours=-4)), hash(time(1, 2, 3, 4, tzinfo=tz.utc)) == hash(td(hours=1, minutes=2, seconds=3, microseconds=4))))
attempt("arithmetic", lambda: [attempt("   " + n, f) for n, f in {"+": lambda: time(1) + td(1), "-": lambda: time(1) - time(0), "r+": lambda: td(1) + time(1), "*": lambda: time(1) * 2}.items()] and None)
print("===== attributes")
t = time(1, 2, 3, 4)
for n in ("hour", "minute", "second", "microsecond", "tzinfo", "fold"):
    attempt("set " + n, lambda: setattr(t, n, 0))
    attempt("descriptor " + n, lambda: (type(vars(time)[n]).__name__, vars(time)[n].__doc__, vars(time)[n].__get__(t)))
attempt("another", lambda: setattr(t, "x", 1))
attempt("min, max, resolution", lambda: (time.min, time.max, time.resolution, T.min))
attempt("__new__ of object", lambda: object.__new__(time))
print("===== tzinfo")
attempt("made", lambda: (type(tzinfo()).__name__, type(tzinfo(1, 2, a=3)).__name__, tzinfo() == tzinfo(), hash(tzinfo()) != 0))
for m in ("utcoffset", "dst", "tzname"):
    attempt(m, lambda: getattr(tzinfo(), m)(None))
    attempt("   none", lambda: getattr(tzinfo(), m)())
    attempt("   two", lambda: getattr(tzinfo(), m)(1, 2))
    attempt("   keyword", lambda: getattr(tzinfo(), m)(dt=None))
attempt("attribute", lambda: setattr(tzinfo(), "x", 1))
attempt("of a derived class", lambda: (lambda z: (setattr(z, "x", 1), vars(z)))(Z()))
attempt("reduce", lambda: (tzinfo().__reduce__(), tzinfo().__reduce_ex__(2)))
attempt("reduce of a derived one", lambda: (Z(td(1)).__reduce__()[1:], Z().__reduce_ex__(2)[1:]))
class ZI(tzinfo):
    def __init__(self, a=None): self.a = a
    def __getinitargs__(self): return (self.a,)
class ZB(tzinfo):
    __getinitargs__ = 5
class ZR(tzinfo):
    def __getinitargs__(self): raise ZeroDivisionError("init args")
class ZL(tzinfo):
    def __getinitargs__(self): return [1]
class ZS(tzinfo):
    __slots__ = ("s",)
class ZG(tzinfo):
    def __getstate__(self): return "the state"
    def __setstate__(self, s): self.s = s
attempt("__getinitargs__", lambda: (ZI(5).__reduce__()[1:], pickle.loads(pickle.dumps(ZI(5))).a))
attempt("   not callable", lambda: ZB().__reduce__())
attempt("   raises", lambda: ZR().__reduce__())
attempt("   a list", lambda: ZL().__reduce__()[1:])
attempt("   slots", lambda: (lambda z: (setattr(z, "s", 1), z.__reduce__()[1:], pickle.loads(pickle.dumps(z)).s))(ZS()))
attempt("   __getstate__", lambda: (ZG().__reduce__()[1:], pickle.loads(pickle.dumps(ZG())).s))
attempt("pickle", lambda: [type(pickle.loads(pickle.dumps(tzinfo(), p))).__name__ for p in range(6)])
attempt("pickle of a derived one", lambda: [vars(pickle.loads(pickle.dumps(Z(td(1), None, "n"), p))) for p in range(6)])
attempt("the class", lambda: (tzinfo.__mro__, tzinfo.__doc__, tzinfo.__module__, tzinfo.__name__, bool(tzinfo.__flags__ & (1 << 10)), sorted(vars(tzinfo))))
print("===== timezone")
for a in ((td(0),), (td(0), None), (td(0), "x"), (td(0), ""), (td(hours=1),), (td(hours=1), "x"), (td(hours=24),), (td(hours=-24),), (td(hours=23, minutes=59, seconds=59, microseconds=999999),), (td(hours=-24, microseconds=1),), (td(days=1, microseconds=-1),), (TD(hours=1),), (TD(0),), (0,), (None,), ("x",), (1.5,),
          (td(0), 5), (td(0), b"x"), (td(1), "x"), (5, 5), (), (td(0), "x", 1), (td(hours=1), type("S", (str,), {})("derived")), (td(seconds=1),), (td(microseconds=1),), (td(microseconds=-1),), (td(hours=1), "\u20ac"), (td(hours=1), "\ud800")):
    attempt("timezone%r" % (a,), lambda: (lambda z: (repr(z), str(z), z.utcoffset(None), z.tzname(None), z.dst(None), z is tz.utc, z == tz.utc, z.__getinitargs__(), type(z.utcoffset(None)).__name__))(tz(*a)))
attempt("keywords", lambda: tz(offset=td(hours=1), name="x"))
attempt("unknown", lambda: tz(td(0), nam="x"))
attempt("constants", lambda: (tz.utc, tz.min, tz.max, datetime.UTC is tz.utc, _datetime.UTC is tz.utc, tz.min.utcoffset(None), tz.max.utcoffset(None), tz.utc.utcoffset(None) is td(0)))
zs = [tz.utc, tz(td(0)), tz(td(0), "x"), tz(td(hours=1)), tz(td(hours=1), "x"), tz(td(hours=1), "y"), tz(td(hours=-1)), tz.min, tz.max]
for name, op in cmps:
    attempt("compared " + name, lambda: [[attemptless(op, a, b) for b in zs] for a in zs])
    for b in (None, 0, td(0), "UTC", Z(td(0)), tzinfo()):
        attempt("   %s" % address.sub("0x", repr(b)), lambda: (attemptless(op, tz.utc, b), attemptless(op, b, tz.utc)))
attempt("hashes", lambda: [hash(z) == hash(z.utcoffset(None)) for z in zs])
for m in ("utcoffset", "dst", "tzname", "fromutc"):
    for v in (None, datetime.datetime(2000, 1, 1), datetime.datetime(2000, 1, 1, tzinfo=tz.utc), datetime.datetime(2000, 1, 1, tzinfo=zs[3]), datetime.date(2000, 1, 1), time(1), 5, "x"):
        attempt("%s(%r)" % (m, v), lambda: getattr(zs[3], m)(v))
    attempt("   none", lambda: getattr(zs[3], m)())
attempt("derive", lambda: type("X", (tz,), {}))
attempt("attribute", lambda: setattr(tz.utc, "x", 1))
attempt("pickle", lambda: [[(lambda r: (r == z, r is z, repr(r)))(pickle.loads(pickle.dumps(z, p))) for p in (0, 2, 5)] for z in zs[:5]])
attempt("pickled", lambda: [pickle.dumps(z, 2) for z in zs[:5]])
attempt("reduce", lambda: [z.__reduce__() for z in zs[:5]])
attempt("copy", lambda: [(copy.copy(z) is z, copy.deepcopy(z) is z, copy.copy(z) == z) for z in zs[:5]])
attempt("the class", lambda: (tz.__mro__, tz.__doc__, tz.__module__, bool(tz.__flags__ & (1 << 10)), sorted(vars(tz))))
attempt("tzinfo.__new__(timezone)", lambda: tzinfo.__new__(tz))
attempt("object.__new__(timezone)", lambda: object.__new__(tz))
attempt("tzinfo.fromutc of a timezone", lambda: tzinfo.fromutc(zs[3], datetime.datetime(2000, 1, 1, tzinfo=zs[3])))
print("done")
