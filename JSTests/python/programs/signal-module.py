# The module _signal, and signal, which is written in Python over it. When a signal comes cannot be compared, so each is waited for or is one that the process sends itself.
import _signal
import _thread
import os
import signal
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
    print(label, "=>", r if isinstance(r, str) else ascii(r))
    sys.stdout.flush()


class Relative:
    here = os.path.dirname(os.path.abspath(__file__)) + os.sep
    def write(self, text): return sys.stdout.write(text.replace(self.here, ""))
    def flush(self): sys.stdout.flush()


sys.stderr = Relative()

# Whatever started this may have left it an alarm, or signals that are being kept back. A shell that starts something and does not wait for it has it ignore SIGINT and SIGQUIT, and Python leaves what is ignored alone.
_signal.alarm(0)
_signal.pthread_sigmask(_signal.SIG_SETMASK, [])
_signal.signal(_signal.SIGINT, _signal.default_int_handler)

print("---- what there is")
t("the module", lambda: (_signal.__name__, _signal.__package__, _signal.__loader__.__name__, _signal.__doc__, sorted(n for n in vars(_signal) if not n.startswith("__"))))
for name in sorted(n for n in vars(_signal) if not n.startswith("__")):
    x = getattr(_signal, name)
    if isinstance(x, int):
        t(name, lambda: (x, type(x).__name__))
    elif not isinstance(x, type):
        t(name, lambda: (type(x).__name__, x.__text_signature__, x.__doc__, x.__module__))
E = _signal.ItimerError
t("ItimerError", lambda: (E.__name__, E.__module__, E.__qualname__, [b.__name__ for b in E.__mro__], sorted(vars(E)), E.__doc__, repr(E), repr(E(1, "x")), E(2, "y").errno, type(E(2, "no such")).__name__, E.__flags__ & (1 << 9) != 0, signal.ItimerError is E))
# Not those that say that something has gone badly wrong, which whatever the engine is part of may have something of its own to do about.
t("as it is at first", lambda: {n: (h if isinstance(h, int) or h is None else h.__name__) for n in ("SIGKILL", "SIGPIPE", "SIGALRM", "SIGTERM", "SIGSTOP", "SIGTSTP", "SIGCONT", "SIGCHLD", "SIGXFSZ", "SIGVTALRM", "SIGPROF", "SIGWINCH", "SIGUSR1", "SIGUSR2") for h in [_signal.getsignal(getattr(_signal, n))]})
t("valid_signals", lambda: (sorted(_signal.valid_signals()), type(_signal.valid_signals()).__name__, attempt(_signal.valid_signals, 1)))
t("strsignal", lambda: [(n, _signal.strsignal(n)) for n in range(1, _signal.NSIG)])
t("the enums", lambda: (signal.SIGINT, repr(signal.SIGINT), signal.SIG_DFL, repr(signal.SIG_IGN), repr(signal.SIG_BLOCK), signal.Signals(2).name, signal.getsignal(signal.SIGPIPE), sorted(signal.valid_signals())[:3], signal.strsignal(signal.SIGTERM)))

print("---- what will not do")
NUMBERS = ((), ("a",), (None,), (1.5,), (0,), (-1,), (_signal.NSIG,), (_signal.NSIG - 1,), (2 ** 31,), (-2 ** 31 - 1,), (2 ** 100,), ([],), (True,))
for f in (_signal.getsignal, _signal.strsignal, _signal.raise_signal):
    t(f.__name__, lambda: [attempt(f, *a) for a in NUMBERS if not (f is _signal.raise_signal and a in ((_signal.NSIG - 1,), (True,), (0,)))] + [attempt(f, 1, 2), attempt(lambda: f(signalnum=1))])
