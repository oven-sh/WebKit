import sys
def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
def attempt(f):
    try: return f()
    except BaseException as e: return type(e).__name__ + ": " + str(e)
BEG, EG = BaseExceptionGroup, ExceptionGroup
def shape(e):
    if e is None: return None
    if isinstance(e, BEG): return (type(e).__name__, e.message, [shape(x) for x in e.exceptions])
    return type(e).__name__ + repr(e.args)
def tb_names(e):
    out = []; tb = e.__traceback__
    while tb: out.append(tb.tb_frame.f_code.co_name); tb = tb.tb_next
    return out
# ---- the classes
show("classes", lambda: (BEG.__mro__, EG.__mro__, BEG.__module__, EG.__module__, BEG.__qualname__, EG.__bases__, BEG.__doc__, EG.__doc__))
show("kinds", lambda: [(n, type(BEG.__dict__[n]).__name__) for n in sorted(BEG.__dict__)])
show("kinds of ExceptionGroup", lambda: [(n, type(EG.__dict__[n]).__name__) for n in sorted(EG.__dict__)])
show("flags", lambda: (BEG.__flags__ & (1 << 9) != 0, EG.__flags__ & (1 << 9) != 0, BEG.__basicsize__ > BaseException.__basicsize__, BEG.__dictoffset__ != 0, EG.__weakrefoffset__ != 0))
show("generic", lambda: (repr(EG[int]), repr(BEG[int, str]), type(EG[int]).__name__, EG[int].__origin__ is EG))
# ---- making one
for a in ((), ("m",), ("m", [ValueError(1)], 3), (1, [ValueError(1)]), (None, [ValueError(1)]), ("m", 1), ("m", None), ("m", {}), ("m", set()), ("m", "ab"), ("m", []), ("m", ()), ("m", [1]), ("m", [ValueError(1), 2]), ("m", [ValueError]), ("m", iter([ValueError(1)])), ("m", (x for x in [ValueError(1)])), ("m", range(0)), ("m", range(1))):
    show("new " + repr(tuple(x if isinstance(x, (str, int, type(None))) else type(x).__name__ + (str(len(x)) if hasattr(x, "__len__") else "") for x in a)), lambda: (shape(attempt(lambda: BEG(*a))) if isinstance(attempt(lambda: BEG(*a)), BEG) else attempt(lambda: BEG(*a)), attempt(lambda: EG(*a)) if not isinstance(attempt(lambda: EG(*a)), BEG) else "made"))
show("keywords", lambda: (attempt(lambda: EG("m", [ValueError(1)], a=1)), attempt(lambda: EG(message="m", exceptions=[ValueError(1)])), attempt(lambda: BEG.__new__(BEG, "m", [ValueError(1)], a=1)).__class__.__name__))
show("which class", lambda: [type(x).__name__ for x in (BEG("m", [ValueError(1)]), BEG("m", [KeyboardInterrupt()]), BEG("m", [ValueError(1), SystemExit()]), EG("m", [EG("n", [ValueError(1)])]), BEG("m", [BEG("n", [KeyboardInterrupt()])]))])
show("cannot nest", lambda: (attempt(lambda: EG("m", [KeyboardInterrupt()])), attempt(lambda: EG("m", [BEG("n", [SystemExit()])]))))
class MyEG(EG): pass
class MyBEG(BEG): pass
class Both(BEG, ValueError): pass
show("subclasses", lambda: [attempt(lambda: type(c("m", [x])).__name__) for c in (MyEG, MyBEG, Both) for x in (ValueError(1), KeyboardInterrupt())])
show("__new__ of another", lambda: (attempt(lambda: BEG.__new__(int, "m", [ValueError(1)])), attempt(lambda: BEG.__new__(ValueError, "m", [ValueError(1)])), attempt(lambda: BEG.__new__()), attempt(lambda: BEG.__new__(1)), type(BEG.__new__(MyBEG, "m", [ValueError(1)])).__name__))
# ---- what it has
e = EG("msg", [ValueError(1), TypeError(2)])
show("attributes", lambda: (e.message, e.exceptions, e.args, str(e), repr(e), type(e.exceptions).__name__))
show("read only", lambda: (attempt(lambda: setattr(e, "message", "x")), attempt(lambda: setattr(e, "exceptions", ())), attempt(lambda: delattr(e, "message"))))
show("str of one", lambda: (str(EG("m", [ValueError(1)])), str(EG("", [ValueError(1)])), str(EG("m", [ValueError(i) for i in range(3)]))))
l = [ValueError(1)]; el = EG("m", l); l.append(TypeError(2))
show("a list that changes", lambda: (repr(el), el.exceptions, el.args))
show("repr of a tuple", lambda: repr(EG("m", (ValueError(1),))))
class Seq:
    def __init__(s, v): s.v = v
    def __len__(s): return len(s.v)
    def __getitem__(s, i): return s.v[i]
    def __repr__(s): return "Seq(%r)" % (s.v,)
