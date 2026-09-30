# Everything in the modules that are written in C++: what each module and each class in it has, and what comes of calling each function and each method wrongly. With no arguments, with nine, with a keyword that it does not have,
# with no instance, with an instance of the wrong class, with each of a score of things that it is unlikely to have been meant for, and, for a method, of an instance that has been made and not initialized. It must never crash.
#
# There is a line for each function and each method, with how many things were tried and a checksum of what came of them. Given `all` as an argument, it prints what came of each, as it comes:
#
#     jsc native-modules.py -- all
import binascii
import sys
import warnings

ALL = sys.argv[1:] == ["all"]

warnings.simplefilter("ignore")
MODULES = ("_abc", "_ast", "_asyncio", "_bisect", "_blake2", "_codecs", "_collections", "_contextvars", "_csv", "_functools", "_heapq", "_imp", "_io", "_md5", "_opcode", "_operator", "_posixsubprocess", "_random", "_sha1", "_sha2", "_sha3", "_signal", "_socket", "_sre", "_stat", "_string", "_struct", "_symtable", "_thread", "_tokenize", "_typing", "_warnings", "_weakref", "array", "atexit", "binascii", "cmath", "errno", "fcntl", "gc", "itertools", "marshal", "math", "posix", "resource", "select", "termios", "time", "unicodedata")

# What would put an end to this, or keep it waiting, or change what it is running in or what is on the disk
SKIP = {
    "posix": {"abort", "_exit", "fork", "forkpty", "fork1", "execv", "execve", "spawnv", "spawnve", "posix_spawn", "posix_spawnp", "system", "sync", "mkdir", "mkfifo", "mknod", "umask", "setsid", "setpgrp", "chroot", "nice", "pause", "wait", "wait3", "getlogin", "openpty", "login_tty", "register_at_fork", "get_terminal_size", "ctermid", "chdir", "fchdir", "unsetenv", "urandom", "lockf", "kill", "killpg", "close", "closerange", "dup", "pipe", "listdir", "scandir"},
    "_signal": {"pause", "raise_signal", "alarm", "setitimer", "signal", "sigwait", "sigwaitinfo", "sigtimedwait", "pthread_kill", "pthread_sigmask", "set_wakeup_fd", "siginterrupt", "sigpending"},
    "_thread": {"start_new_thread", "start_new", "start_joinable_thread", "exit", "exit_thread", "interrupt_main", "stack_size", "daemon_threads_allowed", "_make_thread_handle", "_shutdown", "set_name"},
    "time": {"sleep", "tzset", "clock_settime", "clock_settime_ns"},
    "select": {"select"},
    "_posixsubprocess": {"fork_exec"},
    "_imp": {"acquire_lock", "release_lock", "create_dynamic", "exec_dynamic", "_override_frozen_modules_for_tests", "_override_multi_interp_extensions_check", "_set_lazy_attributes"},
    "atexit": {"register", "unregister", "_run_exitfuncs", "_clear"},
    "_warnings": {"_filters_mutated_lock_held", "_acquire_lock", "_release_lock", "warn", "warn_explicit"},
    "_codecs": {"register", "unregister", "register_error", "_unregister_error"},
    "_io": {"open", "open_code"},
    "_abc": {"_reset_registry", "_reset_caches"},
    # What there is to be found is not the same from one collector to another.
    "gc": {"get_objects", "get_referrers", "get_referents", "get_count", "get_stats", "collect"},
    "resource": {"setrlimit", "prlimit"},
    # What asks something else, which may be slow to answer, or says what the machine is called or what is plugged into it
    "_socket": {"sethostname", "gethostbyname", "gethostbyname_ex", "gethostbyaddr", "getaddrinfo", "getnameinfo", "gethostname", "if_nameindex", "if_nametoindex", "if_indextoname", "close", "dup", "setdefaulttimeout"},
    # These are given the descriptors that this is written to.
    "fcntl": {"fcntl", "ioctl", "flock", "lockf"},
    "termios": {"tcsetattr", "tcsendbreak", "tcdrain", "tcflush", "tcflow", "tcsetwinsize", "tcgetattr", "tcgetwinsize"},
    # 2 ** 70 to the power of itself, or moved along by itself, is not to be waited for.
    "_operator": {"pow", "ipow", "__pow__", "__ipow__", "lshift", "ilshift", "__lshift__", "__ilshift__"},
}
SKIPPED_METHODS = {"__init_subclass__", "__subclasshook__", "__class__", "__doc__", "__del__", "acquire", "acquire_lock", "__enter__", "_acquire_restore", "poll", "control", "__sizeof__", "__reduce_ex__", "__reduce__", "__getstate__", "__dir__", "__format__", "_at_fork_reinit", "join", "_recursion_count", "buffer_info", "__hash__", "__repr__", "__str__", "fileno", "seed", "random", "getrandbits", "getstate",
                   # Of an int, given 2 ** 70, these are not to be waited for.
                   "__round__", "__pow__", "__rpow__", "__lshift__", "__rlshift__"}


