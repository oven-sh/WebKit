# io.open() and io.open_code()
import _codecs
import _io
import _warnings
import posix
import sys

sys.unraisablehook = lambda unraisable: None
_warnings._acquire_lock()
_warnings.filters.insert(0, ("error", None, RuntimeWarning, None, 0))
_warnings._filters_mutated_lock_held()
_warnings._release_lock()


def show(e):
    context = e.__context__
    text = str(e).replace(D, "D/")
    return type(e).__name__ + ": " + text + (" | after %s: %s" % (type(context).__name__, context) if context is not None else "")


def attempt(f, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return show(e)


def t(label, f):
    r = attempt(f)
    print(label.replace(D, "D/"), "=>", (r if isinstance(r, str) else ascii(r)).replace(D, "D/"))


class Decoder:
    def __init__(self, errors="strict"): self.errors, self.buffer = errors, b""
    def decode(self, input, final=False):
        data = self.buffer + input
        result, consumed = _codecs.utf_8_decode(data, self.errors, final)
        self.buffer = data[consumed:]
        return result
    def reset(self): self.buffer = b""
    def getstate(self): return (self.buffer, 0)
    def setstate(self, state): self.buffer = state[0]


class Encoder:
    def __init__(self, errors="strict"): self.errors = errors
    def encode(self, input, final=False): return _codecs.utf_8_encode(input, self.errors)[0]
    def reset(self): pass
    def getstate(self): return 0
    def setstate(self, state): pass


class Info(tuple):
    name = "utf-8"
    incrementalencoder = Encoder
    incrementaldecoder = Decoder


info = Info((_codecs.utf_8_encode, _codecs.utf_8_decode, None, None))
search = lambda name: info if name == "test_utf_8" else None
_codecs.register(search)
E = "test_utf_8"
D = posix.environ.get(b"TMPDIR", b"/tmp").decode().rstrip("/") + "/jsc-python-io-open-" + str(posix.getpid()) + "/"
posix.mkdir(D)
p = D + "file"


def layers(f):
    "What it is made of, from the outside in, and it is closed."
    out = []
    x = f
    while x is not None:
        out.append((type(x).__name__, getattr(x, "mode", None), getattr(x, "name", None) if not isinstance(getattr(x, "name", None), int) else "a descriptor"))
        x = getattr(x, "buffer", None) or getattr(x, "raw", None)
    extra = (f.readable(), f.writable(), f.seekable(), getattr(f, "line_buffering", None), getattr(f, "encoding", None), getattr(f, "errors", None))
    f.close()
    return out, extra


def put(data=b"hello\nworld\n"):
    f = _io.open(p, "wb")
    f.write(data)
    f.close()


def get():
    f = _io.open(p, "rb")
    data = f.read()
    f.close()
    return data


print("---- modes")
LETTERS = "rwxab+t"
modes = [""] + [a for a in LETTERS] + [a + b for a in LETTERS for b in LETTERS] + ["rb+", "r+b", "+rb", "wb+", "ab+", "xb+", "rt+", "w+t", "a+t", "x+t", "rwb", "rbt", "r+b+", "rbb", "U", "rU", "Ur", "R", "r ", " r", "rz", "b+", "t+", "+bt", "r\n"]
for mode in modes:
    def go():
        put()
        if "x" in mode:
            posix.unlink(p)
        return layers(_io.open(p, mode, encoding=None if "b" in mode else E))
    t("open(p, %r)" % mode, go)
put()

print("---- what it takes")
for label, args, kwargs in (("()", (), {}), ("mode=5", (p, 5), {}), ("mode=None", (p, None), {}), ("mode=b'r'", (p, b"r"), {}), ("a zero in the mode", (p, "r\0b"), {}), ("buffering='a'", (p, "rb", "a"), {}), ("buffering=None", (p, "rb", None), {}), ("buffering=1.5", (p, "rb", 1.5), {}), ("buffering=2**40", (p, "rb", 2 ** 40), {}),
                            ("encoding=5", (p, "r", -1, 5), {}), ("encoding=b''", (p, "r", -1, b"a"), {}), ("errors=5", (p, "r", -1, E, 5), {}), ("newline=5", (p, "r", -1, E, None, 5), {}), ("newline='x'", (p, "r", -1, E, None, "x"), {}), ("an encoding that there is not", (p, "r", -1, "test_none_such"), {}), ("nine", (p, "r", -1, E, None, None, True, None, 1), {}),
                            ("another name", (p,), {"other": 1}), ("binary with an encoding", (p, "rb"), {"encoding": E}), ("with errors", (p, "rb"), {"errors": "strict"}), ("with a newline", (p, "rb"), {"newline": ""}), ("with a line at a time", (p, "rb", 1), {}), ("text that is not buffered", (p, "r", 0), {"encoding": E}),
                            ("by name", (), {"file": p, "mode": "r", "buffering": -1, "encoding": E, "errors": "ignore", "newline": "", "closefd": True, "opener": None}), ("a zero in the encoding", (p, "r", -1, "a\0b"), {}), ("a zero in the errors", (p, "r", -1, E, "a\0b"), {}), ("a zero in the newline", (p, "r", -1, E, None, "\0"), {})):
    t(label, lambda: layers(_io.open(*args, **kwargs)))
for buffering in (-1, -2, -100, 0, 1, 2, 10, 4096, 10 ** 6):
    t("buffering=%d, of bytes" % buffering, lambda: [(type(f).__name__, f.__sizeof__() - _io.BufferedReader.__basicsize__ if hasattr(f, "raw") else None, f.close()) for f in [_io.open(p, "rb", buffering)]])
    t("buffering=%d, of text" % buffering, lambda: [(type(f).__name__, f.line_buffering, f.buffer.__sizeof__() - _io.BufferedReader.__basicsize__, f.close()) for f in [_io.open(p, "r", buffering, E)]])


class P:
    def __init__(self, value): self.value = value
    def __fspath__(self): return self.value
    def __repr__(self): return "P(%r)" % (self.value,)


class Index:
    def __init__(self, value): self.value = value
    def __index__(self): return self.value
    def __repr__(self): return "Index()"


class Float:
    def __float__(self): return 1.0
    def __repr__(self): return "Float()"


for label, value in (("bytes", p.encode()), ("what has __fspath__", P(p)), ("that returns bytes", P(p.encode())), ("that returns an int", P(5)), ("None", None), ("a list", [p]), ("a bytearray", bytearray(p.encode())), ("a float", 1.5), ("a complex", 1j), ("what has __float__", Float()), ("-1", -1), ("what is not open", 9999), ("a zero in it", "a\0b"),
                     ("''", ""), ("a directory", D), ("what is not there", D + "absent"), ("True", True), ("2 ** 70", 2 ** 70)):
    t("open(%s)" % label, lambda: layers(_io.open(value, "rb")))
t("what is open already", lambda: [(layers(_io.open(fd, "rb")), attempt(posix.fstat, fd)[:7]) for fd in [posix.open(p, posix.O_RDONLY)]])
t("by its __index__", lambda: [(layers(_io.open(Index(fd), "rb")),) for fd in [posix.open(p, posix.O_RDONLY)]])
t("and left open", lambda: [(layers(_io.open(fd, "rb", closefd=False)), posix.fstat(fd).st_size, posix.close(fd)) for fd in [posix.open(p, posix.O_RDONLY)]])
t("closefd=False with a name", lambda: _io.open(p, "rb", closefd=False))
t("closefd of other kinds", lambda: [(attempt(lambda: layers(_io.open(fd, "rb", closefd=v))[1][:1]), attempt(posix.close, fd)) for v in (0, [], "yes", None) for fd in [posix.open(p, posix.O_RDONLY)]])
asked = []


def opener(path, flags):
    asked.append((path, flags & 3, bool(flags & posix.O_CREAT), bool(flags & posix.O_CLOEXEC)))
    return posix.open(path, flags, 0o600)


t("opener", lambda: (layers(_io.open(p, "rb", opener=opener)), layers(_io.open(p, "w", encoding=E, opener=opener)), layers(_io.open(p.encode(), "ab+", opener=opener)), layers(_io.open(P(p), "rb", opener=opener)), asked))
for label, value in (("returns -1", lambda *a: -1), ("returns None", lambda *a: None), ("returns a str", lambda *a: "a"), ("raises", lambda *a: 1 / 0), ("cannot be called", 5), ("returns what is not open", lambda *a: 9999)):
    t("an opener that " + label, lambda: layers(_io.open(p, "rb", opener=value)))

print("---- what comes of it")
put()
t("read", lambda: [(f.read(), f.close()) for f in [_io.open(p, "r", encoding=E)]])
t("lines", lambda: [(list(f), f.close()) for f in [_io.open(p, encoding=E)]])
t("write", lambda: [(f.write("h\xe9llo\n"), f.close(), get()) for f in [_io.open(p, "w", encoding=E)]])
t("append", lambda: [(f.tell(), f.write("more\n"), f.close(), get()) for f in [_io.open(p, "a", encoding=E)]])
t("both", lambda: [(f.readline(), f.seek(0, 2), f.write("end"), f.seek(0), f.read(), f.close()) for f in [_io.open(p, "r+", encoding=E)]])
t("newlines", lambda: [(f.write("a\nb\n"), f.close(), get(), [(g.read(), g.close()) for g in [_io.open(p, "r", encoding=E)]], [(g.read(), g.close()) for g in [_io.open(p, "r", encoding=E, newline="")]]) for f in [_io.open(p, "w", encoding=E, newline="\r\n")]])
t("a line at a time", lambda: [(f.write("ab"), get(), f.write("c\n"), get(), f.write("d"), get(), f.close(), get()) for f in [_io.open(p, "w", 1, E)]])
t("not buffered", lambda: [(f.write(b"ab"), get(), f.close()) for f in [_io.open(p, "wb", 0)]])
t("with", lambda: [(f.closed, f.__exit__(None, None, None), f.closed, f.buffer.closed, f.buffer.raw.closed) for f in [_io.open(p, encoding=E).__enter__()]])
t("repr", lambda: [(repr(f), repr(f.buffer), repr(f.buffer.raw), f.close(), repr(f)) for f in [_io.open(p, "r", encoding=E)]])
t("x when it is there", lambda: _io.open(p, "x", encoding=E))
t("what was opened is closed if the rest fails", lambda: [(attempt(_io.open, p, "r", -1, "test_none_such", opener=lambda path, flags: (kept.append(posix.open(path, flags)), kept[-1])[1]), attempt(posix.fstat, kept[0])) for kept in [[]]])
told = []
listening = [True]
sys.addaudithook(lambda event, args: told.append((event, tuple(a.replace(D, "D/") if isinstance(a, str) else a for a in args))) if listening[0] and event == "open" else None)
t("who is told", lambda: (_io.open(p, "rb").close(), _io.open(p, "r+", encoding=E).close(), _io.open_code(p).close(), [(e, a[:2]) for e, a in told]))
listening[0] = False

print("---- open_code")
put(b"code")
t("open_code", lambda: [(type(f).__name__, f.mode, f.name, f.read(), f.close()) for f in [_io.open_code(p)]])
for arg in (p.encode(), P(p), 5, None, D + "absent", "", "a\0b"):
    t("open_code(%r)" % (arg,), lambda: _io.open_code(arg))
t("by name", lambda: (_io.open_code(path=p).close(), attempt(_io.open_code), attempt(_io.open_code, p, "rb")))
for name in posix.listdir(D):
    posix.unlink(D + name)
posix.rmdir(D)
_codecs.unregister(search)