show("another sequence", lambda: (repr(EG("m", Seq([ValueError(1)]))), EG("m", Seq([ValueError(1)])).exceptions, type(EG("m", Seq([ValueError(1)])).args[1]).__name__))
show("repr of a subclass", lambda: (repr(MyEG("m", [ValueError(1)])), str(MyEG("m", [ValueError(1)]))))
show("init again", lambda: (e.__init__("other", [KeyError(3)]), e.message, e.exceptions, e.args, attempt(lambda: e.__init__(a=1)), repr(e)))
show("dict", lambda: (setattr(el, "extra", 1), el.__dict__, el.extra))
# ---- derive, subgroup and split
g = EG("top", [ValueError(1), EG("mid", [TypeError(2), ValueError(3), EG("low", [KeyError(4)])]), OSError(5)])
show("derive", lambda: (shape(g.derive([KeyError(9)])), type(MyEG("m", [ValueError(1)]).derive([ValueError(2)])).__name__, shape(g.derive([KeyboardInterrupt()])), attempt(lambda: g.derive([])), attempt(lambda: g.derive()), attempt(lambda: g.derive(1))))
for m in (ValueError, (ValueError, KeyError), Exception, EG, BEG, MemoryError, (), lambda x: isinstance(x, KeyError), lambda x: True, lambda x: False, lambda x: 0, lambda x: "yes"):
    show("split " + getattr(m, "__name__", repr(m)), lambda: (shape(g.split(m)[0]), shape(g.split(m)[1]), shape(g.subgroup(m)), g.subgroup(m) is g, type(g.split(m)).__name__))
for m in (1, None, "a", int, [ValueError], (ValueError, 1), (ValueError, int), ((ValueError,),), object):
    show("bad matcher " + repr(m), lambda: (attempt(lambda: g.split(m)), attempt(lambda: g.subgroup(m))))
class T(tuple): pass
show("a subclass of tuple", lambda: attempt(lambda: g.split(T((ValueError,)))))
class Callable:
    def __call__(s, x): return isinstance(x, TypeError)
show("a callable object", lambda: shape(g.subgroup(Callable())))
show("a predicate is given groups too", lambda: (lambda seen: (g.subgroup(lambda x: seen.append(type(x).__name__)), seen)[1])([]))
show("a predicate that raises", lambda: attempt(lambda: g.split(lambda x: 1 / 0)))
show("wrong numbers", lambda: (attempt(lambda: g.split()), attempt(lambda: g.split(1, 2)), attempt(lambda: g.subgroup()), attempt(lambda: g.split(matcher_value=ValueError))))
# ---- what the parts keep
def raised():
    try:
        try: raise KeyError("ctx")
        except KeyError:
            raise EG("r", [ValueError(1), TypeError(2)]) from OSError("cause")
    except EG as x:
        x.add_note("a note"); return x
r = raised(); a, b = r.split(ValueError)
show("metadata", lambda: [(shape(p), p.__traceback__ is r.__traceback__, p.__cause__ is r.__cause__, p.__context__ is r.__context__, p.__suppress_context__, p.__notes__, p.__notes__ is r.__notes__) for p in (a, b)])
n = EG("n", [ValueError(1), TypeError(2)])
show("with nothing to keep", lambda: [(p.__traceback__, p.__cause__, p.__context__, p.__suppress_context__, hasattr(p, "__notes__")) for p in n.split(ValueError)])
n.__notes__ = 5
show("notes that are no sequence", lambda: [hasattr(p, "__notes__") for p in n.split(ValueError)])
n.__notes__ = ("t",)
show("notes that are a tuple", lambda: [p.__notes__ for p in n.split(ValueError)])
n.__notes__ = "ab"
show("notes that are a string", lambda: [p.__notes__ for p in n.split(ValueError)])
class Deriving(EG):
    def __new__(cls, m, x, code): s = super().__new__(cls, m, x); s.code = code; return s
    def derive(s, x): return Deriving(s.message, x, s.code)
d = Deriving("d", [ValueError(1), TypeError(2)], 42)
show("derive is used", lambda: [(type(p).__name__, p.code, shape(p)) for p in d.split(ValueError)])
class BadDerive(EG):
    def derive(s, x): return 5
