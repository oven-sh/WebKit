# What os, signal and time have on Linux and not on macOS.
import errno
import os
import posix
import select
import signal
import stat
import sys
import tempfile
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


class File:
    def __init__(self, fd): self.fd = fd
    def fileno(self): return self.fd
    def __repr__(self): return "File"


class Index:
    def __init__(self, v): self.v = v
    def __index__(self): return self.v
    def __repr__(self): return "Index"


def closing(*descriptors):
    for d in descriptors:
        os.close(d)


WRONG = ("a", None, 1.5, [], 2 ** 31, -2 ** 31 - 1, 2 ** 64)
HERE = tempfile.mkdtemp()
os.chdir(HERE)

print("---- what there is")
# There is no forking here: see the README.
t("everything in posix", lambda: sorted(n for n in vars(posix) if not n.startswith("__") and n not in ("fork", "forkpty", "register_at_fork")))
t("the numbers", lambda: sorted((n, v) for n, v in vars(posix).items() if type(v) is int))
t("what can be asked of a descriptor, and so on", lambda: [sorted(f.__name__ for f in s) for s in (os.supports_fd, os.supports_dir_fd, os.supports_follow_symlinks, os.supports_effective_ids)])
NAMES = ("copy_file_range eventfd eventfd_read eventfd_write fdatasync getrandom getresgid getresuid getxattr listxattr memfd_create pidfd_open pipe2 posix_fadvise posix_fallocate removexattr sched_getaffinity sched_getparam "
         "sched_getscheduler sched_rr_get_interval sched_setaffinity sched_setparam sched_setscheduler sendfile setns setresgid setresuid setxattr splice timerfd_create timerfd_gettime timerfd_gettime_ns timerfd_settime "
         "timerfd_settime_ns unshare").split()
for name in NAMES:
    t(name, lambda: (lambda f: (type(f).__name__, f.__text_signature__, f.__doc__, f.__module__))(getattr(posix, name)))
    t(name + " with nothing, and with too much", lambda: (attempt(getattr(posix, name)), attempt(getattr(posix, name), *range(9)), attempt(lambda: getattr(posix, name)(nonsense=1))))

print("---- eventfd")
t("as it is made", lambda: [(d > 2, os.get_inheritable(d), os.get_blocking(d), stat.S_IFMT(os.fstat(d).st_mode), os.close(d)) for d in [os.eventfd(0)]])
t("what is written is added up", lambda: [(os.eventfd_write(d, 5), os.eventfd_write(d, 7), os.eventfd_read(d), os.eventfd_write(File(d), 1), os.eventfd_read(File(d)), os.close(d)) for d in [os.eventfd(0)]])
t("what it begins with", lambda: [(os.eventfd_read(d), os.close(d)) for d in [os.eventfd(3)]])
t("with nothing in it", lambda: [(attempt(os.eventfd_read, d), os.close(d)) for d in [os.eventfd(0, os.EFD_NONBLOCK)]])
t("one at a time", lambda: [(os.eventfd_read(d), os.eventfd_read(d), attempt(os.eventfd_read, d), os.close(d)) for d in [os.eventfd(2, os.EFD_SEMAPHORE | os.EFD_NONBLOCK)]])
t("as many as it will hold", lambda: [(os.eventfd_write(d, 2 ** 64 - 2), attempt(os.eventfd_write, d, 1), attempt(os.eventfd_write, d, 2 ** 64 - 1), os.eventfd_read(d), os.close(d)) for d in [os.eventfd(0, os.EFD_NONBLOCK)]])
t("by name", lambda: [(os.eventfd_write(fd=d, value=2), os.eventfd_read(fd=d), os.close(d)) for d in [os.eventfd(initval=1, flags=0)]])
t("it can be waited for", lambda: [(select.select([d], [], [], 0)[0] == [], os.eventfd_write(d, 1), select.select([d], [], [], 0)[0] == [d], os.close(d)) for d in [os.eventfd(0)]])
for a in WRONG + (-1, 2 ** 32, 2 ** 32 - 1, Index(1), True):
    t("eventfd(%s)" % ascii(a), lambda: (lambda d: (os.close(d), "made")[1] if type(d) is int else d)(attempt(os.eventfd, a)))
t("eventfd(0, flags)", lambda: [(lambda d: (os.close(d), "made")[1] if type(d) is int else d)(attempt(os.eventfd, 0, a)) for a in WRONG + (-1, 12345)])
t("eventfd_write(d, value)", lambda: [([attempt(os.eventfd_write, d, a) for a in WRONG + (-1, Index(1), True)], os.close(d)) for d in [os.eventfd(0)]])
t("of what is not one", lambda: (attempt(os.eventfd_read, 9999), attempt(os.eventfd_write, 9999, 1), attempt(os.eventfd_read, -1), attempt(os.eventfd_read, "a"), attempt(os.eventfd_read, File("a"))))

