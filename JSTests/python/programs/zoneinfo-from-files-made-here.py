# zoneinfo.ZoneInfo, from files that are made here, so that it is all one what the system has
import zoneinfo, _zoneinfo, datetime, io, struct, pickle, copy, sys, os, tempfile, shutil, hashlib, gc, weakref
from zoneinfo import ZoneInfo
from datetime import datetime as dt, timedelta as td, time, date, timezone as tz
import re; address = re.compile('0x[0-9a-f]+')
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
        c = e.__context__
        if c is not None: r += " <- %s: %s" % (type(c).__name__, c)
    print(label.encode("ascii", "backslashreplace").decode(), "->", address.sub("0x", ascii(r)))
print(ZoneInfo is _zoneinfo.ZoneInfo)
def tzif(transitions=(), types=((0, 0, "UTC"),), after=b"", version=2):
    "A file of the time zone database: when the clocks change and to which of the types, what the types are, and the rule for afterwards."
    abbreviations = b""; places = {}
    for _, _, name in types:
        if name not in places: places[name] = len(abbreviations); abbreviations += name.encode() + b"\0"
    def block(size, code):
        head = b"TZif" + (b"%d" % version if version > 1 else b"\0") + b"\0" * 15 + struct.pack(">6l", 0, 0, 0, len(transitions), len(types), len(abbreviations))
        body = b"".join(struct.pack(code, t) for t, _ in transitions) + bytes(i for _, i in transitions)
        body += b"".join(struct.pack(">lbb", off, isdst, places[name]) for off, isdst, name in types) + abbreviations
        return head + body
    if version == 1: return block(4, ">l")
    return block(4, ">l") + block(8, ">q") + b"\n" + after + b"\n"
def zone(*a, key=None, **k): return ZoneInfo.from_file(io.BytesIO(tzif(*a, **k)), key=key)
def show(z, d): return (z.utcoffset(d), z.dst(d), z.tzname(d))
def secs(x): return None if x is None else int(x.total_seconds())
print("===== rules for afterwards")
rules = ["UTC0", "EST5", "EST+5", "EST-5", "EST5EDT,M3.2.0,M11.1.0", "EST5EDT,M3.2.0/2,M11.1.0/2", "EST5EDT4,M3.2.0,M11.1.0", "GMT0BST,M3.5.0/1,M10.5.0", "GMT0BST-1,M3.5.0/1:00:00,M10.5.0/2:00:00", "AEST-10AEDT,M10.1.0,M4.1.0/3", "IST-1GMT0,M10.5.0,M3.5.0/1", "<+03>-3", "<-03>3", "<+0330>-3:30", "<-03>3<-02>,M3.5.0/-2,M10.5.0/-1",
         "<+1030>-10:30<+11>-11,M10.1.0,M4.1.0", "AAA3BBB,J60,J300", "AAA3BBB,59,299", "AAA3BBB,J1,J365", "AAA3BBB,0,365", "AAA3BBB,J60/0,J300/25", "AAA3BBB,0/0,J365/25", "AAA3BBB,J1/0,J365/23:59:59", "AAA3BBB,M1.1.0/0,M12.5.6/24", "AAA3BBB,M2.5.0,M2.5.6", "AAA3BBB,M3.2.0/167,M11.1.0/-167", "AAA3BBB,M3.2.0/-1:30:15,M11.1.0/26:30:15",
         "AAA-24", "AAA24", "AAA24:59:59", "AAA-24:59:59", "AAA0:30", "AAA0:00:30", "AAA012", "AAA3BBB2:30,M3.2.0,M11.1.0", "AAA3BBB4,M3.2.0,M11.1.0", "AAA3BBB3,M3.2.0,M11.1.0", "AAA3BBB-20,M3.2.0,M11.1.0", "ABCDEFGHIJ5", "AB5", "A5", "<A>5", "<AB>5", "<a1+-Z>5", "aaa5bbb,M3.2.0,M11.1.0",
         "", " ", "5", "EST", "EST5EDT", "EST5EDT,", "EST5EDT,M3.2.0", "EST5EDT,M3.2.0,", "EST5EDT,M3.2.0,M11.1.0,", "EST5EDT,M3.2.0,M11.1.0x", "EST5EDT,M3.2.0,M11.1.0/", "EST5EDT,M3.2.0/x,M11.1.0", "EST5EDT,M13.2.0,M11.1.0", "EST5EDT,M0.2.0,M11.1.0", "EST5EDT,M3.6.0,M11.1.0", "EST5EDT,M3.0.0,M11.1.0", "EST5EDT,M3.2.7,M11.1.0",
         "EST5EDT,M3.2,M11.1.0", "EST5EDT,M3,M11.1.0", "EST5EDT,M,M11.1.0", "EST5EDT,M3.2.0/168,M11.1.0", "EST5EDT,M3.2.0/-168,M11.1.0", "EST5EDT,M3.2.0/1:60,M11.1.0", "EST5EDT,M3.2.0/1:5,M11.1.0", "EST5EDT,M3.2.0/1:05:5,M11.1.0", "EST5EDT,M3.2.0/1:05:05:05,M11.1.0", "EST5EDT,J0,J300", "EST5EDT,J366,J300", "EST5EDT,366,300", "EST5EDT,J,J300", "EST5EDT,-1,300",
         "EST5EDT,1000,300", "EST25", "EST-25", "EST5:60", "EST5:5", "EST5:", "EST5:00:", "EST+", "EST++5", "EST5 ", " EST5", "EST 5", "E1T5", "<EST5", "<>5", "<E T>5", "<E_T>5", "EST5<EDT,M3.2.0,M11.1.0", "EST5EDT+,M3.2.0,M11.1.0", "EST5EDT25,M3.2.0,M11.1.0", "EST5E,M3.2.0,M11.1.0", "EST5,M3.2.0,M11.1.0", "EST5EDT;M3.2.0;M11.1.0", "EST5\0EDT", "\xe9ST5", "EST5EDT,M03.2.0,M11.1.0", "EST5EDT,M003.2.0,M11.1.0", "EST5EDT,M3.02.0,M11.1.0", "EST1000"]
