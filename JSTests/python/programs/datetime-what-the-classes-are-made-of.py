# What the classes of datetime are made of
import _datetime, datetime, inspect, sys, types, warnings
warnings.simplefilter("ignore")
import re; address = re.compile('0x[0-9a-f]+')
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
    # When it is now is not for here.
    if type(r) in (datetime.datetime, datetime.date) and r.year > 2000: r = type(r).__name__
    print(label, "->", address.sub("0x", ascii(r)))
print(sorted(vars(_datetime)), _datetime.__name__, _datetime.__doc__, _datetime.__spec__.origin, _datetime.__spec__.loader.__name__, hasattr(_datetime, "__file__"))
print(sorted(n for n in vars(datetime) if not n.startswith("__")), datetime.__all__, datetime.MINYEAR, datetime.MAXYEAR)
attempt("the capsule", lambda: (type(_datetime.datetime_CAPI).__name__, repr(_datetime.datetime_CAPI).split(" at ")[0], datetime.datetime_CAPI is _datetime.datetime_CAPI))
for c in (datetime.timedelta, datetime.date, datetime.datetime, datetime.time, datetime.tzinfo, datetime.timezone, type(datetime.date(1, 1, 1).isocalendar())):
    print("=====", c)
    print(c.__name__, c.__qualname__, c.__module__, c.__mro__, c.__bases__, c.__base__, type(c), c.__basicsize__, c.__itemsize__, c.__dictoffset__, c.__weakrefoffset__, hex(c.__flags__), c.__text_signature__)
    print(ascii(c.__doc__))
    attempt("signature", lambda: str(inspect.signature(c)))
    for n in sorted(vars(c)):
        v = vars(c)[n]
        print("  ", n, type(v).__name__, ascii(getattr(v, "__doc__", None))[:90], getattr(v, "__text_signature__", "none"), getattr(v, "__name__", "none"), getattr(v, "__qualname__", "none"), getattr(getattr(v, "__objclass__", None), "__name__", None))
        if callable(v) or isinstance(v, (classmethod, staticmethod)):
            attempt("      signature", lambda: str(inspect.signature(getattr(c, n))))
    print(sorted(set(dir(c)) - set(dir(object))))
    attempt("predicates", lambda: [(n, inspect.ismethoddescriptor(v), inspect.isdatadescriptor(v), inspect.isgetsetdescriptor(v), inspect.ismemberdescriptor(v), inspect.isbuiltin(v), inspect.isroutine(v)) for n, v in sorted(vars(c).items()) if not n.startswith("__")])
one = {"timedelta": datetime.timedelta(1), "date": datetime.date(1, 1, 1), "datetime": datetime.datetime(1, 1, 1), "time": datetime.time(), "tzinfo": datetime.tzinfo(), "timezone": datetime.timezone.utc}
print("===== called wrongly")
for name, o in one.items():
    for n in sorted(set(dir(o)) - set(dir(object)) | {"__reduce__", "__reduce_ex__", "__format__", "__hash__", "__repr__", "__str__", "__eq__"}):
        m = getattr(o, n)
        if not callable(m): continue
        for a in ((1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11), ):
            attempt("%s.%s%r" % (name, n, a), lambda: m(*a))
        attempt("%s.%s(zzz=1)" % (name, n), lambda: m(zzz=1))
        u = getattr(type(o), n)
        attempt("   unbound, of nothing", lambda: u())
        if getattr(u, "__objclass__", None) is not object: attempt("   unbound, of an int", lambda: u(5))
        attempt("   bound", lambda: (type(m).__name__, m.__name__, m.__qualname__, m.__self__ is o or m.__self__ is type(o), repr(m).split(" at ")[0].split(" of ")[0]))
print("===== isinstance")
for name, o in one.items():
    print(name, [isinstance(o, c) for c in (datetime.timedelta, datetime.date, datetime.datetime, datetime.time, datetime.tzinfo, datetime.timezone)], [issubclass(type(o), c) for c in (datetime.date, datetime.tzinfo)])
for bases in ((datetime.date, datetime.tzinfo), (datetime.date, datetime.time), (datetime.datetime, datetime.date), (datetime.date, datetime.datetime), (datetime.timedelta, int), (datetime.tzinfo, dict), (datetime.date, object), (datetime.tzinfo, Exception), (datetime.timezone,), (datetime.date, tuple)):
    attempt("class X%r" % ([b.__name__ for b in bases],), lambda: type("X", bases, {}).__mro__)
for c in (datetime.timedelta, datetime.date, datetime.datetime, datetime.time, datetime.tzinfo):
    attempt("slots on %s" % c.__name__, lambda: (lambda X: (X.__dictoffset__, X.__weakrefoffset__, X.__basicsize__ - c.__basicsize__))(type("X", (c,), {"__slots__": ("a", "b")})))
    attempt("   no slots", lambda: (lambda X: (X.__dictoffset__ != 0, X.__weakrefoffset__ != 0))(type("X", (c,), {})))
    attempt("   __class__", lambda: setattr(c.__new__(c, *((1, 1, 1) if issubclass(c, datetime.date) else ())), "__class__", type("X", (c,), {})))
    attempt("   __class__ among derived ones", lambda: (lambda A, B: (lambda o: (setattr(o, "__class__", B), type(o).__name__))(A(*((1, 1, 1) if issubclass(c, datetime.date) else ()))))(type("A", (c,), {}), type("B", (c,), {})))
print("done")
