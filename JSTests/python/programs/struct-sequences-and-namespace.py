import sys
def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
def attempt(f):
    try: return f()
    except BaseException as e: return type(e).__name__ + ": " + str(e)
# ---- struct sequences
F = type(sys.float_info); V = type(sys.version_info); G = type(sys.flags); H = type(sys.get_asyncgen_hooks()); I = type(sys.int_info)
show("float_info", lambda: (sys.float_info, len(sys.float_info), sys.float_info[0], sys.float_info.max, sys.float_info[-1], sys.float_info[1:3], tuple(sys.float_info)[:2]))
show("int_info", lambda: sys.int_info)
show("version", lambda: (sys.version_info[:2], sys.version_info.major, sys.version_info >= (3, 14), sys.version_info < (4,), type(sys.version_info[:2]).__name__, sys.hexversion >> 16, sys.version.split()[0].rsplit(".", 1)[0]))
show("flags", lambda: (len(sys.flags), G.n_fields, G.n_sequence_fields, G.n_unnamed_fields, sys.flags.optimize, sys.flags.dev_mode, sys.flags.int_max_str_digits, sys.flags.gil, sys.flags.utf8_mode, G.__match_args__))
show("names", lambda: [(t.__name__, t.__qualname__, t.__module__, repr(t)) for t in (F, V, G, H, I)])
show("bases", lambda: [(t.__mro__[1].__name__, isinstance(x, tuple), t.__base__ is tuple) for t, x in ((F, sys.float_info), (V, sys.version_info))])
show("class attributes", lambda: (I.n_fields, I.n_sequence_fields, I.n_unnamed_fields, I.__match_args__, sorted(n for n in vars(I) if n != "__doc__")))
show("kinds", lambda: [type(vars(I)[n]).__name__ for n in ("bits_per_digit", "__new__", "__repr__", "__reduce__", "__replace__", "n_fields", "__match_args__")])
show("new", lambda: (I((1, 2, 3, 4)), I([1, 2, 3, 4]), I(iter((1, 2, 3, 4))), I("abcd"), I(sequence=(1, 2, 3, 4)), H((1, 2)).firstiter))
for a in [(), ((1, 2, 3),), ((1, 2, 3, 4, 5),), (1,), ((1, 2, 3, 4), 5), ((1, 2, 3, 4), {}), ((1, 2, 3, 4), {"x": 1}), ((1, 2, 3, 4), {"bits_per_digit": 1}), ((1, 2, 3, 4), {}, 1), (None,)]:
    show("new" + repr(a), lambda: I(*a))
show("new keyword", lambda: I((1, 2, 3, 4), dict={}))
show("new bad keyword", lambda: I((1, 2, 3, 4), nope=1))
show("cannot be made", lambda: [attempt(lambda: t((1,))) for t in (G, V)])
show("cannot be derived from", lambda: [attempt(lambda: type("X", (t,), {})) for t in (F, G, V)])
show("read only", lambda: [attempt(f) for f in (lambda: setattr(sys.int_info, "bits_per_digit", 1), lambda: delattr(sys.int_info, "bits_per_digit"), lambda: setattr(sys.int_info, "other", 1), lambda: sys.int_info.__setitem__(0, 1), lambda: setattr(I, "x", 1))])
show("reduce", lambda: (sys.int_info.__reduce__(), sys.int_info.__reduce__()[0] is I, H((1, 2)).__reduce__()[1]))
show("flags reduce", lambda: (lambda r: (r[0] is G, len(r[1][0]), sorted(r[1][1])))(sys.flags.__reduce__()))
show("replace", lambda: (sys.int_info.__replace__(), sys.int_info.__replace__(bits_per_digit=1), sys.int_info.__replace__(sizeof_digit=9, bits_per_digit=8), type(sys.int_info.__replace__()).__name__, sys.int_info.__replace__() is sys.int_info))
show("replace hidden", lambda: (lambda f: (f.gil, f.debug, len(f)))(sys.flags.__replace__(gil=7, debug=5)))
show("replace wrong", lambda: sys.int_info.__replace__(nope=1))
show("replace wrong two", lambda: sys.int_info.__replace__(nope=1, other=2, bits_per_digit=3))
show("replace positional", lambda: sys.int_info.__replace__(1))
show("as a tuple", lambda: (sys.int_info == (30, 4, 4300, 640), hash(sys.int_info) == hash((30, 4, 4300, 640)), sys.int_info + (1,), sys.int_info * 2 == (30, 4, 4300, 640) * 2, 30 in sys.int_info, sys.int_info.index(4), sys.int_info.count(4), list(reversed(sys.int_info)), max(sys.int_info)))
def matches(x):
    match x:
        case I(a, b, c, d): return ("positional", a, b, c, d)
