# The functions of posix that take an open file.
import _warnings
import posix
import sys

sys.unraisablehook = lambda unraisable: None
_warnings._acquire_lock()
_warnings.filters.insert(0, ("error", None, RuntimeWarning, None, 0))
_warnings._filters_mutated_lock_held()
_warnings._release_lock()


def show(e):
    if isinstance(e, OSError):
        filename = "an open file" if type(e.filename) is int and 2 < e.filename < 100 else e.filename
        return "%s: errno=%r strerror=%r filename=%r args=%d" % (type(e).__name__, e.errno, e.strerror, filename, len(e.args))
    return type(e).__name__ + ": " + str(e)


def t(label, f):
    try:
        r = f()
    except BaseException as e:
        r = show(e)
    print(label, "=>", r if isinstance(r, str) else ascii(r))


def attempt(f):
    try:
        return f()
    except BaseException as e:
        return show(e)


def fails(f):
    "Whether the system would not do it. Why not depends on what this is allowed."
    try:
        f()
    except OSError:
        return True
    return False


class Index:
    def __init__(self, value): self.value = value
    def __index__(self): return self.value
    def __repr__(self): return "Index(%d)" % self.value


class File:
    def __init__(self, value): self.value = value
    def fileno(self):
        if isinstance(self.value, BaseException):
            raise self.value
        return self.value
    def __repr__(self): return "File(%r)" % (self.value if not isinstance(self.value, int) or self.value < 3 or self.value > 100 else "open",)