t("signal", lambda: [attempt(_signal.signal, *a) for a in ((), (1,), (1, 2, 3), ("a", 0), (0, 0), (-1, 0), (_signal.NSIG, 0), (2 ** 31, 0), (30, None), (30, "x"), (30, 2), (30, -1), (30, 1.0), (30, 0.0), (30, True), (30, False), (30, []), (_signal.SIGKILL, 0), (_signal.SIGKILL, len), (_signal.SIGSTOP, 1), (1.5, 0))] + [attempt(lambda: _signal.signal(signalnum=30, handler=0))])
t("an int of a derived class is neither", lambda: [attempt(_signal.signal, 30, type("I", (int,), {})(v)) for v in (0, 1)])
t("siginterrupt", lambda: [attempt(_signal.siginterrupt, *a) for a in ((), (30,), (30, 1, 2), ("a", 1), (30, "a"), (0, 1), (_signal.NSIG, 1), (30, None), (30, 1.5), (_signal.SIGKILL, 1), (30, True), (30, False), (30, 5), (30, -1))])
t("alarm", lambda: [attempt(_signal.alarm, *a) for a in ((), ("a",), (1.5,), (None,), (2 ** 31,), (1, 2), (0,))])
t("default_int_handler", lambda: [attempt(_signal.default_int_handler, *a) for a in ((), (2,), (2, None), (2, None, 1), ("a", None), (0, 5), (2 ** 31, None))])
t("pause and sigpending", lambda: (attempt(_signal.pause, 1), attempt(_signal.sigpending, 1), attempt(lambda: _signal.sigpending(x=1))))
t("getitimer", lambda: [attempt(_signal.getitimer, *a) for a in ((), ("a",), (-1,), (99,), (1.5,), (0, 1), (2 ** 31,), (None,))] + [_signal.getitimer(w) for w in (_signal.ITIMER_REAL, _signal.ITIMER_VIRTUAL, _signal.ITIMER_PROF)])
t("setitimer", lambda: [attempt(_signal.setitimer, *a) for a in ((), (0,), (0, 1, 2, 3), ("a", 0), (-1, 0), (99, 0), (0, "a"), (0, None), (0, 0, "a"), (0, 0, None), (0, float("nan")), (0, float("inf")), (0, 1e300), (0, -1), (0, 0, -1), (0, 2 ** 64), (0, [],), (0, 0), (0, 0.0, 0.0), (0, 0, 0))])
SETS = ((), (5,), (None,), ("ab",), ([0],), ([-1],), ([_signal.NSIG],), ([2 ** 63],), ([-2 ** 63 - 1],), ([2 ** 100],), (["a"],), ([1.5],), ([None],), ([1, "a"],), ({},), ([],), ((),), (set(),), (iter([]),), ([True],))
t("sigwait", lambda: [attempt(_signal.sigwait, *a) for a in SETS[:14]] + [attempt(_signal.sigwait, [1], 2)])
t("pthread_sigmask", lambda: [attempt(_signal.pthread_sigmask, _signal.SIG_BLOCK, *a) for a in SETS] + [attempt(_signal.pthread_sigmask, *a) for a in ((), (99, []), (-1, []), ("a", []), (1.5, []), (None, []), (2 ** 31, []), (1, [], 2))])
t("pthread_kill", lambda: [attempt(_signal.pthread_kill, *a) for a in ((), (1,), (1, 2, 3), ("a", 0), (None, 0), (1.5, 0), (_thread.get_ident(), "a"), (_thread.get_ident(), 1.5), (_thread.get_ident(), 2 ** 31), (_thread.get_ident(), -1), (_thread.get_ident(), 999), (_thread.get_ident(), 0))])
t("set_wakeup_fd", lambda: [attempt(_signal.set_wakeup_fd, *a, **k) for a, k in (((), {}), (("a",), {}), ((None,), {}), ((1.5,), {}), ((2 ** 31,), {}), ((-2,), {}), ((9999,), {}), ((-1,), {}), ((-1, True), {}), ((-1,), {"warn_on_full_buffer": False}), ((-1,), {"other": 1}), ((), {"fd": -1}), ((-1,), {"warn_on_full_buffer": []}))])

print("---- a function for a signal")
seen = []


def handler(number, frame):
    seen.append((number, type(frame).__name__, frame.f_code.co_name, frame.f_lineno - LINE))


def took():
    out = seen[:]
    seen.clear()
    return out


