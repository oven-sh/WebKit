# The module _socket, and socket, which is written over it: all of it that can be tried without a name or a port being bound, which not everything that runs tests is allowed to do.
import _socket
import array
import copy
import errno
import os
import pickle
import re
import select
import signal
import socket
import struct
import sys
import time
import warnings

warnings.simplefilter("ignore")


def attempt(f, /, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)


def t(label, f):
    r = attempt(f)
    print(label, "=>", r if isinstance(r, str) else ascii(r))


def reached(f, /, *a, **k):
    """What is said of what it is given. Once the system has been asked, what comes of it depends on what whatever runs this is allowed to do, so all that is said then is that it was. An OSError that has no number is
    one that was raised without asking."""
    try:
        r = f(*a, **k)
    except OSError as e:
        if isinstance(e, (_socket.gaierror, _socket.herror)) or e.errno is None:
            return type(e).__name__ + ": " + str(e)
        return "the system was asked"
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)
    if isinstance(r, _socket.socket):
        r.close()
    return "the system was asked"


def attempt_exception(f, /, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return e


def attempt_sent(s):
    "How much more it took, or None once it will take no more"
    try:
        return s.send(b"x" * 65536)
    except BlockingIOError:
        return None


def closing(*sockets):
    for s in sockets:
        s.close()


S = _socket.socket
print("---- what there is")
names = sorted(n for n in vars(_socket) if not n.startswith("__"))
t("the module", lambda: (_socket.__name__, _socket.__package__, _socket.__loader__.__name__, _socket.__doc__, sorted(n for n in vars(_socket) if n.startswith("__"))))
t("the numbers", lambda: [(n, getattr(_socket, n)) for n in names if type(getattr(_socket, n)) is int])
t("the rest", lambda: [(n, type(getattr(_socket, n)).__name__) for n in names if type(getattr(_socket, n)) is not int])
for name in names:
    x = getattr(_socket, name)
    if type(x).__name__ == "builtin_function_or_method":
        t(name, lambda: (x.__text_signature__, x.__doc__, x.__module__, x.__name__, x.__qualname__, x.__self__ is _socket))
t("the exceptions", lambda: [(e.__name__, e.__module__, e.__qualname__, [b.__name__ for b in e.__mro__], sorted(vars(e)), e.__doc__, repr(e(1, "x")), str(e(1, "x")), e(1, "x").errno, e(1, "x").strerror, e(2, "x").args, type(e(errno.ENOENT, "x")).__name__) for e in (_socket.herror, _socket.gaierror)] + [_socket.error is OSError, _socket.timeout is TimeoutError, _socket.SocketType is S, _socket.has_ipv6, re.sub(r"0x[0-9a-f]+", "0x", repr(_socket.CAPI))])
t("socket", lambda: (repr(S), S.__name__, S.__module__, S.__qualname__, [b.__name__ for b in S.__mro__], S.__doc__, S.__text_signature__, S.__basicsize__, S.__flags__ & 0xFFFFF, S.__dictoffset__, S.__weakrefoffset__, attempt(setattr, S, "x", 1)))
for n, v in sorted(vars(S).items()):
    t("socket." + n, lambda: (type(v).__name__, getattr(v, "__text_signature__", None), v.__doc__ if n != "__doc__" and n != "__module__" else None))

print("---- being made")


def shown(s):
    "Without which descriptor it has"
    return re.sub(r"fd=\d+", "fd=N", repr(s))


t("as it comes", lambda: [(shown(s), s.family, s.type, s.proto, s.timeout, s.gettimeout(), s.getblocking(), s.fileno() >= 0, os.get_inheritable(s.fileno()), s.close(), shown(s), s.fileno(), s.close()) for s in [S()]])
t("of each kind", lambda: [(shown(s), s.close())[0] if isinstance(s, S) else s for a, k in (((socket.AF_INET,), {}), ((socket.AF_INET6,), {}), ((socket.AF_UNIX,), {}), ((socket.AF_INET, socket.SOCK_DGRAM), {}), ((socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP), {}), ((socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_TCP), {}), ((-1, -1, -1), {}), ((-1, -1, -1, None), {}), ((), {"family": socket.AF_UNIX, "type": socket.SOCK_DGRAM, "proto": 0, "fileno": None}),
                                                                                                     ) for s in [attempt(S, *a, **k)]] + [reached(S, *a) for a in ((999,), (socket.AF_INET, 999), (socket.AF_INET, socket.SOCK_STREAM, 999), (socket.AF_UNIX, socket.SOCK_STREAM, 5), (0,), (-2,))])
t("how it is called", lambda: [attempt(S, *a, **k) for a, k in ((("a",), {}), ((1.5,), {}), ((None,), {}), ((2 ** 31,), {}), ((-2 ** 31 - 1,), {}), ((2, "a"), {}), ((2, 1, "a"), {}), ((2, 1, 0, "a"), {}), ((2, 1, 0, 1.5), {}), ((2, 1, 0, -1), {}), ((2, 1, 0, -5), {}), ((2, 1, 0, 2 ** 31), {}), ((2, 1, 0, 2 ** 70), {}), ((2, 1, 0, None, 5), {}), ((), {"x": 1}), ((2,), {"family": 2}), ((), {"famly": 2}))])
t("of what is open already", lambda: [(shown(c), c.family, c.type, c.proto, c.fileno() == a.fileno(), c.detach() == a.fileno(), shown(c), closing(a, b)) for a, b in [_socket.socketpair()] for c in [S(fileno=a.fileno())]])
t("of what is said to be otherwise", lambda: [(shown(c), c.detach() >= 0, closing(a, b)) for a, b in [_socket.socketpair()] for c in [S(socket.AF_INET, socket.SOCK_DGRAM, 17, a.fileno())]])
t("of what is not a socket", lambda: [attempt(S, fileno=9999), attempt(S, socket.AF_INET, socket.SOCK_STREAM, 0, 9999)] + [(attempt(S, fileno=r), attempt(S, socket.AF_INET, socket.SOCK_STREAM, 0, r), attempt(S, socket.AF_INET, -1, -1, w), os.close(r), os.close(w))[:3] for r, w in [os.pipe()]] + [(attempt(S, fileno=f), os.close(f))[0] for f in [os.open(os.devnull, os.O_RDONLY)]])
t("made and not begun", lambda: [(shown(s), s.fileno(), s.family, s.type, s.proto, s.timeout, s.getblocking(), attempt(s.recv, 1), attempt(s.send, b"a"), attempt(s.getsockname), attempt(s.setblocking, True), s.close(), s.detach()) for s in [S.__new__(S)]] + [attempt(S.__new__), attempt(S.__new__, int), shown(S.__new__(S, 1, 2, x=3))])
t("begun twice", lambda: [(f >= 0, s.__init__(socket.AF_UNIX), s.family, s.fileno() != f or "the same", os.close(f), s.close()) for s in [S()] for f in [s.fileno()]])


class Sub(S):
    def __init__(self, *a, tag=None, **k):
        super().__init__(*a, **k)
        self.tag = tag


t("derived from", lambda: [(type(s).__name__, s.tag, s.__dict__, shown(s), isinstance(s, S), s.family, s.close()) for s in [Sub(socket.AF_UNIX, tag=5)]] + [[(shown(s), s.x, attempt(setattr, s, "y", 1), s.close()) for s in [type("Slots", (S,), {"__slots__": ("x", "__weakref__")})()] for _ in [setattr(s, "x", 1)]]])
t("what it has", lambda: [([attempt(setattr, s, n, 1) for n in ("family", "type", "proto", "timeout", "x")], [attempt(delattr, s, n) for n in ("family", "timeout")], attempt(getattr, s, "__dict__"), attempt(hash, s) != 0, s == s, s != S, attempt(len, s), attempt(iter, s), attempt(bool, s), attempt(pickle.dumps, s), attempt(copy.copy, s), attempt(__import__("weakref").ref, s), s.close()) for s in [S()]])
t("__del__", lambda: [(f >= 0, s.__del__(), s.fileno(), attempt(os.fstat, f), s.__del__(), attempt(s.__del__, 1)) for s in [S()] for f in [s.fileno()]])


def warned(f):
    with warnings.catch_warnings(record=True) as w:
        warnings.simplefilter("always")
        r = attempt(f)
    return r, [(x.category.__name__, re.sub(r"fd=\d+", "fd=N", str(x.message)), x.source is not None) for x in w]


t("which warns", lambda: [warned(S().__del__), warned(socket.socket().__del__)] + [[(warned(s.__del__), s.fileno()) for s in [S()] for _ in [s.close()]]])


def raising(f):
    "With warnings raised, and what is said of what is raised where nothing can catch it"
    said = []
    hook = sys.unraisablehook
    sys.unraisablehook = lambda u: said.append((u.exc_type.__name__, re.sub(r"fd=\d+", "fd=N", str(u.exc_value)), re.sub(r"fd=\d+", "fd=N", str(u.err_msg)), u.object))
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("error")
            return attempt(f), said
    finally:
        sys.unraisablehook = hook


t("and is not stopped by that", lambda: [(raising(s.__del__), s.fileno()) for s in [S()]])

print("---- whether it waits")
t("setblocking", lambda: [[(s.setblocking(v), s.getblocking(), s.gettimeout(), s.timeout, bool(__import__("fcntl").fcntl(s.fileno(), 3) & os.O_NONBLOCK) if False else None) for v in (False, True, 0, 1, "", "a", [], None, 2.5)] + [attempt(s.setblocking), attempt(s.setblocking, 1, 2), attempt(s.setblocking, flag=1), attempt(s.setblocking, type("B", (), {"__bool__": lambda s: 1 / 0})()), s.close(), attempt(s.setblocking, True), s.getblocking()] for s in [S()]])
t("settimeout", lambda: [[(attempt(s.settimeout, v), s.gettimeout(), s.getblocking(), os.get_blocking(s.fileno())) for v in (None, 0, 0.0, 1, 1.5, 1e-9, 1e-10, 0.1, 10 ** 6, True, False, -1, -0.5, -1e-10, "a", [], 1j, float("inf"), float("nan"), 1e20, 2 ** 63, 2 ** 70, 9223372036, 9223372037, type("F", (), {"__float__": lambda s: 2.0})(), type("I", (), {"__index__": lambda s: 3})(), __import__("fractions").Fraction(1, 2), __import__("decimal").Decimal("0.25"))] + [attempt(s.settimeout), attempt(s.settimeout, 1, 2), attempt(s.gettimeout, 1), attempt(s.getblocking, 1), s.close(), attempt(s.settimeout, 1), s.gettimeout()] for s in [S()]])
t("for new ones", lambda: (_socket.getdefaulttimeout(), [(attempt(_socket.setdefaulttimeout, v), _socket.getdefaulttimeout()) for v in (1.5, 0, None, -1, "a", 2 ** 70)], _socket.setdefaulttimeout(2.5), [(s.gettimeout(), s.getblocking(), os.get_blocking(s.fileno()), s.close()) for s in [S()]], [(a.gettimeout(), b.gettimeout(), closing(a, b)) for a, b in [_socket.socketpair()]], _socket.setdefaulttimeout(0), [(s.gettimeout(), s.getblocking(), os.get_blocking(s.fileno()), s.close()) for s in [S()]], _socket.setdefaulttimeout(None), [(s.gettimeout(), os.get_blocking(s.fileno()), s.close()) for s in [S()]], attempt(_socket.setdefaulttimeout), attempt(_socket.setdefaulttimeout, 1, 2), attempt(_socket.getdefaulttimeout, 1)))

print("---- a pair of them")
t("socketpair", lambda: [(shown(a), shown(b), a.family, a.type, a.proto, os.get_inheritable(a.fileno()), a.getsockname(), a.getpeername(), closing(a, b)) for a, b in [_socket.socketpair()]] + [[(shown(a), closing(a, b))[0] for a, b in [_socket.socketpair(*x)]] for x in ((socket.AF_UNIX,), (socket.AF_UNIX, socket.SOCK_DGRAM), (socket.AF_UNIX, socket.SOCK_STREAM, 0))] + [reached(_socket.socketpair, *x) for x in ((socket.AF_INET,), (999,), (1, 999), (1, 1, 999))] + [attempt(_socket.socketpair, *x, **k) for x, k in ( (("a",), {}), ((1, "a"), {}), ((1, 1, "a"), {}), ((1, 1, 0, 0), {}), ((), {"family": 1}), ((2 ** 31,), {}), ((None,), {}))])


def pair(kind=socket.SOCK_STREAM):
    return _socket.socketpair(socket.AF_UNIX, kind)


t("send and recv", lambda: [(a.send(b"hello"), b.recv(3), b.recv(10), a.send(bytearray(b"ab")), a.send(memoryview(b"cd")), a.send(array.array("H", [0x4142])), b.recv(100), a.send(b""), a.sendall(b"xyz"), b.recv(3, 0), a.send(b"peek"), b.recv(2, socket.MSG_PEEK), b.recv(4), b.recv(0), closing(a), b.recv(5), b.recv(5), closing(b)) for a, b in [pair()]])
t("how send is called", lambda: [[attempt(f, *x, **k) for f in (a.send, a.sendall) for x, k in (((), {}), (("a",), {}), ((5,), {}), ((None,), {}), (([],), {}), ((b"a", "x"), {}), ((b"a", 1.5), {}), ((b"a", None), {}), ((b"a", 2 ** 31), {}), ((b"a", 0, 0), {}), ((), {"data": b"a"}), ((b"a",), {"flags": 0}), ((memoryview(b"abcd")[::2],), {}))] + [closing(a, b)] for a, b in [pair()]])
t("how recv is called", lambda: [[attempt(f, *x, **k) for f in (b.recv, b.recvfrom) for x, k in (((), {}), (("a",), {}), ((1.5,), {}), ((None,), {}), ((-1,), {}), ((-2 ** 63,), {}), ((2 ** 63,), {}), ((2 ** 62,), {}), ((1, "x"), {}), ((1, 1.5), {}), ((1, 2 ** 31), {}), ((1, 0, 0), {}), ((), {"buffersize": 1}), ((1,), {"flags": 0}), ((True,), {}), ((type("I", (), {"__index__": lambda s: 2})(),), {}))] + [closing(a, b)] for a, b in [pair()] for _ in [a.send(b"0123456789")]])
t("given by name, however it is called", lambda: [(attempt(lambda: a.send(data=b"a")), attempt(lambda: S.send(a, data=b"a")), attempt(a.send, data=b"a"), attempt(lambda: a.recv(buffersize=1)), attempt(lambda: S.recv(a, buffersize=1)), attempt(lambda: a.sendto(b"a", address="x")), attempt(lambda: S.sendto(a, b"a", address="x")), attempt(a.sendto, b"a", address="x"), attempt(lambda: a.setsockopt(1, 2, value=3)), attempt(lambda: S.setsockopt(a, 1, 2, value=3)), attempt(a.setsockopt, 1, 2, value=3),
                                                   attempt(lambda: a.bind(address="x")), attempt(lambda: S.bind(a, address="x")), attempt(a.bind, address="x"), attempt(lambda: a.fileno(x=1)), attempt(lambda: S.fileno(a, x=1)), attempt(a.fileno, x=1), attempt(lambda: a.close(x=1)), attempt(lambda: _socket.socketpair(family=1)), attempt(lambda: _socket.CMSG_LEN(length=1)), attempt(lambda: _socket.close(integer=1)), attempt(lambda: _socket.htons(integer=1)), attempt(lambda: a.send()), attempt(lambda: S.send(a)), attempt(lambda: S.send()), attempt(lambda: S.send(5, b"a")), closing(a, b)) for a, b in [pair()]])
t("recv_into", lambda: [(a.send(b"0123456789"), b.recv_into(buf), bytes(buf), b.recv_into(buf, 2), bytes(buf), b.recv_into(buf, 1, 0), bytes(buf), b.recv_into(memoryview(buf)[1:3]), bytes(buf), b.recv_into(buffer=buf, nbytes=1, flags=socket.MSG_PEEK), bytes(buf), b.recv_into(arr), arr.tolist()[:1], b.recv_into(bytearray()), closing(a, b)) for a, b in [pair()] for buf in [bytearray(4)] for arr in [array.array("B", [0] * 3)]])
t("how recv_into is called", lambda: [[attempt(f, *x, **k) for f in (b.recv_into, b.recvfrom_into) for x, k in (((), {}), ((b"abc",), {}), (("abc",), {}), ((5,), {}), ((None,), {}), ((memoryview(b"abc"),), {}), ((bytearray(2), 3), {}), ((bytearray(2), -1), {}), ((bytearray(2), "a"), {}), ((bytearray(2), 1.5), {}), ((bytearray(2), None), {}), ((bytearray(2), 1, "a"), {}), ((bytearray(2), 1, 0, 0), {}), ((bytearray(2),), {"x": 1}), ((bytearray(2),), {"buffer": bytearray(2)}), ((bytearray(2),), {"nbyte": 1}), ((bytearray(2), 2 ** 63), {}), ((memoryview(bytearray(4))[::2],), {}))] + [closing(a, b)] for a, b in [pair()] for _ in [a.send(b"0" * 100)]])
t("recvfrom", lambda: [(a.send(b"abc"), b.recvfrom(10), a.send(b"def"), b.recvfrom_into(buf), bytes(buf), a.send(b"gh"), b.recvfrom_into(buf, 1), a.send(b"ij"), b.recvfrom(0), a.send(b"kl"), b.recvfrom(5), closing(a, b)) for k in (socket.SOCK_STREAM, socket.SOCK_DGRAM) for a, b in [pair(k)] for buf in [bytearray(4)]])
t("one message at a time", lambda: [(a.send(b"first"), a.send(b"second"), a.send(b""), b.recv(3), b.recv(100), b.recv(100), a.send(b"x" * 100), b.recv_into(bytearray(10)), closing(a, b)) for a, b in [pair(socket.SOCK_DGRAM)]])
t("a great deal", lambda: [(sum(1 for _ in iter(lambda: (a.send(data[:4096]), len(b.recv(4096)))[1] != 4096, True)) if False else None, [len(x) for x in [b"".join((a.sendall(data[i:i + 8192]), b.recv(8192, socket.MSG_WAITALL))[1] for i in range(0, len(data), 8192))] if x == data], closing(a, b)) for a, b in [pair()] for data in [bytes(range(256)) * 4096]])
t("when it is closed", lambda: [[attempt(f) for f in (lambda: a.send(b"a"), lambda: a.sendall(b"a"), lambda: a.recv(1), lambda: a.recv(0), lambda: a.recv_into(bytearray(1)), lambda: a.recv_into(bytearray()), lambda: a.recvfrom(1), lambda: a.recvfrom(0), lambda: a.recvmsg(1), lambda: a.sendmsg([b"a"]), lambda: a.getsockname(), lambda: a.getpeername(), lambda: a.getsockopt(socket.SOL_SOCKET, socket.SO_TYPE), lambda: a.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1), lambda: a.shutdown(0), lambda: a.listen(), lambda: a._accept(), lambda: a.bind("x"), lambda: a.connect("x"), lambda: a.connect_ex("x"), lambda: a.fileno(), lambda: a.detach(), lambda: a.close())] + [closing(b)] for a, b in [pair()] for _ in [a.close()]])
t("when the other end is", lambda: [(closing(b), attempt(a.send, b"a"), attempt(a.sendall, b"a"), a.recv(1), attempt(a.getpeername), closing(a)) for a, b in [pair()] for _ in [signal.signal(signal.SIGPIPE, signal.SIG_IGN)]])
t("shutdown", lambda: [(a.shutdown(socket.SHUT_WR), b.recv(1), attempt(a.send, b"x"), b.send(b"y"), a.recv(1), attempt(a.shutdown, socket.SHUT_RD), a.recv(1), attempt(a.shutdown, 5), attempt(a.shutdown, "a"), attempt(a.shutdown, 1.5), attempt(a.shutdown), attempt(a.shutdown, 0, 0), attempt(a.shutdown, 2 ** 31), attempt(a.shutdown, how=0), attempt(a.shutdown, socket.SHUT_RDWR), closing(a, b)) for a, b in [pair()]] + [[(a.shutdown(socket.SHUT_RDWR), a.recv(1), b.recv(1), attempt(a.send, b"x"), closing(a, b)) for a, b in [pair()]]])
t("detach and dup", lambda: [(f == f0, a.fileno(), attempt(a.send, b"x"), d >= 0 and d != f0, os.get_inheritable(d), os.write(d, b"via dup"), b.recv(10), _socket.close(d), attempt(_socket.close, d), _socket.close(f), closing(a, b)) for a, b in [pair()] for f0 in [a.fileno()] for d in [_socket.dup(f0)] for f in [a.detach()]] + [attempt(f, *x, **k) for f in (_socket.dup, _socket.close) for x, k in (((), {}), (("a",), {}), ((1.5,), {}), ((None,), {}), ((-1,), {}), ((9999,), {}), ((2 ** 70,), {}), ((1, 2), {}), ((), {"integer": 1}))])