print("---- memfd_create")
t("as it is made", lambda: [(d > 2, os.get_inheritable(d), os.write(d, b"abc"), os.lseek(d, 0, 0), os.read(d, 10), os.fstat(d).st_size, os.readlink("/proc/self/fd/%d" % d), os.close(d)) for d in [os.memfd_create("a name")]])
t("names", lambda: [(lambda d: (os.readlink("/proc/self/fd/%d" % d), os.close(d))[0] if type(d) is int else d)(attempt(os.memfd_create, n)) for n in ("", b"bytes", "\xe9", "a" * 249, "a" * 250, "a\0b", 5, None, bytearray(b"x"))])
t("flags", lambda: [(lambda d: (os.get_inheritable(d), os.close(d))[0] if type(d) is int else d)(attempt(os.memfd_create, "n", f)) for f in (0, os.MFD_CLOEXEC, os.MFD_ALLOW_SEALING, 2 ** 32, 2 ** 32 + 1, -1, "a", None, 1.5)])
t("by name", lambda: [os.close(d) for d in [os.memfd_create(name="n", flags=0)]])

print("---- pipe2")
t("flags", lambda: [(lambda p: (os.get_inheritable(p[0]), os.get_blocking(p[0]), os.get_inheritable(p[1]), os.get_blocking(p[1]), type(p).__name__, closing(*p)) if type(p) is tuple else p)(attempt(os.pipe2, f)) for f in (0, os.O_CLOEXEC, os.O_NONBLOCK, os.O_CLOEXEC | os.O_NONBLOCK, os.O_DIRECT, -1, 12345) + WRONG])
t("not by name", lambda: attempt(lambda: os.pipe2(flags=0)))

print("---- timerfd")
t("as it is made", lambda: [(d > 2, os.get_inheritable(d), os.get_blocking(d), os.timerfd_gettime(d), os.timerfd_gettime_ns(d), os.close(d)) for d in [os.timerfd_create(time.CLOCK_MONOTONIC)]])
t("it is never passed on", lambda: [(os.get_inheritable(d), os.get_blocking(d), os.close(d)) for d in [os.timerfd_create(time.CLOCK_REALTIME, flags=os.TFD_NONBLOCK)]])
t("set", lambda: [(os.timerfd_settime(d, initial=100, interval=0.25), [0 < x <= y for x, y in zip(os.timerfd_gettime(d), (100, 0.25))], os.timerfd_gettime(d)[1], os.timerfd_gettime_ns(d)[1], [type(x).__name__ for x in os.timerfd_gettime(d) + os.timerfd_gettime_ns(d)],
                   os.timerfd_settime(d)[1], os.timerfd_gettime(d), os.close(d)) for d in [os.timerfd_create(time.CLOCK_MONOTONIC)]])
t("set, in nanoseconds", lambda: [(os.timerfd_settime_ns(d, initial=10 ** 11, interval=250), os.timerfd_gettime_ns(d)[1], os.timerfd_settime_ns(d)[1], os.timerfd_gettime_ns(d), os.close(d)) for d in [os.timerfd_create(time.CLOCK_MONOTONIC)]])
t("it goes off", lambda: [(os.timerfd_settime(d, initial=0.005), select.select([d], [], [], 5)[0] == [d], int.from_bytes(os.read(d, 8), sys.byteorder), os.timerfd_gettime(d), os.close(d)) for d in [os.timerfd_create(time.CLOCK_MONOTONIC)]])
t("at a time that is given", lambda: [(os.timerfd_settime_ns(d, flags=os.TFD_TIMER_ABSTIME, initial=time.clock_gettime_ns(time.CLOCK_MONOTONIC) + 5000000), select.select([d], [], [], 5)[0] == [d], os.close(d)) for d in [os.timerfd_create(time.CLOCK_MONOTONIC)]])
t("something with fileno()", lambda: [(os.timerfd_settime(File(d), initial=50)[0], os.timerfd_gettime(File(d))[0] > 0, os.timerfd_gettime_ns(File(d))[0] > 0, os.timerfd_settime_ns(File(d))[0] > 0, os.close(d)) for d in [os.timerfd_create(time.CLOCK_MONOTONIC)]])
d = os.timerfd_create(time.CLOCK_MONOTONIC)
for a in WRONG + (-1, -0.5, float("nan"), float("inf"), 1e300, 1e19, Index(1), True):
    # What it gives is how long there was to go, which depends on when it is asked.
    t("initial and interval %s" % ascii(a), lambda: [r if type(r) is str else [type(x).__name__ for x in r] for r in (attempt(lambda: os.timerfd_settime(d, initial=a)), attempt(lambda: os.timerfd_settime(d, initial=1, interval=a)), attempt(lambda: os.timerfd_settime_ns(d, initial=a)), attempt(lambda: os.timerfd_settime_ns(d, initial=1, interval=a)))] + [os.timerfd_gettime_ns(d)[1]])