class C:
    pass


def scrub(text):
    "Without where things are"
    out, i = [], 0
    while i < len(text):
        if text.startswith("0x", i):
            i += 2
            while i < len(text) and text[i] in "0123456789abcdefABCDEF":
                i += 1
            out.append("0x")
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


outcomes = []


def attempt(label, f, *a, **k):
    if ALL:
        print(label, end=" ", flush=True)
    try:
        f(*a, **k)
    except OSError as e:
        outcome = "%s %s" % (type(e).__name__, e.errno)
    except BaseException as e:
        outcome = "%s %s" % (type(e).__name__, ascii(scrub(str(e)))[:150])
    else:
        outcome = "fine"
    if ALL:
        print("|", outcome, flush=True)
    outcomes.append(label + " | " + outcome)


def finish(subject):
    "All that has been tried since the last time, in a line"
    if outcomes and not ALL:
        print(subject, "|", len(outcomes), binascii.crc32("\n".join(outcomes).encode()))
    outcomes.clear()


def hostile():
    "Made afresh each time, since what is given one may change it. There is no small number that is not negative among them: that would be a file that is open."
    return (("None", None), ("-1", -1), ("2**70", 2 ** 70), ("-2**70", -2 ** 70), ("1.5", 1.5), ("nan", float("nan")), ("str", "s"), ("surrogate", "\ud800"), ("nul", "a\0b"), ("bytes", b"b"), ("bytearray", bytearray(b"a")), ("list", []), ("tuple", ()), ("dict", {}), ("[None]", [None]), ("object", C()), ("type", type), ("int", int), ("function", scrub), ("memoryview", memoryview(b"m")))


def kinds(namespace):
    return " ".join(sorted(k + ":" + type(v).__name__ for k, v in namespace.items() if k not in ("__spec__", "__loader__", "__file__", "__cached__", "__builtins__")))


