# zoneinfo.ZoneInfo: keys, caches, pickling, and what it is asked about
import zoneinfo, _zoneinfo, datetime, io, struct, pickle, copy, sys, os, tempfile, shutil, gc, weakref, inspect
from zoneinfo import ZoneInfo
from datetime import datetime as dt, timedelta as td, time, date, timezone as tz
import re; address = re.compile('0x[0-9a-f]+')
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
        c = e.__context__
        if c is not None: r += " <- %s: %s" % (type(c).__name__, str(c).replace(where, "<where>"))
    print(label.encode("ascii", "backslashreplace").decode(), "->", address.sub("0x", ascii(r)).replace(where, "<where>"))
def tzif(after):
    head = b"TZif2" + b"\0" * 15 + struct.pack(">6l", 0, 0, 0, 0, 1, 4)
    return head + struct.pack(">lbb", 0, 0, 0) + b"UTC\0" + head + struct.pack(">lbb", 0, 0, 0) + b"UTC\0" + b"\n" + after + b"\n"
where = os.path.realpath(tempfile.mkdtemp())
try:
    os.mkdir(os.path.join(where, "Here"))
    for i in range(12):
        with open(os.path.join(where, "Here", "Z%d" % i), "wb") as f: f.write(tzif(b"ZZZ-%d" % i))
    with open(os.path.join(where, "East"), "wb") as f: f.write(tzif(b"EST5EDT,M3.2.0,M11.1.0"))
    with open(os.path.join(where, "Bad"), "wb") as f: f.write(b"not a time zone")
    with open(os.path.join(where, "Empty"), "wb") as f: pass
    zoneinfo.reset_tzpath(to=[where])
    print("===== keys")
    for k in ("East", "Here/Z1", "Here", "Here/", "Nowhere", "Here/Nowhere", "", ".", "..", "../East", "Here/../East", "Here/./Z1", "Here//Z1", "/East", os.path.join(where, "East"), "East/", "East/x", "Bad", "Empty", " East", "East ", "East\0", "\0", "Ea\udc80st", "Ea\ud800st", "\u20ac", "\U0001f600", "a" * 300, "a" * 5000):
        attempt("ZoneInfo(%a)" % (k.replace(where, "<where>") if len(k) < 400 else "long"), lambda: (lambda z: (repr(z), str(z), z.key == k, z.utcoffset(dt(2000, 1, 1))))(ZoneInfo(k)))
        attempt("   no_cache", lambda: repr(ZoneInfo.no_cache(k)))
    for k in (None, 5, 1.5, b"East", [], ("East",), type("S", (str,), {})("East"), type("P", (), {"__fspath__": lambda s: "East"})(), True):
        attempt("ZoneInfo(%s)" % type(k).__name__, lambda: repr(ZoneInfo(k)))
        attempt("   no_cache", lambda: repr(ZoneInfo.no_cache(k)))
    for label, f in {"none": lambda: ZoneInfo(), "two": lambda: ZoneInfo("East", "East"), "keyword": lambda: ZoneInfo(key="East"), "unknown": lambda: ZoneInfo(name="East"), "both": lambda: ZoneInfo("East", key="East"), "no_cache: none": lambda: ZoneInfo.no_cache(), "no_cache: keyword": lambda: ZoneInfo.no_cache(key="East"), "no_cache: two": lambda: ZoneInfo.no_cache("East", 1),
                     "no_cache: unknown": lambda: ZoneInfo.no_cache(k="East"), "from_file: none": lambda: ZoneInfo.from_file(), "from_file: keyword": lambda: ZoneInfo.from_file(file_obj=io.BytesIO()), "from_file: three": lambda: ZoneInfo.from_file(io.BytesIO(), None, 1), "from_file: key by position": lambda: ZoneInfo.from_file(io.BytesIO(tzif(b"AAA0")), "k").key,
                     "from_file: key by keyword": lambda: ZoneInfo.from_file(io.BytesIO(tzif(b"AAA0")), key="k").key, "clear_cache: positional": lambda: ZoneInfo.clear_cache(["East"]), "clear_cache: unknown": lambda: ZoneInfo.clear_cache(keys=[]), "__new__ of tzinfo": lambda: datetime.tzinfo.__new__(ZoneInfo), "__new__ of object": lambda: object.__new__(ZoneInfo),
                     "__new__ with another class": lambda: ZoneInfo.__new__(int, "East"), "__new__ with tzinfo": lambda: ZoneInfo.__new__(datetime.tzinfo, "East"), "__init__": lambda: ZoneInfo("East").__init__(1, 2, 3)}.items():
        attempt(label, f)
    print("===== the caches")
    ZoneInfo.clear_cache()
    attempt("the same one", lambda: (ZoneInfo("East") is ZoneInfo("East"), ZoneInfo("East") is ZoneInfo.no_cache("East"), ZoneInfo.no_cache("East") is ZoneInfo.no_cache("East"), ZoneInfo("East") == ZoneInfo.no_cache("East"), hash(ZoneInfo("East")) == hash(ZoneInfo("East"))))
    attempt("cleared", lambda: (lambda a: (ZoneInfo.clear_cache(), ZoneInfo("East") is a))(ZoneInfo("East")))
    attempt("cleared, but for others", lambda: (lambda a, b: (ZoneInfo.clear_cache(only_keys=["East"]), ZoneInfo("East") is a, ZoneInfo("Here/Z1") is b))(ZoneInfo("East"), ZoneInfo("Here/Z1")))
    for v in ([], (), "East", ["Nowhere"], iter(["East"]), {"East": 1}, None, 5, [[]], [5], ["East", "East"], (k for k in ["East"]), [None]):
        attempt("clear_cache(only_keys=%s)" % type(v).__name__, lambda: (lambda a: (ZoneInfo.clear_cache(only_keys=v), ZoneInfo("East") is a))(ZoneInfo("East")))
    def kept():
        "Those that were asked for last are kept though nothing else has them."
        ZoneInfo.clear_cache(); refs = []
        for i in range(12): refs.append(weakref.ref(ZoneInfo("Here/Z%d" % i)))
        for _ in range(3): gc.collect()
        return [r() is not None for r in refs]
    attempt("which are kept", kept)
    def kept_in_turn():
        ZoneInfo.clear_cache(); refs = []
        for i in range(8): refs.append(weakref.ref(ZoneInfo("Here/Z%d" % i)))
        ZoneInfo("Here/Z0"); ZoneInfo("Here/Z8"); ZoneInfo("Here/Z9")
        for _ in range(3): gc.collect()
        return [r() is not None for r in refs]
    attempt("asked for again", kept_in_turn)
    class K(str):
        def __eq__(self, o): log.append(("eq", str(self), str(o))); return str.__eq__(self, o)
        __hash__ = str.__hash__
    log = []
    def compared():
        ZoneInfo.clear_cache(); ZoneInfo("Here/Z1"); ZoneInfo("Here/Z2"); log.clear(); z = ZoneInfo(K("Here/Z1")); return list(log), type(z.key).__name__
    attempt("how a key is looked for", compared)
    class BadEq(str):
        def __eq__(self, o): raise ZeroDivisionError("eq")
        __hash__ = str.__hash__
    attempt("a key that cannot be compared", lambda: (ZoneInfo("East"), ZoneInfo(BadEq("East")))[1])
    attempt("   clear_cache", lambda: (ZoneInfo("East"), ZoneInfo.clear_cache(only_keys=[BadEq("East")]))[1])
    print("===== classes derived from it")
    class Z1(ZoneInfo): pass
    class Z2(ZoneInfo): pass
    class Z11(Z1): pass
    attempt("their caches", lambda: (type(Z1._weak_cache).__name__, Z1._weak_cache is Z2._weak_cache, Z11._weak_cache is Z1._weak_cache, hasattr(ZoneInfo, "_weak_cache"), "_weak_cache" in vars(Z11)))
    attempt("made", lambda: (type(Z1("East")).__name__, Z1("East") is Z1("East"), Z1("East") is ZoneInfo("East"), Z1("East") is Z2("East"), Z11("East") is Z1("East"), repr(Z1("East")), str(Z1("East")), isinstance(Z1("East"), ZoneInfo)))
    attempt("the other ways", lambda: (type(Z1.no_cache("East")).__name__, type(Z1.from_file(io.BytesIO(tzif(b"AAA0")))).__name__, repr(Z1.from_file(io.BytesIO(tzif(b"AAA0")), key="k"))))
    attempt("cleared", lambda: (lambda a, b: (Z1.clear_cache(), Z1("East") is a, ZoneInfo("East") is b))(Z1("East"), ZoneInfo("East")))
    attempt("not kept", lambda: (lambda r: ([gc.collect() for _ in range(3)], r() is None)[1])(weakref.ref(Z2("Here/Z5"))))
    attempt("attributes", lambda: (lambda z: (setattr(z, "x", 1), z.x, vars(z)))(Z1("East")))
    attempt("pickle", lambda: [(lambda r: (type(r).__name__, r is Z1("East")))(pickle.loads(pickle.dumps(Z1("East"), p))) for p in (0, 2, 5)] if setattr(sys.modules["__main__"], "Z1", Z1) is None else None)
    def spoiled(v):
        class Z(ZoneInfo): pass
        Z._weak_cache = v
        return Z("East")
    for label, v in {"an int": 5, "None": None, "a dict": {}, "a dict with something else in it": {"East": 5}, "a dict with a ZoneInfo in it": {"East": ZoneInfo("East")}, "get raises": type("C", (), {"get": lambda s, k: 1 / 0})(), "setdefault gives something else": type("C", (), {"get": lambda s, k: None, "setdefault": lambda s, k, v: "other"})(),
                     "get takes two": type("C", (), {"get": lambda s, k, d: None})()}.items():
        attempt("a cache that is %s" % label, lambda: repr(spoiled(v)))
    def no_cache_attribute():
        class Z(ZoneInfo): pass
        del Z._weak_cache
        return Z("East")
    attempt("no cache at all", no_cache_attribute)
    attempt("__init_subclass__ with keywords", lambda: type("Z", (ZoneInfo,), {}, a=1)._weak_cache is not None)
    attempt("__init_subclass__ called", lambda: (ZoneInfo.__init_subclass__(), hasattr(ZoneInfo, "_weak_cache")))
    print("===== pickling")
    for label, make in {"cached": lambda: ZoneInfo("East"), "not cached": lambda: ZoneInfo.no_cache("East"), "from a file": lambda: ZoneInfo.from_file(io.BytesIO(tzif(b"AAA0"))), "from a file, with a key": lambda: ZoneInfo.from_file(io.BytesIO(tzif(b"AAA0")), key="East")}.items():
        z = make()
        attempt(label, lambda: (lambda r: (r[0].__name__, r[0].__self__ is ZoneInfo, r[1]))(z.__reduce__()))
        attempt("   pickle", lambda: [(lambda r: (r is z, r is ZoneInfo("East"), r.key))(pickle.loads(pickle.dumps(z, p))) for p in range(6)])
        attempt("   pickled", lambda: pickle.dumps(z, 2))
        attempt("   copy", lambda: (copy.copy(z) is z, copy.deepcopy(z) is z))
        attempt("   in a datetime", lambda: (lambda d: pickle.loads(pickle.dumps(d)) == d)(dt(2000, 1, 1, tzinfo=z)))
    for a in (("East", 1), ("East", 0), ("East", True), ("East", 256), ("East", 257), ("East", -1), ("East", 2**70), ("East", None), ("East", "1"), ("East", 1.0), ("Nowhere", 1), ("Nowhere", 0), (5, 1), ("East",), (), ("East", 1, 2)):
        attempt("_unpickle%r" % (a,), lambda: (lambda z: (repr(z), z is ZoneInfo("East")))(ZoneInfo._unpickle(*a)))
    attempt("_unpickle: keywords", lambda: ZoneInfo._unpickle(key="East", from_cache=1))
    print("===== what it is asked about")
    e = ZoneInfo("East"); fixed = ZoneInfo("Here/Z3")
    class D(dt): pass
    class Duck:
        def __init__(self, o, h=0, m=0, s=0): self.o, self.hour, self.minute, self.second = o, h, m, s
        def toordinal(self): return self.o
    for label, v in {"None": None, "a datetime": dt(2024, 7, 1), "in winter": dt(2024, 1, 1), "aware": dt(2024, 7, 1, tzinfo=tz.utc), "its own": dt(2024, 7, 1, tzinfo=e), "a fold": dt(2024, 11, 3, 1, 30, fold=1), "no fold": dt(2024, 11, 3, 1, 30), "skipped": dt(2024, 3, 10, 2, 30), "skipped, with a fold": dt(2024, 3, 10, 2, 30, fold=1), "derived": D(2024, 7, 1), "derived, with a fold": D(2024, 11, 3, 1, 30, fold=1),
                     "a date": date(2024, 7, 1), "a time": time(12), "an int": 5, "a str": "x", "the first": dt.min, "the last": dt.max, "toordinal gives a str": Duck("x"), "hour is a str": Duck(1, "x"), "toordinal too large": Duck(2**70), "no minute": type("X", (), {"toordinal": lambda s: 1, "hour": 1})()}.items():
        for m in ("utcoffset", "dst", "tzname"):
            attempt("%s(%s)" % (m, label), lambda: (getattr(e, m)(v), getattr(fixed, m)(v)))
    for m in ("utcoffset", "dst", "tzname", "fromutc"):
        attempt("%s: none" % m, lambda: getattr(e, m)())
        attempt("%s: two" % m, lambda: getattr(e, m)(None, None))
        attempt("%s: keyword" % m, lambda: getattr(e, m)(dt=None))
        attempt("%s: of something else" % m, lambda: getattr(ZoneInfo, m)(tz.utc, None))
    attempt("the same timedelta each time", lambda: (e.utcoffset(dt(2024, 1, 1)) is e.utcoffset(dt(2023, 1, 1)), e.utcoffset(dt(2024, 1, 1)) is ZoneInfo.no_cache("East").utcoffset(dt(2024, 1, 1)), e.dst(dt(2024, 1, 1)) is fixed.dst(None), type(e.utcoffset(dt(2024, 1, 1))).__name__))
    attempt("in a time", lambda: (time(12, tzinfo=e).utcoffset(), time(12, tzinfo=e).tzname(), time(12, tzinfo=fixed).utcoffset(), time(12, tzinfo=fixed).tzname(), time(12, tzinfo=fixed).isoformat(), time(12, tzinfo=e).isoformat()))
    for label, v in {"its own": dt(2024, 7, 1, tzinfo=e), "naive": dt(2024, 7, 1), "another's": dt(2024, 7, 1, tzinfo=tz.utc), "an equal one's": dt(2024, 7, 1, tzinfo=ZoneInfo.no_cache("East")), "a date": date(2024, 7, 1), "None": None, "an int": 5, "repeated": dt(2024, 11, 3, 5, 30, tzinfo=e), "repeated, the second time": dt(2024, 11, 3, 6, 30, tzinfo=e), "derived": D(2024, 7, 1, tzinfo=e),
                     "derived, repeated the second time": D(2024, 11, 3, 6, 30, tzinfo=e), "the first": dt.min.replace(tzinfo=e), "the last": dt.max.replace(tzinfo=e)}.items():
        attempt("fromutc(%s)" % label, lambda: (lambda r: (r.isoformat(), r.fold, type(r).__name__, r.tzinfo is e))(e.fromutc(v)))
    class DR(dt):
        def replace(self, **k): return ("replace", sorted(k.items()))
    attempt("what is asked of a derived class", lambda: e.fromutc(DR(2024, 11, 3, 6, 30, tzinfo=e)))
    class DA(dt):
        def __add__(self, o): return "added"
    attempt("   that adds in its own way", lambda: (e.fromutc(DA(2024, 7, 1, tzinfo=e)), ))
    attempt("   and is in what is repeated", lambda: e.fromutc(DA(2024, 11, 3, 6, 30, tzinfo=e)))
    attempt("with datetime", lambda: (dt(2024, 7, 1, 12, tzinfo=e).astimezone(tz.utc).isoformat(), dt(2024, 7, 1, 12, tzinfo=tz.utc).astimezone(e).isoformat(), dt.fromtimestamp(1720000000, e).isoformat(), dt(2024, 11, 3, 1, 30, tzinfo=e).timestamp(), dt(2024, 11, 3, 1, 30, tzinfo=e, fold=1).timestamp(), (dt(2024, 3, 10, 1, tzinfo=e) + td(hours=2)).isoformat(),
        dt(2024, 11, 3, 1, 30, tzinfo=e) == dt(2024, 11, 3, 5, 30, tzinfo=tz.utc), dt(2024, 7, 1, tzinfo=e) == dt(2024, 7, 1, 4, tzinfo=tz.utc), hash(dt(2024, 7, 1, tzinfo=e)) == hash(dt(2024, 7, 1, 4, tzinfo=tz.utc)), dt(2024, 7, 1, tzinfo=e).strftime("%Z %z"), repr(dt(2024, 7, 1, tzinfo=e)), type(dt.now(e)).__name__))
    print("===== one of them, and the class")
    f = ZoneInfo.from_file(io.BytesIO(tzif(b"AAA0")))
    attempt("from a file", lambda: (address.sub("0x", repr(f)), address.sub("0x", str(f)), f.key))
    attempt("from a file whose repr raises", lambda: ZoneInfo.from_file(type("F", (io.BytesIO,), {"__repr__": lambda s: 1 / 0})(tzif(b"AAA0"))))
    attempt("keys of other kinds", lambda: [(repr(z), str(z) if isinstance(z.key, str) else None, z.key) for z in (ZoneInfo.from_file(io.BytesIO(tzif(b"AAA0")), key=k) for k in ("k", 5, (1, 2), b"k", ""))])
    attempt("str of a key that is no str", lambda: str(ZoneInfo.from_file(io.BytesIO(tzif(b"AAA0")), key=5)))
    attempt("key", lambda: (e.key, type(vars(ZoneInfo)["key"]).__name__))
    attempt("set key", lambda: setattr(e, "key", "x"))
    attempt("delete key", lambda: delattr(e, "key"))
    attempt("another attribute", lambda: setattr(e, "x", 1))
    attempt("weak reference", lambda: weakref.ref(e)() is e)
    attempt("compared", lambda: (e == e, e == ZoneInfo.no_cache("East"), e != fixed, e == "East"))
    attempt("ordered", lambda: e < fixed)
    attempt("bool, isinstance", lambda: (bool(e), isinstance(e, datetime.tzinfo), issubclass(ZoneInfo, datetime.tzinfo)))
    attempt("the class", lambda: (ZoneInfo, ZoneInfo.__name__, ZoneInfo.__qualname__, ZoneInfo.__module__, ZoneInfo.__mro__, ZoneInfo.__doc__, ZoneInfo.__text_signature__, ZoneInfo.__basicsize__, ZoneInfo.__dictoffset__, ZoneInfo.__weakrefoffset__, hex(ZoneInfo.__flags__), sorted(vars(ZoneInfo))))
    for n in sorted(vars(ZoneInfo)):
        v = vars(ZoneInfo)[n]
        attempt("   " + n, lambda: (type(v).__name__, getattr(v, "__doc__", None), getattr(v, "__text_signature__", None), getattr(v, "__qualname__", None)))
    attempt("set on the class", lambda: setattr(ZoneInfo, "x", 1))
    attempt("the module", lambda: (sorted(vars(_zoneinfo)), _zoneinfo.__doc__, _zoneinfo.__name__, zoneinfo.ZoneInfo is _zoneinfo.ZoneInfo))
finally:
    shutil.rmtree(where)
print("done")
