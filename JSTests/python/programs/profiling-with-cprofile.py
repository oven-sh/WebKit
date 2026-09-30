# The module _lsprof, and cProfile and pstats over it. The clock is one that goes on by one each time that it is read, so that how long everything is said to have taken is a matter of what the profiler was told of, and in what order.
import _lsprof
import cProfile
import io
import pstats
import sys


def t(label, f):
    try:
        print(label, "=>", f())
    except Exception as e:
        print(label, "=>", type(e).__name__, e)


class Clock:
    def __init__(self, step=1):
        self.now, self.step = 0, step

    def __call__(self):
        self.now += self.step
        return self.now


def name_of(code):
    return code if isinstance(code, str) else code.co_name


def shown(profiler):
    "What it has, in an order that does not go by where things are in memory"
    result = []
    for e in sorted(profiler.getstats(), key=lambda e: name_of(e.code)):
        calls = e.calls and sorted((name_of(c.code), c.callcount, c.reccallcount, c.totaltime, c.inlinetime) for c in e.calls)
        result.append((name_of(e.code), e.callcount, e.reccallcount, e.totaltime, e.inlinetime, calls))
    return result


def profiled(f, *arguments, **settings):
    p = _lsprof.Profiler(Clock(), 1.0, **settings)
    p.enable(**settings)
    try:
        f(*arguments)
    except Exception:
        pass
    p.disable()
    return shown(p)


def show(label, f, *arguments, **settings):
    print(label)
    for line in profiled(f, *arguments, **settings):
        print("   ", line)


print("---- what there is")
print(sorted(n for n in vars(_lsprof) if not n.startswith("__")), _lsprof.__doc__, _lsprof.__spec__.origin)
c = _lsprof.Profiler
print(c.__module__, c.__qualname__, [b.__name__ for b in c.__mro__[1:]], hex(c.__flags__), c.__text_signature__, repr(c.__doc__), sorted(n for n in vars(c)))
for n in sorted(vars(c)):
    if not n.startswith("__"):
        print("   ", n, type(vars(c)[n]).__name__, vars(c)[n].__text_signature__, repr(vars(c)[n].__doc__)[:60])
for c in (_lsprof.profiler_entry, _lsprof.profiler_subentry):
    print(c.__module__, c.__qualname__, c.n_fields, c.n_sequence_fields, c.__match_args__, c.__doc__, [vars(c)[n].__doc__ for n in c.__match_args__])

print("---- calls")


def leaf():
    pass


def calls_twice():
    leaf()
    leaf()


def outer():
    calls_twice()
    leaf()


def recursive(n):
    if n:
        recursive(n - 1)


def one(n):
    if n:
        other(n - 1)


def other(n):
    if n:
        one(n - 1)


def raises():
    raise KeyError("x")


def catches():
    try:
        raises()
    except KeyError:
        leaf()


def passes_it_on():
    raises()


def generates(n):
    for i in range(n):
        yield i


def goes_through():
    for x in generates(3):
        leaf()


def is_thrown_into():
    g = catching_generator()
    next(g)
    g.throw(KeyError("thrown"))
    g.close()


def catching_generator():
    try:
        yield 1
    except KeyError:
        yield 2


def is_closed_early():
    g = generates(5)
    next(g)
    g.close()


class C:
    def __init__(self):
        leaf()

    def method(self):
        leaf()

    def __len__(self):
        return 3

    def __call__(self):
        leaf()

    @property
    def attribute(self):
        return leaf()

    @staticmethod
    def static():
        leaf()

    @classmethod
    def of_the_class(cls):
        leaf()


def uses_a_class():
    c = C()
    c.method()
    C.method(c)
    len(c)
    c()
    c.attribute
    c.static()
    C.of_the_class()


def uses_builtins():
    l = []
    l.append(1)
    list.append(l, 2)
    len(l)
    sorted(l, key=lambda x: -x)
    "-".join(["a", "b"])
    str.upper("a")
    {}.get(1)
    max(1, 2)
    isinstance(1, int)
    int("5")
    dict(a=1)
    [x for x in l]
    list(map(abs, l))
    bytes.maketrans(b"a", b"b")
    dict.fromkeys("ab")


def a_builtin_raises():
    try:
        int("x")
    except ValueError:
        pass
    try:
        [].pop()
    except IndexError:
        pass
    try:
        len(5)
    except TypeError:
        pass


async def coroutine():
    await awaited()
    return 1


async def awaited():
    leaf()


def runs_a_coroutine():
    try:
        coroutine().send(None)
    except StopIteration:
        pass