print("---- not waiting, and not for long")
t("with nothing there", lambda: [(a.setblocking(False), attempt(a.recv, 1), attempt(a.recv_into, bytearray(1)), attempt(a.recvfrom, 1), attempt(a.recvmsg, 1), attempt(a.recv, 0), type(attempt_exception(a.recv, 1)).__name__, attempt_exception(a.recv, 1).errno == errno.EAGAIN, closing(a, b)) for a, b in [pair()]])
t("with no room", lambda: [(a.setblocking(False), sum(iter(lambda: attempt_sent(a), None)) > 0, attempt(a.send, b"x" * 65536), attempt(a.sendall, b"x" * 65536), attempt(a.sendmsg, [b"x" * 65536]), closing(a, b)) for a, b in [pair()]])


def timed(f, /, *a, least=0.0, most=60.0):
    """What comes of it, and whether it took as long as it was to. A machine with much else to do makes anything take longer, so it is how long it takes at the least that is looked at closely, and how long at the most only where
    there is a great deal to spare."""
    start = time.monotonic()
    r = attempt(f, *a)
    return r, least <= time.monotonic() - start <= most


t("for no longer than", lambda: [(a.settimeout(0.2), timed(a.recv, 1, least=0.2), timed(a.recv_into, bytearray(1), least=0.2), timed(a.recvfrom, 1, least=0.2), timed(a.recvmsg, 1, least=0.2), timed(a.recvmsg_into, [bytearray(1)], least=0.2), timed(a.recv, 0), b.send(b"now"), timed(a.recv, 5), type(attempt_exception(a.recv, 1)).__mro__[1].__name__, attempt_exception(a.recv, 1).args, attempt_exception(a.recv, 1).errno, closing(a, b)) for a, b in [pair()]])
t("to send in", lambda: [(a.settimeout(0.2), a.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4096), timed(a.sendall, b"x" * (1 << 22), least=0.2), timed(lambda: [a.send(b"x" * 65536) for _ in range(100)] and None, least=0.2), closing(a, b)) for a, b in [pair()]])
t("what is there is not waited for", lambda: [(a.settimeout(30), b.send(b"abc"), timed(a.recv, 10, most=20), a.settimeout(1e-9), b.send(b"def"), time.sleep(0.05), a.recv(10), closing(a, b)) for a, b in [pair()]])