t("only by name", lambda: (attempt(os.timerfd_settime, d, 0), attempt(os.timerfd_settime_ns, d, 0), attempt(os.timerfd_create, time.CLOCK_MONOTONIC, 0), attempt(lambda: os.timerfd_create(clockid=1)), attempt(lambda: os.timerfd_gettime(fd=d))))
t("flags", lambda: [(attempt(lambda: os.timerfd_settime(d, flags=f)), attempt(lambda: os.timerfd_settime_ns(d, flags=f))) for f in ("a", None, 12345, -1, 2 ** 31)])
os.close(d)
t("clocks", lambda: [(lambda d: (os.close(d), "made")[1] if type(d) is int else d)(attempt(os.timerfd_create, c)) for c in (time.CLOCK_REALTIME, time.CLOCK_MONOTONIC, time.CLOCK_BOOTTIME, time.CLOCK_PROCESS_CPUTIME_ID, 9999, -1) + WRONG])
t("of what is not one", lambda: (attempt(os.timerfd_gettime, 9999), attempt(os.timerfd_gettime_ns, 0), attempt(lambda: os.timerfd_settime(9999, initial=1)), attempt(os.timerfd_gettime, "a")))

print("---- what is in files")
with open("data", "wb") as f:
    f.write(bytes(range(256)) * 16)
t("fdatasync", lambda: [(os.fdatasync(d), os.fdatasync(File(d)), os.fdatasync(fd=d), os.close(d)) for d in [os.open("data", os.O_RDWR)]] + [attempt(os.fdatasync, a) for a in (9999, -1, "a", None, 1.5, File("a"))] + [[(attempt(os.fdatasync, r), closing(r, w)) for r, w in [os.pipe()]]])
t("posix_fadvise", lambda: [([os.posix_fadvise(d, 0, 0, a) for a in (os.POSIX_FADV_NORMAL, os.POSIX_FADV_SEQUENTIAL, os.POSIX_FADV_RANDOM, os.POSIX_FADV_NOREUSE, os.POSIX_FADV_WILLNEED, os.POSIX_FADV_DONTNEED)], attempt(os.posix_fadvise, d, 0, 0, 999), attempt(os.posix_fadvise, d, 0, -1, 0), os.close(d)) for d in [os.open("data", os.O_RDONLY)]])
t("posix_fadvise, wrongly", lambda: [attempt(os.posix_fadvise, *a) for a in ((9999, 0, 0, 0), (-1, 0, 0, 0), ("a", 0, 0, 0), (0, "a", 0, 0), (0, 0, "a", 0), (0, 0, 0, "a"), (0, 2 ** 63, 0, 0), (0, 0, 2 ** 63, 0), (0, 0, 0, 2 ** 31), (File(0), 0, 0, 0))] + [[(attempt(os.posix_fadvise, r, 0, 0, 0), closing(r, w)) for r, w in [os.pipe()]], attempt(lambda: os.posix_fadvise(fd=0, offset=0, length=0, advice=0))])
t("posix_fallocate", lambda: [(os.posix_fallocate(d, 0, 10), os.fstat(d).st_size, os.posix_fallocate(d, 100, 10), os.fstat(d).st_size, os.posix_fallocate(d, 0, 5), os.fstat(d).st_size, os.pread(d, 200, 0) == bytes(110), attempt(os.posix_fallocate, d, 0, 0), attempt(os.posix_fallocate, d, -1, 1), attempt(os.posix_fallocate, d, 0, -1), os.close(d)) for d in [os.open("room", os.O_RDWR | os.O_CREAT)]])
t("posix_fallocate, wrongly", lambda: [attempt(os.posix_fallocate, *a) for a in ((9999, 0, 1), (-1, 0, 1), ("a", 0, 1), (0, "a", 1), (0, 0, "a"), (0, 2 ** 63, 1), (0, 0, 2 ** 63))] + [[(attempt(os.posix_fallocate, d, 0, 1), os.close(d)) for d in [os.open("data", os.O_RDONLY)]], [(attempt(os.posix_fallocate, w, 0, 1), closing(r, w)) for r, w in [os.pipe()]]])


def copied(function, *a, **k):
    source, destination = os.open("data", os.O_RDONLY), os.open("copy", os.O_RDWR | os.O_CREAT | os.O_TRUNC)
    try:
        return attempt(function, source, destination, *a, **k), os.lseek(source, 0, 1), os.lseek(destination, 0, 1), os.fstat(destination).st_size, os.pread(destination, 6, 0)
    finally:
        closing(source, destination)


