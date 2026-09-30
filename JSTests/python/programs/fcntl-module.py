# The module fcntl.
import array
import errno
import fcntl
import os
import struct
import sys
import tempfile


def t(label, f):
    try:
        print(label, "=>", repr(f()))
    except BaseException as e:
        print(label, "=>", type(e).__name__ + ":", e)


directory = tempfile.mkdtemp()
path = os.path.join(directory, "file")
with open(path, "wb") as f:
    f.write(b"0123456789" * 10)

print("---- what there is")
print(sorted(n for n in dir(fcntl) if not n.startswith("_") and not n.isupper()))
print([(n, getattr(fcntl, n)) for n in sorted(dir(fcntl)) if n.isupper()])
print(all(type(getattr(fcntl, n)) is int for n in dir(fcntl) if n.isupper()))
print(fcntl.__name__, fcntl.__doc__.split("\n")[0], fcntl.__spec__.origin)
for name in ("fcntl", "ioctl", "flock", "lockf"):
    function = getattr(fcntl, name)
    print(name, function.__text_signature__, function.__module__, function.__doc__.split("\n")[0])

print("---- fcntl(), with a number")
r, w = os.pipe()
t("F_GETFD", lambda: fcntl.fcntl(r, fcntl.F_GETFD))
t("F_SETFD", lambda: (fcntl.fcntl(r, fcntl.F_SETFD, 0), fcntl.fcntl(r, fcntl.F_GETFD), os.get_inheritable(r)))
t("and back", lambda: (fcntl.fcntl(r, fcntl.F_SETFD, fcntl.FD_CLOEXEC), fcntl.fcntl(r, fcntl.F_GETFD), os.get_inheritable(r)))
t("F_GETFL", lambda: (fcntl.fcntl(r, fcntl.F_GETFL) & os.O_ACCMODE, fcntl.fcntl(w, fcntl.F_GETFL) & os.O_ACCMODE))
t("F_SETFL", lambda: (fcntl.fcntl(r, fcntl.F_SETFL, os.O_NONBLOCK), bool(fcntl.fcntl(r, fcntl.F_GETFL) & os.O_NONBLOCK), os.get_blocking(r)))
t("and back", lambda: (fcntl.fcntl(r, fcntl.F_SETFL, 0), bool(fcntl.fcntl(r, fcntl.F_GETFL) & os.O_NONBLOCK), os.get_blocking(r)))
def duplicate(command):
    d = fcntl.fcntl(r, command, 100)
    try:
        return d >= 100, os.get_inheritable(d), os.fstat(d).st_ino == os.fstat(r).st_ino
    finally:
        os.close(d)
t("F_DUPFD", lambda: duplicate(fcntl.F_DUPFD))
t("F_DUPFD_CLOEXEC", lambda: duplicate(fcntl.F_DUPFD_CLOEXEC))
t("no third", lambda: fcntl.fcntl(r, fcntl.F_SETFD))
t("and so", lambda: fcntl.fcntl(r, fcntl.F_GETFD))
t("True", lambda: (fcntl.fcntl(r, fcntl.F_SETFD, True), fcntl.fcntl(r, fcntl.F_GETFD)))
class Index:
    def __init__(self, value): self.value = value
    def __index__(self): return self.value
t("what has __index__", lambda: (fcntl.fcntl(r, fcntl.F_SETFD, Index(0)), fcntl.fcntl(r, fcntl.F_GETFD)))
t("as much of it as fits", lambda: (fcntl.fcntl(r, fcntl.F_SETFD, 2 ** 32 + 1), fcntl.fcntl(r, fcntl.F_GETFD)))
t("of a great deal", lambda: (fcntl.fcntl(r, fcntl.F_SETFD, 2 ** 100), fcntl.fcntl(r, fcntl.F_GETFD)))
t("of less than nothing", lambda: (fcntl.fcntl(r, fcntl.F_SETFD, -2 ** 32 + 1), fcntl.fcntl(r, fcntl.F_GETFD)))
t("whose __index__ raises", lambda: fcntl.fcntl(r, fcntl.F_SETFD, type("I", (), {"__index__": lambda s: 1 / 0})()))
t("whose __index__ gives something else", lambda: fcntl.fcntl(r, fcntl.F_SETFD, type("I", (), {"__index__": lambda s: "x"})()))