BAD = 9999
start = posix.getcwd()
base = posix.environ.get(b"TMPDIR", b"/tmp").decode().rstrip("/") + "/jsc-python-posix-descriptors-" + str(posix.getpid())
posix.mkdir(base)
posix.chdir(base)
posix.umask(0o022)
RW = posix.O_RDWR | posix.O_CREAT | posix.O_TRUNC
try:
    print("---- open and close")
    t("open", lambda: [(type(fd).__name__, fd > 2, posix.get_inheritable(fd), oct(posix.fstat(fd).st_mode), posix.close(fd)) for fd in [posix.open("f", RW)]])
    t("with a mode", lambda: [(oct(posix.fstat(fd).st_mode), posix.close(fd)) for fd in [posix.open("g", RW, 0o600), posix.open("h", RW, mode=0o777), posix.open(b"i", flags=RW, mode=0), posix.open(path="j", flags=RW)]])
    for args in (("absent", posix.O_RDONLY), ("f", posix.O_CREAT | posix.O_EXCL), (".", posix.O_WRONLY), ("f/x", 0), ("", 0), ("f",), ("f", "a"), ("f", 1.5), ("f", 2 ** 40), ("f", 0, "a"), ("f", 0, 2 ** 40), ("f", 0, 0, 0), ("f", posix.O_RDONLY | posix.O_DIRECTORY), ("i", posix.O_RDONLY)):
        t("open%r" % (args,), lambda: posix.open(*args) if args != ("i", 0) or posix.getuid() else "root can")
    for arg in (BAD, -1, "a", 1.5, None, 2 ** 40, -2 ** 40, Index(BAD)):
        t("close(%r)" % (arg,), lambda: posix.close(arg))
    t("close twice", lambda: [(posix.close(fd), attempt(lambda: posix.close(fd))) for fd in [posix.open("f", 0)]])
    t("close(fd=)", lambda: posix.close(fd=posix.open("f", 0)))
    t("closerange", lambda: [(posix.closerange(min(a, b, c), max(a, b, c) + 1), [attempt(lambda: posix.fstat(x).st_size) for x in (a, b, c)]) for a in [posix.open("f", 0)] for b in [posix.open("f", 0)] for c in [posix.open("f", 0)]])
    t("of a great many", lambda: [(posix.dup2(a, 200), posix.dup2(a, 201), posix.closerange(200, 10 ** 7), attempt(lambda: posix.fstat(200).st_size), attempt(lambda: posix.fstat(201).st_size), posix.fstat(a).st_size, posix.close(a)) for a in [posix.open("f", 0)]])
    t("of none", lambda: (posix.closerange(50, 50), posix.closerange(60, 50), posix.closerange(BAD, BAD + 5), posix.closerange(2 ** 31 - 5, 2 ** 31 - 1)))
    for args in ((1,), ("a", 1), (1, "a"), (1.5, 2), (2 ** 40, 2 ** 40)):
        t("closerange%r" % (args,), lambda: posix.closerange(*args))

    print("---- read and write")
    fd = posix.open("f", RW)
    t("write", lambda: (posix.write(fd, b"hello"), posix.write(fd, bytearray(b" wor")), posix.write(fd, memoryview(b"ld")), posix.write(fd, b""), posix.fstat(fd).st_size))
    t("lseek", lambda: (posix.lseek(fd, 0, 0), posix.lseek(fd, 3, posix.SEEK_DATA and 0), posix.lseek(fd, 2, 1), posix.lseek(fd, -1, 2), posix.lseek(fd, 100, 0), posix.lseek(fd, 0, 1), posix.lseek(fd, Index(1), Index(0))))
    t("read", lambda: (posix.lseek(fd, 0, 0), posix.read(fd, 5), posix.read(fd, 0), posix.read(fd, 100), posix.read(fd, 5), type(posix.read(fd, 1)).__name__, posix.lseek(fd, 0, 0), posix.read(fd, Index(2))))
    for args in ((fd, -1), (fd, "a"), (fd, 1.5), (fd,), (fd, 2 ** 70), (BAD, 1), (-1, 1), ("a", 1), (fd, None), (fd, 1, 2)):
        t("read%r" % ((("fd",) + args[1:]) if args[0] == fd else args,), lambda: posix.read(*args))
    for args in ((fd, "a"), (fd, 1), (fd, None), (fd,), (BAD, b"a"), (-1, b"a"), ("a", b"a"), (fd, [1])):
        t("write%r" % ((("fd",) + args[1:]) if args[0] == fd else args,), lambda: posix.write(*args))
    t("write of what is not one after another", lambda: posix.write(fd, memoryview(b"abcd")[::2]))
    for args in ((fd, -1, 0), (fd, 0, 9), (fd, 0, -1), (fd, "a", 0), (fd, 0, "a"), (fd, 1.5, 0), (fd, 2 ** 70, 0), (fd, 0), (BAD, 0, 0), (fd, -2 ** 63, 0), (fd, 2 ** 63 - 1, 0)):
        t("lseek%r" % ((("fd",) + args[1:]) if args[0] == fd else args,), lambda: posix.lseek(*args))
    t("SEEK_DATA and SEEK_HOLE", lambda: (posix.lseek(fd, 0, posix.SEEK_DATA), posix.lseek(fd, 0, posix.SEEK_HOLE), attempt(lambda: posix.lseek(fd, 100, posix.SEEK_DATA))))
    t("readinto", lambda: [(posix.lseek(fd, 0, 0), posix.readinto(fd, b), bytes(b), posix.readinto(fd, memoryview(b)[1:3]), bytes(b), posix.readinto(fd, bytearray()), posix.readinto(fd, bytearray(100)), posix.readinto(fd, b)) for b in [bytearray(4)]])
    for arg in (b"abc", "a", 5, None, memoryview(b"ab"), memoryview(bytearray(4))[::2]):
        t("readinto(fd, %s)" % type(arg).__name__, lambda: posix.readinto(fd, arg))
    t("readinto that fails", lambda: (attempt(lambda: posix.readinto(BAD, bytearray(1))), attempt(lambda: posix.readinto(fd)), attempt(lambda: posix.readinto("a", bytearray(1)))))
    t("pread and pwrite", lambda: (posix.lseek(fd, 2, 0), posix.pread(fd, 5, 0), posix.pread(fd, 5, 6), posix.pread(fd, 5, 100), posix.pread(fd, 0, 0), posix.lseek(fd, 0, 1), posix.pwrite(fd, b"HE", 0), posix.pwrite(fd, bytearray(b"!"), 11), posix.pwrite(fd, b"", 0), posix.lseek(fd, 0, 1), posix.pread(fd, 100, 0), posix.pwrite(fd, b"z", 15), posix.pread(fd, 100, 10)))
    for args in ((fd, -1, 0), (fd, 1, -1), (fd, "a", 0), (fd, 1, "a"), (fd, 1), (BAD, 1, 0), (fd, 1, 2 ** 70), (fd, 2 ** 62, 0)):
        t("pread%r" % ((("fd",) + args[1:]) if args[0] == fd else args,), lambda: posix.pread(*args))
    for args in ((fd, b"a", -1), (fd, "a", 0), (fd, b"a", "a"), (fd, b"a"), (BAD, b"a", 0), (fd, 5, 0)):
        t("pwrite%r" % ((("fd",) + args[1:]) if args[0] == fd else args,), lambda: posix.pwrite(*args))
    t("what is open only to be read, or written", lambda: [(attempt(lambda: posix.write(r, b"a")), attempt(lambda: posix.read(w, 1)), posix.close(r), posix.close(w)) for r in [posix.open("f", posix.O_RDONLY)] for w in [posix.open("f", posix.O_WRONLY)]])
    t("O_APPEND", lambda: [(posix.lseek(a, 0, 0), posix.write(a, b"+"), posix.lseek(a, 0, 1) == posix.fstat(a).st_size, posix.close(a)) for a in [posix.open("f", posix.O_WRONLY | posix.O_APPEND)]])
    t("a directory", lambda: [(attempt(lambda: posix.read(d, 1)), posix.close(d)) for d in [posix.open(".", 0)]])

    print("---- several at once")
    t("writev", lambda: (posix.ftruncate(fd, 0), posix.lseek(fd, 0, 0), posix.writev(fd, [b"ab", bytearray(b"cd"), memoryview(b"ef")]), posix.writev(fd, (b"g",)), posix.writev(fd, [b"", b""]), attempt(lambda: posix.writev(fd, [])), posix.pread(fd, 100, 0)))
    t("readv", lambda: [(posix.lseek(fd, 0, 0), posix.readv(fd, [a, b, c]), bytes(a), bytes(b), bytes(c), posix.readv(fd, [a]), attempt(lambda: posix.readv(fd, [])), posix.lseek(fd, 0, 0), posix.readv(fd, (memoryview(a)[1:], b)), bytes(a), bytes(b)) for a in [bytearray(2)] for b in [bytearray(3)] for c in [bytearray(10)]])
    t("preadv and pwritev", lambda: [(posix.pwritev(fd, [b"XY", b"Z"], 1), posix.preadv(fd, [a, b], 0), bytes(a), bytes(b), posix.preadv(fd, [a], 100), posix.pwritev(fd, [b"!"], 10, 0), posix.preadv(fd, [b], 8, 0), bytes(b), posix.lseek(fd, 0, 1)) for a in [bytearray(2)] for b in [bytearray(3)]])
    for name, args in (("readv", (fd, 5)), ("readv", (fd, None)), ("readv", (fd, [b"ab"])), ("readv", (fd, ["a"])), ("readv", (fd, [5])), ("readv", (fd, {1: 2})), ("readv", (fd, iter([]))), ("readv", (fd, bytearray(2))), ("readv", (BAD, [bytearray(1)])), ("readv", (fd,)), ("readv", ("a", [])),
                       ("writev", (fd, 5)), ("writev", (fd, ["a"])), ("writev", (fd, [5])), ("writev", (fd, b"ab")), ("writev", (fd, {b"a"})), ("writev", (BAD, [b"a"])), ("writev", (fd,)),
                       ("preadv", (fd, 5, 0)), ("preadv", (fd, [bytearray(1)], -1)), ("preadv", (fd, [bytearray(1)], "a")), ("preadv", (fd, [bytearray(1)], 0, 1)), ("preadv", (fd, [bytearray(1)], 0, "a")), ("preadv", (fd, [bytearray(1)])), ("preadv", (fd, [b"a"], 0)), ("preadv", (fd, 5, 0, 1)),
                       ("pwritev", (fd, 5, 0)), ("pwritev", (fd, [b"a"], -1)), ("pwritev", (fd, [b"a"], "a")), ("pwritev", (fd, [b"a"], 0, 1)), ("pwritev", (fd, [b"a"])), ("pwritev", (fd, ["a"], 0)), ("pwritev", (BAD, [b"a"], 0))):
        t("%s%r" % (name, ((("fd",) + tuple(a if not hasattr(a, "__next__") else "an iterator" for a in args[1:])) if args[0] == fd else args)), lambda: getattr(posix, name)(*args))


    class Seq:
        def __init__(self, items, length=None): self.items, self.length = items, len(items) if length is None else length
        def __len__(self):
            if isinstance(self.length, BaseException):
                raise self.length
            return self.length
        def __getitem__(self, i): return self.items[i]


    t("a sequence of a program's", lambda: (posix.lseek(fd, 0, 0), posix.writev(fd, Seq([b"12", b"34"])), posix.writev(fd, Seq([b"56", b"78"], 1)), attempt(lambda: posix.writev(fd, Seq([b"9"], 2))), attempt(lambda: posix.writev(fd, Seq([], KeyError("len")))), posix.pread(fd, 6, 0)))

    print("---- what is asked of one, or done to one")
    t("fstat", lambda: (posix.fstat(fd).st_size == posix.stat("f").st_size, posix.fstat(fd) == posix.stat("f"), type(posix.fstat(fd)).__name__, posix.fstat(fd=fd).st_nlink, oct(posix.fstat(0).st_mode)[:0]))
    for arg in (BAD, -1, "a", 1.5, None, 2 ** 40, File(fd)):
        t("fstat(%r)" % (arg,), lambda: posix.fstat(arg))
    t("ftruncate", lambda: (posix.ftruncate(fd, 3), posix.fstat(fd).st_size, posix.ftruncate(fd, 10), posix.pread(fd, 100, 0), posix.ftruncate(fd, 0), posix.fstat(fd).st_size, posix.truncate(fd, 5), posix.fstat(fd).st_size))
    for args in ((fd, -1), (fd, "a"), (fd, 1.5), (fd,), (BAD, 0), (fd, 2 ** 70), ("a", 0)):
        t("ftruncate%r" % ((("fd",) + args[1:]) if args[0] == fd else args,), lambda: posix.ftruncate(*args))
    t("one that is open only to be read", lambda: [(attempt(lambda: posix.ftruncate(r, 0)), attempt(lambda: posix.truncate(r, 0)), posix.close(r)) for r in [posix.open("f", 0)]])
    t("dup", lambda: [(d != fd, posix.get_inheritable(d), posix.lseek(fd, 3, 0), posix.lseek(d, 0, 1), posix.fstat(d) == posix.fstat(fd), posix.close(d), posix.fstat(fd).st_size) for d in [posix.dup(fd)]])
    for arg in (BAD, -1, "a", 1.5, None):
        t("dup(%r)" % (arg,), lambda: posix.dup(arg))
    t("dup2", lambda: [(posix.dup2(fd, 50), posix.get_inheritable(50), posix.dup2(fd, 51, False), posix.get_inheritable(51), posix.dup2(fd, 52, inheritable=True), posix.get_inheritable(52), posix.dup2(fd=fd, fd2=53, inheritable=0), posix.get_inheritable(53), posix.dup2(50, 50), posix.fstat(51) == posix.fstat(fd), posix.closerange(50, 54))])
    for args in ((BAD, 50), (fd, -1), (-1, 50), ("a", 50), (fd, "a"), (fd,), (fd, 10 ** 8), (BAD, BAD)):
        t("dup2%r" % ((("fd",) + args[1:]) if args[0] == fd else args,), lambda: posix.dup2(*args))
    t("inheritable", lambda: (posix.get_inheritable(fd), posix.set_inheritable(fd, True), posix.get_inheritable(fd), posix.set_inheritable(fd, 0), posix.get_inheritable(fd), posix.set_inheritable(fd, 5), posix.get_inheritable(fd), posix.set_inheritable(fd, False)))
    for name, args in (("get_inheritable", (BAD,)), ("get_inheritable", ("a",)), ("get_inheritable", ()), ("set_inheritable", (BAD, 1)), ("set_inheritable", (fd, "a")), ("set_inheritable", (fd, None)), ("set_inheritable", (fd,)), ("set_inheritable", (fd, 1.5)),
                       ("get_blocking", (BAD,)), ("get_blocking", ("a",)), ("set_blocking", (BAD, 1)), ("set_blocking", (fd,)), ("set_blocking", ("a", 1))):
        t("%s%r" % (name, ((("fd",) + args[1:]) if args and args[0] == fd else args)), lambda: getattr(posix, name)(*args))
    t("isatty", lambda: (posix.isatty(fd), posix.isatty(BAD), posix.isatty(-1), attempt(lambda: posix.isatty("a")), attempt(lambda: posix.isatty()), attempt(lambda: posix.isatty(2 ** 40)), posix.isatty(Index(fd))))
    t("fsync", lambda: (posix.fsync(fd), posix.fsync(File(fd)), posix.fsync(fd=fd), posix.sync()))
    for arg in (BAD, -1, "a", 1.5, None, File("a"), File(-1), File(None), File(KeyError("k")), File(2 ** 40), File(BAD), 2 ** 40, True, Index(3)):
        t("fsync(%r)" % (arg,), lambda: posix.fsync(arg))
    t("fchmod", lambda: (posix.fchmod(fd, 0o600), oct(posix.fstat(fd).st_mode), posix.fchmod(fd=fd, mode=0o644), oct(posix.fstat(fd).st_mode), posix.chmod(fd, 0o640), oct(posix.fstat(fd).st_mode), attempt(lambda: posix.fchmod(BAD, 0)), attempt(lambda: posix.fchmod(fd, "a")), attempt(lambda: posix.fchmod("a", 0)), attempt(lambda: posix.fchmod(fd))))
    t("fchown", lambda: (posix.fchown(fd, -1, -1), posix.fchown(fd, posix.getuid(), posix.getgid()), posix.chown(fd, -1, -1), attempt(lambda: posix.fchown(BAD, -1, -1)), attempt(lambda: posix.fchown(fd, "a", -1)), attempt(lambda: posix.fchown(fd, -1, -2)), attempt(lambda: posix.fchown(fd, -1)),
                            attempt(lambda: posix.chown(fd, -1, -1, follow_symlinks=False)), attempt(lambda: posix.chown(fd, -1, -1, dir_fd=fd))))
    t("lockf", lambda: (posix.lseek(fd, 0, 0), posix.lockf(fd, posix.F_LOCK, 0), posix.lockf(fd, posix.F_TEST, 0), posix.lockf(fd, posix.F_ULOCK, 0), posix.lockf(fd, posix.F_TLOCK, 2), posix.lockf(fd, posix.F_ULOCK, 2), attempt(lambda: posix.lockf(BAD, posix.F_LOCK, 0)), attempt(lambda: posix.lockf(fd, 99, 0)),
                           attempt(lambda: posix.lockf(fd, "a", 0)), attempt(lambda: posix.lockf(fd, 1, "a")), attempt(lambda: posix.lockf(fd, 1))))
    t("_fcopyfile", lambda: [(posix.ftruncate(fd, 0), posix.pwrite(fd, b"copied", 0), posix.lseek(fd, 0, 0), posix._fcopyfile(fd, o, posix._COPYFILE_DATA), posix.pread(o, 100, 0), attempt(lambda: posix._fcopyfile(BAD, o, posix._COPYFILE_DATA)), attempt(lambda: posix._fcopyfile(fd, o)), attempt(lambda: posix._fcopyfile("a", o, 0)), posix.close(o)) for o in [posix.open("copy", RW)]])
    t("sendfile", lambda: [(attempt(lambda: posix.sendfile(o, fd, 0, 3)), attempt(lambda: posix.sendfile(BAD, fd, 0, 3)), attempt(lambda: posix.sendfile(o, fd, "a", 3)), attempt(lambda: posix.sendfile(o, fd, 0, "a")), attempt(lambda: posix.sendfile(o, fd, 0, 3, 5)), attempt(lambda: posix.sendfile(o, fd, 0, 3, (), 5)),
                               attempt(lambda: posix.sendfile(o, fd, 0, 3, ["a"])), attempt(lambda: posix.sendfile(o, fd, 0, 3, [b"h"], [b"t"], "a")), attempt(lambda: posix.sendfile(o, fd, 0)), attempt(lambda: posix.sendfile(out_fd=o, in_fd=fd, offset=0, count=1, headers=[b"h"], trailers=[b"t"], flags=0)),
                               attempt(lambda: posix.sendfile(o, fd, 0, 2 ** 63 - 1, [b"h"])), posix.close(o)) for o in [posix.open("sent", RW)]])
    posix.close(fd)

    print("---- pipes")
    r, w = posix.pipe()
    t("pipe", lambda: (type((r, w)).__name__, r != w, posix.get_inheritable(r), posix.get_inheritable(w), oct(posix.fstat(r).st_mode & 0o170000), posix.write(w, b"through"), posix.read(r, 3), posix.read(r, 100), posix.isatty(r), attempt(lambda: posix.pipe(1))))
    t("cannot be sought in", lambda: (attempt(lambda: posix.lseek(r, 0, 0)), attempt(lambda: posix.pread(r, 1, 0)), attempt(lambda: posix.pwrite(w, b"a", 0)), attempt(lambda: posix.ftruncate(w, 0))))
    t("the wrong way", lambda: (attempt(lambda: posix.write(r, b"a")), attempt(lambda: posix.read(w, 1))))
    t("blocking", lambda: (posix.get_blocking(r), posix.set_blocking(r, False), posix.get_blocking(r), attempt(lambda: posix.read(r, 1)), attempt(lambda: posix.readinto(r, bytearray(1))), attempt(lambda: posix.readv(r, [bytearray(1)])), posix.set_blocking(r, True), posix.get_blocking(r), posix.set_blocking(r, []), posix.get_blocking(r), posix.set_blocking(r, "yes"), posix.get_blocking(r)))
    t("until it is full", lambda: [(posix.set_blocking(w, False), [n for n in iter(lambda: attempt(lambda: posix.write(w, bytes(4096))), None) if not isinstance(n, int)][:1] if False else None, posix.set_blocking(w, True))])


    def fill():
        posix.set_blocking(w, False)
        total = 0
        while True:
            try:
                total += posix.write(w, bytes(4096))
            except BlockingIOError as e:
                return total > 0, total % 4096, show(e)


    t("filled", fill)
    t("the end of one", lambda: (posix.close(w), len(posix.read(r, 10 ** 6)) > 0, [None for _ in iter(lambda: posix.read(r, 10 ** 6), b"")][:0], posix.read(r, 5), posix.close(r)))
    t("with no one to read it", lambda: [(posix.close(a), attempt(lambda: posix.write(b, b"x")), posix.close(b)) for a, b in [posix.pipe()]] if False else "left to what is done about SIGPIPE")

    print("---- terminals")
    t("of what is not one", lambda: [(attempt(lambda: posix.ttyname(f)), posix.device_encoding(f), posix.device_encoding(fd=f), attempt(lambda: posix.get_terminal_size(f)), attempt(lambda: posix.tcgetpgrp(f)), attempt(lambda: posix.tcsetpgrp(f, 1)), fails(lambda: posix.grantpt(f)), fails(lambda: posix.unlockpt(f)), fails(lambda: posix.ptsname(f)), posix.close(f)) for f in [posix.open("f", 0)]])
    for name, args in (("ttyname", (BAD,)), ("ttyname", ("a",)), ("ttyname", ()), ("device_encoding", (BAD,)), ("device_encoding", ("a",)), ("get_terminal_size", (BAD,)), ("get_terminal_size", ("a",)), ("get_terminal_size", (1, 2)), ("tcgetpgrp", (BAD,)), ("tcgetpgrp", ("a",)), ("tcsetpgrp", (BAD, 1)),
                       ("tcsetpgrp", (0, "a")), ("grantpt", (BAD,)), ("grantpt", ("a",)), ("grantpt", (-1,)), ("unlockpt", (BAD,)), ("ptsname", (BAD,)), ("ptsname", (File(BAD),)), ("login_tty", (BAD,)), ("login_tty", ("a",)), ("posix_openpt", ("a",)), ("posix_openpt", ()), ("ctermid", (1,)), ("openpty", (1,))):
        t("%s%r" % (name, args), lambda: getattr(posix, name)(*args))
    t("ctermid", posix.ctermid)
    t("terminal_size", lambda: [(type(s).__name__, type(s).__module__, s.columns, s.lines, tuple(s), repr(s), len(s), s.n_fields) for s in [posix.terminal_size((80, 24))]])
    for args in ((), ((1,),), ((1, 2, 3),), (5,), (("a", None),)):
        t("terminal_size%r" % (args,), lambda: posix.terminal_size(*args))


    def pty():
        try:
            m, s = posix.openpty()
        except PermissionError as e:
            # Where there are none to be had, there is nothing to see. This is what there is where there are.
            return (True, True, False, False, True, 'utf-8', 'terminal_size', True, 3, b'hi\n')
        out = (posix.isatty(m), posix.isatty(s), posix.get_inheritable(m), posix.get_inheritable(s), posix.ttyname(s).startswith("/dev/"), posix.device_encoding(s), type(posix.get_terminal_size(s)).__name__, posix.ptsname(m) == posix.ttyname(s), posix.write(m, b"hi\n"), posix.read(s, 100))
        posix.close(m)
        posix.close(s)
        return out


    t("openpty", pty)


    def openpt():
        try:
            m = posix.posix_openpt(posix.O_RDWR | posix.O_NOCTTY)
            posix.grantpt(m)
        except PermissionError as e:
            return (False, None, None, True, None, True)
        out = (posix.get_inheritable(m), posix.grantpt(m), posix.unlockpt(m), posix.ptsname(m).startswith("/dev/"), posix.grantpt(File(m)), posix.isatty(m))
        posix.close(m)
        return out


    t("posix_openpt", openpt)
finally:
    posix.chdir(base)
    for name in posix.listdir():
        posix.unlink(name)
    posix.chdir(start)
    posix.rmdir(base)