t("copy_file_range", lambda: [copied(os.copy_file_range, *a, **k) for a, k in (((10,), {}), ((0,), {}), ((10 ** 6,), {}), ((5, 3), {}), ((5, None, 2), {}), ((5, 3, 2), {}), ((5,), {"offset_src": 250, "offset_dst": 1}), ((5, 4096), {}), ((5, 10 ** 6), {}), ((-1,), {}), ((5, -1), {}), ((5, None, -1), {}),
                                                                       (("a",), {}), ((5, "a"), {}), ((5, None, "a"), {}), ((2 ** 63,), {}), ((5, 2 ** 63), {}), ((Index(4),), {}), ((4, Index(1)), {}))])
t("copy_file_range, by name and of what is not there", lambda: (attempt(lambda: os.copy_file_range(src=9999, dst=9998, count=1)), attempt(os.copy_file_range, "a", 1, 1), attempt(os.copy_file_range, 1, "a", 1), attempt(os.copy_file_range, File(1), 1, 1)))


def spliced(count, *a, **k):
    source = os.open("data", os.O_RDONLY)
    r, w = os.pipe()
    try:
        n = attempt(os.splice, source, w, count, *a, **k)
        return n, os.lseek(source, 0, 1), os.read(r, 10) if type(n) is int and n else None
    finally:
        closing(source, r, w)


t("splice", lambda: [spliced(*a, **k) for a, k in (((5,), {}), ((0,), {}), ((5, 3), {}), ((5,), {"offset_src": 250}), ((5, None, 0), {}), ((5,), {"flags": os.SPLICE_F_MOVE | os.SPLICE_F_NONBLOCK | os.SPLICE_F_MORE}), ((5, None, None, 0), {}), ((-1,), {}), ((5, -1), {}), (("a",), {}), ((5, "a"), {}),
                                                   ((5,), {"flags": "a"}), ((5,), {"flags": -1}), ((5,), {"flags": 2 ** 32}), ((5,), {"flags": 12345}))])
t("splice, of two files", lambda: copied(os.splice, 5))
t("sendfile", lambda: [copied(lambda s, d, *a, **k: os.sendfile(d, s, *a, **k), *a, **k) for a, k in (((0, 10), {}), ((3, 5), {}), ((None, 5), {}), ((4096, 5), {}), ((0, 0), {}), ((-1, 5), {}), ((0, -1), {}), (("a", 5), {}), ((0, "a"), {}), ((), {"offset": 2, "count": 3}), ((0, 5, ()), {}), ((0, 5), {"headers": ()}))])
t("getrandom", lambda: [(type(b).__name__, len(b)) if type(b) is bytes else b for b in [attempt(os.getrandom, *a, **k) for a, k in (((0,), {}), ((1,), {}), ((100,), {}), ((16, os.GRND_NONBLOCK), {}), ((16, os.GRND_RANDOM), {}), ((), {"size": 3, "flags": 0}), ((-1,), {}), ((1, 12345), {}), ((1, -1), {}), (("a",), {}), ((1, "a"), {}),
                                                                                                                                (( 2 ** 63,), {}), ((Index(2),), {}), ((1.5,), {}))]])
t("getrandom, twice", lambda: os.getrandom(16) != os.getrandom(16))