when = [dt(y, m, d, h, mi) for y in (1, 1969, 2000, 2023, 2024, 9999) for m, d in ((1, 1), (2, 28), (3, 1), (3, 10), (3, 12), (3, 31), (6, 15), (10, 27), (10, 29), (11, 3), (11, 5), (12, 31)) for h, mi in ((0, 0), (0, 30), (1, 0), (1, 30), (2, 0), (2, 30), (3, 0), (12, 0), (23, 59))]
for r in rules:
    def go():
        z = zone(after=r.encode("latin-1"))
        lines = []
        for d in when:
            for fold in (0, 1):
                e = d.replace(fold=fold, tzinfo=z)
                lines.append((secs(e.utcoffset()), secs(e.dst()), e.tzname()))
            try: u = z.fromutc(d.replace(tzinfo=z)); lines.append((u.isoformat(), u.fold))
            except OverflowError: lines.append("overflow")
        return sorted(set(l for l in lines if len(l) == 3), key=repr), show(z, None), time(12, tzinfo=z).utcoffset(), hashlib.md5(repr(lines).encode()).hexdigest()
    attempt("rule %r" % r, go)
print("===== each hour about a change, by the rule")
for r in ("EST5EDT,M3.2.0,M11.1.0", "GMT0BST,M3.5.0/1,M10.5.0", "AEST-10AEDT,M10.1.0,M4.1.0/3", "IST-1GMT0,M10.5.0,M3.5.0/1", "<+1030>-10:30<+11>-11,M10.1.0,M4.1.0", "AAA3BBB,J60/0,J300/25", "AAA3BBB4,M3.2.0,M11.1.0"):
    z = zone(after=r.encode())
    for y in (2023, 2024):
        for start in (dt(y, 2, 28), dt(y, 3, 8), dt(y, 3, 25), dt(y, 3, 30), dt(y, 10, 1), dt(y, 10, 26), dt(y, 11, 1), dt(y, 4, 1)):
            out = []
            for k in range(0, 24 * 8 * 2):
                d = start + td(minutes=30 * k)
                a, b = d.replace(tzinfo=z), d.replace(tzinfo=z, fold=1)
                u = z.fromutc(d.replace(tzinfo=z))
                out.append((secs(a.utcoffset()), secs(b.utcoffset()), a.tzname(), b.tzname(), secs(a.dst()), u.isoformat(), u.fold))
            print(r, start.date(), hashlib.md5(repr(out).encode()).hexdigest(), sum(o[0] != o[1] for o in out), sum(o[6] for o in out))