U1, U2 = _signal.SIGUSR1, _signal.SIGUSR2
LINE = sys._getframe().f_lineno
t("signal returns what there was", lambda: (_signal.signal(U1, handler), _signal.signal(U1, handler) is handler, _signal.getsignal(U1) is handler, _signal.signal(U1, _signal.SIG_IGN) is handler, _signal.signal(U1, _signal.SIG_DFL), _signal.signal(U1, handler), _signal.getsignal(U1) is handler))
t("raise_signal", lambda: (_signal.raise_signal(U1), took()))


def within():
    _signal.raise_signal(U1)
    return took()


t("the frame is where it was", within)
t("os.kill", lambda: (os.kill(os.getpid(), U1), [None for _ in range(100)] and took()))
t("pthread_kill", lambda: (_signal.pthread_kill(_thread.get_ident(), U1), took()))
t("ignored", lambda: (_signal.signal(U1, _signal.SIG_IGN) is handler, _signal.raise_signal(U1), took(), _signal.signal(U1, handler)))
t("both, the lower number first", lambda: (_signal.signal(U2, handler), _signal.pthread_sigmask(_signal.SIG_BLOCK, [U1, U2]), _signal.raise_signal(U2), _signal.raise_signal(U1), _signal.raise_signal(U1), took(), sorted(_signal.sigpending()), sorted(_signal.pthread_sigmask(_signal.SIG_UNBLOCK, [U1, U2])), [n for n, *_ in took()], sorted(_signal.sigpending())))
t("anything that can be called", lambda: [(_signal.signal(U1, c) and None, attempt(_signal.raise_signal, U1)) for c in (len, int, lambda *a: seen.append(len(a)), type("C", (), {"__call__": lambda self, n, f: seen.append("called")})())] + [took(), _signal.signal(U1, handler) and None])

print("---- what it raises")


def raises(number, frame):
    raise ValueError("from the handler of %d" % number)


t("it is raised where the signal was seen to", lambda: (_signal.signal(U1, raises) is handler, attempt(_signal.raise_signal, U1), attempt(os.kill, os.getpid(), U1)))
t("and the rest are for later", lambda: (_signal.signal(U2, handler) and None, _signal.pthread_sigmask(_signal.SIG_BLOCK, [U1, U2]), _signal.raise_signal(U1), _signal.raise_signal(U2), attempt(_signal.pthread_sigmask, _signal.SIG_UNBLOCK, [U1, U2]), [n for n, *_ in took()], sorted(_signal.pthread_sigmask(_signal.SIG_BLOCK, []))))
t("SystemExit and KeyboardInterrupt", lambda: [(_signal.signal(U1, lambda n, f, e=e: (_ for _ in ()).throw(e)) and None, attempt(_signal.raise_signal, U1)) for e in (SystemExit(3), KeyboardInterrupt(), GeneratorExit(), StopIteration(1))])
t("SIGINT", lambda: (_signal.getsignal(_signal.SIGINT) is _signal.default_int_handler, attempt(_signal.raise_signal, _signal.SIGINT), attempt(os.kill, os.getpid(), _signal.SIGINT), attempt(_thread.interrupt_main)))
_signal.signal(U1, handler)


def tight():
    os.kill(os.getpid(), _signal.SIGINT)
    while True:
        pass


t("in a loop that does nothing", tight)

print("---- _thread.interrupt_main")
t("goes by what there is for the signal", lambda: (_thread.interrupt_main(U1), [n for n, *_ in took()], _signal.signal(U1, _signal.SIG_IGN) and None, _thread.interrupt_main(U1), took(), _signal.signal(U1, _signal.SIG_DFL), _thread.interrupt_main(U1), took(), _signal.signal(U1, handler), [attempt(_thread.interrupt_main, n) for n in (0, -1, _signal.NSIG, _signal.NSIG - 1, "a", 1.5, 2 ** 31)]))
t("what is none of Python's business", lambda: (_signal.getsignal(_signal.SIGTERM), _thread.interrupt_main(_signal.SIGTERM), took()))