show("derive gives something else", lambda: attempt(lambda: BadDerive("d", [ValueError(1), TypeError(2)]).split(ValueError)))
class NoDerive(EG): pass
show("derive is not overridden", lambda: [type(p).__name__ for p in NoDerive("d", [ValueError(1), TypeError(2)]).split(ValueError)])
def deep(n):
    x = ValueError(0)
    for i in range(n): x = EG("d", [x])
    return x
show("deep", lambda: (attempt(lambda: type(deep(200).split(TypeError)).__name__)[:60]))
# ---- except*
def star(make, *handlers):
    log = []
    def run():
        try: raise make()
        except* ValueError as x: log.append(("V", shape(x)))
        except* (TypeError, KeyError) as x: log.append(("TK", shape(x)))
    r = attempt(run)
    return (log, r)
show("except* all", lambda: star(lambda: EG("g", [ValueError(1), TypeError(2), KeyError(3)])))
show("except* some", lambda: star(lambda: EG("g", [ValueError(1), OSError(2)])))
show("except* none", lambda: star(lambda: EG("g", [OSError(2)])))
show("except* naked", lambda: star(lambda: ValueError("naked")))
show("except* naked, not matched", lambda: star(lambda: OSError("naked")))
show("except* nested", lambda: star(lambda: EG("g", [EG("h", [ValueError(1), OSError(2)]), TypeError(3)])))
def naked_traceback():
    def inner(): raise ValueError("n")
    try: inner()
    except* ValueError as x: r = (tb_names(x), tb_names(x.exceptions[0]), x.message)
    return r
show("what a naked one is wrapped in", naked_traceback)
def what_is_left():
    try:
        try: raise EG("g", [ValueError(1), OSError(2)])
        except* ValueError: pass
    except EG as x: return (shape(x), tb_names(x))
show("what is left", what_is_left)
def raises_in_handler():
    try:
        try: raise EG("g", [ValueError(1), OSError(2)])
        except* ValueError: raise KeyError("new")
    except EG as x: return (shape(x), [shape(i.__context__) for i in x.exceptions], x.__context__, x.__cause__)
show("raising in a handler", raises_in_handler)
def reraises():
    try:
        try: raise EG("g", [ValueError(1), EG("h", [ValueError(2), OSError(3)])])
        except* ValueError: raise
    except EG as x: return shape(x)
show("raising again", reraises)
def reraises_with_notes():
    try:
        try:
            x = EG("g", [ValueError(1), EG("h", [ValueError(2), OSError(3)])]); x.add_note("n"); raise x
        except* ValueError: raise
    except EG as x: return (shape(x), x.__notes__)
show("raising again what has notes", reraises_with_notes)
def reraises_named():
    try:
        try: raise EG("g", [ValueError(1), TypeError(2), OSError(3)])
        except* ValueError as v: raise v
        except* TypeError: pass
    except EG as x: return shape(x)
show("raising what was caught", reraises_named)
def two_raise():
    try:
        try: raise EG("g", [ValueError(1), TypeError(2)])
        except* ValueError: raise KeyError("a")
        except* TypeError: raise IndexError("b")
    except EG as x: return shape(x)
show("two handlers raise", two_raise)
def only_one_raises():
    try:
        try: raise EG("g", [ValueError(1)])
        except* ValueError: raise KeyError("a")
    except BaseException as x: return (shape(x), shape(x.__context__))
show("all handled and one raises", only_one_raises)
for t in (EG, BEG, (ValueError, EG), MyEG, 1, (ValueError, 1), int, "a", None, ()):
    def bad(t=t):
        r = "not caught"
        try: raise EG("g", [ValueError(1)])
        except* t: r = "caught"
        return r
    show("except* " + getattr(t, "__name__", repr(t)), lambda: attempt(bad))
class BadSplit(EG):
    def __new__(cls, m, x, r): s = super().__new__(cls, m, x); s.r = r; return s
    def split(s, t): return s.r
for r in (5, None, [1, 2], (), (1,), (None, None), (None, None, 3)):
    def f(r=r):
        w = "fell through"
        try: raise BadSplit("b", [ValueError(1)], r)
        except* TypeError: w = "caught"
        return w
    show("split gives " + repr(r), lambda: attempt(f))
def base_group():
    try: raise BEG("g", [KeyboardInterrupt(), ValueError(1)])
    except* KeyboardInterrupt as x: r = shape(x)
    except* ValueError: pass
    return r
show("a group of BaseExceptions", lambda: attempt(base_group))
def current():
    try: raise EG("g", [ValueError(1), TypeError(2)])
    except* ValueError: a = shape(sys.exception())
    except* TypeError: b = shape(sys.exception())
    return (a, b, sys.exception())
show("the exception being handled", current)