for f in (leaf, calls_twice, outer, catches, passes_it_on, goes_through, is_thrown_into, is_closed_early, uses_a_class, uses_builtins, a_builtin_raises, runs_a_coroutine):
    show(f.__name__, f)
show("recursive", recursive, 4)
show("one and other", one, 5)
show("a lambda", lambda: leaf())
show("without what calls what", outer, subcalls=False)
show("without what is not written in Python", uses_builtins, builtins=False)
show("without either", uses_builtins, subcalls=False, builtins=False)

print("---- begun and ended part of the way through")


def begins(p):
    p.enable()
    leaf()


def ends(p):
    leaf()
    p.disable()


p = _lsprof.Profiler(Clock(), 1.0)
begins(p)
leaf()
ends(p)
print(shown(p))
p = _lsprof.Profiler(Clock(), 1.0)
p.enable()
leaf()
p.disable()
first = shown(p)
p.enable()
leaf()
outer()
p.disable()
print(first, shown(p))
p.clear()
print("cleared", shown(p), p.getstats())


def clears(p):
    leaf()
    p.clear()
    leaf()


p = _lsprof.Profiler(Clock(), 1.0)
p.enable()
clears(p)
leaf()
p.disable()
print("cleared on the way", shown(p))

print("---- clocks")
for label, make in (("ints, and a unit", lambda: _lsprof.Profiler(Clock(), 0.5)), ("ints, and none", lambda: _lsprof.Profiler(Clock())), ("floats", lambda: _lsprof.Profiler(Clock(0.25))), ("floats, and a unit", lambda: _lsprof.Profiler(Clock(0.25), 2.0)), ("great ints", lambda: _lsprof.Profiler(Clock(2 ** 40), 1.0)),
                    ("a unit less than nothing", lambda: _lsprof.Profiler(Clock(), -1.0)), ("bools", lambda: _lsprof.Profiler(lambda: True, 1.0))):
    hooked = []
    sys.unraisablehook = lambda u: hooked.append((type(u.exc_value).__name__, str(u.exc_value), u.err_msg.split(" at 0x")[0], u.object))
    p = make()
    p.enable()
    outer()
    p.disable()
    print(label, [(e[0], e[3], e[4]) for e in shown(p)], hooked[:1], len(hooked))