print("---- what it will not take")
for label, arguments in (("nothing", ()), ("one", (r,)), ("four", (r, 1, 2, 3)), ("a float", (r, fcntl.F_SETFD, 1.0)), ("None", (r, fcntl.F_SETFD, None)), ("a list", (r, fcntl.F_SETFD, [])), ("an object", (r, fcntl.F_SETFD, object())), ("a class of a module", (r, fcntl.F_SETFD, array.array)), ("an instance of that", (r, fcntl.F_SETFD, tempfile.TemporaryDirectory.__new__(tempfile.TemporaryDirectory))),
                         ("a command that is a float", (r, 1.0)), ("that is a string", (r, "x")), ("that is too big", (r, 2 ** 31)), ("that is too small", (r, -2 ** 31 - 1)), ("that there is none of", (r, 12345)), ("less than nothing", (r, -1)),
                         ("a descriptor that is a float", (1.0, 1)), ("that is a string", ("x", 1)), ("that is None", (None, 1)), ("that is less than nothing", (-1, 1)), ("that is too big", (2 ** 31, 1)), ("that is not open", (9999, fcntl.F_GETFD))):
    t(label, lambda: fcntl.fcntl(*arguments))
t("by name", lambda: fcntl.fcntl(fd=r, cmd=1))

print("---- what has fileno()")
with open(path, "rb") as f:
    t("a file", lambda: fcntl.fcntl(f, fcntl.F_GETFD))
t("that is closed", lambda: fcntl.fcntl(f, fcntl.F_GETFD))
class File:
    def __init__(self, value): self.value = value
    def fileno(self): return self.value
t("an object", lambda: fcntl.fcntl(File(r), fcntl.F_GETFD) == fcntl.fcntl(r, fcntl.F_GETFD))
t("that gives less than nothing", lambda: fcntl.fcntl(File(-1), fcntl.F_GETFD))
t("that gives a string", lambda: fcntl.fcntl(File("x"), fcntl.F_GETFD))
t("that gives a float", lambda: fcntl.fcntl(File(1.0), fcntl.F_GETFD))
t("that gives True", lambda: fcntl.fcntl(File(True), fcntl.F_GETFD) >= 0)
t("that raises", lambda: fcntl.fcntl(type("F", (), {"fileno": lambda s: 1 / 0})(), fcntl.F_GETFD))
t("whose fileno is not to be called", lambda: fcntl.fcntl(type("F", (), {"fileno": 1})(), fcntl.F_GETFD))

print("---- fcntl(), with bytes")
# struct flock, which is not laid out the same everywhere
if sys.platform == "darwin":
    FLOCK = "qqihh"
    pack = lambda kind, whence, start, length: struct.pack(FLOCK, start, length, 0, kind, whence)
    unpack = lambda data: (lambda start, length, pid, kind, whence: (kind, whence, start, length))(*struct.unpack(FLOCK, data))
else:
    FLOCK = "hhqqi4x"
    pack = lambda kind, whence, start, length: struct.pack(FLOCK, kind, whence, start, length, 0)
    unpack = lambda data: struct.unpack(FLOCK, data)[:4]