def samples():
    "One or more of each class that there is a way of making one of"
    import _blake2, _csv, _md5, _sha1, _sha2, _sha3, _socket, _symtable
    import _collections, _contextvars, _io, _random, _sre, _string, _struct, _thread, _tokenize, _weakref, array, itertools as it, posix, re, select, time, unicodedata, _ast, ast
    keep = C()
    var = _contextvars.ContextVar("v")
    pattern = re.compile("(a)")
    made = [
        lambda: _collections.deque([1, 2]), lambda: iter(_collections.deque([1])), lambda: reversed(_collections.deque([1])), lambda: _collections.defaultdict(int),
        lambda: it.count(), lambda: it.cycle([1]), lambda: it.repeat(1), lambda: it.repeat(1, 2), lambda: it.accumulate([1]), lambda: it.chain([1]), lambda: it.compress([1], [1]), lambda: it.dropwhile(bool, [1]), lambda: it.filterfalse(bool, [1]), lambda: it.groupby([1]), lambda: next(it.groupby([1]))[1], lambda: it.islice([1], 1), lambda: it.starmap(max, [(1, 2)]), lambda: it.takewhile(bool, [1]), lambda: it.tee([1])[0], lambda: it.zip_longest([1]), lambda: it.product([1]), lambda: it.permutations([1]), lambda: it.combinations([1], 1), lambda: it.combinations_with_replacement([1], 1), lambda: it.pairwise([1, 2]), lambda: it.batched([1], 1),
        lambda: _io.BytesIO(b"ab"), lambda: _io.StringIO("ab"), lambda: _io.FileIO("/dev/null"), lambda: _io.BufferedReader(_io.BytesIO(b"ab")), lambda: _io.BufferedWriter(_io.BytesIO()), lambda: _io.BufferedRandom(_io.BytesIO(b"ab")), lambda: _io.BufferedRWPair(_io.BytesIO(b"ab"), _io.BytesIO()), lambda: _io.TextIOWrapper(_io.BytesIO(b"ab")), lambda: _io.IncrementalNewlineDecoder(None, True),
        lambda: _thread.allocate_lock(), lambda: _thread.RLock(), lambda: _thread._local(),
        lambda: _weakref.ref(keep), lambda: _weakref.proxy(keep), lambda: _weakref.proxy(scrub),
        lambda: pattern, lambda: pattern.match("a"), lambda: pattern.scanner("a"),
        lambda: _struct.Struct("i"), lambda: _struct.iter_unpack("b", b"ab"), lambda: _random.Random(1),
        lambda: select.kqueue(), lambda: select.kevent(1), lambda: select.poll(),
        lambda: time.localtime(0), lambda: posix.stat("."), lambda: posix.times(), lambda: posix.uname(), lambda: posix.terminal_size((1, 2)), lambda: posix.statvfs("."),
        lambda: unicodedata.ucd_3_2_0, lambda: unicodedata._ucnhash_CAPI,
        lambda: array.array("i", [1, 2]), lambda: array.array("w", "ab"), lambda: array.array("d", [1.5]), lambda: iter(array.array("b", [1])),
        lambda: ast.parse("x = 1"), lambda: ast.parse("x = 1").body[0], lambda: _ast.Constant(1), lambda: _ast.Load(),
        lambda: var, lambda: _contextvars.Context(), lambda: _contextvars.copy_context(), lambda: var.set(1),
        lambda: _tokenize.TokenizerIter(iter(["x\n"]).__next__, extra_tokens=False), lambda: _string.formatter_parser("{a}"), lambda: _string.formatter_field_name_split("a.b")[1],
        lambda: _md5.md5(b"a"), lambda: _sha1.sha1(b"a"), lambda: _sha2.sha224(b"a"), lambda: _sha2.sha512(b"a"), lambda: _sha3.sha3_256(b"a"), lambda: _sha3.shake_128(b"a"), lambda: _blake2.blake2b(b"a", key=b"k"), lambda: _blake2.blake2s(b"a"),
        lambda: _csv.reader(["a,b"]), lambda: _csv.writer(_io.StringIO()), lambda: _csv.Dialect(), lambda: _symtable.symtable("x = 1", "f", "exec"), lambda: _socket.socket(),
    ]
    return made


for name in MODULES:
    try:
        module = __import__(name)
    except ImportError:
        print("module", name, "| 1 there is none")
        continue
    print("module", name, "| 1", kinds(vars(module)))
    for attribute in sorted(vars(module)):
        value = getattr(module, attribute)
        if isinstance(value, type):
            print("class", name, attribute, "| 1", value.__module__, value.__qualname__, [b.__name__ for b in value.__mro__], value.__flags__ & 0x7FFF, value.__basicsize__, kinds(vars(value)))
        if attribute in SKIP.get(name, ()) or attribute.startswith(("set", "sched_set", "__")) or not callable(value):
            continue
        if isinstance(value, type) and issubclass(value, BaseException):
            continue
        label = "%s.%s" % (name, attribute)
        attempt(label + " 0", value)
        attempt(label + " 9", value, *[None] * 9)
        attempt(label + " kw", value, zzz=1)
        attempt(label + " 1kw", value, None, zzz=1)
        for what, v in hostile():
            attempt("%s %s" % (label, what), value, v)
        for what, v in hostile():
            attempt("%s %s twice" % (label, what), value, v, v)
        finish(label)


