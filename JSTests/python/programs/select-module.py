# The module select: select(), poll, and then kevent and kqueue or else epoll, whichever the system has. And selectors, which is written in Python over it.
import os
import select
import selectors
import signal
import sys
import time
import warnings


def show(e):
    return type(e).__name__ + ": " + str(e)


def attempt(f, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return show(e)


def t(label, f):
    r = attempt(f)
    print(label, "=>", r if isinstance(r, str) else ascii(r))
    sys.stdout.flush()


signal.alarm(0)
print("---- what there is")
t("the module", lambda: (select.__name__, select.__package__, select.__loader__.__name__, select.__doc__, sorted(n for n in vars(select) if not n.startswith("__"))))
for name in sorted(n for n in vars(select) if not n.startswith("__")):
    x = getattr(select, name)
    if isinstance(x, int):
        t(name, lambda: x)
    elif not isinstance(x, type):
        t(name, lambda: (type(x).__name__, x.__text_signature__, x.__doc__, x.__module__))
t("error", lambda: select.error is OSError)
for c in [type(select.poll())] + [getattr(select, n) for n in ("kevent", "kqueue", "epoll") if hasattr(select, n)]:
    t(c.__name__, lambda: (c.__module__, c.__qualname__, [b.__name__ for b in c.__mro__], sorted(vars(c)), c.__doc__, c.__text_signature__, c.__basicsize__, c.__flags__ & 0x7FFF, repr(c), attempt(type, "X", (c,), {}), attempt(setattr, c, "x", 1)))
    for name in sorted(vars(c)):
        x = vars(c)[name]
        t("%s.%s" % (c.__name__, name), lambda: (type(x).__name__, getattr(x, "__text_signature__", None), getattr(x, "__doc__", None) if not isinstance(x, str) else None))
t("poll is not made by calling its class", lambda: attempt(type(select.poll())))


class File:
    def __init__(self, fd): self.fd = fd
    def fileno(self): return self.fd
    def __repr__(self): return "File"


class Index:
    def __init__(self, v): self.v = v
    def __index__(self): return self.v
    def __repr__(self): return "Index"


class Nothing:
    def __repr__(self): return "Nothing"


r, w = os.pipe()
r2, w2 = os.pipe()
NAMES = {r: "r", w: "w", r2: "r2", w2: "w2"}


def named(x):
    if isinstance(x, (list, tuple)):
        return type(x)(named(i) for i in x)
    return NAMES.get(x, x) if type(x) is int else x


# What poll() returns: it is only the first of each pair that is a descriptor.
def polled(pairs):
    return sorted((NAMES[f], e) for f, e in pairs)


print("---- select()")
t("nothing to read", lambda: named(select.select([r], [], [], 0)))
t("room to write", lambda: named(select.select([r], [w], [r, w], 0)))
os.write(w, b"x")
t("something to read", lambda: named(select.select([r, r2], [w, w2], [], 0)))
t("with no timeout", lambda: named(select.select([r], [], [])))
t("None is no timeout", lambda: named(select.select([r], [], [], None)))
t("what has fileno() comes back as it is", lambda: [(named(a), b, c) for f in [File(r)] for a, b, c in [select.select([f, r], (), set(), 0)]] and [x is f for f in [File(r)] for x in select.select([f], [], [], 0)[0]])
t("anything that can be gone through", lambda: named([select.select(x, [], [], 0)[0] for x in ((r,), {r}, iter([r]), {r: 1}, (i for i in [r]), range(r, r + 1))]))
t("the same one twice", lambda: named(select.select([r, r], [], [], 0)))
t("what they are", lambda: [type(x).__name__ for x in (select.select([], [], [], 0),) + select.select([], [], [], 0)])
t("it waits", lambda: [(select.select([r2], [], [], 0.05), 0.04 < time.monotonic() - start < 2) for start in [time.monotonic()]])
for a in ((), ([],), ([], []), ([], [], [], 0, 1), (5, [], []), ([], 5, []), ([], [], 5), (None, [], []), ([], [], [], "a"), ([], [], [], []), ([], [], [], -1), ([], [], [], -0.001), ([], [], [], -1e-10), ([], [], [], float("nan")), ([], [], [], float("inf")), ([], [], [], 1e300), ([], [], [], 2 ** 64), ([], [], [], 1j), (["a"], [], []), ([None], [], []), ([1.5], [], []), ([-1], [], []), ([-5], [], []), ([2 ** 31], [], []), ([2 ** 100], [], []), ([1024], [], []), ([1023], [], [], 0), ([File("a")], [], []), ([File(-1)], [], []), ([File(None)], [], []), ([File(2 ** 31)], [], []), ([Nothing()], [], []), ([Index(0)], [], [], 0), ([9999], [], [])):
    t("select%s" % ascii(a), lambda: select.select(*a))
t("by name", lambda: attempt(lambda: select.select(rlist=[], wlist=[], xlist=[])))
t("one that is closed", lambda: [attempt(select.select, [x], [], [], 0) for x, y in [os.pipe()] for _ in [(os.close(x), os.close(y))]][0].replace("[Errno 9] ", ""))
t("too many", lambda: attempt(select.select, [r] * 1025, [], [], 0))
t("as many as there can be", lambda: len(select.select([r] * 1024, [], [], 0)[0]))
t("a list that changes while it is gone through", lambda: [named(select.select(l, [], [], 0)[0]) for l in [[]] for _ in [l.extend([type("F", (), {"fileno": lambda self: (l.clear(), r)[1]})(), r, r])]][0][1:])

print("---- poll")
p = select.poll()
t("with nothing", lambda: (p.poll(0), p.poll(0.0), p.poll(1), type(p.poll(0)).__name__))
t("register", lambda: (p.register(r), polled(p.poll(0)), p.register(w, select.POLLOUT), polled(p.poll(0)), p.register(File(r2), select.POLLIN), polled(p.poll(0))))
t("again is to say otherwise", lambda: (p.register(r, select.POLLOUT), polled(p.poll(0)), p.register(r, 0), polled(p.poll(0)), p.register(r, select.POLLIN | select.POLLPRI), polled(p.poll(0))))
t("modify", lambda: (p.modify(r, select.POLLOUT), polled(p.poll(0)), p.modify(File(r), select.POLLIN), polled(p.poll(0)), attempt(p.modify, w2, select.POLLIN), attempt(p.modify, 9999, 1)))
t("unregister", lambda: (p.unregister(w), polled(p.poll(0)), attempt(p.unregister, w).replace(str(w), "w"), p.unregister(File(r2)), attempt(p.unregister, 9999), polled(p.poll(0))))
t("it waits", lambda: [(p.modify(r, 0), p.poll(50), 0.04 < time.monotonic() - start < 2, p.modify(r, select.POLLIN)) for start in [time.monotonic()]])
t("less than nothing is for ever, and there is something", lambda: [polled(x) for x in (p.poll(-1), p.poll(-1000), p.poll(-0.5), p.poll(None), p.poll())])
for method, cases in (("register", ((), ("a",), (None,), (1.5,), (-1,), (2 ** 31,), (r, "a"), (r, None), (r, 1.5), (r, -1), (r, 65536), (r, 65535), (r, 2 ** 100), (r, -2 ** 100), (r, 1, 2), (File("a"),), (File(-1),), (r, True), (r, Index(1)), (object(),))), ("modify", ((), (r,), (r, "a"), (r, -1), (r, 65536), ("a", 1), (r, 1, 2), (-1, 1))), ("unregister", ((), ("a",), (-1,), (r, 1), (None,), (1.5,))), ("poll", (("a",), ([],), (1, 2), (float("nan"),), (float("inf"),), (1e300,), (2 ** 31,), (2 ** 31 - 1 + 0.5,), (-2 ** 31 - 1,), (2 ** 64,), (1j,), (b"1",)))):
    t(method, lambda: [attempt(getattr(select.poll(), method), *a) for a in cases] + [attempt(lambda: getattr(select.poll(), method)(x=1))])
t("one that is closed", lambda: [(q.register(x), os.close(x), os.close(y), [e for _, e in q.poll(0)] == [select.POLLNVAL]) for q in [select.poll()] for x, y in [os.pipe()]])
t("the other end has gone", lambda: [(q.register(x, select.POLLIN), os.close(y), [e for _, e in q.poll(0)] == [select.POLLHUP], os.close(x)) for q in [select.poll()] for x, y in [os.pipe()]])
t("in the order that they were registered", lambda: [(q.register(w2, select.POLLOUT), q.register(r, select.POLLIN), q.register(w, select.POLLOUT), named([f for f, _ in q.poll(0)]), q.unregister(r), q.register(r, select.POLLIN), named([f for f, _ in q.poll(0)])) for q in [select.poll()]])
t("what it has", lambda: (attempt(setattr, p, "x", 1), attempt(getattr, p, "__dict__"), repr(p).split(" at ")[0], attempt(hash, p) is not None, p == p, p != select.poll(), attempt(lambda: p < p), attempt(iter, p), attempt(len, p), attempt(bool, p)))

if hasattr(select, "kqueue"):
    print("---- kevent")
    K = select.kevent
    FIELDS = ("ident", "filter", "flags", "fflags", "data", "udata")


    def fields(e):
        return tuple(getattr(e, n) for n in FIELDS)


    t("as it is by default", lambda: (fields(K(5)), repr(K(5)), fields(K.__new__(K)), repr(K.__new__(K))))
    t("all given", lambda: (fields(K(1, 2, 3, 4, 5, 6)), repr(K(1, 2, 3, 4, 5, 6)), fields(K(ident=1, filter=2, flags=3, fflags=4, data=5, udata=6)), fields(K(1, udata=6, flags=3)), fields(K(File(7))), fields(K(Index(8))), fields(K(True))))
    t("shown", lambda: [repr(K(*a)) for a in ((0,), (2 ** 64 - 1,), (1, -1), (1, -32768), (1, 32767), (1, 0, 65535), (1, 0, 0, 2 ** 32 - 1), (1, 0, 0, 0, -1), (1, 0, 0, 0, 2 ** 63 - 1), (1, 0, 0, 0, -2 ** 63), (1, 0, 0, 0, 0, 2 ** 64 - 1), (1, 0, 0, 0, 0, 255))])
    t("what is cut down without a word", lambda: [fields(K(*a)) for a in ((1, 0, 65536), (1, 0, 65537), (1, 0, -1), (1, 0, 2 ** 100 + 5), (1, 0, 0, 2 ** 32), (1, 0, 0, -1), (1, 0, 0, 2 ** 100 + 5), (1, 0, 0, 0, 0, 2 ** 64), (1, 0, 0, 0, 0, -1), (1, 0, 0, 0, 0, 2 ** 100 + 5))])
    for a, k in (((), {}), (("a",), {}), ((None,), {}), ((1.5,), {}), ((-1,), {}), ((-2 ** 100,), {}), ((2 ** 64,), {}), ((2 ** 100,), {}), ((File(-1),), {}), ((File("a"),), {}), ((object(),), {}), ((1, "a"), {}), ((1, None), {}), ((1, 1.5), {}), ((1, 32768), {}), ((1, -32769), {}), ((1, 2 ** 100), {}), ((1, 0, "a"), {}), ((1, 0, 1.5), {}), ((1, 0, None), {}), ((1, 0, 0, "a"), {}), ((1, 0, 0, 1.5), {}), ((1, 0, 0, 0, "a"), {}), ((1, 0, 0, 0, 1.5), {}), ((1, 0, 0, 0, 2 ** 63), {}), ((1, 0, 0, 0, -2 ** 63 - 1), {}), ((1, 0, 0, 0, 0, "a"), {}), ((1, 0, 0, 0, 0, 1.5), {}), ((1, 0, 0, 0, 0, None), {}), ((1, 2, 3, 4, 5, 6, 7), {}), ((1,), {"ident": 2}), ((1,), {"other": 2}), ((), {"filter": 1}), ((1, Index(2), Index(3), Index(4), Index(5), Index(6)), {})):
        t("kevent(%s)" % ", ".join([ascii(x) if not isinstance(x, (File, Index)) and type(x) is not object else type(x).__name__ for x in a] + ["%s=%r" % i for i in k.items()]), lambda: fields(K(*a, **k)))
    warnings.simplefilter("error")
    for name in FIELDS:
        for value in (0, 1, -1, 255, 32767, 32768, -32768, -32769, 65535, 65536, 2 ** 31, 2 ** 32 - 1, 2 ** 32, -2 ** 31 - 1, 2 ** 63 - 1, 2 ** 63, -2 ** 63, -2 ** 63 - 1, 2 ** 64 - 1, 2 ** 64, 2 ** 100, -2 ** 100, "a", None, 1.5, True, Index(7), Index(-7)):
            t("%s = %s" % (name, ascii(value) if not isinstance(value, Index) else "Index(%d)" % value.v), lambda: [(attempt(setattr, e, name, value), getattr(e, name)) for e in [K(1, 2, 3, 4, 5, 6)]][0])
        t("del %s" % name, lambda: attempt(delattr, K(1), name))
    warnings.simplefilter("ignore")
    t("with the warning let pass", lambda: [[(setattr(e, name, value), getattr(e, name))[1] for e in [K(1)]][0] for name, value in (("filter", 32768), ("filter", 65537), ("flags", 65536), ("flags", -1), ("fflags", 2 ** 32 + 5), ("fflags", -1), ("ident", -1), ("udata", -5), ("ident", -2 ** 63))])
    warnings.resetwarnings()
    ORDER = [K(1), K(2), K(1, 1), K(1, -2), K(1, -1, 2), K(1, -1, 1, 1), K(1, -1, 1, 0, 1), K(1, -1, 1, 0, -1), K(1, -1, 1, 0, 0, 1), K(1, -1, 1, 0, 0, 2 ** 64 - 1), K(1)]
    t("compared", lambda: [[("<" if a < b else "") + ("=" if a == b else "") + (">" if a > b else "") + ("!" if a != b else "") + ("l" if a <= b else "") + ("g" if a >= b else "") for b in ORDER] for a in ORDER])
    t("with something else", lambda: (K(1) == 1, K(1) != 1, attempt(lambda: K(1) < 1), attempt(lambda: 1 < K(1)), K(1) == None, K(1).__eq__(1), K(1).__lt__("a"), attempt(hash, K(1))))
    t("made again", lambda: [(e.__init__(9), fields(e), attempt(e.__init__, "a"), fields(e), attempt(e.__init__, 1, 2, 3, "a"), fields(e)) for e in [K(1, 2, 3, 4, 5, 6)]])
    t("what it has", lambda: (attempt(setattr, K(1), "x", 1), attempt(getattr, K(1), "__dict__"), attempt(K.__new__, int), attempt(K.__new__), fields(K.__new__(K, "anything", at="all"))))

    print("---- kqueue")
    q = select.kqueue()
    t("as it is", lambda: (q.closed, type(q.fileno()).__name__, q.fileno() > 2, os.get_inheritable(q.fileno()), repr(q).split(" at ")[0]))
    t("nothing yet", lambda: (q.control(None, 0), q.control(None, 1, 0), q.control([], 5, 0), q.control((), 5, 0.0), type(q.control(None, 0)).__name__))
    t("watching", lambda: (q.control([K(r, select.KQ_FILTER_READ, select.KQ_EV_ADD)], 0), [(NAMES[e.ident], e.filter, e.flags, e.fflags, e.data, e.udata, type(e).__name__) for e in q.control(None, 5, 0)]))
    t("more than one", lambda: (q.control([K(w, select.KQ_FILTER_WRITE, select.KQ_EV_ADD, udata=77), K(r2, select.KQ_FILTER_READ, select.KQ_EV_ADD)], 0), sorted((NAMES[e.ident], e.filter, e.udata) for e in q.control(None, 5, 0)), len(q.control(None, 1, 0)), len(q.control(None, 0, 0))))
    t("both at once", lambda: sorted((NAMES[e.ident], e.filter) for e in q.control([K(w2, select.KQ_FILTER_WRITE, select.KQ_EV_ADD)], 5, 0)))
    t("no longer", lambda: (q.control([K(w, select.KQ_FILTER_WRITE, select.KQ_EV_DELETE), K(w2, select.KQ_FILTER_WRITE, select.KQ_EV_DELETE), K(r, select.KQ_FILTER_READ, select.KQ_EV_DELETE)], 0), q.control(None, 5, 0)))
    t("what is not there", lambda: (attempt(q.control, [K(w, select.KQ_FILTER_WRITE, select.KQ_EV_DELETE)], 0), [(NAMES[e.ident], e.flags & select.KQ_EV_ERROR != 0, e.data) for e in q.control([K(w, select.KQ_FILTER_WRITE, select.KQ_EV_DELETE)], 5, 0)]))
    t("it waits", lambda: [(q.control(None, 1, 0.05), 0.04 < time.monotonic() - start < 2) for start in [time.monotonic()]])
    t("a timer", lambda: (q.control([K(1, select.KQ_FILTER_TIMER, select.KQ_EV_ADD | select.KQ_EV_ONESHOT, 0, 10)], 0), [(e.ident, e.filter, e.data) for e in q.control(None, 5, 2)], q.control(None, 5, 0)))
    t("anything that can be gone through", lambda: [q.control(x, 0) for x in (iter([]), (i for i in ()), {}, set(), "", range(0))])
    for a in ((), (None,), (None, 1, 0, 1), (5, 0), ("a", 0), ([1], 0), ([None], 0), ([K(r), 1], 0), (None, "a"), (None, None), (None, 1.5), (None, -1), (None, -5), (None, 2 ** 31), (None, 0, "a"), (None, 0, []), (None, 0, -1), (None, 0, -0.001), (None, 0, float("nan")), (None, 0, float("inf")), (None, 0, 1e300), (None, 0, 2 ** 64), (None, 0, 1j), (object(), 0)):
        t("control%s" % ascii(tuple("kevent" if isinstance(x, list) and x and isinstance(x[0], K) else "object" if type(x) is object else x for x in a)), lambda: q.control(*a))
    t("by name", lambda: attempt(lambda: q.control(changelist=None, maxevents=0)))
    # Each is given a descriptor of its own, since it closes what it has when it goes, which is at another time here.
    t("fromfd", lambda: [(type(o).__name__, o.fileno() == d, o.closed, o.control(None, 0), o.close(), attempt(os.fstat, d)) for d in [os.dup(q.fileno())] for o in [select.kqueue.fromfd(d)]] + [attempt(select.kqueue.fromfd, *a) for a in ((), ("a",), (-5,), (1.5,), (None,), (2 ** 31,), (1, 2))] + [(type(o).__name__, o.fileno() > 2, o.close()) for o in [select.kqueue.fromfd(-1)]] + [(attempt(o.control, None, 1, 0), o.close()) for o in [select.kqueue.fromfd(os.dup(r))]])
    t("how it is made", lambda: [attempt(select.kqueue, *a, **k) for a, k in (((1,), {}), ((), {"x": 1}))])
    t("close", lambda: (q.close(), q.closed, q.close(), attempt(q.fileno), attempt(q.control, None, 0), attempt(q.control, "wrong", "too"), attempt(q.control, None, -1)))
    t("what it has", lambda: (attempt(setattr, q, "x", 1), attempt(setattr, q, "closed", 1), attempt(q.close, 1), attempt(q.fileno, 1), attempt(iter, q), attempt(q.__enter__) if hasattr(q, "__enter__") else "no with"))

if hasattr(select, "epoll"):
    print("---- epoll")
    IN, OUT, PRI, ERR, HUP, ET, ONESHOT = select.EPOLLIN, select.EPOLLOUT, select.EPOLLPRI, select.EPOLLERR, select.EPOLLHUP, select.EPOLLET, select.EPOLLONESHOT
    e = select.epoll()
    t("as it is", lambda: (e.closed, type(e.fileno()).__name__, e.fileno() > 2, os.get_inheritable(e.fileno()), repr(e).split(" at ")[0]))
    t("nothing yet", lambda: (e.poll(0), e.poll(0.0), e.poll(0, 1), e.poll(0, -1), e.poll(timeout=0, maxevents=5), type(e.poll(0)).__name__))
    # There is something to read in r already, and room to write in w and w2.
    t("watching", lambda: (e.register(r, IN), polled(e.poll(0)), [(type(f).__name__, type(m).__name__, type(p).__name__) for p in e.poll(0) for f, m in [p]]))
    t("more than one", lambda: (e.register(w, OUT), e.register(r2, IN), e.register(w2), polled(e.poll(0))))
    t("no more than are asked for", lambda: (len(e.poll(0, 1)), len(e.poll(0, 2)), len(e.poll(0, 100))))
    t("twice", lambda: attempt(e.register, r, IN))
    t("otherwise", lambda: (e.modify(w, 0), e.modify(w2, IN), polled(e.poll(0)), e.modify(w, OUT), polled(e.poll(0))))
    t("no longer", lambda: (e.unregister(w), e.unregister(w2), e.unregister(r), e.poll(0)))
    t("what is not there", lambda: (attempt(e.unregister, w), attempt(e.modify, w, OUT), attempt(e.register, 9999, IN), attempt(e.unregister, 9999), attempt(e.register, -1), attempt(e.register, e.fileno(), IN)))
    t("what cannot be watched", lambda: [(attempt(e.register, f, IN), f.close()) for f in [open(__file__)]])
    t("it waits", lambda: [(e.poll(0.05), 0.04 < time.monotonic() - start < 2) for start in [time.monotonic()]])
    t("for at least as long as it was asked to", lambda: [(e.poll(0.0001), time.monotonic() - start >= 0.0001) for start in [time.monotonic()]])
    t("something with fileno()", lambda: (e.register(File(r), IN), polled(e.poll(0)), e.modify(File(r), IN | PRI), e.unregister(File(r))))
    t("by name", lambda: (e.register(fd=r, eventmask=IN), e.modify(eventmask=IN, fd=r), polled(e.poll(maxevents=3, timeout=0)), e.unregister(fd=r)))
    t("told once of what has not changed", lambda: (e.register(r, IN | ET), polled(e.poll(0)), e.poll(0), os.write(w, b"more"), polled(e.poll(0)), e.poll(0), e.unregister(r)))
    t("told once and no more", lambda: (e.register(r, IN | ONESHOT), polled(e.poll(0)), e.poll(0), os.write(w, b"more"), e.poll(0), e.modify(r, IN | ONESHOT), polled(e.poll(0)), e.unregister(r)))
    t("the other end is closed", lambda: [(e.register(a, IN), e.register(b, OUT), os.close(a), [m for _, m in e.poll(0)], e.unregister(b), os.close(b)) for a, b in [os.pipe()]] + [(e.register(a, IN), os.close(b), [m for _, m in e.poll(0)], e.unregister(a), os.close(a)) for a, b in [os.pipe()]])
    t("one inside another", lambda: [(o.register(e.fileno(), IN), o.poll(0), e.register(r, IN), [m for _, m in o.poll(0)], e.unregister(r), o.poll(0), o.close()) for o in [select.epoll()]])
    t("a mask is the low 32 bits", lambda: [(attempt(e.register, r, m), polled(e.poll(0)), attempt(e.unregister, r)) for m in (IN + 2 ** 32, IN - 2 ** 32, Index(IN), True)])
    for a in ((), (r, IN, 1), ("a",), (None,), (1.5,), (Nothing(),), (File("a"),), (File(-1),), (2 ** 31,), (-2 ** 31 - 1,), (r, "a"), (r, None), (r, 1.5), (r, -1), (r, 2 ** 64)):
        t("register%s" % ascii(named(a)), lambda: (attempt(e.register, *a), attempt(e.unregister, r) and None))
    for a in ((), (r,), (r, IN, 1), ("a", IN), (r, "a"), (r, None)):
        t("modify%s" % ascii(named(a)), lambda: e.modify(*a))
    for a in ((), (r, 1), ("a",), (None,)):
        t("unregister%s" % ascii(named(a)), lambda: e.unregister(*a))
    for a in ((None,), (-1, 1), (-5, 1), (-0.5, 1), (0, 0), (0, -2), (0, -1), (0, 2 ** 31), (0, "a"), (0, None), (0, 1.5), (0, Index(2)), ("a",), ([],), (Index(0),), (True,), (float("nan"),), (float("inf"),), (1e300,), (2 ** 31,), (2147483.647,), (2147483.648,), (-1e300, 1), (0, 1, 2)):
        # What would wait for as long as it takes has something to be told of.
        t("poll%s" % ascii(a), lambda: (e.register(r, IN), attempt(lambda: polled(e.poll(*a))), e.unregister(r))[1])
    t("wrong names", lambda: [attempt(lambda: f(**k)) for f, k in ((e.poll, {"x": 1}), (e.register, {"fd": r, "x": 1}), (e.register, {"eventmask": IN}), (e.unregister, {"x": 1}), (e.close, {"x": 1}))])
    # Each is given a descriptor of its own, since it closes what it has when it goes, which is at another time here.
    t("fromfd", lambda: [(type(o).__name__, o.fileno() == d, o.closed, o.poll(0), o.close(), attempt(os.fstat, d)) for d in [os.dup(e.fileno())] for o in [select.epoll.fromfd(d)]] + [attempt(select.epoll.fromfd, *a) for a in ((), ("a",), (1, 2), (None,), (1.5,), (2 ** 31,))] + [attempt(lambda: select.epoll.fromfd(fd=1))])
    t("fromfd, of what is no epoll", lambda: [(o.fileno() == d, attempt(o.poll, 0), attempt(o.register, r, IN), o.close()) for d in [os.dup(r)] for o in [select.epoll.fromfd(d)]])
    t("fromfd(-1) makes one", lambda: [(o.fileno() > 2, o.poll(0), o.close()) for o in [select.epoll.fromfd(-1)]])
    t("how it is made", lambda: [(lambda o: (o.close(), type(o).__name__)[1] if isinstance(o, select.epoll) else o)(attempt(select.epoll, *a, **k)) for a, k in (((), {}), ((-1,), {}), ((1,), {}), ((100, 0), {}), ((-1, select.EPOLL_CLOEXEC), {}), ((), {"sizehint": 5, "flags": 0}), ((0,), {}), ((-2,), {}), ((-1, 1), {}), ((-1, -1), {}), (("a",), {}), ((1.5,), {}), ((None,), {}), ((1, "a"), {}), ((2 ** 31,), {}), ((1, 2, 3), {}), ((), {"x": 1}), ((Index(3),), {}))])
    t("with", lambda: [(o.__enter__() is o, o.closed, o.__exit__(None, None, None), o.closed, attempt(o.__enter__), o.__exit__(), o.__exit__(1), attempt(o.__exit__, 1, 2, 3, 4), attempt(lambda: o.__exit__(exc_type=None))) for o in [select.epoll()]])

    t("it is not derived from", lambda: attempt(type, "X", (select.epoll,), {}))
    t("close", lambda: (e.close(), e.closed, e.close(), attempt(e.fileno), attempt(e.poll, 0), attempt(e.register, r, IN), attempt(e.modify, r, IN), attempt(e.unregister, r), attempt(e.__enter__)))
    t("what is wrong with the arguments is said first", lambda: (attempt(e.poll, 0, "a"), attempt(e.poll, "a"), attempt(e.poll, 0, 0), attempt(e.register, "a"), attempt(e.register, r, "a"), attempt(e.unregister, None)))
    t("what it has", lambda: (attempt(setattr, e, "x", 1), attempt(setattr, e, "closed", 1), attempt(delattr, e, "closed"), attempt(e.close, 1), attempt(e.fileno, 1), attempt(iter, e), attempt(hash, e) == hash(e), e == e, attempt(bool, e)))

print("---- signals")
seen = []
signal.signal(signal.SIGALRM, lambda n, f: seen.append(n))
for name, wait in (("select", lambda s: select.select([r2], [], [], s)), ("poll", lambda s: [x.poll(s * 1000) for x in [select.poll()] for _ in [x.register(r2, select.POLLIN)]][0]), ("kqueue", lambda s: [(x.control([K(r2)], 1, s), x.close())[0] for x in [select.kqueue()]][0]), ("epoll", lambda s: [(x.register(r2, select.EPOLLIN), x.poll(s), x.close())[1] for x in [select.epoll()]][0])):
    if name in ("kqueue", "epoll") and not hasattr(select, name):
        continue
    t("%s goes on afterwards" % name, lambda: [(signal.setitimer(signal.ITIMER_REAL, 0.01), wait(0.1), time.monotonic() - start >= 0.09, seen[:], seen.clear()) for start in [time.monotonic()]])


def raises(n, f):
    raise ValueError("from the handler")


signal.signal(signal.SIGALRM, raises)
for name, wait in (("select", lambda s: select.select([r2], [], [], s)), ("poll", lambda s: [x.poll(s * 1000) for x in [select.poll()] for _ in [x.register(r2, select.POLLIN)]][0]), ("kqueue", lambda s: [(attempt(x.control, [K(r2)], 1, s), x.close())[0] for x in [select.kqueue()]][0]), ("epoll", lambda s: [(x.register(r2, select.EPOLLIN), attempt(x.poll, s), x.close())[1] for x in [select.epoll()]][0])):
    if name in ("kqueue", "epoll") and not hasattr(select, name):
        continue
    t("%s, unless the function raises" % name, lambda: [(signal.setitimer(signal.ITIMER_REAL, 0.01), attempt(wait, 5), time.monotonic() - start < 4) for start in [time.monotonic()]])
t("and it can be used again", lambda: [(x.register(r2, select.POLLIN), signal.setitimer(signal.ITIMER_REAL, 0.01), attempt(x.poll, 5000), x.poll(0)) for x in [select.poll()]])
signal.signal(signal.SIGALRM, signal.SIG_DFL)

print("---- selectors")
t("which there are", lambda: (selectors.DefaultSelector.__name__, [n for n in ("SelectSelector", "PollSelector", "EpollSelector", "DevpollSelector", "KqueueSelector") if hasattr(selectors, n)]))
for c in [selectors.SelectSelector, selectors.PollSelector] + [getattr(selectors, n) for n in ("KqueueSelector", "EpollSelector") if hasattr(selectors, n)]:
    with c() as s:
        t(c.__name__, lambda: (s.register(r, selectors.EVENT_READ, "reading")[2:], s.register(w2, selectors.EVENT_WRITE, "writing")[2:], sorted((k.data, e) for k, e in s.select(0)), s.unregister(r).data, [(k.data, e) for k, e in s.select(0)], s.modify(w2, selectors.EVENT_READ)[2:], s.select(0), len(s.get_map()), attempt(s.unregister, r).replace(str(r), "r")))
for fd in (r, w, r2, w2):
    os.close(fd)
