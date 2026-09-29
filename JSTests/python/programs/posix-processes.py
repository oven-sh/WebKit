# The functions of posix about this process and others.
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
        return "%s: errno=%r strerror=%r filename=%r args=%d" % (type(e).__name__, e.errno, e.strerror, e.filename, len(e.args))
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


class Index:
    def __init__(self, value): self.value = value
    def __index__(self): return self.value
    def __repr__(self): return "Index(%d)" % self.value


class P:
    def __init__(self, value): self.value = value
    def __fspath__(self): return self.value
    def __repr__(self): return "P(%r)" % (self.value,)


SH = "/bin/sh"
start = posix.getcwd()
base = posix.environ.get(b"TMPDIR", b"/tmp").decode().rstrip("/") + "/jsc-python-posix-processes-" + str(posix.getpid())
posix.mkdir(base)
posix.chdir(base)


def read(path):
    fd = posix.open(path, posix.O_RDONLY)
    data = posix.read(fd, 100000)
    posix.close(fd)
    return data


def run(script, env=None, spawn=posix.posix_spawn, path=SH, **kwargs):
    "What a script for the shell writes, and how it ended."
    actions = [(posix.POSIX_SPAWN_OPEN, 1, "out", posix.O_WRONLY | posix.O_CREAT | posix.O_TRUNC, 0o644)]
    pid = spawn(path, ["sh", "-c", script], env if env is not None else {"PATH": "/bin:/usr/bin"}, file_actions=actions + kwargs.pop("more", []), **kwargs)
    got, status = posix.waitpid(pid, 0)
    return got == pid, posix.waitstatus_to_exitcode(status), read("out")