print("---- interrupted")


def interrupted(handler, f, after=0.1, least=None, most=60.0):
    "What comes of `f` when a signal comes while it waits"
    old = signal.signal(signal.SIGALRM, handler)
    signal.setitimer(signal.ITIMER_REAL, after)
    try:
        return timed(f, least=after if least is None else least, most=most)
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, old)


def raiser(*_):
    raise KeyError("from the handler")


t("by what raises", lambda: [(interrupted(raiser, lambda: a.recv(1)), interrupted(raiser, lambda: a.recv_into(bytearray(1))), interrupted(raiser, lambda: a.recvmsg(1)), interrupted(raiser, lambda: a.recvfrom(1)), a.settimeout(5), interrupted(raiser, lambda: a.recv(1)), a.settimeout(None), a.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4096), interrupted(raiser, lambda: a.sendall(b"x" * (1 << 22))), closing(a, b)) for a, b in [pair()]])
t("by what does not, and goes on", lambda: [(interrupted(lambda *_: (seen.append(1), b.send(b"sent by the handler")), lambda: a.recv(100)), seen, a.settimeout(1), interrupted(lambda *_: seen.append(2), lambda: a.recv(1), after=0.8, least=1, most=1.6), seen, closing(a, b)) for a, b in [pair()] for seen in [[]]])
# CPython will not have it made shorter while it is being received into. Where it can be, no more is to be put there than there is then room for.
t("by what makes where it is going shorter", lambda: [(r == len(buf) and bytes(buf) == b"0123456789"[:r], len(buf) in (2, 8), closing(a, b)) for a, b in [pair()] for buf in [bytearray(8)] for r in [interrupted(lambda *_: (attempt(buf.clear), attempt(buf.extend, b"ab"), b.send(b"0123456789")), lambda: a.recv_into(buf))[0]]])
t("or what is being sent", lambda: [(a.settimeout(0.4), interrupted(lambda *_: attempt(data.clear), lambda: a.sendall(data))[0] in (None, "TimeoutError: timed out"), len(data) in (0, 1 << 21), closing(a, b)) for a, b in [pair()] for data in [bytearray(1 << 21)]])
t("by what closes it", lambda: [(interrupted(lambda *_: a.close(), lambda: a.recv(1))[0], a.fileno(), closing(b)) for a, b in [pair()]])