print("---- what is kept with a file besides what is in it")
os.symlink("data", "link")
os.symlink("nowhere", "dangling")
t("none yet", lambda: (os.listxattr("data"), os.listxattr(b"data"), os.listxattr("link"), os.listxattr("link", follow_symlinks=False), os.listxattr(), os.listxattr(None), os.listxattr(path=".")))
t("set and got", lambda: (os.setxattr("data", "user.one", b"1"), os.setxattr(b"data", b"user.two", bytearray(b"22")), os.setxattr("link", "user.three", memoryview(b"333")), sorted(os.listxattr("data")), os.getxattr("data", "user.one"), os.getxattr(b"data", b"user.two"), os.getxattr("link", "user.three"), type(os.getxattr("data", "user.one")).__name__))
t("by a descriptor", lambda: [(sorted(os.listxattr(d)), os.getxattr(d, "user.one"), os.setxattr(d, "user.four", b""), os.getxattr(d, "user.four"), os.removexattr(d, "user.four"), sorted(os.listxattr(d)), os.close(d)) for d in [os.open("data", os.O_RDONLY)]])
t("a descriptor, and not following", lambda: [attempt(lambda: f(0, *a, follow_symlinks=False)) for f, a in ((os.getxattr, ("user.one",)), (os.setxattr, ("user.one", b"")), (os.removexattr, ("user.one",)), (os.listxattr, ()))])
t("only if it is there, or is not", lambda: (attempt(os.setxattr, "data", "user.one", b"x", os.XATTR_CREATE), attempt(os.setxattr, "data", "user.none", b"x", os.XATTR_REPLACE), os.setxattr("data", "user.one", b"replaced", os.XATTR_REPLACE), os.getxattr("data", "user.one"), os.setxattr("data", "user.five", b"5", flags=os.XATTR_CREATE), os.removexattr("data", "user.five")))
t("long ones", lambda: (os.setxattr("data", "user.long", b"x" * 1000), len(os.getxattr("data", "user.long")), os.setxattr("data", "user.long", b"y" * 128), len(os.getxattr("data", "user.long")), os.setxattr("data", "user.long", b"z" * 129), len(os.getxattr("data", "user.long")), os.removexattr("data", "user.long")))
t("a great many", lambda: ([os.setxattr("data", "user.many%03d" % i, b"") for i in range(40)] and None, len(os.listxattr("data")), [os.removexattr("data", "user.many%03d" % i) for i in range(40)] and None, sorted(os.listxattr("data"))))
t("removed", lambda: (os.removexattr("data", "user.one"), os.removexattr(b"data", b"user.two"), os.removexattr("link", "user.three"), os.listxattr("data")))
t("what is not there", lambda: (attempt(os.getxattr, "data", "user.none"), attempt(os.removexattr, "data", "user.none"), attempt(os.getxattr, "nowhere", "user.one"), attempt(os.setxattr, "nowhere", "user.one", b""), attempt(os.removexattr, "nowhere", "user.one"), attempt(os.listxattr, "nowhere"), attempt(os.listxattr, "dangling"),
                                        os.listxattr("dangling", follow_symlinks=False), attempt(os.getxattr, 9999, "user.one"), attempt(os.listxattr, 9999), attempt(os.getxattr, b"nowhere", "user.one")))
t("a link cannot have any of the user's", lambda: (attempt(lambda: os.setxattr("link", "user.x", b"", follow_symlinks=False)), attempt(lambda: os.getxattr("link", "user.x", follow_symlinks=False)), attempt(lambda: os.removexattr("link", "user.x", follow_symlinks=False))))
t("names that will not do", lambda: [attempt(os.setxattr, "data", n, b"") for n in ("plain", "", "user.", "nonsense.x", "user." + "a" * 300, "user.a\0b", 5, None, 1.5)])
t("values that will not do", lambda: [attempt(os.setxattr, "data", "user.v", v) for v in ("text", 5, None, [1])] + [attempt(os.setxattr, "data", "user.v", b"", f) for f in ("a", None, 12345, 2 ** 31)])
t("paths that will not do", lambda: [attempt(f, p, *a) for p in (1.5, [], "a\0b", -1, 2 ** 31) for f, a in ((os.getxattr, ("user.x",)), (os.listxattr, ()))] + [attempt(os.getxattr, None, "user.x")])
t("something with __fspath__()", lambda: [(os.setxattr(p, "user.p", b"p"), os.getxattr(p, "user.p"), os.listxattr(p), os.removexattr(p, "user.p")) for p in [__import__("pathlib").Path("data")]])
seen = []
sys.addaudithook(lambda name, a: seen.append((name, a)) if "xattr" in name else None)
t("what is told of", lambda: (os.setxattr("data", "user.a", b"v", 0), os.getxattr("data", b"user.a"), os.listxattr("data"), os.listxattr(), os.removexattr("data", "user.a"), attempt(os.getxattr, "nowhere", "user.a"), seen))

print("---- who the process is")
t("all three", lambda: (os.getresuid() == (os.getuid(), os.geteuid(), os.geteuid()), os.getresgid() == (os.getgid(), os.getegid(), os.getegid()), type(os.getresuid()).__name__, [type(x).__name__ for x in os.getresuid() + os.getresgid()]))
t("left as they are", lambda: (os.setresuid(-1, -1, -1), os.setresgid(-1, -1, -1), os.setresuid(*os.getresuid()), os.setresgid(*os.getresgid())))
t("wrongly", lambda: [attempt(f, *a) for f in (os.setresuid, os.setresgid) for a in (("a", -1, -1), (-1, "a", -1), (-1, -1, "a"), (-2, -1, -1), (2 ** 32, -1, -1), (-1, 2 ** 32 - 1, -1), (1.5, -1, -1), (None, -1, -1))] + [attempt(lambda: os.setresuid(ruid=-1, euid=-1, suid=-1))])
if os.getuid():
    t("what is not allowed", lambda: (attempt(os.setresuid, 0, 0, 0), attempt(os.setresgid, 0, 0, 0)))