print("---- masks")
t("pthread_sigmask", lambda: (sorted(_signal.pthread_sigmask(_signal.SIG_BLOCK, [U1])), sorted(_signal.pthread_sigmask(_signal.SIG_BLOCK, (U2,))), sorted(_signal.pthread_sigmask(_signal.SIG_SETMASK, {U1})), sorted(_signal.pthread_sigmask(_signal.SIG_UNBLOCK, iter([U1]))), sorted(_signal.pthread_sigmask(_signal.SIG_SETMASK, [])), type(_signal.pthread_sigmask(_signal.SIG_BLOCK, [])).__name__))
t("what cannot be kept back", lambda: (sorted(_signal.pthread_sigmask(_signal.SIG_BLOCK, [_signal.SIGKILL, _signal.SIGSTOP, U1])), sorted(_signal.pthread_sigmask(_signal.SIG_SETMASK, []))))
t("all of them", lambda: (sorted(_signal.pthread_sigmask(_signal.SIG_BLOCK, range(1, _signal.NSIG))), sorted(_signal.pthread_sigmask(_signal.SIG_SETMASK, [])) == sorted(_signal.valid_signals() - {_signal.SIGKILL, _signal.SIGSTOP})))
t("sigwait", lambda: (_signal.pthread_sigmask(_signal.SIG_BLOCK, [U1, U2]) and None, _signal.raise_signal(U2), _signal.sigwait([U1, U2]), _signal.raise_signal(U1), _signal.sigwait({U1}), took(), sorted(_signal.sigpending()), sorted(_signal.pthread_sigmask(_signal.SIG_UNBLOCK, [U1, U2])), took()))
t("signal's own", lambda: (signal.pthread_sigmask(signal.SIG_BLOCK, [signal.SIGUSR1]), signal.pthread_sigmask(signal.SIG_UNBLOCK, [signal.SIGUSR1]), signal.sigpending()))

print("---- timers")


def wait_for(count, seconds=5):
    end = time.monotonic() + seconds
    while len(seen) < count and time.monotonic() < end:
        pass
    return [n for n, *_ in took()]


_signal.signal(_signal.SIGALRM, handler)
_signal.signal(_signal.SIGVTALRM, handler)
_signal.signal(_signal.SIGPROF, handler)
t("setitimer", lambda: (_signal.setitimer(_signal.ITIMER_REAL, 0.01), wait_for(1), _signal.getitimer(_signal.ITIMER_REAL)))
t("over and over", lambda: (_signal.setitimer(_signal.ITIMER_REAL, 0.005, 0.005), wait_for(3), [round(v, 3) for v in _signal.setitimer(_signal.ITIMER_REAL, 0)][1], _signal.getitimer(_signal.ITIMER_REAL)))
t("what there was", lambda: (_signal.setitimer(_signal.ITIMER_REAL, 100, 50), [(99 < a <= 100, b) for a, b in [_signal.setitimer(_signal.ITIMER_REAL, 0)]], [(99 < a <= 100, b) for _ in [_signal.setitimer(_signal.ITIMER_REAL, 100.5, 2.25)] for a, b in [(_signal.getitimer(_signal.ITIMER_REAL)[0] - 0.5, _signal.getitimer(_signal.ITIMER_REAL)[1])]], _signal.setitimer(_signal.ITIMER_REAL, 0) and None))
t("rounded up", lambda: (_signal.setitimer(_signal.ITIMER_REAL, 100, 1e-9), _signal.setitimer(_signal.ITIMER_REAL, 0)[1], _signal.setitimer(_signal.ITIMER_REAL, 100, 1.0000001) and None, _signal.setitimer(_signal.ITIMER_REAL, 0)[1]))
t("of the time that the process uses", lambda: (_signal.setitimer(_signal.ITIMER_VIRTUAL, 0.01), wait_for(1), _signal.setitimer(_signal.ITIMER_PROF, 0.01), wait_for(1)))
t("alarm", lambda: (_signal.alarm(100), _signal.alarm(50), _signal.alarm(0), _signal.alarm(0)))
t("sleep goes on afterwards", lambda: [(_signal.setitimer(_signal.ITIMER_REAL, 0.01), time.sleep(0.1), time.monotonic() - start >= 0.1, [n for n, *_ in took()]) for start in [time.monotonic()]])
t("unless the function raises", lambda: [(_signal.signal(_signal.SIGALRM, raises) and None, _signal.setitimer(_signal.ITIMER_REAL, 0.01), attempt(time.sleep, 5), time.monotonic() - start < 4, _signal.signal(_signal.SIGALRM, handler) and None) for start in [time.monotonic()]])
t("pause", lambda: (_signal.setitimer(_signal.ITIMER_REAL, 0.01), _signal.pause(), [n for n, *_ in took()]))
t("reading is begun again", lambda: [(_signal.setitimer(_signal.ITIMER_REAL, 0.01, 0.01), _signal.signal(_signal.SIGALRM, lambda n, f: (seen.append(n), len(seen) == 3 and (_signal.setitimer(_signal.ITIMER_REAL, 0), os.write(w, b"done")))) and None, os.read(r, 10), took(), os.close(r), os.close(w), _signal.signal(_signal.SIGALRM, handler) and None) for r, w in [os.pipe()]])

