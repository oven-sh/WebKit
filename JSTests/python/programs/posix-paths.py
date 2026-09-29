# The functions of posix that take the name of a file. It is all done in a directory made for it, by names that begin there.
import _warnings
import posix
import sys

sys.unraisablehook = lambda unraisable: None
# What is warned of is raised, so that it is seen here.
_warnings._acquire_lock()
_warnings.filters.insert(0, ("error", None, RuntimeWarning, None, 0))
_warnings._filters_mutated_lock_held()
_warnings._release_lock()


def show(e):
    if isinstance(e, OSError):
        # Which numbers open files have depends on what else is open.
        filename = "an open file" if type(e.filename) is int and 2 < e.filename < 100 else e.filename
        return "%s: errno=%r strerror=%r filename=%r filename2=%r args=%d" % (type(e).__name__, e.errno, e.strerror, filename, e.filename2, len(e.args))
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


def remove_all(path):
    for entry in list(posix.scandir(path)):
        if entry.is_dir(follow_symlinks=False):
            posix.chmod(entry.path, 0o700)
            remove_all(entry.path)
            posix.rmdir(entry.path)
        else:
            posix.unlink(entry.path)


def write(path, data=b""):
    fd = posix.open(path, posix.O_WRONLY | posix.O_CREAT | posix.O_TRUNC, 0o644)
    posix.write(fd, data)
    posix.close(fd)


def read(path):
    fd = posix.open(path, posix.O_RDONLY)
    data = posix.read(fd, 10000)
    posix.close(fd)
    return data


class P:
    def __init__(self, value): self.value = value
    def __fspath__(self):
        if isinstance(self.value, BaseException):
            raise self.value
        return self.value


class NoneFspath:
    __fspath__ = None


class Index:
    def __init__(self, value): self.value = value
    def __index__(self): return self.value
    def __repr__(self): return "Index(%d)" % self.value


class S(str):
    pass


class B(bytes):
    pass


