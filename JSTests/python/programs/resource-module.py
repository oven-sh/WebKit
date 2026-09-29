# The module resource. How much of anything a process has used is not the same from one to the next, so it is what kind of thing is given that is looked at, and everything that can go wrong, and limits that are set and read back.
import os
import pickle
import resource
import sys


def attempt(f, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)


def t(label, f):
    r = attempt(f)
    print(label, "=>", r if isinstance(r, str) else ascii(r))


class I:
    def __init__(self, v): self.v = v
    def __index__(self): return self.v


print("---- what there is")
names = sorted(n for n in vars(resource) if not n.startswith("__"))
t("the module", lambda: (resource.__name__, resource.__package__, resource.__loader__.__name__, resource.__doc__, names))
for name in names:
    x = getattr(resource, name)
    t(name, lambda: x if isinstance(x, int) else (type(x).__name__, x.__text_signature__, x.__doc__, x.__module__))
t("error", lambda: resource.error is OSError)
R = resource.struct_rusage
t("struct_rusage", lambda: (R.__name__, R.__module__, R.__qualname__, repr(R), [b.__name__ for b in R.__mro__], R.n_fields, R.n_sequence_fields, R.n_unnamed_fields, R.__match_args__, [(n, type(v).__name__, v.__doc__) for n, v in sorted(vars(R).items()) if n.startswith("ru_")], attempt(R), attempt(R, range(15)), attempt(R, range(17)), R(range(16)), R(range(16)).ru_nivcsw, pickle.loads(pickle.dumps(R(range(16)))) == R(range(16))))

print("---- getrusage")
t("what it gives", lambda: [(type(u).__name__, len(u), [type(v).__name__ for v in u], all(v >= 0 for v in u), u.ru_utime == u[0], u.ru_maxrss > 0 or who != resource.RUSAGE_SELF) for who in (resource.RUSAGE_SELF, resource.RUSAGE_CHILDREN) for u in [resource.getrusage(who)]])
t("it goes up", lambda: (lambda a: (sum(i * i for i in range(300000)), resource.getrusage(resource.RUSAGE_SELF).ru_utime >= a)[1])(resource.getrusage(resource.RUSAGE_SELF).ru_utime))
t("what os.wait4() gives is one", lambda: type(os.wait4(os.posix_spawn("/usr/bin/true", ["true"], {}), 0)[2]) is R)
t("what it is given", lambda: [attempt(resource.getrusage, *a, **k) for a, k in (((), {}), ((5,), {}), ((-2,), {}), ((100,), {}), ((2 ** 31,), {}), ((-2 ** 31 - 1,), {}), ((2 ** 70,), {}), (("a",), {}), ((None,), {}), ((1.5,), {}), ((0, 1), {}), ((), {"who": 0}))] + [type(resource.getrusage(v)).__name__ for v in (I(0), False)])

print("---- getrlimit")
LIMITS = [n for n in names if n.startswith("RLIMIT_")]
t("what it gives", lambda: [(n, type(l).__name__, len(l), [type(v).__name__ for v in l], all(v >= 0 or v == resource.RLIM_INFINITY for v in l), l[0] <= l[1] or resource.RLIM_INFINITY in l) for n in LIMITS for l in [resource.getrlimit(getattr(resource, n))]])
t("what it is given", lambda: [attempt(resource.getrlimit, *a, **k) for a, k in (((), {}), ((-1,), {}), ((9,), {}), ((100,), {}), ((2 ** 31,), {}), ((2 ** 70,), {}), (("a",), {}), ((None,), {}), ((1.5,), {}), ((0, 1), {}), ((), {"resource": 0}))] + [type(resource.getrlimit(v)).__name__ for v in (I(0), True)])

print("---- setrlimit")
CORE = resource.RLIMIT_CORE
was = resource.getrlimit(CORE)
t("set and read back", lambda: [(resource.setrlimit(CORE, (v, was[1])), resource.getrlimit(CORE)[0] == v) for v in (0, 1, 4096, 0)])
t("by what", lambda: [(attempt(resource.setrlimit, CORE, l), resource.getrlimit(CORE)[0]) for l in ([1, was[1]], iter((2, was[1])), (I(3), I(was[1])), (True, was[1]), {4: 0, was[1]: 0} if was[1] != 4 else None, (x for x in (5, was[1])))])
t("what will not do", lambda: [attempt(resource.setrlimit, CORE, l) for l in ((), (1,), (1, 2, 3), 5, None, "ab", "a", (1.5, 2), ("a", "b"), (None, None), (-1, -1), (-2, 0), (0, -2), (-2 ** 63, 0), (-2 ** 70, 0), (2 ** 64, 0), (0, 2 ** 64), (2 ** 70, 2 ** 70), (I("a"), 0), (I(-5), 0), (2, 1))])
t("as much as can be written", lambda: [attempt(resource.setrlimit, CORE, l) for l in ((0, 2 ** 64 - 1), (0, 2 ** 63))] if was[1] != resource.RLIM_INFINITY else "no limit to go over")
t("what it is given", lambda: [attempt(resource.setrlimit, *a, **k) for a, k in (((), {}), ((CORE,), {}), ((CORE, (0, 0), 1), {}), ((-1, (0, 0)), {}), ((9, (0, 0)), {}), ((100, "nonsense"), {}), ((2 ** 70, (0, 0)), {}), (("a", (0, 0)), {}), ((), {"resource": 0, "limits": (0, 0)}))])
resource.setrlimit(CORE, (0, was[1]))
seen = []
sys.addaudithook(lambda event, args: seen.append((event, args)) if event.startswith("resource.") else None)
t("what is told of it", lambda: (resource.setrlimit(CORE, [0, was[1]]) or seen[-1][0], seen[-1][1][0] == CORE, type(seen[-1][1][1]).__name__, attempt(resource.setrlimit, CORE, "nonsense"), seen[-1][1][1], attempt(resource.setrlimit, -1, ()), len(seen)))
resource.setrlimit(CORE, was)

print("---- getpagesize")
t("getpagesize", lambda: (type(resource.getpagesize()).__name__, resource.getpagesize() == os.sysconf("SC_PAGESIZE"), resource.getpagesize() & (resource.getpagesize() - 1), attempt(resource.getpagesize, 1), attempt(resource.getpagesize, x=1)))