print("---- options")
SOL, IP, TCP = socket.SOL_SOCKET, socket.IPPROTO_IP, socket.IPPROTO_TCP
t("getsockopt", lambda: [(s.getsockopt(SOL, socket.SO_TYPE), s.getsockopt(SOL, socket.SO_ERROR), s.getsockopt(SOL, socket.SO_REUSEADDR), s.getsockopt(SOL, socket.SO_TYPE, 0), s.getsockopt(SOL, socket.SO_TYPE, 4), s.getsockopt(SOL, socket.SO_TYPE, 100), s.getsockopt(SOL, socket.SO_TYPE, 1024), len(s.getsockopt(SOL, socket.SO_LINGER, 100)), s.getsockopt(SOL, socket.SO_TYPE, 2), s.getsockopt(TCP, socket.TCP_NODELAY), s.close()) for s in [S()]])
t("how it is called", lambda: [[attempt(s.getsockopt, *x, **k) for x, k in (((), {}), ((1,), {}), ((1, 2, 3, 4), {}), (("a", 1), {}), ((1, "a"), {}), ((1, 1, "a"), {}), ((1.5, 1), {}), ((None, 1), {}), ((SOL, socket.SO_TYPE, -1), {}), ((SOL, socket.SO_TYPE, 1025), {}), ((SOL, socket.SO_TYPE, 2 ** 31), {}), ((SOL, socket.SO_TYPE, -2 ** 31), {}), ((2 ** 31, 1), {}), ((SOL, 9999), {}), ((9999, 1), {}), ((SOL, 9999, 10), {}), ((), {"level": 1, "option": 1}))] + [s.close()] for s in [S()]])
t("setsockopt", lambda: [(s.setsockopt(SOL, socket.SO_REUSEADDR, 1), bool(s.getsockopt(SOL, socket.SO_REUSEADDR)), s.setsockopt(SOL, socket.SO_REUSEADDR, 0), s.getsockopt(SOL, socket.SO_REUSEADDR), s.setsockopt(SOL, socket.SO_REUSEADDR, True), bool(s.getsockopt(SOL, socket.SO_REUSEADDR)), s.setsockopt(SOL, socket.SO_REUSEADDR, struct.pack("i", 0)), s.getsockopt(SOL, socket.SO_REUSEADDR), s.setsockopt(SOL, socket.SO_LINGER, struct.pack("ii", 1, 5)), struct.unpack("ii", s.getsockopt(SOL, socket.SO_LINGER, 8)),
                             s.setsockopt(SOL, socket.SO_REUSEADDR, bytearray(struct.pack("i", 1))), s.setsockopt(SOL, socket.SO_REUSEADDR, memoryview(struct.pack("i", 1))), s.setsockopt(TCP, socket.TCP_NODELAY, 1), bool(s.getsockopt(TCP, socket.TCP_NODELAY)), s.setsockopt(SOL, socket.SO_RCVBUF, 32768), s.getsockopt(SOL, socket.SO_RCVBUF), s.setsockopt(SOL, socket.SO_KEEPALIVE, type("I", (), {"__index__": lambda s: 1})()), bool(s.getsockopt(SOL, socket.SO_KEEPALIVE)), s.close()) for s in [S()]])
t("how that is called", lambda: [[attempt(s.setsockopt, *x, **k) for x, k in (((), {}), ((1,), {}), ((1, 2), {}), ((1, 2, 3, 4), {}), ((1, 2, 3, 4, 5), {}), (("a", 1, 1), {}), ((1, "a", 1), {}), ((SOL, socket.SO_REUSEADDR, "a"), {}), ((SOL, socket.SO_REUSEADDR, 1.5), {}), ((SOL, socket.SO_REUSEADDR, None), {}), ((SOL, socket.SO_REUSEADDR, []), {}), ((SOL, socket.SO_REUSEADDR, 2 ** 31), {}), ((SOL, socket.SO_REUSEADDR, 2 ** 70), {}), ((SOL, socket.SO_REUSEADDR, -1), {}), ((SOL, socket.SO_REUSEADDR, b""), {}), ((SOL, socket.SO_REUSEADDR, b"a"), {}), ((SOL, socket.SO_REUSEADDR, b"a" * 100), {}),
                                                                                    ((SOL, socket.SO_REUSEADDR, None, 0), {}), ((SOL, socket.SO_REUSEADDR, None, 4), {}), ((SOL, socket.SO_REUSEADDR, None, "a"), {}), ((SOL, socket.SO_REUSEADDR, None, None), {}), ((SOL, socket.SO_REUSEADDR, None, -1), {}), ((SOL, socket.SO_REUSEADDR, None, 2 ** 70), {}), ((SOL, socket.SO_REUSEADDR, 1, 4), {}), ((SOL, socket.SO_REUSEADDR, b"abcd", 4), {}), ((2 ** 31, 1, 1), {}), ((SOL, 9999, 1), {}), ((9999, 1, 1), {}), ((1.5, 1, 1), {}), ((1.5, 1, b"a"), {}))] + [s.close()] for s in [S()]])


class Counted:
    "How many times it is asked what number it is"
    def __init__(self, v): self.v, self.n = v, 0
    def __index__(self):
        self.n += 1
        return self.v


t("how many times each is asked", lambda: [[(attempt(s.setsockopt, *x), [c.n for c in x if isinstance(c, Counted)]) for x in ((Counted(SOL), Counted(socket.SO_REUSEADDR), Counted(1)), (Counted(SOL), Counted(socket.SO_REUSEADDR), b"\1\0\0\0"), (Counted(SOL), Counted(socket.SO_REUSEADDR), None, Counted(0)), (Counted(SOL), Counted(socket.SO_REUSEADDR), "a"), (Counted(SOL), Counted(socket.SO_REUSEADDR), Counted(2 ** 40)), (Counted(SOL), Counted(socket.SO_REUSEADDR), Counted(1), Counted(1)), (Counted(2 ** 40), Counted(1), Counted(1)))] + [s.close()] for s in [S()]])
t("listen", lambda: [[attempt(s.listen, *x, **k) for x, k in ((("a",), {}), ((1.5,), {}), ((None,), {}), ((1, 2), {}), ((2 ** 31,), {}), ((), {"backlog": 1}))] + [s.close()] for s in [S()]])

print("---- what goes along with what is sent")
t("CMSG_LEN and CMSG_SPACE", lambda: [[attempt(f, v) for v in (0, 1, 3, 4, 5, 8, 100, 2 ** 20, 2 ** 31 - 30, 2 ** 31 - 12, 2 ** 31 - 13, 2 ** 31 - 16, 2 ** 31 - 17, 2 ** 31 - 1, 2 ** 31, 2 ** 40, 2 ** 63 - 1, 2 ** 63, -1, "a", 1.5, None, True)] + [attempt(f), attempt(f, 1, 2)] for f in (_socket.CMSG_LEN, _socket.CMSG_SPACE)])
t("sendmsg and recvmsg", lambda: [(a.sendmsg([b"ab", b"cd"]), b.recvmsg(10), attempt(a.sendmsg, []), attempt(a.sendmsg, [b""]), a.sendmsg((b"x", bytearray(b"y"), memoryview(b"z"))), b.recvmsg(2), b.recvmsg(10, 0), a.sendmsg(iter([b"it"])), b.recvmsg(10, 0, 0), a.sendmsg([b"q"], []), a.sendmsg([b"r"], (), 0), a.sendmsg([b"s"], [], 0, None), b.recvmsg(10), a.sendmsg([b"peek"]), b.recvmsg(10, 0, socket.MSG_PEEK), b.recvmsg(10), b.recvmsg(0) if False else None, closing(a, b)) for a, b in [pair()]])
t("recvmsg_into", lambda: [(a.send(b"0123456789"), b.recvmsg_into([x, y]), bytes(x), bytes(y), b.recvmsg_into([memoryview(x)[1:]]), bytes(x), b.recvmsg_into(iter([y]), 0, 0), bytes(y), a.send(b"z"), b.recvmsg_into([bytearray(), x]), bytes(x), closing(a, b)) for a, b in [pair()] for x, y in [(bytearray(3), bytearray(2))]])
t("one message, cut short", lambda: [(a.sendmsg([b"0123456789"]), b.recvmsg(4), a.sendmsg([b"abc"]), b.recvmsg_into([bytearray(1)]), closing(a, b)) for a, b in [pair(socket.SOCK_DGRAM)]])


