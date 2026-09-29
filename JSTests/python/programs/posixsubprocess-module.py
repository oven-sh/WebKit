# The module _posixsubprocess, and subprocess, which is written in Python over it.
import _posixsubprocess
import os
import signal
import subprocess
import sys


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


signal.alarm(0)
F = _posixsubprocess.fork_exec
t("the module", lambda: (_posixsubprocess.__name__, _posixsubprocess.__package__, _posixsubprocess.__loader__.__name__, _posixsubprocess.__doc__, sorted(n for n in vars(_posixsubprocess) if not n.startswith("__"))))
t("fork_exec", lambda: (type(F).__name__, F.__text_signature__, F.__doc__, F.__module__))

print("---- what will not do")
# args, executable_list, close_fds, pass_fds, cwd, env, p2cread, p2cwrite, c2pread, c2pwrite, errread, errwrite, errpipe_read, errpipe_write, restore_signals, call_setsid, pgid_to_set, gid, extra_groups, uid, child_umask, preexec_fn
GOOD = [[b"true"], [b"/usr/bin/true"], True, (), None, None, -1, -1, -1, -1, -1, -1, 100, 101, True, False, -1, None, None, None, -1, None]


class Path:
    def __init__(self, v): self.v = v
    def __fspath__(self): return self.v


class BadBool:
    def __bool__(self): raise ValueError("no truth in it")


class Seq:
    def __init__(self, *items): self.items = items
    def __len__(self): return len(self.items)
    def __getitem__(self, i): return self.items[i]


def wrong(index, value):
    a = list(GOOD)
    a[index] = value
    return attempt(F, *a)


t("how many", lambda: [attempt(F, *GOOD[:n]) for n in (0, 1, 21)] + [attempt(F, *GOOD, 1), attempt(lambda: F(*GOOD[:21], preexec_fn=None))])
for index, name, values in ((0, "args", (5, [5], [None], ["a\0b"], [b"a\0b"], [Path(5)], object())), (1, "executable_list", (5, None, ["a"], [5], [b"a\0b"], [bytearray(b"a")], object(), Seq("a"))), (2, "close_fds", (BadBool(),)), (3, "pass_fds", (5, None, [], [3], ("a",), (1.5,), (-1,), (2 ** 31,), (2 ** 100,), (5, 4), (4, 4), (None,), (True, False))), (4, "cwd", (5, "a\0b", b"a\0b", [], Path(5))), (5, "env", (5, ["a"], [5], [b"a\0b"], object())),
                            (6, "p2cread", ("a", None, 1.5, 2 ** 31)), (13, "errpipe_write", ("a", None, 1.5, 2 ** 31, 2, 0, -1)), (14, "restore_signals", (BadBool(),)), (15, "call_setsid", (BadBool(),)), (16, "pgid_to_set", ("a", None, 1.5, 2 ** 31)), (17, "gid", ("a", 1.5, -2, 2 ** 32, 2 ** 100, [])), (18, "extra_groups", (5, (), "a", ["a"], [1.5], [None], [-2], [2 ** 32], [2 ** 100], [0] * 100000)), (19, "uid", ("a", 1.5, -2, 2 ** 32, 2 ** 100, [])), (20, "child_umask", ("a", None, 1.5, 2 ** 31))):
    t(name, lambda: [wrong(index, v) for v in values])
t("errpipe_write may be anything if nothing is to be closed", lambda: [type(x).__name__ for x in [wrong(2, False)]] and None)
t("args that change while they are gone through", lambda: [attempt(F, *([l] + GOOD[1:])) for l in [[]] for _ in [l.extend([type("P", (), {"__fspath__": lambda self: (l.clear(), b"x")[1]})(), b"y"])]])

print("---- starting something")


def run(args, executables=None, close_fds=True, pass_fds=(), cwd=None, env=None, stdin=None, restore_signals=True, setsid=False, pgid=-1, umask=-1):
    "As Popen._execute_child() does it. What the program wrote, how it ended, and what came down the pipe for errors."
    out_r, out_w = os.pipe()
    err_r, err_w = os.pipe()
    # It has to be 3 or more.
    low = []
    while err_w < 3:
        low.append(err_w)
        err_w = os.dup(err_w)
    for fd in low:
        os.close(fd)
    in_r = in_w = -1
    if stdin is not None:
        in_r, in_w = os.pipe()
    pid = F(args, executables or [os.fsencode(args[0])], close_fds, tuple(sorted(set(pass_fds) | {err_w})), cwd, env, in_r, in_w, out_r, out_w, -1, -1, err_r, err_w, restore_signals, setsid, pgid, None, None, None, umask, None)
    os.close(out_w)
    os.close(err_w)
    if stdin is not None:
        os.close(in_r)
        os.write(in_w, stdin)
        os.close(in_w)
    data = b""
    while chunk := os.read(out_r, 65536):
        data += chunk
    error = b""
    while chunk := os.read(err_r, 65536):
        error += chunk
    os.close(out_r)
    os.close(err_r)
    return type(pid).__name__, data, os.waitstatus_to_exitcode(os.waitpid(pid, 0)[1]), error