with open(path, "r+b") as f:
    t("F_GETLK", lambda: unpack(fcntl.fcntl(f, fcntl.F_GETLK, pack(fcntl.F_WRLCK, 0, 0, 0)))[0] == fcntl.F_UNLCK)
    t("what comes back", lambda: (type(fcntl.fcntl(f, fcntl.F_GETLK, pack(fcntl.F_WRLCK, 0, 0, 0))), len(fcntl.fcntl(f, fcntl.F_GETLK, pack(fcntl.F_WRLCK, 0, 0, 0))) == struct.calcsize(FLOCK)))
    t("F_SETLK", lambda: fcntl.fcntl(f, fcntl.F_SETLK, pack(fcntl.F_WRLCK, 0, 0, 10)) == pack(fcntl.F_WRLCK, 0, 0, 10))
    t("a bytearray, which is left as it is", lambda: (lambda b: (type(fcntl.fcntl(f, fcntl.F_GETLK, b)), b == pack(fcntl.F_WRLCK, 0, 0, 0)))(bytearray(pack(fcntl.F_WRLCK, 0, 0, 0))))
    t("a memoryview", lambda: type(fcntl.fcntl(f, fcntl.F_GETLK, memoryview(pack(fcntl.F_WRLCK, 0, 0, 0)))))
    t("an array", lambda: type(fcntl.fcntl(f, fcntl.F_GETLK, array.array("b", pack(fcntl.F_WRLCK, 0, 0, 0)))))
    t("a memoryview that skips", lambda: fcntl.fcntl(f, fcntl.F_GETLK, memoryview(bytes(100))[::2]))
    t("a string", lambda: fcntl.fcntl(f, fcntl.F_GETFD, "abc"))
    t("a string that is not ASCII", lambda: fcntl.fcntl(f, fcntl.F_GETFD, "\xe9€"))
    t("a string with half a character in it", lambda: fcntl.fcntl(f, fcntl.F_GETFD, "\ud800"))
    t("nothing", lambda: fcntl.fcntl(f, fcntl.F_GETFD, b""))
    t("as long as can be", lambda: len(fcntl.fcntl(f, fcntl.F_GETFD, bytes(1024))))
    t("longer", lambda: fcntl.fcntl(f, fcntl.F_GETFD, bytes(1025)))
    t("a string that is longer once it is encoded", lambda: fcntl.fcntl(f, fcntl.F_GETFD, "\xe9" * 513))
    t("that the system will not have", lambda: fcntl.fcntl(f, fcntl.F_SETLK, pack(99, 0, 0, 0)))
    if hasattr(fcntl, "F_GETPATH"):
        t("F_GETPATH", lambda: os.path.samefile(fcntl.fcntl(f, fcntl.F_GETPATH, bytes(1024)).rstrip(b"\0"), path))
    else:
        print("F_GETPATH => True")

print("---- flock()")
a = open(path, "rb")
b = open(path, "rb")
t("one has it", lambda: fcntl.flock(a, fcntl.LOCK_EX))
t("and so the other cannot", lambda: fcntl.flock(b, fcntl.LOCK_EX | fcntl.LOCK_NB))
def refused(f):
    try:
        f()
    except OSError as e:
        return type(e).__name__, e.errno in (errno.EAGAIN, errno.EACCES, errno.EWOULDBLOCK), e.filename
t("what that is", lambda: refused(lambda: fcntl.flock(b, fcntl.LOCK_SH | fcntl.LOCK_NB)))
t("let go of", lambda: fcntl.flock(a, fcntl.LOCK_UN))
t("now it can", lambda: fcntl.flock(b, fcntl.LOCK_EX | fcntl.LOCK_NB))
t("let go of twice", lambda: (fcntl.flock(b, fcntl.LOCK_UN), fcntl.flock(b, fcntl.LOCK_UN)))
t("both can share it", lambda: (fcntl.flock(a, fcntl.LOCK_SH), fcntl.flock(b, fcntl.LOCK_SH | fcntl.LOCK_NB)))
t("and then neither can have it", lambda: refused(lambda: fcntl.flock(a, fcntl.LOCK_EX | fcntl.LOCK_NB)))
t("closing lets go", lambda: (b.close(), fcntl.flock(a, fcntl.LOCK_EX | fcntl.LOCK_NB)))
t("by its number", lambda: fcntl.flock(a.fileno(), fcntl.LOCK_UN))
for label, arguments in (("nothing", ()), ("one", (a,)), ("three", (a, 1, 2)), ("no such thing", (a, 0)), ("nor this", (a, 12345)), ("less than nothing", (a, -1)), ("a float", (a, 1.0)), ("too big", (a, 2 ** 31)), ("what has __index__", (a, Index(fcntl.LOCK_UN))), ("not open", (9999, fcntl.LOCK_SH)), ("closed", (b, fcntl.LOCK_SH)), ("a pipe", (r, fcntl.LOCK_SH))):
    t(label, lambda: fcntl.flock(*arguments))
a.close()

