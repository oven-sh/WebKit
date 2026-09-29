# io.FileIO, and the classes that the rest of io is derived from.
import _io
import _warnings
import posix
import sys
from _io import FileIO, _IOBase, _RawIOBase, _BufferedIOBase, _TextIOBase, UnsupportedOperation

# What is let go of while it is open is closed when it is found that nothing refers to it, and when that is is not the same everywhere.
sys.unraisablehook = lambda unraisable: None
# What is warned of is raised, so that it is seen here.
_warnings._acquire_lock()
_warnings.filters.insert(0, ("error", None, RuntimeWarning, None, 0))
_warnings._filters_mutated_lock_held()
_warnings._release_lock()
D = posix.environ.get(b"TMPDIR", b"/tmp").decode().rstrip("/") + "/jsc-python-file-io-" + str(posix.getpid()) + "/"
posix.mkdir(D)


def t(label, f):
    try:
        r = f()
    except BaseException as e:
        r = type(e).__name__ + ": " + str(e).replace(D, "D/")
    print(label, "=>", (r if isinstance(r, str) else repr(r)).replace(D, "D/"))


print("---- classes")
for c in (_IOBase, _RawIOBase, _BufferedIOBase, _TextIOBase, FileIO, UnsupportedOperation):
    t(c.__name__, lambda: (c.__module__, c.__qualname__, [b.__name__ for b in c.__mro__], sorted(k for k in vars(c) if k != "__doc__"), c.__dictoffset__ != 0, c.__weakrefoffset__ != 0, bool(c.__flags__ & 1 << 10)))
print("---- _IOBase")
b = _IOBase()
for name, args in (("seek", (0,)), ("seek", (0, 1)), ("seek", ("a",)), ("seek", ()), ("seek", (0, 0, 0)), ("tell", ()), ("truncate", ()), ("truncate", (1,)), ("truncate", ("a",)), ("flush", ()), ("seekable", ()), ("readable", ()), ("writable", ()),
                   ("_checkClosed", ()), ("_checkSeekable", ()), ("_checkReadable", ()), ("_checkWritable", ()), ("fileno", ()), ("isatty", ()), ("readline", ()), ("readlines", ()), ("writelines", ([],)), ("writelines", ([b"a"],)), ("__next__", ()),
                   ("readline", ("a",)), ("readline", (None,)), ("readline", (1.5,)), ("readlines", ("a",))):
    t("%s%r" % (name, args), lambda: getattr(b, name)(*args))
t("closed", lambda: (b.closed, vars(b), b.close(), b.closed, vars(b), b.close()))
for name in ("flush", "isatty", "__enter__", "__iter__", "_checkClosed", "readline", "seekable", "fileno", "tell"):
    t(name + " when closed", lambda: getattr(b, name)())
t("writelines when closed", lambda: b.writelines([]))
t("with", lambda: [(x.closed, x.__exit__(), x.closed) for x in [_IOBase().__enter__()]])
t("closed cannot be set", lambda: setattr(_IOBase(), "closed", 1))
t("attributes", lambda: (setattr(b, "x", 1), b.x, b.__dict__))
t("__dict__ cannot be set", lambda: setattr(b, "__dict__", {}))
t("__del__", lambda: [(x.__del__(), x.closed, vars(x)) for x in [_IOBase()]])


class Raw(_RawIOBase):
    def __init__(self, data, chunk=3): self.data, self.chunk, self.log = data, chunk, []
    def readable(self): return True
    def readinto(self, b):
        self.log.append(("readinto", type(b).__name__, len(b)))
        n = min(len(b), self.chunk, len(self.data))
        b[:n] = self.data[:n]
        self.data = self.data[n:]
        return n


print("---- _RawIOBase")
t("read", lambda: [(r.read(2), r.read(10), r.read(0), r.read(), r.log) for r in [Raw(b"abcdefghijkl")]])
t("readall", lambda: [(r.readall(), r.readall(), len(r.log), r.log[0]) for r in [Raw(b"abcdefg")]])
t("readline", lambda: [(r.readline(), r.readline(3), r.readline(), r.readline(), r.readline()) for r in [Raw(b"ab\ncdefg\n\nh")]])
t("readlines", lambda: (Raw(b"a\nb\nc").readlines(), Raw(b"a\nbb\nccc\nd").readlines(2), Raw(b"a\nbb\nccc\nd").readlines(3), Raw(b"a\nb").readlines(None), Raw(b"a\nb").readlines(0)))
t("iteration", lambda: (list(Raw(b"a\nb\n\nc")), next(Raw(b"x\ny"))))
t("read('a')", lambda: Raw(b"").read("a"))
t("read(None)", lambda: Raw(b"").read(None))
t("read(1, 2)", lambda: Raw(b"").read(1, 2))
t("the base's readinto and write", lambda: _RawIOBase().readinto(bytearray(1)))
t("write", lambda: _RawIOBase().write(b""))
t("read of the base", lambda: _RawIOBase().read(1))
for label, value in (("None", None), ("too many", 100), ("negative", -1), ("a str", "a"), ("huge", 2 ** 70), ("a float", 1.5)):
    class Odd(_RawIOBase):
        def readinto(self, b): return value
    t("readinto that returns " + label, lambda: Odd().read(4))