def passed(count, room, kind=socket.SOCK_STREAM):
    "Open files sent from one to the other, and what can be read from what arrives"
    a, b = pair(kind)
    pipes = [os.pipe() for _ in range(count)]
    for i, (r, w) in enumerate(pipes):
        os.write(w, b"pipe %d" % i)
    sent = a.sendmsg([b"fds"], [(SOL, socket.SCM_RIGHTS, array.array("i", [r for r, w in pipes]))])
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        data, ancillary, flags, address = b.recvmsg(10, room)
    got = []
    shape = []
    for level, kind_, payload in ancillary:
        fds = array.array("i")
        fds.frombytes(payload[:len(payload) - len(payload) % fds.itemsize])
        shape.append((level == SOL, kind_ == socket.SCM_RIGHTS, len(payload)))
        for fd in fds:
            got.append((os.read(fd, 20), os.get_inheritable(fd)))
            os.close(fd)
    for r, w in pipes:
        os.close(r)
        os.close(w)
    closing(a, b)
    return sent, data, shape, got, bool(flags & socket.MSG_CTRUNC), address, [(w.category.__name__, str(w.message)) for w in caught]


t("open files", lambda: [passed(1, _socket.CMSG_SPACE(4)), passed(1, _socket.CMSG_LEN(4)), passed(3, _socket.CMSG_SPACE(12)), passed(2, 1024), passed(1, _socket.CMSG_SPACE(4), socket.SOCK_DGRAM)])
t("with no room for them", lambda: [passed(1, 0), passed(2, _socket.CMSG_LEN(4)), passed(1, _socket.CMSG_LEN(0)), passed(1, 1), passed(1, _socket.CMSG_LEN(4) - 1)])
t("how sendmsg is called", lambda: [[attempt(a.sendmsg, *x, **k) for x, k in (((), {}), ((5,), {}), ((None,), {}), ((b"ab",), {}), (("ab",), {}), (([5],), {}), ((["a"],), {}), (([None],), {}), (([b"a", 5],), {}), (([b"a"], 5), {}), (([b"a"], None), {}), (([b"a"], [5]), {}), (([b"a"], [()]), {}), (([b"a"], [(1, 2)]), {}), (([b"a"], [(1, 2, 3)]), {}), (([b"a"], [(1, 2, b"", 4)]), {}), (([b"a"], [("a", 2, b"")]), {}), (([b"a"], [(1, "a", b"")]), {}), (([b"a"], [(1, 2, "a")]), {}), (([b"a"], [[1, 2, b""]]), {}), (([b"a"], ["abc"]), {}), (([b"a"], [b"abc"]), {}),
                                                                                       (([b"a"], [(2 ** 31, 2, b"")]), {}), (([b"a"], [(1.5, 2, b"")]), {}), (([b"a"], [], "a"), {}), (([b"a"], [], 1.5), {}), (([b"a"], [], None), {}), (([b"a"], [], 0, 5), {}), (([b"a"], [], 0, None, 5), {}), ((), {"buffers": [b"a"]}), (([memoryview(b"abcd")[::2]],), {}), (([b"a"], [(SOL, socket.SCM_RIGHTS, b"\xff\xff\xff\x7f")]), {}), (([b"a"], [(9999, 9999, b"abcd")]), {}), ((type("I", (), {"__iter__": lambda s: 1 / 0})(),), {}), (([b"a"], type("I", (), {"__iter__": lambda s: 1 / 0})()), {}))] + [closing(a, b)] for a, b in [pair()]])
t("how recvmsg is called", lambda: [[attempt(b.recvmsg, *x, **k) for x, k in (((), {}), (("a",), {}), ((1.5,), {}), ((None,), {}), ((-1,), {}), ((2 ** 63,), {}), ((2 ** 62,), {}), ((1, "a"), {}), ((1, -1), {}), ((1, 2 ** 31), {}), ((1, 2 ** 31 - 1), {}) if False else ((1, 1.5), {}), ((1, None), {}), ((1, 0, "a"), {}), ((1, 0, 2 ** 31), {}), ((1, 0, 0, 0), {}), ((), {"bufsize": 1}))] + [attempt(b.recvmsg_into, *x, **k) for x, k in (((), {}), ((5,), {}), ((None,), {}), ((bytearray(2),), {}), (([b"ab"],), {}), ((["ab"],), {}), (([5],), {}), (([bytearray(1), b"a"],), {}), (([memoryview(bytearray(4))[::2]],), {}), (([bytearray(1)], "a"), {}), (([bytearray(1)], -1), {}), (([bytearray(1)], 2 ** 31), {}), (([bytearray(1)], 0, "a"), {}), (([bytearray(1)], 0, 0, 0), {}), ((), {"buffers": []}), ((type("I", (), {"__iter__": lambda s: 1 / 0})(),), {}))] + [closing(a, b)] for a, b in [pair()] for _ in [a.send(b"0" * 200)]])

print("---- addresses that will not do")
BAD_INET = (5, None, "a", b"a", [], ["127.0.0.1", 80], (), ("127.0.0.1",), ("127.0.0.1", 80, 0), (5, 80), (None, 80), (1.5, 80), ([], 80), ("127.0.0.1", "80"), ("127.0.0.1", None), ("127.0.0.1", 1.5), ("127.0.0.1", -1), ("127.0.0.1", 65536), ("127.0.0.1", 2 ** 31), ("127.0.0.1", 2 ** 70), ("127.0.0.1", -2 ** 31 - 1), ("a\0b", 80), (b"a\0b", 80), ("\xe9\0", 80), ("::1", 80), ("1.2.3", -1), ("256.1.1.1.1", 80) if False else ("<broadcast>", -1), ("", -1), ("\ud800", 80), ("." * 5, 80), ("a" * 64 + ".com", 80) if False else ("\xe9" * 64, 80), (bytearray(b"1.2.3.4"), -1), (memoryview(b"1.2.3.4"), 80), (type("S", (str,), {})("1.2.3.4"), -1), (type("T", (tuple,), {})(("1.2.3.4", -1))))
for family, extra in ((socket.AF_INET, ()), (socket.AF_INET6, (("::1", 80, "a"), ("::1", 80, None), ("::1", 80, 1.5), ("::1", 80, 0, "a"), ("::1", 80, 0, 0, 0), ("::1", 80, 2 ** 20), ("::1", 80, 2 ** 20 - 1, -1) if False else ("::1", -1, 2 ** 20), ("::1", 80, -1), ("::1", 80, 2 ** 32 + 2 ** 20), ("::1", 80, 2 ** 70), ("127.0.0.1", 80), ("::1", 65536, 0, 0), ("<broadcast>", 80), ("255.255.255.255", 80)))):
    t("of family %d" % family, lambda: [[(reached(s.bind, x), reached(s.connect, x), reached(s.connect_ex, x), reached(s.sendto, b"a", x), reached(s.sendto, b"a", 0, x), reached(s.sendmsg, [b"a"], [], 0, x)) for x in BAD_INET + extra] + [s.close()] for s in [S(family, socket.SOCK_DGRAM)]])