try:
    print("---- who this is")
    for name in ("getpid", "getppid", "getuid", "geteuid", "getgid", "getegid", "getpgrp"):
        t(name, lambda: [(type(v).__name__, v >= 0, v == getattr(posix, name)(), attempt(lambda: getattr(posix, name)(1))) for v in [getattr(posix, name)()]])
    t("that agree", lambda: (posix.getpgid(0) == posix.getpgrp(), posix.getpgid(posix.getpid()) == posix.getpgrp(), posix.getpgid(pid=0) == posix.getpgrp(), type(posix.getsid(0)).__name__, posix.getsid(0) == posix.getsid(posix.getpid()), posix.getuid() == posix.stat(".").st_uid))
    for name, args in (("getpgid", (99999999,)), ("getpgid", ("a",)), ("getpgid", (1.5,)), ("getpgid", (2 ** 40,)), ("getpgid", ()), ("getsid", (99999999,)), ("getsid", ("a",)), ("getsid", (None,)), ("setpgid", (99999999, 0)), ("setpgid", ("a", 0)), ("setpgid", (0, "a")), ("setpgid", (0,))):
        t("%s%r" % (name, args), lambda: getattr(posix, name)(*args))
    t("getgroups", lambda: [(type(g).__name__, all(type(x) is int for x in g), len(g) > 0, sorted(g) == sorted(posix.getgroups()), attempt(lambda: posix.getgroups(1))) for g in [posix.getgroups()]])
    t("getgrouplist", lambda: [(type(g).__name__, 0 in g, all(type(x) is int for x in g)) for g in [posix.getgrouplist("root", 0)]])
    for args in ((), ("root",), (5, 0), (b"root", 0), ("root", "a"), ("root", 1.5), ("ro\0ot", 0), ("root", 2 ** 40), (None, 0)):
        t("getgrouplist%r" % (args,), lambda: posix.getgrouplist(*args))
    t("one that there is not", lambda: posix.getgrouplist("no such user as this", 12345))
    if posix.getuid():
        for name, args in (("setuid", (0,)), ("seteuid", (0,)), ("setgid", (0,)), ("setegid", (0,)), ("setreuid", (0, 0)), ("setregid", (0, 0)), ("setgroups", ([0],)), ("initgroups", ("root", 0)), ("chroot", ("/",))):
            t("%s%r, not being root" % (name, args), lambda: getattr(posix, name)(*args))
    t("to what it is already", lambda: (posix.setuid(posix.getuid()), posix.seteuid(posix.geteuid()), posix.setgid(posix.getgid()), posix.setegid(posix.getegid()), posix.setreuid(-1, -1), posix.setregid(-1, -1), posix.setreuid(posix.getuid(), posix.geteuid())))
    for name, args in (("setuid", ("a",)), ("setuid", (1.5,)), ("setuid", (-2,)), ("setuid", (2 ** 32,)), ("setuid", (2 ** 64,)), ("setuid", ()), ("setuid", (None,)), ("setgid", ("a",)), ("setgid", (-2,)), ("setgid", (2 ** 64,)), ("seteuid", ("a",)), ("setegid", ("a",)), ("setreuid", ("a", 0)), ("setreuid", (0, "a")),
                       ("setreuid", (0,)), ("setregid", ("a", 0)), ("setregid", (0, -5)), ("setgroups", (5,)), ("setgroups", (None,)), ("setgroups", (["a"],)), ("setgroups", ([1.5],)), ("setgroups", ([-2],)), ("setgroups", ([2 ** 64],)), ("setgroups", (range(100000),)), ("setgroups", ({1: 2},)), ("setgroups", ()),
                       ("initgroups", (5, 0)), ("initgroups", ("root", "a")), ("initgroups", ("ro\0ot", 0)), ("initgroups", ("root",))):
        t("%s%r" % (name, args), lambda: getattr(posix, name)(*args))
    t("getlogin", lambda: [type(v).__name__ if not isinstance(v, str) or not v.startswith("OSError") else "str" for v in [attempt(posix.getlogin)]])

    print("---- the environment")
    t("environ", lambda: (type(posix.environ).__name__, all(type(k) is bytes and type(v) is bytes for k, v in posix.environ.items()), b"PATH" in posix.environ, posix._create_environ() == posix.environ, posix._create_environ() is not posix.environ))
    t("putenv", lambda: (posix.putenv("JSC_PY_A", "one"), posix.putenv(b"JSC_PY_B", b"two"), posix.putenv(P("JSC_PY_C"), P(b"three")), posix.putenv("JSC_PY_D", ""), posix.putenv("JSC_PY_E", "a=b"), posix.putenv("JSC_PY_F", "\xe9\udcff"), b"JSC_PY_A" in posix.environ,
                            [(k, v) for k, v in sorted(posix._create_environ().items()) if k.startswith(b"JSC_PY_")]))
    t("what is started from here has them", lambda: run("echo $JSC_PY_A $JSC_PY_B $JSC_PY_C [$JSC_PY_D] $JSC_PY_E", env=posix._create_environ()))
    t("if it is given None too", lambda: [(posix.waitpid(pid, 0)[1], read("out")) for pid in [posix.posix_spawn(SH, ["sh", "-c", "echo $JSC_PY_A"], None, file_actions=[(posix.POSIX_SPAWN_OPEN, 1, "out", posix.O_WRONLY | posix.O_CREAT | posix.O_TRUNC, 0o644)])]])
    t("and by system", lambda: (posix.system("echo $JSC_PY_B > out"), read("out")))
    t("unsetenv", lambda: (posix.unsetenv("JSC_PY_A"), posix.unsetenv(b"JSC_PY_B"), posix.unsetenv(P("JSC_PY_C")), posix.unsetenv("JSC_PY_NEVER_SET"), [k for k in sorted(posix._create_environ()) if k.startswith(b"JSC_PY_")], posix.unsetenv("JSC_PY_D"), posix.unsetenv("JSC_PY_E"), posix.unsetenv("JSC_PY_F")))
    for name, args in (("putenv", ("A=B", "c")), ("putenv", ("", "c")), ("putenv", ("A\0", "c")), ("putenv", ("A", "c\0")), ("putenv", (5, "c")), ("putenv", ("A", 5)), ("putenv", ("A",)), ("putenv", ("A", None)), ("putenv", (bytearray(b"A"), "c")), ("putenv", ("\ud800", "c")),
                       ("unsetenv", ("A=B",)), ("unsetenv", ("",)), ("unsetenv", ("A\0",)), ("unsetenv", (5,)), ("unsetenv", ()), ("unsetenv", (None,))):
        t("%s%r" % (name, args), lambda: getattr(posix, name)(*args))

    print("---- the system")
    t("uname", lambda: [(type(u).__name__, type(u).__module__, len(u), u.n_fields, [type(x).__name__ for x in u], u.sysname, u.machine, u == posix.uname(), tuple(u) == (u.sysname, u.nodename, u.release, u.version, u.machine), repr(u).startswith("posix.uname_result(sysname='"), attempt(lambda: posix.uname(1))) for u in [posix.uname()]])
    t("times", lambda: [(type(x).__name__, type(x).__module__, len(x), [type(v).__name__ for v in x], x.user >= 0, x.system >= 0, x.children_user >= 0, x.children_system >= 0, x.elapsed > 0, repr(x).startswith("posix.times_result(user="), attempt(lambda: posix.times(1))) for x in [posix.times()]])
    t("getloadavg", lambda: [(type(x).__name__, len(x), [type(v).__name__ for v in x], all(v >= 0 for v in x)) for x in [posix.getloadavg()]])
    t("cpu_count", lambda: (type(posix.cpu_count()).__name__, posix.cpu_count() >= 1, posix.cpu_count() == posix.sysconf("SC_NPROCESSORS_ONLN"), attempt(lambda: posix.cpu_count(1))))
    t("strerror", lambda: [posix.strerror(n) for n in (0, 1, 2, 13, 17, 22, 32, 106, 107, 1000, -1, 2 ** 31 - 1)])
    for arg in ("a", 1.5, None, 2 ** 40):
        t("strerror(%r)" % (arg,), lambda: posix.strerror(arg))
    t("urandom", lambda: (type(posix.urandom(4)).__name__, [len(posix.urandom(n)) for n in (0, 1, 16, 1000, 100000)], posix.urandom(16) != posix.urandom(16), len(posix.urandom(Index(3))), len(set(posix.urandom(10000))) > 200))
    for arg in (-1, "a", 1.5, None, 2 ** 70):
        t("urandom(%r)" % (arg,), lambda: posix.urandom(arg))
    t("sysconf", lambda: (posix.sysconf("SC_PAGESIZE") == posix.sysconf("SC_PAGE_SIZE"), posix.sysconf("SC_PAGESIZE") in (4096, 16384, 65536), posix.sysconf("SC_CLK_TCK"), posix.sysconf(posix.sysconf_names["SC_CLK_TCK"]), posix.sysconf("SC_OPEN_MAX") > 0, posix.sysconf("SC_ARG_MAX") > 0, posix.sysconf(Index(posix.sysconf_names["SC_CLK_TCK"]))))
    for arg in ("SC_NOTHING", 99999, -1, 1.5, None, b"SC_CLK_TCK", 2 ** 40):
        t("sysconf(%r)" % (arg,), lambda: posix.sysconf(arg))
    t("confstr", lambda: (posix.confstr("CS_PATH"), posix.confstr(posix.confstr_names["CS_PATH"]), type(posix.confstr("CS_PATH")).__name__))
    for arg in ("CS_NOTHING", 99999, -1, 1.5, None, 2 ** 40):
        t("confstr(%r)" % (arg,), lambda: posix.confstr(arg))
    t("_inputhook", lambda: (posix._inputhook(), posix._is_inputhook_installed()))

    print("---- priorities")
    t("getpriority", lambda: (type(posix.getpriority(posix.PRIO_PROCESS, 0)).__name__, posix.getpriority(posix.PRIO_PROCESS, 0) == posix.getpriority(which=posix.PRIO_PROCESS, who=posix.getpid()), posix.setpriority(posix.PRIO_PROCESS, 0, posix.getpriority(posix.PRIO_PROCESS, 0)), posix.nice(0) == posix.getpriority(posix.PRIO_PROCESS, 0)))
    for name, args in (("getpriority", (99, 0)), ("getpriority", (posix.PRIO_PROCESS, 99999999)), ("getpriority", ("a", 0)), ("getpriority", (0, "a")), ("getpriority", (0,)), ("setpriority", (99, 0, 0)), ("setpriority", (0, 0, "a")), ("setpriority", (0, 0)), ("nice", ("a",)), ("nice", ()), ("nice", (1.5,)), ("nice", (2 ** 40,))):
        t("%s%r" % (name, args), lambda: getattr(posix, name)(*args))
    t("the scheduler", lambda: ([(posix.sched_get_priority_min(p) <= posix.sched_get_priority_max(p)) for p in (posix.SCHED_OTHER, posix.SCHED_FIFO, posix.SCHED_RR)], posix.sched_get_priority_max(policy=posix.SCHED_FIFO), posix.sched_get_priority_min(posix.SCHED_FIFO), posix.sched_yield(), attempt(lambda: posix.sched_yield(1))))
    for name, args in (("sched_get_priority_max", (999,)), ("sched_get_priority_min", (999,)), ("sched_get_priority_max", ("a",)), ("sched_get_priority_min", ())):
        t("%s%r" % (name, args), lambda: getattr(posix, name)(*args))

    print("---- starting others")
    t("posix_spawn", lambda: run("echo hello; exit 3"))
    t("posix_spawnp", lambda: run("echo found", spawn=posix.posix_spawnp, path="sh", env={"PATH": "/bin"}))
    t("names of other kinds", lambda: (run("echo b", path=SH.encode()), run("echo p", path=P(SH)), [(posix.waitpid(pid, 0)[1], read("out")) for pid in [posix.posix_spawn(SH, (b"sh", P("-c"), P(b"echo mixed > out")), {b"A": P("1"), P("B"): b"2"})]]))
    t("its environment", lambda: (run("echo $A-$B-$PATH", env={"A": "1", b"B": b"2"}), run("env | grep -v -e ^PWD= -e ^_= | sort", env={"PATH": "/usr/bin:/bin"}), run("env | grep -v -e ^PWD= -e ^_= | sort", env={"Z": "", "A": "=x", "PATH": "/usr/bin:/bin"})))
    t("what it is to do with its files", lambda: (run("echo err >&2; echo out", more=[(posix.POSIX_SPAWN_DUP2, 1, 2)]), run("echo x >&5", more=[(posix.POSIX_SPAWN_DUP2, 1, 5)]), run("echo gone >&1 || echo failed >&2", more=[(posix.POSIX_SPAWN_CLOSE, 1), (posix.POSIX_SPAWN_OPEN, 2, "err", posix.O_WRONLY | posix.O_CREAT, 0o600)])[:2],
                                                     run("cat", more=[(posix.POSIX_SPAWN_OPEN, 0, b"err", posix.O_RDONLY, 0)])[:2]))
    t("what is open here is not open there", lambda: [(run("(echo x >&%d) 2>/dev/null || echo closed" % fd)[1:], posix.set_inheritable(fd, True), run("echo x >&%d && echo open" % fd)[1:], posix.close(fd)) for fd in [posix.dup2(posix.open("keep", posix.O_WRONLY | posix.O_CREAT), 40, False)]])
    t("options", lambda: (run("echo g", setpgroup=0), run("echo r", resetids=True), run("echo s", setsid=True), run("echo m", setsigmask=[1, 2]), run("echo d", setsigdef=(15,)), run("echo n", setpgroup=None, resetids=0, setsid=[], setsigmask=(), setsigdef=[], scheduler=None)))
    t("a mask", lambda: (run("kill -TERM $$; echo survived", setsigmask=[15]), run("kill -TERM $$; echo survived")))
    for label, args, kwargs in (("no such program", ("absent", ["a"], {}), {}), ("a directory", (".", ["a"], {}), {}), ("argv not a list", (SH, "a", {}), {}), ("argv an iterator", (SH, iter(["a"]), {}), {}), ("argv empty", (SH, [], {}), {}), ("argv ()", (SH, (), {}), {}), ("argv ['']", (SH, [""], {}), {}), ("argv [5]", (SH, [5], {}), {}),
                                ("argv with a zero", (SH, ["a\0"], {}), {}), ("env a list", (SH, ["a"], [1]), {}), ("env an int", (SH, ["a"], 5), {}), ("env with an int for a name", (SH, ["a"], {5: "a"}), {}), ("env with an int", (SH, ["a"], {"a": 5}), {}), ("env with = in a name", (SH, ["a"], {"a=b": "c"}), {}),
                                ("env with = first", (SH, ["sh", "-c", "exit 0"], {"=a": "c"}), {}), ("env with no name", (SH, ["a"], {"": "c"}), {}), ("env with a zero", (SH, ["a"], {"a\0": "c"}), {}), ("two arguments", (SH, ["a"]), {}), ("four", (SH, ["a"], {}, ()), {}), ("another name", (SH, ["a"], {}), {"other": 1}),
                                ("file_actions an int", (SH, ["a"], {}), {"file_actions": 5}), ("of ints", (SH, ["a"], {}), {"file_actions": [5]}), ("of empty tuples", (SH, ["a"], {}), {"file_actions": [()]}), ("of lists", (SH, ["a"], {}), {"file_actions": [[posix.POSIX_SPAWN_CLOSE, 1]]}), ("unknown", (SH, ["a"], {}), {"file_actions": [(99,)]}),
                                ("a str for which", (SH, ["a"], {}), {"file_actions": [("a",)]}), ("close with none", (SH, ["a"], {}), {"file_actions": [(posix.POSIX_SPAWN_CLOSE,)]}), ("close with two", (SH, ["a"], {}), {"file_actions": [(posix.POSIX_SPAWN_CLOSE, 1, 2)]}), ("close of a str", (SH, ["a"], {}), {"file_actions": [(posix.POSIX_SPAWN_CLOSE, "a")]}),
                                ("close of a float", (SH, ["a"], {}), {"file_actions": [(posix.POSIX_SPAWN_CLOSE, 1.5)]}), ("close of too much", (SH, ["a"], {}), {"file_actions": [(posix.POSIX_SPAWN_CLOSE, 2 ** 40)]}), ("close of too little", (SH, ["a"], {}), {"file_actions": [(posix.POSIX_SPAWN_CLOSE, -2 ** 40)]}),
                                ("close of -1", (SH, ["a"], {}), {"file_actions": [(posix.POSIX_SPAWN_CLOSE, -1)]}), ("dup2 with one", (SH, ["a"], {}), {"file_actions": [(posix.POSIX_SPAWN_DUP2, 1)]}), ("dup2 of a str", (SH, ["a"], {}), {"file_actions": [(posix.POSIX_SPAWN_DUP2, 1, "a")]}),
                                ("open with four", (SH, ["a"], {}), {"file_actions": [(posix.POSIX_SPAWN_OPEN, 1, "x", 0)]}), ("open of an int", (SH, ["a"], {}), {"file_actions": [(posix.POSIX_SPAWN_OPEN, 1, 5, 0, 0)]}), ("open with a str for the flags", (SH, ["a"], {}), {"file_actions": [(posix.POSIX_SPAWN_OPEN, 1, "x", "a", 0)]}),
                                ("open with a str for the mode", (SH, ["a"], {}), {"file_actions": [(posix.POSIX_SPAWN_OPEN, 1, "x", 0, "a")]}), ("open of what is not there", (SH, ["sh", "-c", "exit 0"], {}), {"file_actions": [(posix.POSIX_SPAWN_OPEN, 0, "absent", 0, 0)]}), ("open with a zero", (SH, ["a"], {}), {"file_actions": [(posix.POSIX_SPAWN_OPEN, 1, "x\0", 0, 0)]}),
                                ("setpgroup a str", (SH, ["a"], {}), {"setpgroup": "a"}), ("setpgroup too much", (SH, ["a"], {}), {"setpgroup": 2 ** 40}), ("setsigmask an int", (SH, ["a"], {}), {"setsigmask": 5}), ("of strs", (SH, ["a"], {}), {"setsigmask": ["a"]}), ("of 0", (SH, ["a"], {}), {"setsigmask": [0]}), ("of -1", (SH, ["a"], {}), {"setsigmask": [-1]}),
                                ("of 1000", (SH, ["a"], {}), {"setsigmask": [1000]}), ("of too much", (SH, ["a"], {}), {"setsigmask": [2 ** 70]}), ("of floats", (SH, ["a"], {}), {"setsigmask": [1.5]}), ("setsigmask None", (SH, ["a"], {}), {"setsigmask": None}), ("setsigdef an int", (SH, ["a"], {}), {"setsigdef": 5}), ("setsigdef of 0", (SH, ["a"], {}), {"setsigdef": [0]}),
                                ("scheduler an int", (SH, ["a"], {}), {"scheduler": 5}), ("scheduler a list", (SH, ["a"], {}), {"scheduler": [1, 2]}), ("scheduler a tuple", (SH, ["a"], {}), {"scheduler": (None, 1)}), ("scheduler ()", (SH, ["a"], {}), {"scheduler": ()})):
        for name in ("posix_spawn", "posix_spawnp"):
            def go():
                pid = getattr(posix, name)(*args, **kwargs)
                return "started", posix.waitpid(pid, 0)[1]
            t("%s: %s" % (name, label), go)


    class Mapping:
        def __init__(self, keys, values, length=None): self.k, self.v, self.n = keys, values, len(keys) if length is None else length
        def __len__(self): return self.n
        def __getitem__(self, key): raise KeyError(key)
        def keys(self): return self.k
        def values(self): return self.v


    t("an environment of a program's", lambda: (run("echo $A$B", env=Mapping(["A", "B"], ["1", "2"])), run("echo $A$B", env=Mapping(("A", "B"), iter(["1", "2"]))), run("echo $A[$B]", env=Mapping(["A", "B"], ["1", "2"], 1)), attempt(lambda: run("", env=Mapping(["A"], ["1"], 2))), attempt(lambda: run("", env=Mapping(5, [])))))
    t("system", lambda: (posix.system("exit 0"), posix.system("exit 3"), posix.waitstatus_to_exitcode(posix.system("exit 3")), posix.system(b"exit 1"), posix.system(P("exit 2")), posix.system(command="true"), posix.system("kill -9 $$"), posix.system("")))
    for arg in (5, None, "a\0b", ["a"]):
        t("system(%r)" % (arg,), lambda: posix.system(arg))
    for args in (("absent", ["a"]), (".", ["a"]), (SH, "a"), (SH, []), (SH, ()), (SH, [""]), (SH, [5]), (SH, ["a\0"]), (SH,), (SH, ["a"], {}), (SH, iter(["a"])), (SH, None), ("", ["a"]), ("out", ["a"])):
        t("execv%r" % (tuple(a if not hasattr(a, "__next__") else "an iterator" for a in args),), lambda: posix.execv(*args))
    for args in (("absent", ["a"], {}), (".", ["a"], {}), (SH, "a", {}), (SH, [], {}), (SH, [""], {}), (SH, [5], {}), (SH, ["a"], 5), (SH, ["a"], None), (SH, ["a"], [1]), (SH, ["a"], {"a=b": "c"}), (SH, ["a"], {"": "c"}), (SH, ["a"], {5: "c"}), (SH, ["a"], {"a": 5}), (SH, ["a"]), (5, ["a"], {}), ("out", ["a"], {})):
        t("execve%r" % (args,), lambda: posix.execve(*args))
    t("execve by name", lambda: posix.execve(path="absent", argv=["a"], env={}))
    t("execv, in what has been started", lambda: run("exec /bin/echo replaced"))

    print("---- waiting")
    spawn = lambda script: posix.posix_spawn(SH, ["sh", "-c", script], {})
    t("waitpid", lambda: [(r[0] == pid, r[1], type(r).__name__, attempt(lambda: posix.waitpid(pid, 0))) for pid in [spawn("exit 7")] for r in [posix.waitpid(pid, 0)]])
    t("wait", lambda: [(r[0] == pid, r[1], attempt(posix.wait), attempt(lambda: posix.wait(1))) for pid in [spawn("exit 1")] for r in [posix.wait()]])
    t("wait3", lambda: [(r[0] == pid, r[1], type(r[2]).__name__, type(r[2]).__module__, len(r[2]), [type(v).__name__ for v in r[2]], r[2].ru_utime >= 0, r[2].ru_maxrss > 0, attempt(lambda: posix.wait3(0))) for pid in [spawn("exit 2")] for r in [posix.wait3(0)]])
    t("wait4", lambda: [(r[0] == pid, r[1], type(r[2]).__name__, r[2].ru_stime >= 0, attempt(lambda: posix.wait4(pid, 0))) for pid in [spawn("exit 4")] for r in [posix.wait4(pid, 0)]])
    t("by name", lambda: [(posix.wait4(pid=pid, options=0)[1]) for pid in [spawn("exit 5")]] + [posix.wait3(options=0)[1] for pid in [spawn("exit 6")]])
    t("waitid", lambda: [(type(r).__name__, type(r).__module__, len(r), r.si_pid == pid, r.si_uid == posix.getuid(), r.si_signo, r.si_status, r.si_code == posix.CLD_EXITED, attempt(lambda: posix.waitid(posix.P_PID, pid, posix.WEXITED))) for pid in [spawn("exit 9")] for r in [posix.waitid(posix.P_PID, pid, posix.WEXITED)]])
    t("of what was killed", lambda: [(r.si_status, r.si_code == posix.CLD_KILLED) for pid in [spawn("kill -9 $$")] for r in [posix.waitid(posix.P_PID, pid, posix.WEXITED)]])
    t("and left to be waited for again", lambda: [(posix.waitid(posix.P_PID, pid, posix.WEXITED | posix.WNOWAIT).si_status, posix.waitpid(pid, 0)[1]) for pid in [spawn("exit 8")]])


    def not_yet():
        r, w = posix.pipe()
        posix.set_inheritable(r, True)
        pid = posix.posix_spawn(SH, ["sh", "-c", "read x <&%d; exit 5" % r], {})
        out = (posix.waitpid(pid, posix.WNOHANG), posix.wait4(pid, posix.WNOHANG)[:2], tuple(posix.wait4(pid, posix.WNOHANG)[2]) == (0.0, 0.0) + (0,) * 14, posix.wait3(posix.WNOHANG)[:2], posix.waitid(posix.P_PID, pid, posix.WEXITED | posix.WNOHANG), posix.kill(pid, 0))
        posix.write(w, b"go\n")
        posix.close(w)
        posix.close(r)
        return out, posix.waitpid(pid, 0)[1]


    t("what has not ended yet", not_yet)
    for name, args in (("waitpid", (99999999, 0)), ("waitpid", ("a", 0)), ("waitpid", (0, "a")), ("waitpid", (0,)), ("waitpid", (1.5, 0)), ("waitpid", (2 ** 40, 0)), ("waitpid", (-1, 0)), ("waitpid", (0, 99999)), ("wait3", ("a",)), ("wait3", ()), ("wait4", ("a", 0)), ("wait4", (0, "a")), ("wait4", (0,)),
                       ("waitid", (posix.P_PID, 99999999, posix.WEXITED)), ("waitid", (posix.P_ALL, 0, posix.WEXITED)), ("waitid", (99, 0, posix.WEXITED)), ("waitid", (posix.P_PID, 1, 0)), ("waitid", ("a", 0, 0)), ("waitid", (0, "a", 0)), ("waitid", (0, 0, "a")), ("waitid", (0, 0))):
        t("%s%r" % (name, args), lambda: getattr(posix, name)(*args))
    STATUSES = (0, 1, 2, 9, 15, 127, 128, 129, 137, 255, 256, 257, 0x7f, 0x137f, 0x117f, 0xffff, 0x100, 0x300, 0xff00, 0x8b, 0x13, -1, 65536, 2 ** 31 - 1)
    for name in ("WCOREDUMP", "WIFCONTINUED", "WIFSTOPPED", "WIFSIGNALED", "WIFEXITED", "WEXITSTATUS", "WTERMSIG", "WSTOPSIG"):
        t(name, lambda: [getattr(posix, name)(s) for s in STATUSES])
        t(name + " of what is not a status", lambda: (attempt(lambda: getattr(posix, name)("a")), attempt(lambda: getattr(posix, name)()), attempt(lambda: getattr(posix, name)(2 ** 40)), attempt(lambda: getattr(posix, name)(1.5)), attempt(lambda: getattr(posix, name)(status=0))))
    t("waitstatus_to_exitcode", lambda: [attempt(lambda: posix.waitstatus_to_exitcode(s)) for s in STATUSES])
    for arg in ("a", 1.5, None, 2 ** 40, Index(256)):
        t("waitstatus_to_exitcode(%r)" % (arg,), lambda: posix.waitstatus_to_exitcode(arg))
    t("waitstatus_to_exitcode(status=)", lambda: posix.waitstatus_to_exitcode(status=512))

    print("---- signals")
    t("kill", lambda: [(posix.kill(pid, 0), posix.kill(pid, 15), posix.waitpid(pid, 0)[1], attempt(lambda: posix.kill(pid, 0))) for pid in [spawn("sleep 30")]])
    t("killpg", lambda: [(posix.killpg(pid, 9), posix.waitpid(pid, 0)[1], attempt(lambda: posix.killpg(pid, 0))) for pid in [posix.posix_spawn(SH, ["sh", "-c", "sleep 30"], {}, setpgroup=0)]])
    t("of this", lambda: (posix.kill(posix.getpid(), 0), posix.kill(0, 0), posix.killpg(posix.getpgrp(), 0)))
    for name, args in (("kill", (99999999, 0)), ("kill", (posix.getpid(), 999)), ("kill", ("a", 0)), ("kill", (0, "a")), ("kill", (0,)), ("kill", (1.5, 0)), ("kill", (2 ** 40, 0)), ("kill", (0, 2 ** 70)), ("killpg", (99999999, 0)), ("killpg", ("a", 0)), ("killpg", (0, "a")), ("killpg", (0, 2 ** 40)), ("killpg", (0,))):
        t("%s%r" % (name, tuple(a if a != posix.getpid() else "this" for a in args)), lambda: getattr(posix, name)(*args))
    for name, args in (("_exit", ("a",)), ("_exit", ()), ("_exit", (2 ** 40,)), ("_exit", (1.5,)), ("abort", (1,))):
        t("%s%r" % (name, args), lambda: getattr(posix, name)(*args))
    print("---- who is told")
    told = []
    listening = [True]
    sys.addaudithook(lambda event, args: told.append((event, tuple(a if not isinstance(a, int) or a < 100 or a > 10 ** 7 else "a process" for a in args))) if listening[0] else None)


    def heard(f):
        del told[:]
        result = attempt(f)
        return told[:], result if isinstance(result, str) else None


    fd = posix.open("audited", posix.O_RDWR | posix.O_CREAT, 0o600)
    posix.dup2(fd, 41)
    posix.close(fd)
    for label, f in (("open", lambda: posix.close(posix.open("audited", posix.O_RDONLY))), ("open of bytes", lambda: posix.close(posix.open(b"audited", posix.O_RDONLY, dir_fd=None))), ("open of what has __fspath__", lambda: posix.close(posix.open(P("audited"), 0))), ("open that fails", lambda: posix.open("absent", 0)),
                     ("mkdir", lambda: posix.mkdir("ad", 0o700)), ("mkdir by way of a directory", lambda: posix.mkdir("ad", dir_fd=41)), ("chdir", lambda: posix.chdir(".")), ("fchdir", lambda: posix.fchdir(41)), ("chdir to an open file", lambda: posix.chdir(41)), ("listdir", lambda: posix.listdir("ad")), ("listdir()", lambda: posix.listdir()),
                     ("scandir", lambda: posix.scandir("ad").close()), ("scandir()", lambda: posix.scandir().close()), ("rename", lambda: posix.rename("ad", "ae")), ("replace", lambda: posix.replace("ae", b"ad")), ("rmdir", lambda: posix.rmdir("ad")), ("chmod", lambda: posix.chmod("audited", 0o600)), ("fchmod", lambda: posix.fchmod(41, 0o600)),
                     ("lchmod", lambda: posix.lchmod("audited", 0o600)), ("chmod of an open file", lambda: posix.chmod(41, 0o600)), ("chown", lambda: posix.chown("audited", -1, -1)), ("fchown", lambda: posix.fchown(41, -1, -1)), ("lchown", lambda: posix.lchown("audited", -1, -1)), ("chflags", lambda: posix.chflags("audited", 0)),
                     ("lchflags", lambda: posix.lchflags("audited", 0)), ("link", lambda: posix.link("audited", "al")), ("symlink", lambda: posix.symlink("audited", "as")), ("unlink", lambda: posix.unlink("al")), ("remove", lambda: posix.remove("as")), ("truncate", lambda: posix.truncate("audited", 3)), ("ftruncate", lambda: posix.ftruncate(41, 2)),
                     ("truncate of an open file", lambda: posix.truncate(41, 1)), ("utime", lambda: posix.utime("audited")), ("utime with times", lambda: posix.utime("audited", (1, 2))), ("utime in nanoseconds", lambda: posix.utime("audited", ns=(1, 2))), ("lockf", lambda: posix.lockf(41, posix.F_TEST, 0)), ("putenv", lambda: posix.putenv("JSC_PY_G", "v")),
                     ("unsetenv", lambda: posix.unsetenv("JSC_PY_G")), ("system", lambda: posix.system("true")), ("kill", lambda: posix.kill(posix.getpid(), 0)), ("killpg", lambda: posix.killpg(posix.getpgrp(), 0)), ("posix_spawn", lambda: posix.waitpid(posix.posix_spawn(SH, ["sh", "-c", "true"], {"A": "b"}), 0)),
                     ("posix_spawnp", lambda: posix.waitpid(posix.posix_spawnp("sh", ("sh", "-c", "true"), None), 0)), ("execv", lambda: posix.execv("absent", ["a"])), ("execve", lambda: posix.execve("absent", ["a"], {"A": "b"})), ("what is wrong before anyone is told", lambda: posix.mkdir(5)), ("stat", lambda: posix.stat("audited")), ("access", lambda: posix.access("audited", 0))):
        t(label, lambda: heard(f))


    def refuse(event, args):
        if listening[0] and event in ("os.mkdir", "os.listdir", "open", "os.system", "os.posix_spawn", "os.putenv", "os.kill"):
            raise RuntimeError("not allowed: " + event)


    sys.addaudithook(refuse)
    t("what is refused is not done", lambda: (attempt(lambda: posix.mkdir("refused")), attempt(lambda: posix.listdir()), attempt(lambda: posix.open("refused", posix.O_CREAT)), attempt(lambda: posix.system("touch refused")), attempt(lambda: posix.posix_spawn(SH, ["sh", "-c", "touch refused"], {})), attempt(lambda: posix.putenv("JSC_PY_H", "v")),
                                                 attempt(lambda: posix.kill(0, 0)), posix.access("refused", 0), b"JSC_PY_H" in posix._create_environ()))
    listening[0] = False
    posix.close(41)
    t("there is no fork", lambda: (hasattr(posix, "fork"), hasattr(posix, "forkpty"), hasattr(posix, "register_at_fork")) and "as may be")
finally:
    posix.chdir(base)
    for name in posix.listdir():
        posix.unlink(name)
    posix.chdir(start)
    posix.rmdir(base)