print("===== changes that are listed")
H = 3600
listed = {
    "one change": (((0, 1),), ((-5 * H, 0, "LMT"), (-4 * H, 0, "AST")), b""),
    "one change, and a rule": (((0, 1),), ((-5 * H, 0, "LMT"), (-4 * H, 0, "AST")), b"AST4"),
    "forward and back": (((1000000, 1), (16000000, 0), (32000000, 1), (48000000, 0)), ((-5 * H, 0, "EST"), (-4 * H, 1, "EDT")), b"EST5EDT,M3.2.0,M11.1.0"),
    "forward and back, and no rule": (((1000000, 1), (16000000, 0), (32000000, 1)), ((-5 * H, 0, "EST"), (-4 * H, 1, "EDT")), b""),
    "ends in daylight saving, and no rule": (((1000000, 1),), ((-5 * H, 0, "EST"), (-4 * H, 1, "EDT")), b""),
    "all daylight saving": (((1000000, 1), (16000000, 0)), ((-5 * H, 1, "AAA"), (-4 * H, 1, "BBB")), b""),
    "daylight saving first": (((1000000, 1), (16000000, 0)), ((-4 * H, 1, "EDT"), (-5 * H, 0, "EST")), b""),
    "double summer time": (((1000000, 1), (8000000, 2), (16000000, 1), (24000000, 0)), ((0, 0, "GMT"), (H, 1, "BST"), (2 * H, 1, "BDST")), b"GMT0"),
    "a change of standard time": (((1000000, 1), (16000000, 2), (32000000, 3)), ((-5 * H, 0, "EST"), (-4 * H, 1, "EDT"), (-6 * H, 0, "CST"), (-5 * H, 1, "CDT")), b"CST6CDT,M3.2.0,M11.1.0"),
    "daylight saving that is no different": (((1000000, 1), (16000000, 0)), ((H, 0, "AAA"), (H, 1, "BBB")), b""),
    "negative daylight saving": (((1000000, 1), (16000000, 0), (32000000, 1)), ((H, 0, "IST"), (0, 1, "GMT")), b"IST-1GMT0,M10.5.0,M3.5.0/1"),
    "a type that is not used": (((1000000, 1),), ((0, 0, "AAA"), (H, 0, "BBB"), (2 * H, 1, "CCC")), b""),
    "the same name twice": (((1000000, 1), (16000000, 2)), ((0, 0, "AAA"), (H, 1, "AAA"), (2 * H, 0, "AAA")), b""),
    "before 1970": (((-2000000000, 1), (-1000000000, 2), (-5, 0)), ((0, 0, "AAA"), (H, 0, "BBB"), (-H, 0, "CCC")), b"AAA0"),
    "far away": (((-2**40, 1), (2**40, 0)), ((0, 0, "AAA"), (H, 0, "BBB")), b""),
    "a large jump": (((1000000, 1), (16000000, 0)), ((-11 * H, 0, "AAA"), (13 * H, 0, "BBB")), b""),
    "odd offsets": (((1000000, 1), (16000000, 2)), ((-17762, 0, "LMT"), (12345, 0, "AAA"), (-1, 1, "BBB")), b""),
    "no changes, one type": ((), ((H, 0, "AAA"),), b""),
    "no changes, one type, and the same rule": ((), ((H, 0, "AAA"),), b"AAA-1"),
    "no changes, one type, and another rule": ((), ((H, 0, "AAA"),), b"BBB-1"),
    "no changes, one type, and a rule with another offset": ((), ((H, 0, "AAA"),), b"AAA-2"),
    "no changes, two types": ((), ((H, 0, "AAA"), (2 * H, 0, "BBB")), b""),
    "no changes, one type for daylight saving": ((), ((H, 1, "AAA"),), b""),
    "changes at the same time": (((1000000, 1), (1000000, 0)), ((0, 0, "AAA"), (H, 0, "BBB")), b""),
    "changes out of order": (((16000000, 1), (1000000, 0)), ((0, 0, "AAA"), (H, 0, "BBB")), b""),
}
for label, (transitions, types, after) in listed.items():
    def go():
        z = zone(transitions, types, after)
        lines = []
        points = sorted(set([-2**41, -3000000000, 0, 2**41, 60000000, 10**9] + [t + k * 1800 for t, _ in transitions for k in range(-60, 61)]))
        for t in points:
            if not -62135596800 + 90000 < t < 253402300800 - 90000: continue
            d = dt(1970, 1, 1) + td(seconds=t)
            a, b = d.replace(tzinfo=z), d.replace(tzinfo=z, fold=1)
            u = z.fromutc(d.replace(tzinfo=z))
            lines.append((t, secs(a.utcoffset()), secs(b.utcoffset()), secs(a.dst()), secs(b.dst()), a.tzname(), b.tzname(), u.isoformat(), u.fold))
        kinds = sorted(set(l[1:2] + l[3:4] + l[5:6] for l in lines), key=repr)
        return kinds, show(z, None), sum(l[8] for l in lines), sum(l[1] != l[2] for l in lines), hashlib.md5(repr(lines).encode()).hexdigest()
    attempt(label, go)
    attempt("   version 1", lambda: show(zone(transitions, types, version=1), dt(2000, 1, 1)) if all(-2**31 <= t < 2**31 for t, _ in transitions) else None)