t("of AF_UNIX", lambda: [[(reached(s.bind, x), reached(s.connect, x), reached(s.connect_ex, x), reached(s.sendto, b"a", x)) for x in (5, None, 1.5, [], (), ("a", 1), "a" * 104, "a" * 200, b"a" * 104, "\xe9" * 52, "\ud800" * 200, bytearray(b"a" * 104), memoryview(b"a" * 104), type("P", (), {"__fspath__": lambda s: "x"})())] + [s.close()] for s in [S(socket.AF_UNIX, socket.SOCK_DGRAM)]])
t("of a family that is not known", lambda: [[attempt(f) for f in (lambda: s.bind(("a", 1)), lambda: s.connect("a"), lambda: s.connect_ex(5), lambda: s.sendto(b"a", None), lambda: s.getsockname(), lambda: s.getpeername(), lambda: s._accept(), lambda: s.recvfrom(1), lambda: s.recvmsg(1), lambda: s.recvfrom_into(bytearray(1)))] + [s.detach() >= 0, closing(a, b)] for a, b in [pair()] for s in [S(socket.AF_ROUTE if hasattr(socket, "AF_ROUTE") else 17, socket.SOCK_STREAM, 0, a.fileno())]])
t("how they are called", lambda: [[attempt(f, *x, **k) for f in (s.bind, s.connect, s.connect_ex) for x, k in (((), {}), ((1, 2), {}), ((), {"address": 1}))] + [attempt(s.sendto, *x, **k) for x, k in (((), {}), ((b"a",), {}), ((b"a", 0, ("1.2.3.4", 1), 5), {}), (("a", ("1.2.3.4", 1)), {}), ((5, ("1.2.3.4", 1)), {}), ((b"a", "x", ("1.2.3.4", 1)), {}), ((b"a", 1.5, ("1.2.3.4", 1)), {}), ((b"a", None, ("1.2.3.4", 1)), {}), ((b"a", 2 ** 31, ("1.2.3.4", 1)), {}))] + [attempt(f, 1) for f in (s._accept, s.getsockname, s.getpeername, s.fileno, s.detach, s.close)] + [s.close()] for s in [S(socket.AF_INET, socket.SOCK_DGRAM)]])
t("before it has one", lambda: [(s.getsockname(), attempt(s.getpeername), attempt(s._accept), attempt(s.recv, 1) if k == socket.SOCK_STREAM else None, attempt(s.send, b"a"), s.close()) for f in (socket.AF_INET, socket.AF_INET6, socket.AF_UNIX) for k in (socket.SOCK_STREAM, socket.SOCK_DGRAM) for s in [S(f, k)]])

print("---- the order of bytes")
INTS = (0, 1, 0x1234, 0xFFFF, 0x10000, 0x12345678, 0xFFFFFFFF, 0x100000000, 2 ** 64, 2 ** 70, -1, -2 ** 70, True, "a", 1.5, None, b"a", type("I", (), {"__index__": lambda s: 258})(), type("I", (), {"__index__": lambda s: -1})(), type("I", (), {"__index__": lambda s: 1 / 0})(), type("S", (int,), {})(258))
for f in (_socket.ntohs, _socket.htons, _socket.ntohl, _socket.htonl):
    t(f.__name__, lambda: [attempt(f, v) for v in INTS] + [attempt(f), attempt(f, 1, 2)])

print("---- addresses, written out and packed")
TEXTS = ("1.2.3.4", "0.0.0.0", "255.255.255.255", "127.1", "1.2.3", "1", "0x7f.1", "017.1.1.1", "256.1.1.1", "1.2.3.4.5", "1.2.3.4 ", " 1.2.3.4", "", "a", "1.2.3.a", "4294967295", "4294967296", "::1", "::", "1::2", "fe80::1%lo0", "::ffff:1.2.3.4", "1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7:8:9", ":::", "g::", "12345::", "\xe9", "\ud800", "1.2.3.4\0", "\0", 5, None, b"1.2.3.4", bytearray(b"1.2.3.4"), 1.5, [], type("S", (str,), {})("1.2.3.4"))
t("inet_aton", lambda: [attempt(_socket.inet_aton, v) for v in TEXTS] + [attempt(_socket.inet_aton), attempt(_socket.inet_aton, "1.2.3.4", 1), attempt(_socket.inet_aton, ip_addr="1.2.3.4")])
PACKED = (b"\1\2\3\4", b"\0\0\0\0", b"\xff\xff\xff\xff", b"", b"\1", b"\1\2\3", b"\1\2\3\4\5", bytes(16), bytes(15) + b"\1", bytes(range(16)), bytes(17), bytearray(b"\1\2\3\4"), memoryview(b"\1\2\3\4"), array.array("B", [1, 2, 3, 4]), array.array("I", [1]), memoryview(b"\1\2\3\4\5\6\7\10")[::2], "abcd", 5, None, [1, 2, 3, 4])
t("inet_ntoa", lambda: [attempt(_socket.inet_ntoa, v) for v in PACKED] + [attempt(_socket.inet_ntoa), attempt(_socket.inet_ntoa, b"abcd", 1), attempt(_socket.inet_ntoa, packed_ip=b"abcd")])
for family in (socket.AF_INET, socket.AF_INET6, socket.AF_UNIX, socket.AF_UNSPEC, 999, -1):
    t("inet_pton and inet_ntop, of family %d" % family, lambda: ([attempt(_socket.inet_pton, family, v) for v in TEXTS], [attempt(_socket.inet_ntop, family, v) for v in PACKED]))
t("how they are called", lambda: [attempt(f, *x, **k) for f in (_socket.inet_pton, _socket.inet_ntop) for x, k in (((), {}), ((2,), {}), ((2, "1.2.3.4", 3), {}), (("a", "1.2.3.4"), {}), ((1.5, "1.2.3.4"), {}), ((None, b"abcd"), {}), ((2 ** 31, "1.2.3.4"), {}), ((), {"af": 2, "ip": "1.2.3.4"}))])

print("---- looking things up, where there is nothing to ask anybody")
G = _socket.getaddrinfo
NUM = socket.AI_NUMERICHOST | socket.AI_NUMERICSERV
t("getaddrinfo", lambda: [attempt(G, *x, **k) for x, k in ((("1.2.3.4", 80, 0, 0, 0, NUM), {}), (("1.2.3.4", 80, socket.AF_INET, socket.SOCK_STREAM, 0, NUM), {}), (("1.2.3.4", "80", socket.AF_INET, socket.SOCK_DGRAM, 0, NUM), {}), (("1.2.3.4", b"80", socket.AF_INET, socket.SOCK_STREAM, 0, NUM), {}), ((b"1.2.3.4", 80, socket.AF_INET, socket.SOCK_STREAM, 0, NUM), {}), (("::1", 80, 0, socket.SOCK_STREAM, 0, NUM), {}), (("::1", 80, socket.AF_INET6, socket.SOCK_STREAM, socket.IPPROTO_TCP, NUM), {}), (("fe80::1%lo0", 80, socket.AF_INET6, socket.SOCK_STREAM, 0, NUM), {}),
                                                            (("1.2.3.4", None, socket.AF_INET, socket.SOCK_STREAM, 0, NUM), {}), (("1.2.3.4", 0, socket.AF_INET, socket.SOCK_STREAM, 0, NUM), {}), (("1.2.3.4", "0", socket.AF_INET, socket.SOCK_STREAM, 0, NUM), {}), ((None, 80, socket.AF_INET, socket.SOCK_STREAM, 0, NUM), {}), ((None, 80, socket.AF_INET, socket.SOCK_STREAM, 0, NUM | socket.AI_PASSIVE), {}), ((None, 80, socket.AF_INET6, socket.SOCK_STREAM, 0, NUM | socket.AI_PASSIVE), {}), (("1.2.3.4", 80), {"family": socket.AF_INET, "type": socket.SOCK_STREAM, "proto": 0, "flags": NUM}), ((), {"host": "1.2.3.4", "port": 80, "type": socket.SOCK_STREAM, "flags": NUM}),
                                                            (("1.2.3.4", True, socket.AF_INET, socket.SOCK_STREAM, 0, NUM), {}), (("1.2.3.4", type("I", (), {"__index__": lambda s: 81})(), socket.AF_INET, socket.SOCK_STREAM, 0, NUM), {}), (("1.2.3.4", 65535, socket.AF_INET, socket.SOCK_STREAM, 0, NUM), {}), (("1.2.3.4", "http", socket.AF_INET, socket.SOCK_STREAM, 0, socket.AI_NUMERICHOST), {}), (("1.2.3.4", 80, socket.AF_INET, socket.SOCK_STREAM, 0, NUM | socket.AI_CANONNAME), {}), (("", 80, socket.AF_INET, socket.SOCK_STREAM, 0, NUM), {}))])
