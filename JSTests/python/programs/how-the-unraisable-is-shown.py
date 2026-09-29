# What sys.unraisablehook writes at first. Its traceback is written by C in CPython, not by traceback.py: a line for each frame, with nothing under it to say what part of it.
import atexit
import os
import sys


# Where this file is has nothing to do with it.
class Relative:
    here = os.path.dirname(os.path.abspath(__file__)) + os.sep
    def write(self, text): return sys.stdout.write(text.replace(self.here, ""))
    def flush(self): sys.stdout.flush()


sys.stderr = Relative()


def report(function):
    atexit._clear()
    atexit.register(function)
    atexit._run_exitfuncs()
    sys.stdout.flush()


class Named:
    def __init__(self, f): self.f = f
    def __call__(self): return self.f()
    def __repr__(self): return "<it>"


def part_of_a_line(): x = 1; raise ValueError("part of a line")


def over_several_lines():
    return (1 +
            [2,
             3] +
            4)


def inner(): return 1 / 0
def middle(): return inner() + 1
def outer(): return [middle()]


def again(n):
    if not n:
        raise KeyError("the bottom")
    return again(n - 1)


def one(n): return two(n)
def two(n): return one(n - 1) if n else 1 / 0


def indented():
	 	raise ValueError("after a tab, a space and a tab")


print("---- as it is")
for f in (part_of_a_line, over_several_lines, outer, indented, lambda: again(0), lambda: again(2), lambda: again(3), lambda: again(4), lambda: again(5), lambda: again(30), lambda: one(4)):
    report(Named(f))

print("---- what has no file")
report(Named(eval("lambda: 1 / 0")))
namespace = {}
exec(compile("def f():\n    raise ValueError('in a string')\n", "<made up>", "exec"), namespace)
report(Named(namespace["f"]))
exec(compile("def f():\n    raise ValueError('in no such file')\n", "/no/such/file.py", "exec"), namespace)
report(Named(namespace["f"]))
exec(compile("def f():\n    raise ValueError('found by the end of its name')\n", "/elsewhere/atexit-module.py", "exec"), namespace)
before = sys.path[:]
for path in ([], [5, None, b"x", "/no/such/directory", Relative.here], [Relative.here.rstrip(os.sep)], ["/no/such/directory"], (Relative.here,), None):
    sys.path = path
    report(Named(namespace["f"]))
sys.path = before
exec(compile("\n" * 100000 + "def f():\n    raise ValueError('past the end')\n", __file__, "exec"), namespace)
report(Named(namespace["f"]))
exec(compile("def f():\n    raise ValueError('<half')\n", "<half", "exec"), namespace)
report(Named(namespace["f"]))

print("---- sys.tracebacklimit")
for limit in (None, 1000, 5, 3, 2, 1, 0, -1, -5, 2 ** 100, -2 ** 100, True, False, 1.5, "2", [], type("I", (int,), {})(2)):
    print("limit", ascii(limit) if not isinstance(limit, int) or type(limit) in (int, bool) else "I(2)")
    sys.tracebacklimit = limit
    report(Named(outer))
sys.tracebacklimit = 4
report(Named(lambda: again(30)))
del sys.tracebacklimit

print("---- what is said of it")


class NoRepr:
    def __call__(self): raise KeyError("no repr")
    def __repr__(self): raise RuntimeError("cannot be shown")


class NoStr(Exception):
    def __str__(self): raise RuntimeError("cannot be said")


class Elsewhere(Exception):
    __module__ = "some.where"


class NoModule(Exception):
    __module__ = 5


class Inner:
    class Deep(Exception):
        pass


def raiser(e):
    def f(): raise e
    return Named(f)


report(NoRepr())
for e in (NoStr(), Elsewhere("x"), NoModule("x"), Inner.Deep("x"), ValueError(), ValueError(""), ValueError("a", "b"), KeyError("k"), SystemExit(3), KeyboardInterrupt(), OSError(2, "no", "file"), SyntaxError("bad", ("f.py", 1, 2, "text")), ExceptionGroup("g", [ValueError(1), TypeError(2)]), UnicodeDecodeError("utf-8", b"\xff", 0, 1, "bad"), ValueError("two\nlines"), ValueError("\xe9中\U0001F600")):
    report(raiser(e))


def chained():
    try:
        1 / 0
    except ZeroDivisionError as e:
        raise ValueError("neither the cause nor the context is shown") from e


report(Named(chained))
noted = ValueError("nor are notes")
noted.add_note("a note")
report(raiser(noted))