for label, value in (("None at once", [None]), ("None later", [b"ab", None]), ("a str", ["a"]), ("a bytearray", [bytearray(b"a")]), ("bytes", [b"ab", b"c", b""])):
    class Reads(_RawIOBase):
        def __init__(self): self.values = list(value)
        def read(self, n=-1): return self.values.pop(0)
    t("readall with read that returns " + label, lambda: Reads().readall())


class Peeks(Raw):
    def peek(self, n):
        self.log.append(("peek", n))
        return self.data[:5]
    def read(self, n=-1):
        self.log.append(("read", n))
        r, self.data = self.data[:n], self.data[n:]
        return r


t("readline with peek", lambda: [(r.readline(), r.readline(2), r.readline(), r.log) for r in [Peeks(b"ab\ncdefghi\nj")]])


class BadPeek(Raw):
    def peek(self, n): return "text"


t("peek that returns a str", lambda: BadPeek(b"a").readline())


class BadRead(_RawIOBase):
    def read(self, n=-1): return "text"


t("read that returns a str", lambda: BadRead().readline())

print("---- _BufferedIOBase and _TextIOBase")
for c in (_BufferedIOBase, _TextIOBase):
    for name, args in (("detach", ()), ("read", ()), ("read", (1,)), ("read", ("a",)), ("read", (None,)), ("read1", ()), ("readline", ()), ("readline", (1,)), ("write", (b"a",)), ("write", ("a",)), ("write", (1,)), ("write", ()), ("readinto", (bytearray(2),)),
                       ("readinto", (b"ab",)), ("readinto1", (bytearray(2),)), ("readinto", (1,))):
        if hasattr(c, name):
            t("%s.%s%r" % (c.__name__, name, args), lambda: getattr(c(), name)(*args))
t("_TextIOBase attributes", lambda: (_TextIOBase().encoding, _TextIOBase().newlines, _TextIOBase().errors))


class Buffered(_BufferedIOBase):
    def __init__(self, result): self.result, self.log = result, []
    def read(self, n=-1):
        self.log.append(("read", n))
        return self.result
    def read1(self, n=-1):
        self.log.append(("read1", n))
        return self.result


for label, result in (("bytes", b"ab"), ("too much", b"abcdef"), ("a str", "ab"), ("nothing", b""), ("a bytearray", bytearray(b"a")), ("None", None)):
    def go():
        x, target = Buffered(result), bytearray(b"....")
        return x.readinto(target), target, x.readinto1(memoryview(target)), x.log
    t("readinto with read that returns " + label, go)

print("---- FileIO")
p = D + "a.bin"
f = FileIO(p, "w")
t("new", lambda: (repr(f), f.name, f.mode, f.closed, f.closefd, f.readable(), f.writable(), f.seekable(), f.isatty(), f._isatty_open_only(), f.fileno() > 2, f._blksize > 0, f._finalizing, vars(f)))
t("write", lambda: (f.write(b"hello"), f.write(bytearray(b" ")), f.write(memoryview(b"world")), f.write(b""), f.tell()))
t("write('a')", lambda: f.write("a"))
t("write(1)", lambda: f.write(1))
t("write()", lambda: f.write())
t("read", lambda: f.read())
t("readall", lambda: f.readall())
t("readinto", lambda: f.readinto(bytearray(1)))
t("seek", lambda: (f.seek(0), f.tell(), f.seek(2, 1), f.seek(-1, 2), f.seek(100), f.seek(0, 2)))
t("seek(-1)", lambda: f.seek(-1))
t("seek('a')", lambda: f.seek("a"))
t("seek(0, 'a')", lambda: f.seek(0, "a"))
t("seek(0, 9)", lambda: f.seek(0, 9))
t("seek(1.5)", lambda: f.seek(1.5))
t("seek(2 ** 70)", lambda: f.seek(2 ** 70))
t("seek()", lambda: f.seek())
t("truncate", lambda: (f.seek(5), f.truncate(), f.tell(), f.truncate(3), f.tell(), f.truncate(8), f.seek(0, 2), f.truncate(None)))
t("truncate(-1)", lambda: f.truncate(-1))
t("truncate('a')", lambda: f.truncate("a"))
t("close", lambda: (f.close(), f.closed, repr(f), f.close(), f.mode, f.closefd, f.name, vars(f)))
for name, args in (("read", ()), ("readall", ()), ("readinto", (bytearray(1),)), ("write", (b"",)), ("seek", (0,)), ("tell", ()), ("truncate", ()), ("fileno", ()), ("isatty", ()), ("_isatty_open_only", ()), ("readable", ()), ("writable", ()), ("seekable", ()),
                   ("flush", ()), ("readline", ()), ("__enter__", ()), ("__iter__", ()), ("readlines", ()), ("writelines", ([],))):
    t(name + " when closed", lambda: getattr(f, name)(*args))