print("---- lockf()")
with open(path, "r+b") as f:
    t("all of it", lambda: fcntl.lockf(f, fcntl.LOCK_EX))
    t("let go of", lambda: fcntl.lockf(f, fcntl.LOCK_UN))
    t("some of it", lambda: fcntl.lockf(f, fcntl.LOCK_EX | fcntl.LOCK_NB, 10, 5, os.SEEK_SET))
    t("from where it is", lambda: (f.seek(20), fcntl.lockf(f, fcntl.LOCK_EX, 10, 0, os.SEEK_CUR))[1])
    t("from the end", lambda: fcntl.lockf(f, fcntl.LOCK_SH, 10, -10, os.SEEK_END))
    t("backwards", lambda: fcntl.lockf(f, fcntl.LOCK_EX, -5, 50))
    t("before the beginning", lambda: fcntl.lockf(f, fcntl.LOCK_EX, 1, -5))
    t("a great deal", lambda: fcntl.lockf(f, fcntl.LOCK_EX, 2 ** 62, 0))
    t("too much", lambda: fcntl.lockf(f, fcntl.LOCK_EX, 2 ** 63, 0))
    t("from too far", lambda: fcntl.lockf(f, fcntl.LOCK_EX, 0, 2 ** 63))
    t("what has __index__", lambda: fcntl.lockf(f, fcntl.LOCK_EX, Index(1), Index(2), Index(0)))
    t("floats", lambda: fcntl.lockf(f, fcntl.LOCK_EX, 1.0))
    t("a float to start from", lambda: fcntl.lockf(f, fcntl.LOCK_EX, 1, 1.0))
    t("None", lambda: fcntl.lockf(f, fcntl.LOCK_EX, None))
    t("from nowhere", lambda: fcntl.lockf(f, fcntl.LOCK_EX, 0, 0, 99))
    t("no such thing", lambda: fcntl.lockf(f, 0))
    t("only not to wait", lambda: fcntl.lockf(f, fcntl.LOCK_NB))
    t("to let go and something else", lambda: fcntl.lockf(f, fcntl.LOCK_UN | fcntl.LOCK_NB))
    t("both kinds", lambda: fcntl.lockf(f, fcntl.LOCK_SH | fcntl.LOCK_EX))
    t("nothing", lambda: fcntl.lockf())
    t("one", lambda: fcntl.lockf(f))
    t("six", lambda: fcntl.lockf(f, 1, 2, 3, 4, 5))
    t("let go of all", lambda: fcntl.lockf(f, fcntl.LOCK_UN))
with open(path, "rb") as f:
    t("to write, of what is only read", lambda: fcntl.lockf(f, fcntl.LOCK_EX))
    t("to read", lambda: fcntl.lockf(f, fcntl.LOCK_SH))
t("not open", lambda: fcntl.lockf(9999, fcntl.LOCK_SH))
t("what is looked at first", lambda: fcntl.lockf(9999, 0))

print("---- ioctl()")
FIONREAD = 0x4004667F if sys.platform == "darwin" else 0x541B
TIOCGWINSZ = 0x40087468 if sys.platform == "darwin" else 0x5413
os.write(w, b"abcde")
def into(buffer, *more):
    result = fcntl.ioctl(r, FIONREAD, buffer, *more)
    return result, bytes(buffer)[:4] == struct.pack("i", 5), len(buffer)