t("setns and unshare", lambda: [attempt(os.setns, *a) for a in ((9999,), (-1,), ("a",), (0, "a"), (File(9999),), (0, 2 ** 31))] + [attempt(lambda: os.setns(fd=9999, nstype=0)), [(attempt(os.setns, d), os.close(d)) for d in [os.open("data", os.O_RDONLY)]], os.unshare(0), os.unshare(flags=0)] + [attempt(os.unshare, a) for a in ("a", None, 2 ** 31, -1)])

print("---- when a process is given its turn, and where")
P = os.sched_param
t("sched_param", lambda: (P.__name__, P.__qualname__, P.__module__, [b.__name__ for b in P.__mro__], P.__doc__, P.__text_signature__, sorted(n for n in vars(P) if n != "__doc__"), P.n_fields, P.n_sequence_fields, P.n_unnamed_fields, P.__match_args__, attempt(type, "X", (P,), {})))
t("one", lambda: [(p, p.sched_priority, p[0], len(p), tuple(p), p == (5,), hash(p) == hash((5,)), isinstance(p, tuple), p.__reduce__(), p.__replace__(sched_priority=6), __import__("copy").copy(p), __import__("pickle").loads(__import__("pickle").dumps(p)), attempt(setattr, p, "sched_priority", 1)) for p in [P(5)]])
t("of anything", lambda: [P(x) for x in ("a", None, 1.5, (1, 2), 2 ** 100)] + [P(sched_priority=3)])
t("how it is made", lambda: [attempt(P, *a, **k) for a, k in (((), {}), ((1, 2), {}), ((), {"x": 1}), ((1,), {"sched_priority": 1}), (((1,),), {}))])
t("as things are", lambda: (os.sched_getscheduler(0) == os.SCHED_OTHER, os.sched_getscheduler(os.getpid()) == os.SCHED_OTHER, os.sched_getparam(0), type(os.sched_getparam(0)) is P, os.sched_setparam(0, P(0)), os.sched_setscheduler(0, os.SCHED_OTHER, P(0)), type(os.sched_rr_get_interval(0)).__name__, os.sched_rr_get_interval(0) >= 0))
t("what will not do for a sched_param", lambda: [(attempt(os.sched_setparam, 0, x), attempt(os.sched_setscheduler, 0, os.SCHED_OTHER, x)) for x in (0, (0,), None, "a", P("a"), P(None), P(1.5), P(2 ** 31), P(-2 ** 31 - 1), P(2 ** 64), P(Index(0)), P(True), P(99))])
t("processes that will not do", lambda: [[attempt(f, p, *a) for p in ("a", None, 1.5, -1, 2 ** 31, 2 ** 22 - 3)] for f, a in ((os.sched_getscheduler, ()), (os.sched_getparam, ()), (os.sched_rr_get_interval, ()), (os.sched_getaffinity, ()), (os.sched_setparam, (P(0),)), (os.sched_setscheduler, (0, P(0))), (os.sched_setaffinity, ([0],)))])
t("policies that will not do", lambda: [attempt(os.sched_setscheduler, 0, x, P(0)) for x in ("a", None, 999, -1, 2 ** 31)])
t("not by name", lambda: (attempt(lambda: os.sched_getparam(pid=0)), attempt(lambda: os.sched_getaffinity(pid=0)), attempt(lambda: os.sched_setaffinity(pid=0, mask=[0]))))
mine = os.sched_getaffinity(0)
t("where it may run", lambda: (type(mine).__name__, len(mine) > 0, all(type(c) is int for c in mine), mine == os.sched_getaffinity(os.getpid()), os.process_cpu_count() == len(mine), os.cpu_count() >= len(mine)))
one = min(mine)
t("in one place", lambda: (os.sched_setaffinity(0, [one]), os.sched_getaffinity(0) == {one}, os.process_cpu_count(), os.sched_setaffinity(0, mine), os.sched_getaffinity(0) == mine))
t("anything that can be gone through", lambda: [(os.sched_setaffinity(0, x), os.sched_getaffinity(0) == {one}) for x in ((one,), {one}, iter([one]), (c for c in [one, one]), {one: 1}, [Index(one)], [one, one + 100000], range(one, one + 1))] + [os.sched_setaffinity(0, mine)])
t("what will not do", lambda: [attempt(os.sched_setaffinity, 0, x) for x in ([], 5, None, "a", ["a"], [None], [1.5], [-1], [2 ** 31 - 1], [2 ** 31], [2 ** 64], [-2 ** 64], [100000], [[0]], [one, "a"])] + [os.sched_getaffinity(0) == mine])


def raising():
    yield one
    raise KeyError("from what is gone through")