# What CPython 3.14.7 itself crashes on, so that there is nothing to compare with: super.__new__(super).__get__(x) looks for the class of x among the bases of no class at all.
CRASHES_CPYTHON = {("raw builtins.super", "__get__"), ("raw _asyncio.Task", "get_context")}


def exercise(label, make, initialized):
    t = type(make())
    for name in sorted(dir(t)):
        if name in SKIPPED_METHODS or (label, name) in CRASHES_CPYTHON:
            continue
        try:
            if not callable(getattr(make(), name)) or isinstance(getattr(make(), name), type):
                continue
        except BaseException:
            continue
        attempt("%s %s 0" % (label, name), getattr(make(), name))
        attempt("%s %s 9" % (label, name), getattr(make(), name), *[None] * 9)
        attempt("%s %s kw" % (label, name), getattr(make(), name), zzz=1)
        for what, v in hostile():
            attempt("%s %s %s" % (label, name, what), getattr(make(), name), v)
        if initialized:
            u = getattr(t, name)
            attempt("%s %s unbound" % (label, name), u)
            attempt("%s %s wrong self" % (label, name), u, ...)
            attempt("%s %s wrong self and an argument" % (label, name), u, ..., None)
            for what, v in hostile():
                attempt("%s %s %s twice" % (label, name, what), getattr(make(), name), v, v)
        finish("%s %s" % (label, name))


seen = {}
for i, make in enumerate(samples()):
    t = type(make())
    n = seen[t] = seen.get(t, 0) + 1
    exercise("%s.%s#%d" % (t.__module__, t.__qualname__, n), make, True)

# What has been made and not initialized. Here what is in builtins is looked at as well, and what is built in and is in no module, which types has names for.
for name in MODULES + ("builtins", "types"):
    try:
        module = __import__(name)
    except ImportError:
        continue
    for attribute in sorted(vars(module)):
        t = getattr(module, attribute)
        if not isinstance(t, type) or name == "_ast" and attribute not in ("AST", "Constant", "Module", "Name"):
            continue
        if issubclass(t, BaseException) and name != "builtins" or t.__module__ == "types" or attribute == "JSError":
            continue
        try:
            t.__new__(t)
        except BaseException as e:
            print("raw %s.%s | 1 %s %s" % (name, attribute, type(e).__name__, ascii(scrub(str(e)))[:150]))
            continue
        exercise("raw %s.%s" % (name, attribute), lambda: t.__new__(t), False)
        for what in ("repr", "str", "hash", "iter", "len", "bool"):
            attempt("raw %s.%s %s()" % (name, attribute, what), getattr(__builtins__, what) if not isinstance(__builtins__, dict) else __builtins__[what], t.__new__(t))
        finish("raw %s.%s by what is built in" % (name, attribute))
        if ("raw %s.%s" % (name, attribute), "__get__") in CRASHES_CPYTHON:
            continue
        # As an attribute of a class, where it is asked whether it is a descriptor, and taken for one if it says so
        holder = type("Holder", (), {"a": t.__new__(t)})
        attempt("raw %s.%s got from a class" % (name, attribute), getattr, holder, "a")
        attempt("raw %s.%s got from an instance" % (name, attribute), getattr, holder(), "a")
        attempt("raw %s.%s called from an instance" % (name, attribute), lambda: holder().a())
        attempt("raw %s.%s set" % (name, attribute), setattr, holder(), "a", 1)
        attempt("raw %s.%s deleted" % (name, attribute), delattr, holder(), "a")
        attempt("raw %s.%s got by super()" % (name, attribute), lambda: getattr(super(type("Sub", (holder,), {}), type("Sub", (holder,), {})), "a", None))
        finish("raw %s.%s as an attribute of a class" % (name, attribute))