t("echo", lambda: run(["/bin/echo", "a", b"b", Path("c"), Path(b"d")]))
t("a tuple, and anything that can be gone through", lambda: [run(x, [b"/bin/echo"])[1] for x in (("echo", "a"), iter(["echo", "b"]), (i for i in ("echo", "c")), {"echo": 1, "d": 2})])
t("what it is called need not be what it is", lambda: run(["something else", "-c", "echo $0"], [b"/bin/sh"]))
t("the first that there is", lambda: run(["echo", "found"], [b"/no/such/echo", b"/nor/this/echo", b"/bin/echo", b"/usr/bin/false"]))
t("none of them", lambda: run(["x"], [b"/no/such/x", b"/nor/this/x"]))
t("one that cannot be run", lambda: (run(["x"], [b"/etc/hosts"]), run(["x"], [b"/etc"]), run(["x"], [b"/no/such/x", b"/etc/hosts", b"/nor/this"]), run(["x"], [b"/etc/hosts/x"])))
t("no executables at all", lambda: run(["x"], Seq()))
t("how it ends", lambda: [run(["/bin/sh", "-c", c])[1:3] for c in ("exit 0", "exit 1", "exit 255", "kill -TERM $$", "kill -KILL $$", "echo out; exit 3")])
t("stdin", lambda: run(["/bin/cat"], stdin=b"through it"))
t("env", lambda: (run(["/usr/bin/env"], env=[b"A=1", b"B=two words"])[1], run(["/usr/bin/env"], env=[])[1], run(["/usr/bin/env"], env=(b"C=3",))[1], run(["/usr/bin/env"], env=Seq(b"D=4"))[1], b"PATH=" in run(["/usr/bin/env"])[1]))
t("cwd", lambda: (run(["/bin/pwd"], cwd="/usr")[1], run(["/bin/pwd"], cwd=b"/usr/bin")[1], run(["/bin/pwd"], cwd=Path("/usr/lib"))[1], run(["/bin/pwd"], cwd="/no/such/directory")[1:], run(["/bin/pwd"], cwd="/etc/hosts")[1:]))
t("umask", lambda: [run(["/bin/sh", "-c", "umask"], umask=m)[1] for m in (0, 0o22, 0o77, 0o777)])
t("a session and a group of its own", lambda: (run(["/bin/sh", "-c", "[ $(ps -o pgid= -p $$) -eq $$ ] && echo leads"], setsid=True)[1:3], run(["/bin/sh", "-c", "[ $(ps -o pgid= -p $$) -eq $$ ] && echo leads"], pgid=0)[1:3]) if False else "left out: ps is not always allowed")

print("---- what is left open")
COUNT = "for f in 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do if { true >&$f; } 2>/dev/null; then printf '%s ' open; fi; done"
keep_r, keep_w = os.pipe()
os.set_inheritable(keep_w, True)
t("nothing, if it is all to be closed", lambda: run(["/bin/sh", "-c", COUNT])[1])
t("what is asked for", lambda: run(["/bin/sh", "-c", "echo kept >&%d" % keep_w], pass_fds=(keep_w,))[1:3] + (os.read(keep_r, 100),))
t("though it was not to be inherited", lambda: (os.set_inheritable(keep_w, False), run(["/bin/sh", "-c", "echo kept >&%d" % keep_w], pass_fds=(keep_w,))[1:3], os.read(keep_r, 100), os.get_inheritable(keep_w)))
t("and not otherwise", lambda: (os.set_inheritable(keep_w, True), run(["/bin/sh", "-c", "{ echo kept >&%d; } 2>/dev/null" % keep_w])[2] != 0))
t("what can be inherited, if nothing is to be closed", lambda: (run(["/bin/sh", "-c", "echo kept >&%d" % keep_w], close_fds=False)[1:3], os.read(keep_r, 100)))
os.close(keep_r)
os.close(keep_w)