t("_blksize when closed", lambda: f._blksize)
r = FileIO(p)
t("read", lambda: (repr(r), r.mode, r.readable(), r.writable(), r.read(2), r.read(0), r.read(100), r.read(1), r.read(), r.seek(0), r.readall(), r.readall()))
t("read(None)", lambda: (r.seek(1), r.read(None), r.seek(1), r.read(-5)))
t("read('a')", lambda: r.read("a"))
t("read(2 ** 70)", lambda: r.read(2 ** 70))
t("readinto", lambda: [(r.seek(0), r.readinto(x), x, r.readinto(memoryview(x)[1:3]), x, r.readinto(bytearray())) for x in [bytearray(4)]])
t("readinto(bytes)", lambda: r.readinto(b"ab"))
t("readinto('a')", lambda: r.readinto("a"))
t("write", lambda: r.write(b"a"))
t("truncate", lambda: r.truncate())
t("readline and iteration", lambda: (FileIO(p, "w").write(b"a\nbb\n\nccc"), r.seek(0), r.readline(), r.readline(1), r.readlines(), r.seek(0), list(r)))
r.close()
for mode in ("r", "w", "a", "x", "r+", "w+", "a+", "x+", "rb", "wb", "br", "b+r", "rb+", "+", "", "b", "rw", "ra", "rr", "r++", "z", "rt", "U", "r\x00", "é", "wx"):
    def go():
        if "x" in mode:
            path = D + "x%d.bin" % len(made)
            made.append(path)
        else:
            path = p
        x = FileIO(path, mode)
        result = x.mode, x.readable(), x.writable(), x.tell(), repr(x)
        x.close()
        return result
    made = globals().setdefault("made", [])
    FileIO(p, "w").write(b"12345")
    t("mode %r" % mode, go)
t("x of what is there", lambda: FileIO(p, "x"))
t("what is not there", lambda: FileIO(D + "missing"))
t("its attributes", lambda: [(e.errno, e.strerror, e.filename, e.filename2, type(e).__name__) for e in [attempt(lambda: FileIO(D + "missing"))]])
t("a directory", lambda: FileIO(D))
t("a directory, to write", lambda: FileIO(D, "w"))
t("in what is not there", lambda: FileIO(D + "no/such", "w"))


def attempt(f):
    try:
        return f()
    except Exception as e:
        return e


t("its attributes", lambda: [(e.errno, e.strerror, e.filename.replace(D, "D/"), e.filename2, type(e).__name__) for e in [attempt(lambda: FileIO(D + "missing"))]])
for label, args, kwargs in (("()", (), {}), ("(1.5)", (1.5,), {}), ("(None)", (None,), {}), ("([])", ([],), {}), ("(-1)", (-1,), {}), ("(-5)", (-5,), {}), ("(2 ** 40)", (2 ** 40,), {}), ("(99999)", (99999,), {}), ("(p, 1)", (p, 1), {}), ("(p, None)", (p, None), {}),
                            ("(p, 'r', False)", (p, "r", False), {}), ("(p, 'r', True, None, 1)", (p, "r", True, None, 1), {}), ("(p, other=1)", (p,), {"other": 1}), ("(file=p, mode='r', closefd=1, opener=None)", (), {"file": p, "mode": "r", "closefd": 1, "opener": None}),
                            ("(b'...')", (p.encode(),), {}), ("('a\\0b')", ("a\0b",), {}), ("(b'a\\0b')", (b"a\0b",), {}), ("(bytearray)", (bytearray(p.encode()),), {}), ("('\\udc80')", (D + "\udc80",), {}), ("('\\ud800')", (D + "\ud800",), {})):
    t("FileIO" + label, lambda: [(x.name, x.mode, x.close()) for x in [FileIO(*args, **kwargs)]])


class Path:
    def __init__(self, p): self.p = p
    def __fspath__(self): return self.p