def matches_by_name(x):
    match x:
        case I(sizeof_digit=s): return ("by name", s)
def matches_sequence(x):
    match x:
        case [a, *rest]: return ("sequence", a, rest)
show("match", lambda: (matches(sys.int_info), matches_by_name(sys.int_info), matches_sequence(sys.int_info)))
a, b, c, d = sys.int_info
show("unpacked", lambda: (a, b, c, d))
show("str and format", lambda: (str(sys.int_info), "%s %s %s %s" % sys.int_info, "{0.bits_per_digit}".format(sys.int_info)))
show("docs", lambda: (I.__doc__, vars(I)["bits_per_digit"].__doc__, F.__doc__.split("\n")[0], I.__new__.__text_signature__))
show("thread_info", lambda: (type(sys.thread_info).__name__, len(sys.thread_info), sys.thread_info._fields if hasattr(sys.thread_info, "_fields") else "no _fields"))
show("hash_info", lambda: (sys.hash_info.width, sys.hash_info.modulus, sys.hash_info.inf, sys.hash_info.nan, sys.hash_info.imag, len(sys.hash_info), type(sys.hash_info.algorithm).__name__))
# ---- SimpleNamespace
N = type(sys.implementation)
show("namespace", lambda: (N(), N(a=1, b="x"), N({"a": 1}), N([("a", 1)]), N({"a": 1}, a=2, b=3), N.__name__, N.__qualname__, N.__module__, repr(N)))
show("namespace attributes", lambda: (lambda n: (n.a, setattr(n, "b", 2), n.b, n.__dict__, delattr(n, "a"), n, vars(n)))(N(a=1)))
show("namespace missing", lambda: N().x)
show("namespace compare", lambda: (N(a=1) == N(a=1), N(a=1) != N(a=2), N(a=1) == {"a": 1}, N() == N(), N(a=1, b=2) == N(b=2, a=1), N(a=1).__eq__(1), N(a=1).__lt__(N())))
show("namespace order", lambda: N(a=1) < N(a=2))
show("namespace hash", lambda: hash(N()))
show("namespace two", lambda: N(1, 2))
show("namespace bad", lambda: N(1))
show("namespace bad keys", lambda: N({1: 2}))
show("namespace reduce", lambda: (N(a=1).__reduce__(), N(a=1).__reduce__()[0] is N))
show("namespace replace", lambda: (N(a=1, b=2).__replace__(a=5, c=6), N(a=1).__replace__(), N(a=1).__replace__() is not None))
show("namespace replace positional", lambda: N().__replace__(1))
def recursive():
    n = N(); n.me = n; return repr(n)
show("namespace recursive", recursive)
class SN(N): pass
show("derived namespace", lambda: (SN(a=1), type(SN(a=1).__replace__(a=2)).__name__, SN(a=1) == N(a=1), SN.__mro__[1] is N))
class BadNew(N):
    def __new__(cls, *a, **k): return 5 if flag else super().__new__(cls)
flag = False
def bad_replace():
    global flag
    n = BadNew(a=1); flag = True
    try: return n.__replace__()
    finally: flag = False
show("replace of the wrong kind", bad_replace)
show("odd names", lambda: (lambda n: (n.__dict__.update({"": 1, 2: 3, "ok": 4}), repr(n)))(N()))
show("dict read only", lambda: setattr(N(), "__dict__", {}))
show("namespace own", lambda: sorted(n for n in vars(N) if n != "__doc__"))
show("implementation", lambda: (type(sys.implementation.name).__name__, sys.implementation.version is sys.version_info or sys.implementation.version == sys.version_info, sys.implementation.hexversion == sys.hexversion, hasattr(sys.implementation, "cache_tag")))