t("what goes wrong with it", lambda: [attempt(G, *x, **k) for x, k in (((), {}), (("1.2.3.4",), {}), ((5, 80), {}), ((1.5, 80), {}), (([], 80), {}), ((bytearray(b"1.2.3.4"), 80), {}), (("1.2.3.4", 1.5), {}), (("1.2.3.4", []), {}), (("1.2.3.4", bytearray(b"80")), {}), (("1.2.3.4", 80, "a"), {}), (("1.2.3.4", 80, 0, "a"), {}), (("1.2.3.4", 80, 0, 0, "a"), {}), (("1.2.3.4", 80, 0, 0, 0, "a"), {}), (("1.2.3.4", 80, 0, 0, 0, 0, 0), {}), (("1.2.3.4", 80), {"x": 1}), (("1.2.3.4", 80), {"host": "a"}), (("1.2.3.4", 80), {"famly": 2}), (("1.2.3.4", 80, 2 ** 31), {}), (("1.2.3.4", 80, None), {}),
                                                                        (("not a number", 80, 0, 0, 0, NUM), {}), (("1.2.3.4", "http", 0, 0, 0, NUM), {}), (("1.2.3.4", 80, 999, 0, 0, NUM), {}), (("1.2.3.4", 80, 0, 999, 0, NUM), {}), (("1.2.3.4", 80, socket.AF_INET6, 0, 0, NUM), {}), (("::1", 80, socket.AF_INET, 0, 0, NUM), {}), ((None, None, 0, 0, 0, NUM), {}), (("1.2.3.4", 80, 0, 0, 0, -1), {}), (("a\0b", 80, 0, 0, 0, NUM), {}), (("\ud800", 80, 0, 0, 0, NUM), {}), (("1.2.3.4", "\ud800", 0, 0, 0, NUM), {}), (("." * 3, 80, 0, 0, 0, NUM), {}), (("\xe9" * 64, 80, 0, 0, 0, NUM), {}), (("1.2.3.4", type("I", (), {"__index__": lambda s: 1 / 0})(), 0, 0, 0, NUM), {}))])
t("what it raises", lambda: [(type(e).__name__, e.errno == socket.EAI_NONAME, e.args[0] == socket.EAI_NONAME, type(e.args[1]).__name__, e.strerror == e.args[1], e.filename, isinstance(e, OSError)) for e in [attempt_exception(G, "not a number", 80, 0, 0, 0, NUM)]])
N = _socket.getnameinfo
NN = socket.NI_NUMERICHOST | socket.NI_NUMERICSERV
t("getnameinfo", lambda: [attempt(N, *x, **k) for x, k in (((("1.2.3.4", 80), NN), {}), ((("::1", 80), NN), {}), ((("::1", 80, 0), NN), {}), ((("::1", 80, 0, 0), NN), {}), ((("::1", 80, 5, 0), NN), {}), ((("fe80::1", 80, 0, 1), NN), {}), ((("::1", 80, 2 ** 20 - 1, 0), NN), {}), ((("1.2.3.4", 80), socket.NI_NUMERICHOST), {}), ((("1.2.3.4", 80), socket.NI_NUMERICHOST | socket.NI_DGRAM), {}), ((("1.2.3.4", 0), NN), {}), ((("1.2.3.4", 65535), NN), {}), ((("1.2.3.4", 65536), NN), {}), ((("1.2.3.4", -1), NN), {}), ((("1.2.3.4", 80, 0), NN), {}), ((("1.2.3.4", 80, 0, 0), NN), {}), ((type("T", (tuple,), {})(("1.2.3.4", 80)), NN), {}),
                                                            ((), {}), ((("1.2.3.4", 80),), {}), ((("1.2.3.4", 80), NN, 0), {}), ((["1.2.3.4", 80], NN), {}), (("1.2.3.4", NN), {}), ((None, NN), {}), (((), NN), {}), ((("1.2.3.4",), NN), {}), ((("1.2.3.4", 80, 0, 0, 0), NN), {}), (((5, 80), NN), {}), (((b"1.2.3.4", 80), NN), {}), (((None, 80), NN), {}), ((("1.2.3.4", "80"), NN), {}), ((("1.2.3.4", 1.5), NN), {}), ((("1.2.3.4", 2 ** 31), NN), {}), ((("::1", 80, "a"), NN), {}), ((("::1", 80, 2 ** 20), NN), {}), ((("::1", 80, -1), NN), {}), ((("::1", 80, 0, "a"), NN), {}), ((("1.2.3.4", 80), "a"), {}), ((("1.2.3.4", 80), 1.5), {}), ((("1.2.3.4", 80), 2 ** 31), {}),
                                                            ((("not a number", 80), NN), {}), ((("a\0b", 80), NN), {}), ((("\ud800", 80), NN), {}), ((("", 80), NN), {}), ((), {"sockaddr": ("1.2.3.4", 80), "flags": NN}))])
t("services", lambda: [attempt(_socket.getservbyname, *x, **k) for x, k in ((("http",), {}), (("http", "tcp"), {}), (("http", "udp"), {}), (("domain", "udp"), {}), (("ssh",), {}), (("https", "tcp"), {}), (("nope",), {}), (("http", "nope"), {}), (("",), {}), (("http", ""), {}), ((), {}), (("http", "tcp", 1), {}), ((5,), {}), ((None,), {}), ((b"http",), {}), (("http", 5), {}), (("http", None), {}), (("http", b"tcp"), {}), (("a\0b",), {}), (("http", "a\0"), {}), (("\ud800",), {}), (("\xe9",), {}), ((), {"servicename": "http"}))]
  + [attempt(_socket.getservbyport, *x, **k) for x, k in (((80,), {}), ((80, "tcp"), {}), ((80, "udp"), {}), ((53, "udp"), {}), ((22,), {}), ((443, "tcp"), {}), ((0,), {}), ((65535,), {}), ((65536,), {}), ((-1,), {}), ((80, "nope"), {}), ((), {}), ((80, "tcp", 1), {}), (("80",), {}), ((1.5,), {}), ((None,), {}), ((80, 5), {}), ((80, None), {}), ((80, "a\0"), {}), ((2 ** 31,), {}), ((True,), {}), ((65536, 5), {}), ((), {"port": 80}))])
t("protocols", lambda: [attempt(_socket.getprotobyname, *x, **k) for x, k in ((("tcp",), {}), (("udp",), {}), (("icmp",), {}), (("ip",), {}), (("ipv6",), {}), (("TCP",), {}), (("nope",), {}), (("",), {}), ((), {}), (("tcp", 1), {}), ((5,), {}), ((None,), {}), ((b"tcp",), {}), (("a\0",), {}), (("\ud800",), {}), ((), {"name": "tcp"}))])
t("hosts that are numbers", lambda: [attempt(_socket.gethostbyname, v) for v in ("1.2.3.4", "0.0.0.0", "255.255.255.255", "<broadcast>", "", b"1.2.3.4", bytearray(b"1.2.3.4"), "127.1", "::1", 5, None, 1.5, [], memoryview(b"1.2.3.4"), "a\0b", b"a\0b", "\ud800", "." * 3, "\xe9" * 64, type("S", (str,), {})("1.2.3.4"))] + [attempt(f, *x, **k) for f in (_socket.gethostbyname, _socket.gethostbyname_ex, _socket.gethostbyaddr) for x, k in (((), {}), (("a", "b"), {}), ((5,), {}), ((None,), {}), (("a\0",), {}), ((), {"host": "a"}))] + [attempt(_socket.gethostbyname_ex, "1.2.3.4")])
t("this host", lambda: (type(_socket.gethostname()).__name__, len(_socket.gethostname()) > 0, _socket.gethostname() == os.uname().nodename, attempt(_socket.gethostname, 1), attempt(_socket.gethostname, x=1), [attempt(_socket.sethostname, *x, **k) for x, k in (((), {}), ((5,), {}), ((None,), {}), (("a", "b"), {}), (("a\0b",), {}), ((1.5,), {}), (([],), {}), ((), {"name": "a"}))], [type(attempt_exception(_socket.sethostname, v)).__name__ for v in (_socket.gethostname(), os.fsencode(_socket.gethostname()))]))
t("interfaces", lambda: (type(_socket.if_nameindex()).__name__, all(type(i) is tuple and len(i) == 2 and type(i[0]) is int and type(i[1]) is str for i in _socket.if_nameindex()), all(_socket.if_nametoindex(n) == i and _socket.if_indextoname(i) == n for i, n in _socket.if_nameindex()), _socket.if_nametoindex(os.fsencode(_socket.if_nameindex()[0][1])) == _socket.if_nameindex()[0][0], "lo0" in dict((n, i) for i, n in _socket.if_nameindex()) or "lo" in dict((n, i) for i, n in _socket.if_nameindex()),
                          [attempt(_socket.if_nametoindex, *x, **k) for x, k in ((("nope",), {}), (("",), {}), ((), {}), (("a", "b"), {}), ((5,), {}), ((None,), {}), (("a\0",), {}), ((b"nope",), {}), (("\ud800",), {}), (("x" * 100,), {}), ((), {"oname": "a"}))], [attempt(_socket.if_indextoname, *x, **k) for x, k in (((0,), {}), ((99999,), {}), ((), {}), ((1, 2), {}), (("a",), {}), ((None,), {}), ((1.5,), {}), ((-1,), {}), ((2 ** 32,), {}), ((2 ** 32 - 1,), {}), ((2 ** 70,), {}), ((), {"if_index": 1}))], attempt(_socket.if_nameindex, 1)))