t("what has __fspath__", lambda: [(type(x.name).__name__, x.close()) for x in [FileIO(Path(p))]])
t("that returns an int", lambda: FileIO(Path(1)))
print("---- by number")
owner = FileIO(p, "r+")
n = owner.fileno()
t("closefd=False", lambda: [(repr(x).replace(str(n), "N"), x.name == n, x.closefd, x.read(2), x.close(), x.closed, owner.read(2)) for x in [FileIO(n, "r", closefd=False)]])
t("True", lambda: FileIO(True, "w", closefd=False).closefd)
t("closefd=True", lambda: [(x.closefd, x.close(), attempt(owner.read).__class__.__name__, attempt(owner.close).__class__.__name__, owner.closed) for x in [FileIO(n, "r")]])
print("---- opener")
log = []


def opener(path, flags):
    log.append((path.replace(D, "D/"), flags))
    return keep.fileno()


keep = FileIO(p, "r")
t("is asked", lambda: [(x.read(3), x.name.replace(D, "D/"), x.closefd, log, x.fileno() == keep.fileno()) for x in [FileIO(p, "r", opener=opener)]])
for label, result in (("a str", "a"), ("None", None), ("-1", -1), ("-7", -7), ("a float", 1.5), ("huge", 2 ** 40), ("one that is not open", 99999)):
    t("that returns " + label, lambda: FileIO(p, "r", opener=lambda *a: result))
t("that raises", lambda: FileIO(p, "r", opener=lambda *a: 1 / 0))
t("that is not callable", lambda: FileIO(p, "r", opener=5))
print("---- again, and the rest")
g = FileIO(p, "r")
first = g.fileno()
t("__init__ again", lambda: (g.__init__(p, "w"), g.mode, g.readable(), g.writable(), g.closed))
t("__init__ that fails", lambda: g.__init__(D + "missing"))
t("leaves it closed", lambda: (g.closed, g.mode))
t("__new__ alone", lambda: [(x.closed, repr(x), x.mode, x.closefd, attempt(x.read).__class__.__name__, x.close()) for x in [FileIO.__new__(FileIO)]])
t("pickle", lambda: FileIO(p).__getstate__())
t("_finalizing", lambda: [(x._finalizing, setattr(x, "_finalizing", True), x._finalizing) for x in [FileIO.__new__(FileIO)]])
t("_finalizing = 1", lambda: setattr(FileIO.__new__(FileIO), "_finalizing", 1))
t("del _finalizing", lambda: delattr(FileIO.__new__(FileIO), "_finalizing"))
t("mode cannot be set", lambda: setattr(g, "mode", "r"))
t("name can", lambda: [(setattr(x, "name", "other"), repr(x), delattr(x, "name"), repr(x).replace(str(x.fileno()), "N"), x.close()) for x in [FileIO(p)]])


class Self:
    def __repr__(self): return repr(holder)


holder = FileIO(p)
holder.name = Self()
t("a name that shows the file", lambda: repr(holder))


class Sub(FileIO):
    def __init__(self, *a, extra=None):
        super().__init__(*a)
        self.extra = extra


t("derived from", lambda: [(repr(x), x.extra, x.read(1), isinstance(x, _RawIOBase), x.close()) for x in [Sub(p, extra=5)]])
t("_dealloc_warn", lambda: [(x._dealloc_warn("the source"), x.close(), x._dealloc_warn(1)) for x in [FileIO(p)]])
t("append", lambda: [(FileIO(p, "w").write(b"abc"), x.tell(), x.write(b"de"), x.seek(0), x.write(b"f"), x.close(), FileIO(p).read()) for x in [FileIO(p, "a")]][0][1:] if False else None)
w = FileIO(p, "w"); w.write(b"abc"); w.close()
a = FileIO(p, "a")
t("append", lambda: (a.tell(), a.write(b"de"), a.seek(0), a.write(b"f"), a.tell(), a.close(), FileIO(p).read()))
t("with", lambda: [(x.closed, x.__exit__(None, None, None), x.closed) for x in [FileIO(p).__enter__()]])
t("a large one", lambda: [(w.write(b"0123456789" * 30000), w.close(), len(FileIO(p).read()), len(FileIO(p).readall()), [(x.seek(100000), len(x.read())) for x in [FileIO(p)]]) for w in [FileIO(p, "w")]])
told = []
sys.addaudithook(lambda e, a: told.append((e, tuple(x.replace(D, "D/") if isinstance(x, str) else x for x in a))) if e == "open" else None)
t("who is told", lambda: (FileIO(p).close(), FileIO(p, "wb+").close(), attempt(lambda: FileIO(p, "z")).__class__.__name__, told))

for name in posix.listdir(D):
    posix.unlink(D + name)
posix.rmdir(D)