for label, timer, unit in (("raises", lambda: 1 // 0, 1.0), ("a str", lambda: "1", 1.0), ("a str, and no unit", lambda: "1", 0.0), ("None", lambda: None, 0.0), ("too great", lambda: 2 ** 70, 1.0), ("too great, and no unit", lambda: 1e300, 0.0), ("not a number", lambda: float("nan"), 0.0), ("not to be called", 5, 0.0),
                           ("None for a clock", None, 0.0), ("takes an argument", lambda x: 1, 0.0)):
    hooked = []
    sys.unraisablehook = lambda u: hooked.append((type(u.exc_value).__name__, str(u.exc_value), u.err_msg.split(" at 0x")[0], u.object))
    p = _lsprof.Profiler(timer, unit)
    p.enable()
    leaf()
    p.disable()
    print(label, [(e[0], e[1], e[3], e[4]) for e in shown(p)], hooked[:1], len(hooked))
sys.unraisablehook = sys.__unraisablehook__
p = _lsprof.Profiler()
p.enable()
outer()
p.disable()
print("its own", [(name_of(e.code), e.callcount, type(e.totaltime).__name__, 0 <= e.inlinetime <= e.totaltime < 1) for e in sorted(p.getstats(), key=lambda e: name_of(e.code))])
outcomes = []


def meddles():
    if not outcomes:
        for f in (p.disable, p.clear):
            try:
                outcomes.append(f())
            except RuntimeError as e:
                outcomes.append(str(e))
        outcomes.append(len(p.getstats()))
        leaf()
    return 1


p = _lsprof.Profiler(meddles, 1.0)
p.enable()
leaf()
p.disable()
print("a clock that meddles", outcomes, shown(p))

print("---- what will not do")
for label, f in (("timeunit a str", lambda: _lsprof.Profiler(None, "1")), ("five", lambda: _lsprof.Profiler(None, 0.0, 1, 1, 1)), ("something else", lambda: _lsprof.Profiler(other=1)), ("by name", lambda: type(_lsprof.Profiler(timer=Clock(), timeunit=1, subcalls=0, builtins=[])).__name__),
                 ("will not say whether it is true", lambda: _lsprof.Profiler(subcalls=type("B", (), {"__bool__": lambda self: 1 // 0})())), ("enable(1, 2, 3)", lambda: _lsprof.Profiler().enable(1, 2, 3)), ("enable(other=1)", lambda: _lsprof.Profiler().enable(other=1)), ("disable(1)", lambda: _lsprof.Profiler().disable(1)),
                 ("clear(1)", lambda: _lsprof.Profiler().clear(1)), ("getstats(1)", lambda: _lsprof.Profiler().getstats(1)), ("disable() when it is not enabled", lambda: _lsprof.Profiler().disable()), ("getstats() of nothing", lambda: _lsprof.Profiler().getstats()),
                 ("Profiler.x = 1", lambda: setattr(_lsprof.Profiler, "x", 1)), ("an attribute", lambda: setattr(_lsprof.Profiler(), "x", 1)), ("_pystart_callback()", lambda: _lsprof.Profiler()._pystart_callback()), ("_ccall_callback(1, 2, 3)", lambda: _lsprof.Profiler()._ccall_callback(1, 2, 3)),
                 ("made and not initialized", lambda: [q := _lsprof.Profiler.__new__(_lsprof.Profiler), q.getstats(), q.disable(), q.clear()][1:]), ("initialized twice", lambda: [q := _lsprof.Profiler(Clock(), 1.0), q.__init__(), q.getstats()][1:])):
    t(label, f)
a, b = _lsprof.Profiler(), _lsprof.Profiler()
a.enable()
t("two at once", lambda: b.enable())
t("the same one twice", lambda: a.enable())
print(sys.monitoring.get_tool(sys.monitoring.PROFILER_ID))
a.disable()
print(sys.monitoring.get_tool(sys.monitoring.PROFILER_ID), sys.monitoring.get_events(sys.monitoring.PROFILER_ID) if False else None)
t("and then the other", lambda: [b.enable(), b.disable()])
sys.monitoring.use_tool_id(sys.monitoring.PROFILER_ID, "something else")
t("something else has its place", lambda: a.enable())
sys.monitoring.free_tool_id(sys.monitoring.PROFILER_ID)
print("told by hand", [q := _lsprof.Profiler(Clock(), 1.0), q._pystart_callback(leaf.__code__, 0), q._ccall_callback(leaf.__code__, 0, len, sys.monitoring.MISSING), q._creturn_callback(leaf.__code__, 0, len, sys.monitoring.MISSING), q._ccall_callback(leaf.__code__, 0, list.append, sys.monitoring.MISSING),
                       q._ccall_callback(leaf.__code__, 0, list.append, 5), q._ccall_callback(leaf.__code__, 0, list.append, []), q._creturn_callback(leaf.__code__, 0, [1].append, sys.monitoring.MISSING), q._ccall_callback(leaf.__code__, 0, leaf, 1), q._pyreturn_callback(leaf.__code__, 0, None),
                       q._pyreturn_callback(leaf.__code__, 0, None), q._pyreturn_callback(outer.__code__, 0, None), shown(q)][-1])

print("---- cProfile and pstats")


def report(profile, *restrictions, sort="stdname", how="print_stats"):
    stream = io.StringIO()
    # By name as well, or what is otherwise the same comes in the order in which the profiler has it.
    stats = pstats.Stats(profile, stream=stream).strip_dirs().sort_stats(sort, "stdname")
    getattr(stats, how)(*restrictions)
    return "\n".join("    " + line.rstrip() for line in stream.getvalue().splitlines() if line.strip())


print(cProfile.Profile.__mro__[1] is _lsprof.Profiler, cProfile.Profile.__module__, sorted(n for n in vars(cProfile.Profile) if not n.startswith("__")))
p = cProfile.Profile(Clock(), 0.001)
print(p.runcall(lambda a, b=2: (outer(), a + b)[1], 1, b=5))
print(report(p))
print(report(p, how="print_callers"))
print(report(p, how="print_callees"))
print(report(p, "leaf", sort="calls"))
p = cProfile.Profile(Clock(), 0.001)
p.runctx("recursive(n); uses_builtins()", globals(), {"n": 3})
print(report(p, sort="cumulative"))
with cProfile.Profile(Clock(), 0.001) as p:
    goes_through()
print(report(p))
p.create_stats()
print(sorted((k[2], v[:4], sorted((c[2], n) for c, n in v[4].items())) for k, v in p.stats.items()))
p = cProfile.Profile(Clock(), 0.001, subcalls=False, builtins=False)
p.enable(subcalls=False, builtins=False)
uses_a_class()
p.disable()
print(report(p))
profile = pstats.Stats(p).get_stats_profile()
print(profile.total_tt, sorted((name, f.ncalls, f.tottime, f.cumtime) for name, f in profile.func_profiles.items()))


class Derived(cProfile.Profile):
    def __init__(self, label):
        super().__init__(Clock(), 1.0)
        self.label = label


d = Derived("mine")
d.enable()
leaf()
d.disable()
print(d.label, shown(d))