print("---- signals")
t("what Python ignores is not ignored there", lambda: (signal.getsignal(signal.SIGPIPE), run(["/bin/sh", "-c", "kill -PIPE $$; echo survived"])[1:3], run(["/bin/sh", "-c", "kill -PIPE $$; echo survived"], restore_signals=False)[1:3]))
t("what a program has a function for is as it is by default", lambda: (signal.signal(signal.SIGUSR1, lambda n, f: None), run(["/bin/sh", "-c", "kill -USR1 $$; echo survived"])[1:3], signal.signal(signal.SIGUSR1, signal.SIG_DFL) and None))
t("what it ignores is ignored", lambda: (signal.signal(signal.SIGUSR1, signal.SIG_IGN), run(["/bin/sh", "-c", "kill -USR1 $$; echo survived"])[1:3], signal.signal(signal.SIGUSR1, signal.SIG_DFL)))

print("---- what is told of")
audited = []
sys.addaudithook(lambda event, args: audited.append((event, args)) if event.startswith("_posixsubprocess") else None)
t("fork_exec", lambda: (run(["/usr/bin/true"], env=[b"A=1"])[2], audited, audited.clear()))
t("not if it is not got as far as", lambda: (wrong(19, "a"), audited))

print("---- subprocess")
t("run", lambda: (subprocess.run(["/bin/echo", "hello"], capture_output=True), subprocess.run("echo $((1 + 2)); echo err >&2; exit 3", shell=True, capture_output=True, text=True)))
t("check_output", lambda: (subprocess.check_output(["/usr/bin/tr", "a-z", "A-Z"], input=b"shout"), attempt(subprocess.check_output, ["/usr/bin/false"]), attempt(subprocess.check_call, ["/bin/sh", "-c", "exit 7"])))
t("what there is none of", lambda: (attempt(subprocess.run, ["/no/such/program"]), attempt(subprocess.run, ["no-such-program-anywhere"]), attempt(subprocess.run, ["/bin/echo"], cwd="/no/such/directory"), attempt(subprocess.run, ["/etc/hosts"]), attempt(subprocess.run, [])))
t("communicate", lambda: [(p.communicate(b"through cat"), p.returncode, p.poll(), p.wait()) for p in [subprocess.Popen(["/bin/cat"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)]])
t("a good deal, both ways at once", lambda: [len(subprocess.run(["/bin/cat"], input=b"x" * n, capture_output=True).stdout) for n in (0, 1, 65536, 1000000)])
t("stderr to stdout", lambda: subprocess.run("echo out; echo err >&2", shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout)
t("to nowhere", lambda: subprocess.run("echo out; echo err >&2", shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL))
t("one into another", lambda: [(b.communicate()[0], a.wait()) for a in [subprocess.Popen(["/bin/echo", "b\na\nc"], stdout=subprocess.PIPE)] for b in [subprocess.Popen(["/usr/bin/sort"], stdin=a.stdout, stdout=subprocess.PIPE)] for _ in [a.stdout.close()]])
t("env and cwd", lambda: (subprocess.run(["/usr/bin/env"], env={"ONLY": "this"}, capture_output=True).stdout, subprocess.run(["/bin/pwd"], cwd="/usr", capture_output=True).stdout))
t("signals", lambda: (subprocess.run(["/bin/sh", "-c", "kill -TERM $$"]).returncode, [(p.terminate(), p.wait()) for p in [subprocess.Popen(["/bin/sleep", "30"])]], [(p.kill(), p.wait()) for p in [subprocess.Popen(["/bin/sleep", "30"])]], [(p.send_signal(signal.SIGINT), p.wait()) for p in [subprocess.Popen(["/bin/sleep", "30"])]]))
t("timeout", lambda: (attempt(subprocess.run, ["/bin/sleep", "30"], timeout=0.1), [(attempt(p.wait, 0.05), p.kill(), p.wait()) for p in [subprocess.Popen(["/bin/sleep", "30"])]]))
t("with", lambda: [(p.stdout.read(), p.returncode) for q in [subprocess.Popen(["/bin/echo", "in a with"], stdout=subprocess.PIPE)] for p in [q.__enter__()]] and None)
t("text", lambda: (subprocess.run(["/bin/echo", "\xe9中"], capture_output=True, text=True, encoding="utf-8").stdout, subprocess.run(["/bin/cat"], input="a\r\nb\n", capture_output=True, text=True).stdout, subprocess.getoutput("echo got"), subprocess.getstatusoutput("exit 4")))
t("pass_fds", lambda: [(subprocess.run(["/bin/sh", "-c", "echo through >&%d" % w], pass_fds=(w,)).returncode, os.read(r, 100), os.close(r), os.close(w)) for r, w in [os.pipe()]])
t("start_new_session, process_group and umask", lambda: (subprocess.run(["/usr/bin/true"], start_new_session=True).returncode, subprocess.run(["/usr/bin/true"], process_group=0).returncode, subprocess.run(["/bin/sh", "-c", "umask"], umask=0o27, capture_output=True).stdout))
t("os.popen and os.system", lambda: (os.popen("echo popen").read(), os.system("exit 3") >> 8))