print("---- set_wakeup_fd")
r, w = os.pipe()
t("it must not block", lambda: attempt(_signal.set_wakeup_fd, w).replace(str(w), "N"))
os.set_blocking(w, False)
os.set_blocking(r, False)
t("what is written", lambda: (_signal.set_wakeup_fd(w), _signal.raise_signal(U1), _signal.raise_signal(U2), _signal.raise_signal(U1), list(os.read(r, 100)), [n for n, *_ in took()], _signal.set_wakeup_fd(-1) == w, _signal.raise_signal(U1), attempt(os.read, r, 100), took() and None, _signal.set_wakeup_fd(-1)))
t("interrupt_main writes too", lambda: (_signal.set_wakeup_fd(w), _thread.interrupt_main(U2), list(os.read(r, 100)), took() and None, _signal.set_wakeup_fd(-1) == w))
t("not for what is ignored", lambda: (_signal.set_wakeup_fd(w), _signal.signal(U1, _signal.SIG_IGN) and None, _signal.raise_signal(U1), attempt(os.read, r, 100), _signal.signal(U1, handler), _signal.set_wakeup_fd(-1) == w))
full = 0
try:
    while True:
        full += os.write(w, b"x" * 4096)
except BlockingIOError:
    pass
t("when there is no room it is said", lambda: (_signal.set_wakeup_fd(w), _signal.raise_signal(U1), took() and None))
t("unless it is not to be", lambda: (_signal.set_wakeup_fd(w, warn_on_full_buffer=False) == w, _signal.raise_signal(U1), took() and None, _signal.set_wakeup_fd(-1) == w))
os.close(r)
t("what else goes wrong is said whatever", lambda: (_signal.set_wakeup_fd(w, warn_on_full_buffer=False), _signal.raise_signal(U1), took() and None, _signal.set_wakeup_fd(-1) == w))
os.close(w)

print("---- what is told of")
audited = []
sys.addaudithook(lambda event, args: audited.append((event, args[1:])) if event.startswith("signal.") else None)
t("pthread_kill", lambda: (_signal.pthread_kill(_thread.get_ident(), U1), took() and None, attempt(_signal.pthread_kill, _thread.get_ident(), 999), audited))

print("---- signal.py")
t("signal", lambda: (signal.signal(signal.SIGUSR1, signal.SIG_IGN) is handler, signal.signal(signal.SIGUSR1, signal.SIG_DFL), signal.signal(signal.SIGUSR1, handler), signal.getsignal(signal.SIGUSR1) is handler, signal.getsignal(signal.SIGUSR2) is handler, signal.signal(signal.SIGUSR2, signal.Handlers.SIG_DFL) is handler, signal.getsignal(signal.SIGUSR2)))
t("sigwait", lambda: (signal.pthread_sigmask(signal.SIG_BLOCK, [signal.SIGUSR1]), signal.raise_signal(signal.SIGUSR1), signal.sigpending(), signal.sigwait([signal.SIGUSR1]), signal.pthread_sigmask(signal.SIG_UNBLOCK, [signal.SIGUSR1])))

print("---- the end")
signal.signal(signal.SIGTERM, lambda n, f: print("SIGTERM, and then the program goes on"))
os.kill(os.getpid(), signal.SIGTERM)
for _ in range(100):
    pass
print("as it does")