t("into a bytearray", lambda: into(bytearray(4)))
t("into an array", lambda: (lambda a: (fcntl.ioctl(r, FIONREAD, a), a))(array.array("i", [0])))
t("into a memoryview", lambda: into(memoryview(bytearray(4))))
t("into one that is long", lambda: into(bytearray(1024)))
t("into one that is longer than is copied", lambda: into(bytearray(1025)))
t("into one that is very long", lambda: into(bytearray(100000)))
t("said to be written to", lambda: into(bytearray(4), True))
t("said not to be", lambda: (lambda b: (fcntl.ioctl(r, FIONREAD, b, False) == struct.pack("i", 5), bytes(b)))(bytearray(4)))
t("said with something else", lambda: (lambda b: (fcntl.ioctl(r, FIONREAD, b, []) == struct.pack("i", 5), bytes(b)))(bytearray(4)))
t("said not to be, and too long", lambda: fcntl.ioctl(r, FIONREAD, bytearray(1025), False))
t("bytes", lambda: fcntl.ioctl(r, FIONREAD, bytes(4)) == struct.pack("i", 5))
t("bytes, which are left as they are", lambda: (lambda b: (fcntl.ioctl(r, FIONREAD, b, True), b)[1])(bytes(4)))
t("bytes that are too long", lambda: fcntl.ioctl(r, FIONREAD, bytes(1025)))
t("a memoryview that cannot be written to", lambda: fcntl.ioctl(r, FIONREAD, memoryview(bytes(4))) == struct.pack("i", 5))
t("a string", lambda: fcntl.ioctl(r, FIONREAD, "abcd") == struct.pack("i", 5))
t("a string that is too long", lambda: fcntl.ioctl(r, FIONREAD, "a" * 1025))
t("a memoryview that skips", lambda: fcntl.ioctl(r, FIONREAD, memoryview(bytearray(8))[::2]))
t("what the flag raises", lambda: fcntl.ioctl(r, FIONREAD, bytearray(4), type("B", (), {"__bool__": lambda s: 1 / 0})()))
t("of what is not a terminal", lambda: fcntl.ioctl(r, TIOCGWINSZ, bytes(8)))
t("with a bytearray", lambda: fcntl.ioctl(r, TIOCGWINSZ, bytearray(8)))
t("with a number", lambda: fcntl.ioctl(r, TIOCGWINSZ, 0))
t("with nothing", lambda: fcntl.ioctl(r, TIOCGWINSZ))
t("as much of the request as fits", lambda: into(bytearray(4)) == (lambda b: (fcntl.ioctl(r, FIONREAD + 2 ** 64, b), bytes(b)[:4] == struct.pack("i", 5), len(b)))(bytearray(4)))
t("a request that is less than nothing", lambda: (lambda b: (fcntl.ioctl(r, FIONREAD - 2 ** 64, b), bytes(b) == struct.pack("i", 5)))(bytearray(4)))
for label, arguments in (("nothing", ()), ("one", (r,)), ("five", (r, 1, 2, 3, 4)), ("a request that is a float", (r, 1.0)), ("that is a string", (r, "x")), ("that is None", (r, None)), ("that has __index__", (r, Index(TIOCGWINSZ))), ("a float", (r, FIONREAD, 1.0)), ("None", (r, FIONREAD, None)), ("a list", (r, FIONREAD, [])), ("a number that is too big", (r, TIOCGWINSZ, 2 ** 31)), ("that is too small", (r, TIOCGWINSZ, -2 ** 31 - 1)), ("not open", (9999, FIONREAD, bytes(4))), ("less than nothing", (-1, FIONREAD)), ("a descriptor that is a string", ("x", FIONREAD))):
    t(label, lambda: fcntl.ioctl(*arguments))

print("---- what is told of")
told = []
sys.addaudithook(lambda event, arguments: told.append((event, arguments)) if event.startswith("fcntl.") else None)
def telling(f):
    told.clear()
    try:
        f()
    except Exception as e:
        told.append(type(e).__name__)
    return [(e[0], tuple("fd" if i == 0 else a for i, a in enumerate(e[1]))) if isinstance(e, tuple) else e for e in told]
with open(path, "r+b") as f:
    t("fcntl", lambda: telling(lambda: fcntl.fcntl(f, fcntl.F_GETFD)))
    t("with a number", lambda: telling(lambda: fcntl.fcntl(f, fcntl.F_SETFD, 1)))
    t("with bytes", lambda: telling(lambda: fcntl.fcntl(f, fcntl.F_GETFD, b"x")))
    t("with what it will not take", lambda: telling(lambda: fcntl.fcntl(f, fcntl.F_GETFD, None)))
    t("with a command it will not take", lambda: telling(lambda: fcntl.fcntl(f, "x")))
    t("ioctl", lambda: telling(lambda: fcntl.ioctl(f, TIOCGWINSZ)))
    t("with bytes", lambda: telling(lambda: fcntl.ioctl(f, TIOCGWINSZ, b"x", False)))
    t("with a request of which some does not fit", lambda: telling(lambda: fcntl.ioctl(f, TIOCGWINSZ + 2 ** 64)))
    t("flock", lambda: telling(lambda: fcntl.flock(f, fcntl.LOCK_UN)))
    t("lockf", lambda: telling(lambda: fcntl.lockf(f, fcntl.LOCK_UN)))
    t("with everything", lambda: telling(lambda: fcntl.lockf(f, fcntl.LOCK_SH, 1, 2, 0)))
    t("with what there is none of", lambda: telling(lambda: fcntl.lockf(f, 0, "a", "b")))

os.close(r)
os.close(w)
os.unlink(path)
os.rmdir(directory)