t("what is gone through raises", lambda: (attempt(os.sched_setaffinity, 0, raising()), os.sched_getaffinity(0) == mine))
t("posix_spawn, with a scheduler", lambda: [(lambda p: os.waitpid(p, 0)[1] if type(p) is int else p)(attempt(lambda: os.posix_spawn("/bin/true", ["true"], {}, scheduler=s))) for s in ((None, P(0)), (os.SCHED_OTHER, P(0)), (os.SCHED_BATCH, P(0)), (os.SCHED_OTHER, P(5)), (999, P(0)), ("a", P(0)), (None, 0), (None, (0,)), (None, P("a")), (), (None,), (None, P(0), 1), [None, P(0)], 5)])


def open_in_a_child(*actions):
    r, w = os.pipe()
    kept = [os.dup(0) for _ in range(3)]
    for d in kept:
        os.set_inheritable(d, True)
    try:
        p = attempt(lambda: os.posix_spawn("/bin/sh", ["sh", "-c", "for d in %s; do if [ -e /proc/self/fd/$d ]; then echo open; else echo closed; fi; done" % " ".join(map(str, kept))], {}, file_actions=[(os.POSIX_SPAWN_DUP2, w, 1)] + [a(kept) for a in actions]))
        os.close(w)
        return (os.waitpid(p, 0)[1], os.read(r, 100).split()) if type(p) is int else p
    finally:
        closing(r, *kept)


t("posix_spawn, closing all from one on", lambda: [open_in_a_child(*a) for a in ((), (lambda k: (os.POSIX_SPAWN_CLOSEFROM, k[0]),), (lambda k: (os.POSIX_SPAWN_CLOSEFROM, k[1]),), (lambda k: (os.POSIX_SPAWN_CLOSEFROM, k[2] + 1),), (lambda k: (os.POSIX_SPAWN_CLOSEFROM,),), (lambda k: (os.POSIX_SPAWN_CLOSEFROM, 3, 4),),
                                                                                    (lambda k: (os.POSIX_SPAWN_CLOSEFROM, "a"),), (lambda k: (os.POSIX_SPAWN_CLOSEFROM, -1),), (lambda k: (os.POSIX_SPAWN_CLOSEFROM, 2 ** 31),), (lambda k: (os.POSIX_SPAWN_CLOSEFROM, None),), (lambda k: (4, 3),))])

print("---- processes as descriptors")
child = os.posix_spawn("/bin/sleep", ["sleep", "60"], {})
d = os.pidfd_open(child)
t("as it is made", lambda: (d > 2, os.get_inheritable(d), select.select([d], [], [], 0)[0]))
t("a signal that does nothing", lambda: (signal.pidfd_send_signal(d, 0), signal.pidfd_send_signal(d, 0, None), signal.pidfd_send_signal(d, 0, None, 0)))
t("wrongly", lambda: [attempt(signal.pidfd_send_signal, *a) for a in ((d, 0, 5), (d, 0, ()), (d, 999), (d, -1), (d, 0, None, 12345), (9999, 0), (-1, 0), ("a", 0), (d, "a"), (d, 0, None, "a"), (d,), (), (d, 0, None, 0, 0), (0, 0))] + [attempt(lambda: signal.pidfd_send_signal(pidfd=d, signalnum=0))])
t("one that ends it", lambda: (signal.pidfd_send_signal(d, signal.SIGTERM), select.select([d], [], [], 10)[0] == [d], (lambda r: (r.si_pid == child, r.si_signo == signal.SIGCHLD, r.si_status == signal.SIGTERM, r.si_code == os.CLD_KILLED))(os.waitid(os.P_PIDFD, d, os.WEXITED)), attempt(signal.pidfd_send_signal, d, 0), os.close(d)))
t("pidfd_open", lambda: [(lambda x: (os.close(x), "made")[1] if type(x) is int else x)(attempt(os.pidfd_open, *a, **k)) for a, k in (((os.getpid(),), {}), ((os.getpid(), 0), {}), ((os.getpid(), os.PIDFD_NONBLOCK), {}), ((), {"pid": os.getpid(), "flags": 0}), ((child,), {}), ((0,), {}), ((-1,), {}), ((2 ** 22 - 3,), {}), ((os.getpid(), 12345), {}),
                                                                                                                       ((os.getpid(), -1), {}), ((os.getpid(), 2 ** 32), {}), (("a",), {}), ((os.getpid(), "a"), {}), ((2 ** 31,), {}), ((None,), {}))])

print("---- waiting for a signal, and being told about it")
S = signal.struct_siginfo
U1, U2 = signal.SIGUSR1, signal.SIGUSR2
t("struct_siginfo", lambda: (S.__name__, S.__qualname__, S.__module__, [b.__name__ for b in S.__mro__], S.__doc__, S.__text_signature__, sorted(n for n in vars(S) if n != "__doc__"), S.n_fields, S.n_sequence_fields, S.n_unnamed_fields, S.__match_args__, attempt(type, "X", (S,), {})))
t("made by hand", lambda: [attempt(S, x) for x in (range(7), (1, 2, 3), range(8), 5)] + [S(range(7)).si_band, attempt(S)])
for name in ("sigwaitinfo", "sigtimedwait", "pidfd_send_signal"):
    t(name, lambda: (lambda f: (type(f).__name__, f.__text_signature__, f.__doc__, f.__module__))(getattr(__import__("_signal"), name)))