print("---- what is told to whoever is listening")
heard = []
sys.addaudithook(lambda event, args: heard.append((event, tuple(shown(a) if isinstance(a, S) else a for a in args))) if event.startswith("socket.") and listening else None)
listening = True
for f in (lambda: S(socket.AF_UNIX, socket.SOCK_DGRAM).close(), lambda: S().close(), lambda: attempt(S, 999, 998, 997), lambda: G("1.2.3.4", 80, 2, 1, 6, NUM), lambda: G(None, "80", flags=NUM), lambda: attempt(G, b"x", None, 0, 0, 0, NUM), lambda: N(("1.2.3.4", 80), NN), lambda: _socket.gethostname(), lambda: _socket.gethostbyname("1.2.3.4"), lambda: _socket.gethostbyname(b"1.2.3.4"), lambda: _socket.gethostbyname_ex("1.2.3.4"), lambda: _socket.getservbyname("http"), lambda: _socket.getservbyname("http", "tcp"), lambda: _socket.getservbyport(80), lambda: _socket.getservbyport(80, "tcp"),
          lambda: attempt(_socket.sethostname, "\0"), lambda: attempt(_socket.getservbyport, -1), lambda: attempt(_socket.gethostbyname, 5)):
    del heard[:]
    f()
    print(ascii(heard))
for a, b in [pair(socket.SOCK_DGRAM)]:
    for f in (lambda: a.sendmsg([b"x"]), lambda: a.sendmsg([b"x"], [], 0, None), lambda: attempt(a.sendmsg, [b"x"], [], 0, 5), lambda: attempt(a.sendmsg, 5), lambda: attempt(a.bind, 5), lambda: attempt(a.connect, 5), lambda: attempt(a.sendto, b"x", 5), lambda: attempt(a.bind, "a" * 200)):
        del heard[:]
        f()
        print(ascii(heard))
    closing(a, b)
listening = False
stop = []
sys.addaudithook(lambda event, args: (_ for _ in ()).throw(RuntimeError("not allowed: " + event)) if stop and event.startswith("socket.") else None)
stop.append(1)
t("who can say no", lambda: [attempt(f) for f in (S, lambda: G("1.2.3.4", 80, 0, 0, 0, NUM), lambda: N(("1.2.3.4", 80), NN), _socket.gethostname, lambda: _socket.gethostbyname("1.2.3.4"), lambda: _socket.gethostbyname_ex("1.2.3.4"), lambda: _socket.gethostbyaddr("1.2.3.4"), lambda: _socket.getservbyname("http"), lambda: _socket.getservbyport(80), lambda: _socket.sethostname("x"))])
del stop[:]
a, b = pair(socket.SOCK_DGRAM)
stop.append(1)
t("of what a socket does too", lambda: [attempt(f) for f in (lambda: a.bind("x"), lambda: a.connect("x"), lambda: a.connect_ex("x"), lambda: a.sendto(b"x", "x"), lambda: a.sendmsg([b"x"]), lambda: a.sendmsg([b"x"], [], 0, "x"), lambda: a.send(b"x"))])
del stop[:]
closing(a, b)

print("---- socket, which is written over it")
K = socket.socket
t("what it is", lambda: (K.__mro__[1] is S, K.__slots__, [(re.sub(r"fd=\d+", "fd=N", repr(s)), s.family, s.type, s.proto, type(s.family).__name__, type(s.type).__name__, s.get_inheritable(), s.close(), re.sub(r"fd=-?\d+", "fd=N", repr(s)), s._closed) for s in [K()]], [(type(x).__name__, repr(x)) for x in (socket.AF_INET, socket.SOCK_STREAM, socket.MSG_PEEK, socket.AI_PASSIVE, socket.AF_UNIX, socket.SOCK_DGRAM)], socket.SOMAXCONN, socket.has_ipv6, socket.has_dualstack_ipv6() in (True, False)))
t("a pair", lambda: [(type(a).__name__, a.family.name, a.type.name, a.send(b"x"), b.recv(1), re.sub(r"fd=\d+", "fd=N", repr(a)), closing(a, b)) for a, b in [socket.socketpair()]])
t("with", lambda: [(s.__enter__() is s, s.__exit__(None, None, None), s._closed, s.fileno()) for s in [K()]])
t("dup and fromfd", lambda: [(type(d).__name__, d.fileno() != a.fileno(), d.family.name, d.gettimeout(), d.send(b"dup"), b.recv(5), type(f).__name__, f.send(b"fromfd"), b.recv(10), closing(a, b, d, f)) for a, b in [socket.socketpair()] for _ in [a.settimeout(3)] for d in [a.dup()] for f in [socket.fromfd(a.fileno(), a.family, a.type)]])
t("makefile", lambda: [(type(r).__name__, type(w).__name__, w.write("line one\nline two\n"), w.flush(), r.readline(), r.readline(), type(rb).__name__, a.send(b"raw"), rb.read(3), type(rb.raw).__name__, rb.raw.readable(), rb.raw.writable(), rb.raw.fileno() == b.fileno(), rb.raw.name == b.fileno(), rb.raw.mode, b._io_refs, closing(r, rb), b._io_refs, w.close(), closing(a, b)) for a, b in [socket.socketpair()] for w in [a.makefile("w")] for r in [b.makefile("r")] for rb in [b.makefile("rb")]])
t("which keeps it open", lambda: [(b.close(), b._closed, b.fileno() >= 0, a.send(b"still"), f.read(5), f.close(), b.fileno(), closing(a)) for a, b in [socket.socketpair()] for f in [b.makefile("rb")]])
t("makefile, and not waiting", lambda: [(b.settimeout(0.1), attempt(f.read, 1), attempt(f.read, 1), b.setblocking(False), attempt(u.read, 1), attempt(u.readinto, bytearray(1)), attempt(b.makefile, "x"), attempt(b.makefile, "r", 0), closing(f, u, a, b)) for a, b in [socket.socketpair()] for f in [b.makefile("rb")] for u in [b.makefile("rb", 0)]])
t("open files, sent", lambda: [(socket.send_fds(a, [b"m"], [r]), [(m, len(fds), fl, ad, os.write(w, b"through"), os.read(fds[0], 10), os.close(fds[0])) for m, fds, fl, ad in [socket.recv_fds(b, 10, 4)]], os.close(r), os.close(w), closing(a, b)) for a, b in [socket.socketpair()] for r, w in [os.pipe()]])
t("sendfile", lambda: [(f.write(b"0123456789" * 100), f.flush(), f.seek(0), a.sendfile(f), len(b.recv(1000, socket.MSG_WAITALL)), f.seek(0), a.sendfile(f, 990), b.recv(100), a.sendfile(f, 0, 5), b.recv(100), f.close(), closing(a, b)) for a, b in [socket.socketpair()] for f in [__import__("tempfile").TemporaryFile()]])
t("select and the like", lambda: [(select.select([a, b], [a, b], [], 0) == ([], [a, b], []), a.send(b"x"), select.select([a, b], [], [], 1) == ([b], [], []), [(p.register(b, select.POLLIN), [(fd == b.fileno(), ev) for fd, ev in p.poll(1000)]) for p in [select.poll()]], closing(a, b)) for a, b in [socket.socketpair()]])
t("selectors", lambda: [(sel.register(b, 1, "data").data, a.send(b"x"), [(k.fileobj is b, k.data, ev) for k, ev in sel.select(1)], sel.unregister(b).data, sel.close(), closing(a, b)) for a, b in [socket.socketpair()] for sel in [__import__("selectors").DefaultSelector()]])
t("what does not need anybody", lambda: (socket.getfqdn("1.2.3.4") if False else None, attempt(socket.create_connection, ("1.2.3.4", -1)), attempt(socket.create_server, ("1.2.3.4", 80), family=socket.AF_UNIX) if False else None, reached(socket.create_server, ("", 0), family=999), socket.getaddrinfo("1.2.3.4", 80, type=socket.SOCK_STREAM, flags=NUM), attempt(socket.socket, fileno=-1), attempt(socket.close, -1), socket.errorTab if hasattr(socket, "errorTab") else None, socket.timeout is TimeoutError, socket.error is OSError, sorted(n for n in socket.__all__ if not hasattr(socket, n))))
t("not pickled", lambda: [(attempt(pickle.dumps, s), attempt(copy.copy, s), attempt(s.__getstate__), s.close()) for s in [K()]])