start = posix.getcwd()
base = posix.environ.get(b"TMPDIR", b"/tmp").decode().rstrip("/") + "/jsc-python-posix-paths-" + str(posix.getpid())
posix.mkdir(base)
posix.chdir(base)
posix.umask(0o022)
try:
    print("---- what will do for a name")
    write("file", b"hello")
    for label, value in (("a str", "file"), ("bytes", b"file"), ("what has __fspath__", P("file")), ("that returns bytes", P(b"file")), ("derived from str", S("file")), ("derived from bytes", B(b"file")), ("a bytearray", bytearray(b"file")), ("a memoryview", memoryview(b"file")),
                         ("None", None), ("an int", 5), ("a float", 1.5), ("a list", ["file"]), ("__fspath__ that returns an int", P(5)), ("that returns None", P(None)), ("that returns what has __fspath__", P(P("file"))), ("that raises", P(KeyError("k"))),
                         ("__fspath__ that is None", NoneFspath()), ("a zero in a str", "fi\0le"), ("a zero in bytes", b"fi\0le"), ("''", ""), ("a lone surrogate", "\ud800"), ("an escaped byte", "\udcff"), ("not ASCII", "\xe9€\U0001F600")):
        t("lstat(%s)" % label, lambda: posix.lstat(value).st_size)
        t("stat(%s)" % label, lambda: posix.stat(value).st_size if not isinstance(value, int) else attempt(lambda: posix.stat(value)) and "an open file")
        t("listdir(%s)" % label, lambda: sorted(posix.listdir(value)) if not isinstance(value, int) else "an open file")
        t("fspath(%s)" % label, lambda: (type(posix.fspath(value)).__name__, posix.fspath(value)))
    for value in (True, 2 ** 31, -2 ** 31 - 1, 2 ** 70, -2 ** 70, -1, Index(-1), Index(2 ** 40)):
        t("stat(%s)" % (value if isinstance(value, int) else "Index(%d)" % value.value), lambda: posix.stat(value))
    for name, args in (("rename", (5, "x")), ("rename", ("x", 5)), ("replace", ("x\0", "y")), ("replace", ("x", "y\0")), ("link", (None, "x")), ("link", ("x", None)), ("symlink", (5, "x")), ("symlink", ("x", b"\0")), ("mkdir", (5,)), ("rmdir", (5,)), ("unlink", (5,)),
                       ("remove", (5,)), ("readlink", (5,)), ("chdir", (1.5,)), ("chmod", (1.5, 0)), ("chown", (1.5, 0, 0)), ("access", (5, 0)), ("truncate", (1.5, 0)), ("utime", (1.5,)), ("scandir", (1.5,)), ("statvfs", (1.5,)), ("pathconf", (1.5, 1)), ("open", (5, 0)),
                       ("mkfifo", (5,)), ("mknod", (5,)), ("lchown", (5, 0, 0)), ("chroot", (5,)), ("_path_normpath", (5,)), ("_path_splitroot_ex", (5,)), ("lchmod", (5, 0)), ("chflags", (5, 0)), ("lchflags", (5, 0)), ("execv", (5, ["a"])), ("execve", (5, ["a"], {}))):
        t("%s%r" % (name, args), lambda: getattr(posix, name)(*args))

    print("---- stat")
    posix.mkdir("dir", 0o750)
    posix.symlink("file", "link")
    posix.symlink("nowhere", "dangling")
    posix.symlink("dir", "dirlink")
    for name in ("file", "dir", "link", "dangling", "dirlink", "absent", "file/under"):
        t("stat(%r)" % name, lambda: [(oct(s.st_mode), s.st_size if name != "dir" and name != "dirlink" else None, s.st_nlink if name not in ("dir", "dirlink") else None) for s in [posix.stat(name)]])
        t("lstat(%r)" % name, lambda: [(oct(s.st_mode), s.st_size if name != "dir" else None) for s in [posix.lstat(name)]])
        t("stat(%r, follow_symlinks=False)" % name, lambda: oct(posix.stat(name, follow_symlinks=False).st_mode))
    s = posix.stat("file")
    t("what it is", lambda: (type(s).__name__, type(s).__module__, len(s), s.n_fields, s.n_sequence_fields, s.n_unnamed_fields, isinstance(s, tuple), type(s).__match_args__))
    t("of what kinds", lambda: [(name, type(getattr(s, name)).__name__) for name in sorted(dir(s)) if name.startswith("st_")])
    t("by position", lambda: (s[0] == s.st_mode, s[1] == s.st_ino, s[2] == s.st_dev, s[3] == s.st_nlink, s[4] == s.st_uid, s[5] == s.st_gid, s[6] == s.st_size, s[7] == int(s.st_atime), s[8] == int(s.st_mtime), s[9] == int(s.st_ctime), [type(x).__name__ for x in s]))
    t("the times agree", lambda: (s.st_mtime_ns // 10 ** 9 == s[8], abs(s.st_mtime - s.st_mtime_ns / 1e9) < 1e-6, s.st_atime_ns // 10 ** 9 == s[7], s.st_ctime_ns // 10 ** 9 == s[9], s.st_birthtime <= s.st_mtime + 1, s.st_uid == posix.getuid()))
    t("the same file", lambda: (posix.stat("link")[:7] == s[:7], posix.stat(b"file") == s, posix.lstat("link").st_ino != s.st_ino))
    t("repr", lambda: [(r.startswith("os.stat_result(st_mode=33188, st_ino="), r.count("="), r.endswith(")")) for r in [repr(s)]])
    for args in ((), (1,), ((1, 2, 3),), (range(9),), (range(10),), (range(11),), (range(22),), (range(23),), ([None] * 10,), (range(10), {"st_atime": 1.5}), (range(10), {"st_flags": 7, "other": 1}), (range(10), 5), ("abcdefghij",)):
        t("stat_result%r" % (args,), lambda: [(tuple(r), r.st_atime, r.st_mtime, r.st_ctime, r.st_atime_ns, r.st_blksize, r.st_flags, r.st_birthtime) for r in [posix.stat_result(*args)]])
    t("__reduce__", lambda: [(f is posix.stat_result, a[0] == tuple(s), sorted(a[1])) for f, a in [s.__reduce__()]])
    t("__replace__", lambda: [(r.st_mode, r.st_size == s.st_size, r.st_mtime == s.st_mtime) for r in [s.__replace__(st_mode=5)]] if False else attempt(lambda: s.__replace__(st_mode=5)))
    t("cannot be set", lambda: setattr(s, "st_mode", 1))
    for kwargs in ({"dir_fd": "a"}, {"dir_fd": 1.5}, {"dir_fd": 2 ** 40}, {"dir_fd": -2 ** 40}, {"dir_fd": None}, {"follow_symlinks": None}, {"other": 1}, {"dir_fd": True}):
        t("stat('file', **%r)" % (kwargs,), lambda: posix.stat("file", **kwargs).st_size)
    t("stat('file', None)", lambda: posix.stat("file", None))
    t("stat()", lambda: posix.stat())
    t("stat(path='file')", lambda: posix.stat(path="file").st_size)

    print("---- by way of an open directory")
    d = posix.open(".", posix.O_RDONLY)
    f = posix.open("file", posix.O_RDONLY)
    t("stat", lambda: (posix.stat("file", dir_fd=d).st_size, oct(posix.stat("link", dir_fd=d, follow_symlinks=False).st_mode), oct(posix.lstat("link", dir_fd=d).st_mode), posix.stat("file", dir_fd=Index(d)).st_size))
    t("of an open file", lambda: (posix.stat(f).st_size, posix.stat(Index(f)).st_size, posix.fstat(f) == posix.stat(f)))
    t("both", lambda: posix.stat(f, dir_fd=d))
    t("an open file, and not to follow links", lambda: posix.stat(f, follow_symlinks=False))
    t("one that is not a directory", lambda: posix.stat("file", dir_fd=f))
    t("one that is not open", lambda: posix.stat("file", dir_fd=9999))
    t("the rest", lambda: (posix.mkdir("made", dir_fd=d), posix.rename("made", "moved", src_dir_fd=d, dst_dir_fd=d), posix.replace("moved", "again", src_dir_fd=d), posix.rmdir("again", dir_fd=d), posix.symlink("x", "sl", dir_fd=d), posix.readlink("sl", dir_fd=d),
                           posix.link("file", "hard", src_dir_fd=d, dst_dir_fd=d), posix.stat("file").st_nlink, posix.unlink("hard", dir_fd=d), posix.remove("sl", dir_fd=d), posix.access("file", posix.R_OK, dir_fd=d), posix.chmod("file", 0o600, dir_fd=d), oct(posix.stat("file").st_mode),
                           posix.chmod("file", 0o644), posix.utime("file", (1, 2), dir_fd=d), posix.stat("file").st_mtime, posix.mkfifo("ff", dir_fd=d), oct(posix.stat("ff").st_mode), posix.unlink("ff"), posix.chown("file", -1, -1, dir_fd=d)))
    t("that fail", lambda: [attempt(g) for g in (lambda: posix.mkdir("dir", dir_fd=d), lambda: posix.rmdir("absent", dir_fd=d), lambda: posix.unlink("absent", dir_fd=d), lambda: posix.rename("absent", "b", src_dir_fd=d), lambda: posix.readlink("file", dir_fd=d),
                                                 lambda: posix.symlink("a", "file", dir_fd=d), lambda: posix.link("absent", "b", src_dir_fd=d), lambda: posix.open("absent", 0, dir_fd=d))])
    t("listdir and scandir", lambda: (sorted(posix.listdir(d)), sorted(posix.listdir(d)), sorted(e.name for e in posix.scandir(d)), attempt(lambda: posix.listdir(f)), attempt(lambda: posix.scandir(f)), attempt(lambda: posix.listdir(9999))))
    t("chdir", lambda: (posix.chdir("dir"), posix.getcwd() == base + "/dir" or posix.getcwd().endswith("/dir"), posix.chdir(d), posix.listdir() == posix.listdir("."), posix.fchdir(d), attempt(lambda: posix.chdir(f)), attempt(lambda: posix.fchdir(f)), attempt(lambda: posix.fchdir(-1)), attempt(lambda: posix.fchdir("a"))))
    posix.close(f)
    posix.close(d)

    print("---- making, moving and removing")
    t("mkdir", lambda: (posix.mkdir("a"), oct(posix.stat("a").st_mode), posix.mkdir("b", 0o700), oct(posix.stat("b").st_mode), posix.mkdir(b"c", mode=0o777), oct(posix.stat("c").st_mode), posix.mkdir(P("e")), sorted(posix.listdir())))
    for args in (("a",), ("file",), ("absent/x",), ("file/x",), ("",), ("a", "x"), ("z", 2 ** 40), ("z", 1.5)):
        t("mkdir%r" % (args,), lambda: posix.mkdir(*args))
    write("a/inside")
    for name in ("a", "file", "absent", "link", "", "."):
        t("rmdir(%r)" % name, lambda: posix.rmdir(name))
    for name in ("a", "absent", "", "file/x"):
        t("unlink(%r)" % name, lambda: posix.unlink(name))
        t("remove(%r)" % name, lambda: posix.remove(name))
    t("rename", lambda: (write("r1", b"1"), write("r2", b"2"), posix.rename("r1", "r3"), read("r3"), posix.rename("r3", "r2"), read("r2"), posix.replace(b"r2", P("r4")), read("r4"), posix.rename("r4", "r4"), posix.unlink("r4")))
    for args in (("absent", "x"), ("file", "a"), ("a", "file"), ("a", "b/c/d"), ("a", "a/sub"), ("b", "a"), (b"absent", "x"), ("absent", b"x"), (P("absent"), P(b"x")), ("", "x"), ("file", "")):
        t("rename%r" % (tuple(a if not isinstance(a, P) else "P(%r)" % a.value for a in args),), lambda: posix.rename(*args))
        t("replace of the same", lambda: posix.replace(*args))
    t("link", lambda: (posix.link("file", "hard"), posix.stat("file").st_nlink, posix.stat("hard").st_ino == posix.stat("file").st_ino, posix.unlink("hard"), posix.stat("file").st_nlink))
    t("of a link", lambda: (posix.link("link", "h1"), oct(posix.lstat("h1").st_mode), posix.link("link", "h2", follow_symlinks=False), oct(posix.lstat("h2").st_mode), posix.link("link", "h3", follow_symlinks=True), oct(posix.lstat("h3").st_mode), posix.unlink("h1"), posix.unlink("h2"), posix.unlink("h3")))
    for args in (("absent", "x"), ("file", "file"), ("dir", "x"), ("file", "absent/x")):
        t("link%r" % (args,), lambda: posix.link(*args))
    t("symlink and readlink", lambda: (posix.symlink("target", "s1"), posix.readlink("s1"), posix.readlink(b"s1"), posix.readlink(P("s1")), posix.readlink(P(b"s1")), posix.symlink(b"\xff\xfe", "s2"), posix.readlink("s2"), posix.readlink(b"s2"), posix.symlink("\xe9", "s3", True),
                                       posix.readlink("s3"), posix.readlink(b"s3"), posix.symlink("x", "s4", target_is_directory=[]), posix.unlink("s1"), posix.unlink("s2"), posix.unlink("s3"), posix.unlink("s4")))
    for args in (("x", "file"), ("x", "absent/y"), ("", "s5"), ("x", "")):
        t("symlink%r" % (args,), lambda: posix.symlink(*args))
    for name in ("file", "absent", "dir", ""):
        t("readlink(%r)" % name, lambda: posix.readlink(name))
    t("names that are not text", lambda: (write(b"\xff\xfe.bin", b"x"), [n for n in posix.listdir() if "bin" in n], [n for n in posix.listdir(b".") if b"bin" in n], read("\udcff\udcfe.bin"), posix.unlink("\udcff\udcfe.bin")) if sys.platform != "darwin" else attempt(lambda: write(b"\xff\xfe.bin", b"x")))
    t("names that are not ASCII", lambda: (write("\xe9€\U0001F600", b"x"), [n for n in posix.listdir() if ord(n[0]) > 127 or ord(n[-1]) > 127].__len__(), read("\xe9€\U0001F600"), posix.unlink("\xe9€\U0001F600".encode())))

    print("---- listdir and scandir")
    t("listdir", lambda: (sorted(posix.listdir()), sorted(posix.listdir(None)) == sorted(posix.listdir(".")), sorted(posix.listdir("a")), sorted(posix.listdir(b"a")), sorted(posix.listdir(P("a"))), sorted(posix.listdir(P(b"a"))), posix.listdir("b"), sorted(posix.listdir(path="a")), type(posix.listdir()).__name__))
    for name in ("absent", "file", "", "dangling", "dirlink"):
        t("listdir(%r)" % name, lambda: posix.listdir(name))
        t("scandir(%r)" % name, lambda: [e.name for e in posix.scandir(name)])
    it = posix.scandir()
    t("what scandir gives", lambda: (type(it).__name__, type(it).__module__, iter(it) is it, sorted(k for k in vars(type(it)) if k != "__doc__"), attempt(lambda: type(it)())))
    entries = {e.name: e for e in it}
    t("gone through", lambda: (sorted(entries), attempt(lambda: next(it)), it.close(), it.close(), attempt(lambda: next(it))))
    for name in ("file", "dir", "link", "dangling", "dirlink"):
        e = entries[name]
        t("the entry for %r" % name, lambda: (e.name, e.path, repr(e), e.is_file(), e.is_dir(), e.is_symlink(), e.is_junction(), e.is_file(follow_symlinks=False), e.is_dir(follow_symlinks=False), e.__fspath__(), posix.fspath(e), e.inode() == posix.lstat(name).st_ino,
                                              attempt(lambda: oct(e.stat().st_mode)), oct(e.stat(follow_symlinks=False).st_mode), attempt(lambda: e.stat() is e.stat()), e.stat(follow_symlinks=False) is e.stat(follow_symlinks=False)))
    e = entries["file"]
    t("what it is", lambda: (type(e).__name__, type(e).__module__, type(e) is posix.DirEntry, attempt(lambda: posix.DirEntry()), attempt(lambda: setattr(e, "name", 1)), attempt(lambda: setattr(e, "x", 1)), posix.DirEntry[int], attempt(lambda: hash(e) == hash(e)), e == e, e != entries["dir"]))
    for name, args, kwargs in (("is_dir", (True,), {}), ("is_file", (), {"other": 1}), ("stat", (1,), {}), ("is_symlink", (1,), {}), ("is_symlink", (), {"a": 1}), ("inode", (1,), {}), ("is_junction", (1,), {}), ("__fspath__", (1,), {}), ("stat", (), {"follow_symlinks": 0})):
        t("%s(*%r, **%r)" % (name, args, kwargs), lambda: type(getattr(e, name)(*args, **kwargs)).__name__)
    t("what is asked once is kept", lambda: [(x.stat().st_size, write("kept", b"longer"), x.stat().st_size, posix.stat("kept").st_size, posix.unlink("kept"), x.stat().st_size, x.is_file()) for _ in [write("kept", b"ab")] for x in [y for y in posix.scandir() if y.name == "kept"]])
    t("what has gone since", lambda: [(posix.unlink("gone"), x.is_file(), x.is_dir(), x.is_symlink(), attempt(x.stat)) for _ in [write("gone")] for x in [y for y in posix.scandir() if y.name == "gone"]])
    t("a link to what has gone", lambda: [(x.is_file(), x.is_dir(), x.is_symlink(), attempt(x.stat)) for x in [entries["dangling"]]])
    t("paths", lambda: (sorted(x.path for x in posix.scandir("a")), sorted(x.path for x in posix.scandir("a/")), sorted(x.path for x in posix.scandir(b"a")), sorted(x.path for x in posix.scandir(P("a"))), sorted(x.path for x in posix.scandir("./a//")), sorted(repr(x) for x in posix.scandir(b"a")),
                        sorted(x.path for x in posix.scandir(None))[:2], sorted(x.path for x in posix.scandir("."))[:2], sorted(x.path for x in posix.scandir(path="a"))))
    t("with", lambda: [(x.__enter__() is x, next(x).name != "", x.__exit__(None, None, None), attempt(lambda: next(x)), x.__exit__(), x.__exit__(1, 2, 3, 4)) for x in [posix.scandir()]])
    t("close(1)", lambda: posix.scandir().close(1))
    t("by an open directory", lambda: [(sorted((x.name, x.path, x.is_file(), x.stat().st_size) for x in posix.scandir(fd)), posix.close(fd)) for fd in [posix.open("a", posix.O_RDONLY)]])

    print("---- permissions, owners and times")
    t("chmod", lambda: [(posix.chmod("file", m), oct(posix.stat("file").st_mode))[1] for m in (0, 0o777, 0o4755, 0o644)])
    t("of a link", lambda: (posix.chmod("link", 0o600), oct(posix.stat("file").st_mode), oct(posix.lstat("link").st_mode), posix.chmod("link", 0o700, follow_symlinks=False), oct(posix.stat("file").st_mode), oct(posix.lstat("link").st_mode), posix.lchmod("link", 0o755), oct(posix.lstat("link").st_mode), posix.chmod("file", 0o644)))
    for args in (("absent", 0), ("file", "a"), ("file", 1.5), ("file", 2 ** 40), ("file",), ("dangling", 0)):
        t("chmod%r" % (args,), lambda: posix.chmod(*args))
    t("access", lambda: [(posix.access("file", m), posix.access("dir", m), posix.access("absent", m), posix.access("dangling", m), posix.access("dangling", m, follow_symlinks=False), posix.access("file", m, effective_ids=True)) for m in (posix.F_OK, posix.R_OK, posix.W_OK, posix.X_OK, posix.R_OK | posix.W_OK)])
    t("what cannot be read", lambda: (posix.chmod("file", 0), posix.access("file", posix.R_OK), posix.access("file", posix.F_OK), attempt(lambda: posix.open("file", 0)) if posix.getuid() else None, posix.chmod("file", 0o644)))
    for args in (("file",), ("file", "a"), ("file", 2 ** 40), ("", 0)):
        t("access%r" % (args,), lambda: posix.access(*args))
    t("chown", lambda: (posix.chown("file", -1, -1), posix.chown("file", posix.getuid(), posix.getgid()), posix.lchown("link", -1, -1), posix.chown("link", -1, -1, follow_symlinks=False), posix.chown("file", Index(-1), Index(-1))))
    for args in (("absent", -1, -1), ("file", "a", -1), ("file", -1, "a"), ("file", 1.5, -1), ("file", -2, -1), ("file", -1, -2), ("file", 2 ** 32, -1), ("file", 2 ** 32 - 1, -1), ("file", -1, 2 ** 64), ("file", 2 ** 63, -1), ("file", -2 ** 70, -1), ("file", None, None), ("file", -1)):
        t("chown%r" % (args,), lambda: posix.chown(*args) if args[1:] != (2 ** 32 - 1, -1) else "as -1 is")
    t("utime", lambda: [(posix.utime("file", a), posix.stat("file").st_atime_ns, posix.stat("file").st_mtime_ns, posix.stat("file").st_atime, posix.stat("file").st_mtime)[1:] for a in ((1, 2), (1.5, 2.25), (0, 0), (10 ** 9, 10 ** 9 + 0.123456789), (1.9999999999, 2.0000000001), (True, False), (-1.5, -1), (Index(7), 8))])
    t("in nanoseconds", lambda: [(posix.utime("file", ns=a), posix.stat("file").st_atime_ns, posix.stat("file").st_mtime_ns)[1:] for a in ((1, 2), (10 ** 9 + 1, 2 * 10 ** 9 - 1), (123456789012345678, 0), (-1, -10 ** 9 - 1), (True, 5))])
    t("now", lambda: (posix.utime("file"), posix.stat("file").st_mtime > 10 ** 9, posix.utime("file", None), posix.utime("file", times=None), posix.stat("file").st_atime > 10 ** 9))
    t("of a link", lambda: (posix.utime("file", (5, 5)), posix.utime("link", (9, 9), follow_symlinks=False), posix.stat("file").st_mtime, posix.lstat("link").st_mtime, posix.utime("link", (7, 7)), posix.stat("file").st_mtime, posix.lstat("link").st_mtime))
    t("of an open file", lambda: [(posix.utime(fd, (3, 4)), posix.stat("file").st_mtime, posix.utime(fd, ns=(5, 6)), posix.stat("file").st_mtime_ns, attempt(lambda: posix.utime(fd, (1, 1), follow_symlinks=False)), attempt(lambda: posix.utime(fd, (1, 1), dir_fd=fd)), posix.close(fd)) for fd in [posix.open("file", 0)]])


    class Divmod:
        def __init__(self, result): self.result = result
        def __divmod__(self, other): return self.result


    for args, kwargs in ((((1, 2),), {"ns": (1, 2)}), ((None,), {"ns": (1, 2)}), (([1, 2],), {}), (((1,),), {}), (((1, 2, 3),), {}), ((5,), {}), ((("a", 1),), {}), (((1, None),), {}), (((float("nan"), 1),), {}), (((float("inf"), 1),), {}), (((2 ** 70, 1),), {}), (((1e30, 1),), {}),
                         ((), {"ns": [1, 2]}), ((), {"ns": (1,)}), ((), {"ns": None}), ((), {"ns": (1.5, 1)}), ((), {"ns": ("a", 1)}), ((), {"ns": (2 ** 100, 1)}), ((), {"ns": (Divmod(5), 1)}), ((), {"ns": (Divmod((1,)), 1)}), ((), {"ns": (Divmod((1, 2)), 1)}), ((), {"ns": (Divmod(("a", 2)), 1)}),
                         ((), {"ns": (Divmod((1, "b")), 1)}), ((), {"ns": (Divmod((1, 2 ** 70)), 1)})):
        t("utime('file', *%r, **%s)" % (args, {k: tuple(x if not isinstance(x, Divmod) else "Divmod(%r)" % (x.result,) for x in v) if isinstance(v, tuple) else v for k, v in kwargs.items()}), lambda: posix.utime("file", *args, **kwargs))
    t("utime('absent')", lambda: posix.utime("absent"))
    t("umask", lambda: (oct(posix.umask(0o077)), write("masked"), oct(posix.stat("masked").st_mode), posix.mkdir("maskeddir"), oct(posix.stat("maskeddir").st_mode), oct(posix.umask(0o022)), attempt(lambda: posix.umask("a")), attempt(lambda: posix.umask()), attempt(lambda: posix.umask(mask=1))))
    t("chflags", lambda: (posix.chflags("file", 0), posix.stat("file").st_flags, posix.lchflags("link", 0), posix.chflags("link", 0, False), posix.chflags("file", 0, follow_symlinks=True), attempt(lambda: posix.chflags("absent", 0)), attempt(lambda: posix.chflags("file", "a")), attempt(lambda: posix.chflags("file", 1.5)),
                             attempt(lambda: posix.lchflags("file", None)), attempt(lambda: posix.lchflags("absent", 0))))
    t("truncate", lambda: (write("tr", b"0123456789"), posix.truncate("tr", 4), read("tr"), posix.truncate(b"tr", 6), read("tr"), posix.truncate(P("tr"), length=0), read("tr"), attempt(lambda: posix.truncate("tr", -1)), attempt(lambda: posix.truncate("absent", 0)), attempt(lambda: posix.truncate("dir", 0)),
                              attempt(lambda: posix.truncate("tr", "a")), attempt(lambda: posix.truncate("tr", 2 ** 70)), attempt(lambda: posix.truncate("tr", 1.5)), attempt(lambda: posix.truncate("tr"))))

    print("---- special files")
    t("mkfifo", lambda: (posix.mkfifo("fifo"), oct(posix.stat("fifo").st_mode), posix.mkfifo("fifo2", 0o600), oct(posix.stat("fifo2").st_mode), attempt(lambda: posix.mkfifo("fifo")), attempt(lambda: posix.mkfifo("absent/f")), attempt(lambda: posix.mkfifo("f", "a"))))
    t("mknod", lambda: (posix.mknod("node"), oct(posix.stat("node").st_mode), posix.mknod("node2", 0o644 | 0o100000, 0), oct(posix.stat("node2").st_mode), attempt(lambda: posix.mknod("node")), attempt(lambda: posix.mknod("n", "a")), attempt(lambda: posix.mknod("n", 0o600, "a")), attempt(lambda: posix.mknod("n", 0o600, -5)),
                           attempt(lambda: posix.mknod("n", 0o600, 2 ** 70)), attempt(lambda: posix.mknod("n", 0o600, 2 ** 40))))
    t("device numbers", lambda: [(posix.major(x), posix.minor(x)) for x in (0, 1, 255, 256, 2 ** 24, 2 ** 24 + 5, 2 ** 31, 2 ** 32 - 1, -1, Index(300))])
    t("makedev", lambda: [posix.makedev(a, b) for a, b in ((0, 0), (1, 2), (255, 2 ** 24 - 1), (256, 0), (0, 2 ** 24), (-1, -1), (1, -1), (Index(1), Index(1)))])
    for name, args in (("major", (-2,)), ("major", (2 ** 32,)), ("major", (2 ** 64,)), ("major", ("a",)), ("major", (1.5,)), ("minor", (-2,)), ("minor", (2 ** 70,)), ("makedev", (2 ** 32, 0)), ("makedev", (0, 2 ** 32)), ("makedev", (-2, 0)), ("makedev", ("a", 0)), ("makedev", (1,))):
        t("%s%r" % (name, args), lambda: getattr(posix, name)(*args))

    print("---- the file system")
    v = posix.statvfs(".")
    t("statvfs", lambda: (type(v).__name__, type(v).__module__, len(v), v.n_fields, [type(x).__name__ for x in v], type(v.f_fsid).__name__, v.f_namemax, v.f_bsize > 0, v.f_frsize > 0, v.f_blocks >= v.f_bfree >= 0, v == posix.statvfs(b"."), v.f_flag & ~3))
    t("of an open file", lambda: [(posix.statvfs(fd)[:3] == v[:3], posix.fstatvfs(fd)[:3] == v[:3], posix.close(fd)) for fd in [posix.open(".", 0)]])
    t("that fail", lambda: (attempt(lambda: posix.statvfs("absent")), attempt(lambda: posix.statvfs(9999)), attempt(lambda: posix.fstatvfs(9999)), attempt(lambda: posix.fstatvfs("a"))))
    t("pathconf", lambda: (posix.pathconf(".", "PC_NAME_MAX"), posix.pathconf(b".", posix.pathconf_names["PC_NAME_MAX"]), posix.pathconf(P("."), "PC_PATH_MAX"), posix.pathconf(path=".", name="PC_LINK_MAX") > 0, posix.pathconf(".", Index(posix.pathconf_names["PC_NAME_MAX"]))))
    t("of an open file", lambda: [(posix.pathconf(fd, "PC_NAME_MAX"), posix.fpathconf(fd, "PC_NAME_MAX"), posix.close(fd)) for fd in [posix.open(".", 0)]])
    for args in ((".", "PC_NOTHING"), (".", 99999), (".", 1.5), (".", None), (".", b"PC_NAME_MAX"), ("absent", "PC_NAME_MAX"), (".", 2 ** 40), (".",), (9999, "PC_NAME_MAX")):
        t("pathconf%r" % (args,), lambda: posix.pathconf(*args))
    for args in ((9999, "PC_NAME_MAX"), ("a", 1), (-1, 1), (0, "PC_NOTHING"), (1.5, 1)):
        t("fpathconf%r" % (args,), lambda: posix.fpathconf(*args))
    t("names that a program has added", lambda: (posix.pathconf_names.__setitem__("MINE", posix.pathconf_names["PC_NAME_MAX"]), posix.pathconf(".", "MINE"), posix.pathconf_names.__setitem__("ODD", "a"), attempt(lambda: posix.pathconf(".", "ODD")), posix.pathconf_names.pop("MINE"), posix.pathconf_names.pop("ODD")))

    print("---- where this is")
    t("getcwd", lambda: (type(posix.getcwd()).__name__, type(posix.getcwdb()).__name__, posix.getcwd().encode() == posix.getcwdb(), posix.stat(posix.getcwd()).st_ino == posix.stat(".").st_ino, attempt(lambda: posix.getcwd(1))))
    for name in ("absent", "file", "", "dangling"):
        t("chdir(%r)" % name, lambda: posix.chdir(name))
    t("chdir", lambda: (posix.chdir("dirlink"), posix.getcwd().rpartition("/")[2], posix.chdir(".."), posix.chdir(b"dir"), posix.chdir(P("..")), posix.chdir(path="."), posix.stat(".").st_ino == posix.stat(base).st_ino))
    t("one that has gone", lambda: (posix.mkdir("going"), posix.chdir("going"), posix.rmdir("../going"), attempt(posix.getcwd), attempt(posix.getcwdb), posix.chdir(base)))
    t("chroot", lambda: (attempt(lambda: posix.chroot("absent")), attempt(lambda: posix.chroot(".")) if posix.getuid() else None))
finally:
    posix.chdir(base)
    remove_all(".")
    posix.chdir(start)
    posix.rmdir(base)

print("---- what is only a matter of names")
PATHS = ["", ".", "..", "/", "//", "///", "////", "a", "a/", "a//", "/a", "//a", "///a", "a/b", "a//b", "a/./b", "a/../b", "a/b/..", "a/b/../..", "a/b/../../..", "../a", "../../a", "/..", "/../a", "//..", "//../a", "/.", "/./", "./", "./a", "././a", ".//a", "a/.", "a/./", "a/..", "..a", "a..", "...", ".../a",
         "a/.../b", "/a/b/c/../../d", "a/b/c/../../../../d", "./..", "../.", ".././..", "a/\0/b", "\0", "a\0", "\xe9/../€", "\U0001F600/./x", "/a/", "//a//b//", "a b/ c", ".a/.b", "-/..", "\udcff/x/.."]
for p in PATHS:
    t("_path_normpath(%a)" % p, lambda: (posix._path_normpath(p), posix._path_normpath(p.encode("utf-8", "surrogateescape")), posix._path_splitroot_ex(p), posix._path_splitroot_ex(p.encode("utf-8", "surrogateescape"))))
t("of other things", lambda: (posix._path_normpath(P("a/../b")), posix._path_normpath(P(b"a/../b")), posix._path_normpath(path="x//y"), posix._path_splitroot_ex(p="/x"), posix._path_splitroot_ex(P("//x")), type(posix._path_normpath(S("a"))).__name__, type(posix._path_normpath(B(b"a"))).__name__))
state = [88172645]


def random(n):
    v = state[0]
    v ^= (v << 13) & 0xFFFFFFFF
    v ^= v >> 17
    v ^= (v << 5) & 0xFFFFFFFF
    state[0] = v
    return v % n


digest = 0
for i in range(20000):
    p = "".join(("/", "/", ".", ".", "a", "b")[random(6)] for j in range(random(12)))
    for ch in posix._path_normpath(p) + "|" + "|".join(posix._path_splitroot_ex(p)):
        digest = (digest * 31 + ord(ch)) % 1000000007
print("at random =>", digest)