signal.pthread_sigmask(signal.SIG_BLOCK, [U1, U2])


def told(i):
    # si_band is in the same place as si_pid and si_uid, and is what those come to.
    return None if i is None else (type(i) is S, signal.Signals(i.si_signo).name, i.si_code, i.si_errno, i.si_pid == os.getpid(), i.si_uid == os.getuid(), i.si_status, type(i.si_band).__name__, len(i))


t("sigwaitinfo", lambda: (signal.raise_signal(U1), told(signal.sigwaitinfo([U1, U2])), os.kill(os.getpid(), U2), told(signal.sigwaitinfo({U2})), sorted(signal.sigpending())))
t("sigtimedwait", lambda: (signal.raise_signal(U2), told(signal.sigtimedwait([U1, U2], 5)), told(signal.sigtimedwait([U1, U2], 0)), told(signal.sigtimedwait([U1], 0.01)), signal.raise_signal(U1), told(signal.sigtimedwait([U1], 0))))
t("it waits", lambda: [(signal.sigtimedwait([U1], 0.05), 0.04 < time.monotonic() - start < 2) for start in [time.monotonic()]])
t("sent by another process", lambda: (os.waitpid(os.posix_spawnp("kill", ["kill", "-USR1", str(os.getpid())], os.environ), 0)[1], (lambda i: (signal.Signals(i.si_signo).name, i.si_code, i.si_pid != os.getpid(), i.si_pid > 0, i.si_uid == os.getuid()))(signal.sigwaitinfo([U1]))))
t("times that will not do", lambda: [attempt(signal.sigtimedwait, [U1], x) for x in (-1, -0.001, "a", None, [], float("nan"), float("inf"), 1e300, 2 ** 64)])
t("sets that will not do", lambda: [(attempt(signal.sigwaitinfo, x), attempt(signal.sigtimedwait, x, 0)) for x in (5, None, "a", [0], [-1], [65], [1000], ["a"], [1.5], [2 ** 64])] + [signal.sigtimedwait([], 0), signal.sigtimedwait(iter([U1]), 0)])
t("how they are called", lambda: (attempt(signal.sigwaitinfo), attempt(signal.sigwaitinfo, [U1], 1), attempt(signal.sigtimedwait, [U1]), attempt(signal.sigtimedwait, [U1], 0, 0), attempt(lambda: signal.sigwaitinfo(sigset=[U1])), attempt(lambda: signal.sigtimedwait(sigset=[U1], timeout=0))))
calls = []
signal.signal(signal.SIGALRM, lambda n, f: calls.append(n))
t("some other signal comes meanwhile, and it goes on waiting", lambda: [(signal.setitimer(signal.ITIMER_REAL, 0.01), signal.sigtimedwait([U1], 0.1), time.monotonic() - start >= 0.09, len(calls)) for start in [time.monotonic()]])


def raises(n, f):
    raise ValueError("from the function")


signal.signal(signal.SIGALRM, raises)
t("unless what is called for that raises", lambda: [(signal.setitimer(signal.ITIMER_REAL, 0.01), attempt(signal.sigtimedwait, [U1], 5), signal.setitimer(signal.ITIMER_REAL, 0.01), attempt(signal.sigwaitinfo, [U1]), time.monotonic() - start < 4) for start in [time.monotonic()]])
signal.signal(signal.SIGALRM, signal.SIG_DFL)
signal.pthread_sigmask(signal.SIG_UNBLOCK, [U1, U2])

print("---- the clock of a thread")
import _thread
t("pthread_getcpuclockid", lambda: (lambda f: (type(f).__name__, f.__text_signature__, f.__doc__, f.__module__))(time.pthread_getcpuclockid))
t("of this one", lambda: [(type(c).__name__, c < 0, time.clock_gettime(c) > 0, 0 <= time.clock_gettime(time.CLOCK_THREAD_CPUTIME_ID) - time.clock_gettime(c) < 1) for c in [time.pthread_getcpuclockid(_thread.get_ident())]])
t("wrongly", lambda: [attempt(time.pthread_getcpuclockid, *a) for a in ((), (1, 2), ("a",), (None,), (1.5,))] + [attempt(lambda: time.pthread_getcpuclockid(thread_id=1))])

os.chdir("/")
__import__("shutil").rmtree(HERE)
