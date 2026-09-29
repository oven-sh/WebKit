# The module time. What time it is cannot be compared, so this is about what is made of a time that is given, and what is said of one that will not do.
import os
import sys
import time


def show(e):
    return type(e).__name__ + ": " + str(e)


def attempt(f, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return show(e)


def t(label, f):
    r = attempt(f)
    print(label, "=>", ascii(r)[1:-1] if isinstance(r, str) else ascii(r))


def zone(name):
    os.environ["TZ"] = name
    time.tzset()


zone("UTC")
print("---- what there is")
t("the module", lambda: (time.__name__, time.__package__, time.__loader__.__name__, time.__doc__, sorted(n for n in vars(time) if not n.startswith("__"))))
for name in sorted(n for n in vars(time) if not n.startswith("__")):
    x = getattr(time, name)
    if callable(x) and not isinstance(x, type):
        t(name, lambda: (type(x).__name__, x.__text_signature__, x.__doc__, x.__module__, x.__self__ is time))
    elif isinstance(x, int):
        t(name, lambda: x)
S = time.struct_time
t("struct_time", lambda: (S.__name__, S.__module__, S.__qualname__, [b.__name__ for b in S.__mro__], sorted(vars(S)), S.__doc__, S.__text_signature__, S.n_fields, S.n_sequence_fields, S.n_unnamed_fields, S.__match_args__, S.__basicsize__, S.__itemsize__, S.__flags__ & 0x7FFF, repr(S), attempt(type, "X", (S,), {}), attempt(setattr, S, "x", 1)))
for name in ("tm_year", "tm_mon", "tm_mday", "tm_hour", "tm_min", "tm_sec", "tm_wday", "tm_yday", "tm_isdst", "tm_zone", "tm_gmtoff"):
    t(name, lambda: (type(vars(S)[name]).__name__, vars(S)[name].__doc__))

print("---- struct_time")
t("made", lambda: (S(range(9)), S(range(10)), S(range(11)), S((1,) * 9), S([2] * 9), S("abcdefghi"), S(range(9), {"tm_zone": "Z"}), S(range(9), {"tm_zone": "Z", "tm_gmtoff": 5}), S(sequence=range(9)), S(range(9), dict={"tm_gmtoff": 1})))
for a in ((), (range(8),), (range(12),), (5,), (None,), (range(9), 5), (range(9), {}, 1), (range(10), {"tm_zone": "Z"}), (range(9), {"other": 1})):
    t("struct_time%s" % ascii(tuple(list(x) if isinstance(x, range) else x for x in a)), lambda: S(*a))
x = S(range(11))
t("what it has", lambda: (len(x), tuple(x), x[0], x[-1], x[2:4], x.tm_year, x.tm_isdst, x.tm_zone, x.tm_gmtoff, attempt(lambda: x[9]), x == tuple(range(9)), hash(x) == hash(tuple(range(9))), x + (1,), x * 2 == tuple(range(9)) * 2, 3 in x, 9 in x, x.index(3), x.count(3), attempt(setattr, x, "tm_year", 1), attempt(setattr, x, "y", 1), repr(x), x.__reduce__(), x.__replace__(tm_year=99), x.__replace__(tm_zone="Q").tm_zone, attempt(x.__replace__, other=1)))
t("with less", lambda: [(y.tm_zone, y.tm_gmtoff, repr(y), y.__reduce__()) for y in (S(range(9)), S(range(10)))])

print("---- taking a time apart")
TIMES = (0, 1, -1, 59, 60, 3599, 86399, 86400, 951782400, 951868800, 1000000000, 1234567890, 1709164800, 1719792000, 2147483647, 2147483648, 4102444800, 32503680000, 253402300799, 253402300800, -86400, -2208988800, -62135596800, -62135596801, -62167219200, 10 ** 11, 10 ** 12, -10 ** 11, 0.0, 0.5, 0.999999, -0.5, -0.000001, 1.5, 1e9, 1234567890.987654, True, False)
ZONES = ("UTC", "America/New_York", "Europe/London", "Asia/Kolkata", "Australia/Lord_Howe", "Pacific/Apia", "America/St_Johns", "EST5EDT", "XYZ-3:30", "<+0330>-3:30", "AAA3BBB,M3.2.0,M11.1.0", "Nowhere/Nothing", "")
for name in ZONES:
    zone(name)
    t("zone %r" % name, lambda: (time.timezone, time.altzone, time.daylight, time.tzname))
    for when in TIMES:
        t("%r %r" % (name, when), lambda: [(tuple(g), g.tm_zone, g.tm_gmtoff, tuple(l), l.tm_zone, l.tm_gmtoff, time.mktime(l), time.ctime(when), time.asctime(l), time.strftime("%Y-%m-%d %H:%M:%S %Z %z", l)) for g in [time.gmtime(when)] for l in [time.localtime(when)]])
zone("UTC")
for f in (time.gmtime, time.localtime, time.ctime):
    for a in (("a",), ([],), (1, 2), (float("nan"),), (float("inf"),), (-float("inf"),), (2 ** 63,), (-2 ** 63 - 1,), (2 ** 63 - 1,), (-2 ** 63,), (1e19,), (-1e19,), (9.3e18,), (1e300,), (2 ** 55,), (-2 ** 55,), (67768036191676799,), (67768036191676800,), (-67768040609740800,), (-67768040609740801,), (1j,), (b"1",)):
        t("%s%r" % (f.__name__, a), lambda: (lambda r: tuple(r) if isinstance(r, S) else r)(f(*a)))
    t(f.__name__ + " by name", lambda: attempt(lambda: f(seconds=1)))
    t(f.__name__ + " of None is now", lambda: (type(f(None)).__name__, type(f()).__name__))


class Index:
    def __init__(self, v): self.v = v
    def __index__(self): return self.v


class Float:
    def __float__(self): return 1.5


class F(float):
    pass


class I(int):
    pass


t("what can be made a number", lambda: (tuple(time.gmtime(Index(60))), attempt(time.gmtime, Float()), tuple(time.gmtime(F(60.5))), tuple(time.gmtime(I(60))), attempt(time.sleep, Float()), time.sleep(Index(0)), time.sleep(F(0.0)), time.sleep(I(0))))

print("---- putting one together")
GOOD = (2024, 2, 29, 12, 30, 45, 3, 60, 0)
for name in ("UTC", "America/New_York", "Australia/Lord_Howe"):
    zone(name)
    for tup in (GOOD, (1970, 1, 1, 0, 0, 0, 0, 0, 0), (2024, 3, 10, 2, 30, 0, 0, 0, -1), (2024, 3, 10, 2, 30, 0, 0, 0, 0), (2024, 3, 10, 2, 30, 0, 0, 0, 1), (2024, 11, 3, 1, 30, 0, 0, 0, -1), (2024, 11, 3, 1, 30, 0, 0, 0, 0), (2024, 11, 3, 1, 30, 0, 0, 0, 1), (2024, 13, 32, 25, 61, 61, 0, 0, -1), (2024, 0, 0, 0, 0, 0, 0, 0, -1), (2024, -5, -5, -5, -5, -5, 0, 0, -1), (1900, 1, 1, 0, 0, 0, 0, 0, 0), (1, 1, 1, 0, 0, 0, 0, 0, 0), (0, 1, 1, 0, 0, 0, 0, 0, 0), (-1, 1, 1, 0, 0, 0, 0, 0, 0), (9999, 12, 31, 23, 59, 59, 0, 0, 0), (10000, 1, 1, 0, 0, 0, 0, 0, 0), (100000, 1, 1, 0, 0, 0, 0, 0, 0),
                (2024, 1, 1, 0, 0, 0, 99, 999, 5), (2024, 1, 1, 0, 0, 10 ** 9, 0, 0, 0), (2024, 1, 10 ** 6, 0, 0, 0, 0, 0, 0), (2 ** 31 - 1, 1, 1, 0, 0, 0, 0, 0, 0), (-2 ** 31, 1, 1, 0, 0, 0, 0, 0, 0), (-2 ** 31 + 1900, 1, 1, 0, 0, 0, 0, 0, 0), (-2 ** 31 + 1899, 1, 1, 0, 0, 0, 0, 0, 0), (2 ** 31 - 1, 12, 31, 23, 59, 59, 0, 0, 0)):
        t("mktime %r %r" % (name, tup), lambda: time.mktime(tup))
zone("UTC")
BAD = ((), (1,) * 8, (1,) * 10, [1] * 9, None, 5, "abcdefghi", ("a",) + (1,) * 8, (1.5,) + (1,) * 8, (None,) * 9, (2 ** 31,) + (1,) * 8, (1, 2 ** 31) + (1,) * 7, (-2 ** 31 - 1,) + (1,) * 8, (2 ** 100,) + (1,) * 8, (1,) * 8 + ("x",), (Index(2024),) + (1,) * 8, (True,) * 9, (I(2024),) + (1,) * 8, (F(1.0),) + (1,) * 8)
for f in (time.mktime, time.asctime, lambda x: time.strftime("%Y", x)):
    for tup in BAD:
        t("%s(%s)" % (getattr(f, "__name__", "strftime"), ascii(tuple("Index" if isinstance(v, Index) else v for v in tup) if isinstance(tup, tuple) else tup)), lambda: f(tup))
FIELDS = ("year", "month", "day", "hour", "minute", "second", "weekday", "yearday", "isdst")
for i, name in enumerate(FIELDS):
    for value in (-2 ** 31, -1000, -2, -1, 0, 1, 6, 7, 11, 12, 13, 23, 24, 31, 32, 59, 60, 61, 62, 365, 366, 367, 1000, 2 ** 31 - 1):
        tup = GOOD[:i] + (value,) + GOOD[i + 1:]
        t("%s %d" % (name, value), lambda: (attempt(time.asctime, tup), attempt(time.strftime, "%Y %m %d %H %M %S %w %j %a %b %y %C %e %I %p %u %U %W %V %G %g", tup)))
t("asctime", lambda: (time.asctime(GOOD), time.asctime((2024, 1, 5, 1, 2, 3, 0, 1, 0)), time.asctime((12345, 1, 1, 0, 0, 0, 0, 1, 0)), time.asctime((-5, 1, 1, 0, 0, 0, 0, 1, 0)), time.asctime((5, 1, 1, 0, 0, 0, 0, 1, 0)), time.asctime(S(GOOD)), type(time.asctime()).__name__, attempt(time.asctime, GOOD, 1), attempt(lambda: time.asctime(tuple=GOOD)), attempt(time.asctime, None)))

print("---- strftime")
for c in "aAbBcCdDeEfFgGhHiIjJkKlLmMnNoOpPqQrRsStTuUvVwWxXyYzZ0123456789%+-_^#:. ":
    t("%%%s" % c, lambda: (time.strftime("%" + c, GOOD), time.strftime("[%" + c + "]", (1, 1, 1, 0, 0, 0, 0, 1, -1)), time.strftime("%" + c, (9999, 12, 31, 23, 59, 61, 6, 366, 1))))
for f in ("", "%", "%%", "a%", "%%%", "%E", "%Ec", "%EY", "%Ey", "%Od", "%OH", "%-d", "%_d", "%0d", "%^a", "%#a", "%10Y", "%3d", "%:z", "%::z", "abc", "a\0b", "\0", "%Y\0%m", "\xe9", "\xe9%Y", "%Y\xe9%m", "%\xe9", "\xe9%", "%Y\U0001F600%d", "\U0001F600", "%%\xe9%%", "\ud800%Y", "\xe9\xe9%Y%Y\xe9", "%Y" * 100, "x" * 3000, "%c" * 200, " ", "\n%n\t%t", "%Z" * 3, "%5Z", "%z%z"):
    t("strftime %s" % ascii(f[:30]), lambda: time.strftime(f, GOOD))
for year in (-10000, -1000, -100, -1, 0, 1, 9, 10, 99, 100, 999, 1000, 1899, 1900, 1969, 9999, 10000, 12345, 123456, 2 ** 31 - 1):
    t("year %d" % year, lambda: time.strftime("%Y|%y|%C|%G|%g|%F|%D|%c|%x", (year,) + GOOD[1:]))
t("how it is called", lambda: (attempt(time.strftime), attempt(time.strftime, 5), attempt(time.strftime, None), attempt(time.strftime, b"%Y"), attempt(time.strftime, "%Y", GOOD, 1), attempt(lambda: time.strftime(format="%Y")), attempt(time.strftime, "%Y", None), type(time.strftime("%Y")).__name__, time.strftime(type("T", (str,), {})("%Y"), GOOD), type(time.strftime(type("T", (str,), {})(""), GOOD)).__name__))
t("what a struct_time says its zone is", lambda: (time.strftime("%Z %z", S(GOOD, {"tm_zone": "QQQ", "tm_gmtoff": 3600})), time.strftime("%Z %z", S(GOOD, {"tm_zone": "\xe9中", "tm_gmtoff": -12345})), time.strftime("%Z %z", S(GOOD)), time.strftime("%Z %z", GOOD), attempt(time.strftime, "%Z", S(GOOD, {"tm_zone": 5})), attempt(time.strftime, "%Z", S(GOOD, {"tm_zone": "\ud800"})), attempt(time.strftime, "%z", S(GOOD, {"tm_gmtoff": "x"})), attempt(time.strftime, "%z", S(GOOD, {"tm_gmtoff": 2 ** 63})), attempt(time.strftime, "%z", S(GOOD, {"tm_gmtoff": 1.5})), time.strftime("%z", S(GOOD, {"tm_gmtoff": 10 ** 6})), attempt(time.mktime, S(GOOD, {"tm_zone": 5})), time.mktime(S(GOOD, {"tm_zone": "QQQ", "tm_gmtoff": 3600}))))
for name in ("America/New_York", "Asia/Kolkata"):
    zone(name)
    t("in %s" % name, lambda: (time.strftime("%Z %z %c", GOOD), time.strftime("%Z %z", GOOD[:8] + (1,)), time.strftime("%Z %z", GOOD[:8] + (-1,)), time.strftime("%Z %z", GOOD[:8] + (100,)), time.strftime("%Z %z", GOOD[:8] + (-100,)), time.strftime("%s", GOOD)))
zone("UTC")

print("---- strptime")
t("strptime", lambda: (tuple(time.strptime("2024-02-29 12:30:45", "%Y-%m-%d %H:%M:%S")), tuple(time.strptime("Thu Feb 29 12:30:45 2024")), time.strptime("2024 +0530", "%Y %z").tm_gmtoff, time.strptime("2024 UTC", "%Y %Z").tm_zone, attempt(time.strptime, "x", "%Y"), attempt(time.strptime, "2024", "%Q"), attempt(time.strptime), attempt(time.strptime, 5), attempt(time.strptime, "1", "%H", 3), attempt(lambda: time.strptime(string="1", format="%H")), type(time.strptime("1", "%H")).__name__))

print("---- clocks")
for name in ("time", "monotonic", "perf_counter", "process_time", "thread_time"):
    t(name, lambda: [(type(a).__name__, type(n).__name__, a <= b, n <= m, a >= 0, abs(n / 1e9 - a) < 5, attempt(f, 1), attempt(g, 1), attempt(lambda: f(x=1))) for f in [getattr(time, name)] for g in [getattr(time, name + "_ns")] for a in [f()] for n in [g()] for b in [f()] for m in [g()]])
    t("get_clock_info(%r)" % name, lambda: [(type(i).__name__, sorted(vars(i)), i.implementation, i.monotonic, i.adjustable, i.resolution, repr(i)) for i in [time.get_clock_info(name)]])
t("get_clock_info", lambda: [attempt(time.get_clock_info, *a) for a in ((), ("x",), ("",), (5,), (None,), (b"time",), ("time", 1), ("time\0",), ("clock",), ("TIME",))] + [attempt(lambda: time.get_clock_info(name="time"))])
CLOCKS = sorted(n for n in vars(time) if n.startswith("CLOCK_"))
for name in CLOCKS:
    c = getattr(time, name)
    t(name, lambda: (type(time.clock_gettime(c)).__name__, type(time.clock_gettime_ns(c)).__name__, time.clock_getres(c), time.clock_gettime(c) <= time.clock_gettime(c), abs(time.clock_gettime_ns(c) / 1e9 - time.clock_gettime(c)) < 5))
for f in (time.clock_gettime, time.clock_gettime_ns, time.clock_getres):
    t(f.__name__, lambda: [attempt(f, *a) for a in ((), (-1,), (999,), ("a",), (1.5,), (None,), (2 ** 31,), (-2 ** 31 - 1,), (2 ** 100,), (0, 1), (True,), (Index(0),))][:9] + [attempt(lambda: f(clk_id=0))])
for f in (time.clock_settime, time.clock_settime_ns):
    t(f.__name__, lambda: [attempt(f, *a) for a in ((), (0,), (0, 1, 2), ("a", 1), (1.5, 1), (0, "a"), (0, None), (0, float("nan")), (0, 1e300), (0, 2 ** 64), (999, 1), (-1, 1), (time.CLOCK_MONOTONIC, 1), (0, 1.5), (2 ** 31, 1))] + [attempt(lambda: f(clk_id=0, time=1))])

print("---- sleep")
t("sleep", lambda: [attempt(time.sleep, *a) for a in ((), (0,), (0.0,), (-0.0,), (1e-9,), (0.001,), (-1,), (-0.001,), (-1e-10,), ("a",), (None,), ([],), (float("nan"),), (float("inf"),), (-float("inf"),), (1e300,), (2 ** 63,), (10 ** 10 * 10 ** 9,), (1, 2), (True and 0,), (1j,))] + [attempt(lambda: time.sleep(seconds=0)), attempt(lambda: time.sleep(secs=0))])
t("it takes as long as it says", lambda: [(b - a >= 0.02, b - a < 2) for a in [time.monotonic()] for _ in [time.sleep(0.02)] for b in [time.monotonic()]])
seen = []
sys.addaudithook(lambda event, args: seen.append((event, args)) if event.startswith("time.") else None)
t("it is told of", lambda: (time.sleep(0), attempt(time.sleep, "x"), attempt(time.sleep, -1), seen))

print("---- tzset")
t("tzset", lambda: (attempt(time.tzset, 1), attempt(lambda: time.tzset(x=1)), time.tzset()))
t("what is set is set again", lambda: [(setattr(time, "timezone", "mine"), delattr(time, "tzname"), time.tzset(), time.timezone, time.tzname)])
del os.environ["TZ"]
t("with none", lambda: (time.tzset(), type(time.timezone).__name__, type(time.tzname).__name__, len(time.tzname)))