print("===== files that will not do")
good = tzif(((1000000, 1),), ((0, 0, "AAA"), (H, 0, "BBB")), b"AAA0")
for label, data in {"nothing": b"", "not one": b"hello world, this is not a time zone file at all, no", "the magic only": b"TZif", "cut short": good[:50], "cut in the second part": good[:len(good) - 20], "no rule at the end": good[:good.rindex(b"\nAAA0")], "no newline at the end": good[:-1], "version 3": good.replace(b"TZif2", b"TZif3"), "version 9": good.replace(b"TZif2", b"TZif9"),
                    "an index that is too large": tzif(((1000000, 5),), ((0, 0, "AAA"),)), "no types": tzif((), ()), "no types, and a rule": tzif((), (), b"AAA0"), "no types, and a change": tzif(((1000000, 0),), ()), "a bad rule": tzif(after=b"nonsense"), "a name that is not ASCII": tzif((), ((0, 0, "\xe9\xe9\xe9"),))}.items():
    attempt(label, lambda: show(ZoneInfo.from_file(io.BytesIO(data)), dt(2000, 1, 1)))
for label, f in {"None": None, "an int": 5, "bytes": good, "a str": "x", "text": io.StringIO("TZif"), "closed": (lambda f: (f.close(), f)[1])(io.BytesIO(good)), "read raises": type("F", (), {"read": lambda s, n=-1: 1 / 0})(), "read gives a str": type("F", (), {"read": lambda s, n=-1: "x" * (n if n > 0 else 1)})()}.items():
    attempt("from_file(%s)" % label, lambda: ZoneInfo.from_file(f))
print("===== what load_data() can give")
common = sys.modules["zoneinfo._common"]; real = common.load_data
for label, v in {"None": None, "a list": [(), (), (0,), (0,), ("A",), None], "a tuple subclass": type("T", (tuple,), {})(((), (), (0,), (0,), ("A",), None)), "too short": ((), ()), "empty": (), "good": ((), (), (0,), (0,), ("A",), None), "no types": ((), (), (), (), (), None), "offsets no ints": ((), (), ("x",), (0,), ("A",), None),
                 "offset too large": ((), (), (2**70,), (0,), ("A",), None), "offset more than a day": ((), (), (10**6,), (0,), ("A",), None), "offset far too much": ((), (), (10**15,), (0,), ("A",), None), "isdst too short": ((), (), (0,), (), ("A",), None), "names too short": ((), (), (0,), (0,), (), None), "a name that is no str": ((), (), (0,), (0,), (5,), None),
                 "indices too short": ((), (5,), (0,), (0,), ("A",), None), "an index of -2": ((-2,), (5,), (0,), (0,), ("A",), None), "an index too large": ((1,), (5,), (0,), (0,), ("A",), None), "an index that is a float": ((0.0,), (5,), (0,), (0,), ("A",), None), "a time that is a float": ((0,), (5.0,), (0,), (0,), ("A",), None),
                 "a time too large": ((0,), (2**70,), (0,), (0,), ("A",), None), "True for an index": ((True,), (5,), (0, 1), (0, 0), ("A", "B"), None), "the rule a str": ((), (), (0,), (0,), ("A",), "AAA0"), "the rule an int": ((), (), (0,), (0,), ("A",), 5), "the rule empty": ((), (), (0,), (0,), ("A",), b""), "the rule a bytearray": ((), (), (0,), (0,), ("A",), bytearray(b"AAA0")),
                 "isdst that cannot say": ((), (), (0,), (type("B", (), {"__bool__": lambda s: 1 / 0})(),), ("A",), None), "seven": ((), (), (0,), (0,), ("A",), None, "more")}.items():
    common.load_data = lambda f: v
    attempt(label, lambda: show(ZoneInfo.from_file(io.BytesIO()), dt(2000, 1, 1)))
common.load_data = real
print("done with the first part")
