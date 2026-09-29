# The module atexit: what is registered, in what order it is called, and what becomes of what it raises. The end of this file is what is done when the program ends.
import atexit
import os
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


t("the module", lambda: (atexit.__name__, atexit.__package__, atexit.__loader__.__name__, atexit.__doc__, sorted(n for n in vars(atexit) if not n.startswith("__"))))
for name in sorted(n for n in vars(atexit) if not n.startswith("__")):
    f = getattr(atexit, name)
    t(name, lambda: (type(f).__name__, f.__text_signature__, f.__doc__, f.__module__))
atexit._clear()
calls = []


def note(*a, **k):
    calls.append((a, k))


def ran():
    out = list(calls)
    calls.clear()
    return out


t("register", lambda: (atexit.register(note) is note, atexit.register(note, 1, 2), atexit.register(note, 3, x=4), atexit.register(note, **{"func": 5}), atexit._ncallbacks()) and atexit._ncallbacks())
t("the last first", lambda: (atexit._run_exitfuncs(), ran(), atexit._ncallbacks(), atexit._run_exitfuncs(), ran()))
t("what will not do", lambda: [attempt(atexit.register, *a, **k) for a, k in (((), {}), ((5,), {}), ((None,), {}), (("a",), {}), ((), {"func": note}), (([],), {}))] + [atexit._ncallbacks()])
t("how the rest are called", lambda: [attempt(f, *a, **k) for f in (atexit._clear, atexit._run_exitfuncs, atexit._ncallbacks) for a, k in (((1,), {}), ((), {"x": 1}))] + [attempt(atexit.unregister), attempt(atexit.unregister, 1, 2), attempt(lambda: atexit.unregister(func=note)), atexit.unregister(5), atexit.unregister(None)])
t("as a decorator", lambda: [atexit.register(lambda: calls.append("decorated")).__name__, atexit._run_exitfuncs(), ran()])
t("_clear", lambda: (atexit.register(note, 1), atexit.register(note, 2), atexit._ncallbacks(), atexit._clear(), atexit._ncallbacks(), atexit._run_exitfuncs(), ran())[2:])


def other(*a):
    calls.append(("other", a))


t("unregister", lambda: (atexit.register(note, 1), atexit.register(other, 2), atexit.register(note, 3), atexit.register(other, 4), atexit.unregister(note), atexit._ncallbacks(), atexit._run_exitfuncs(), ran())[4:])
t("unregister what is not there", lambda: (atexit.register(note, 1), atexit.unregister(other), atexit._ncallbacks(), atexit._clear())[1:])


class Equal:
    def __init__(self, name, answer): self.name, self.answer = name, answer
    def __call__(self): calls.append(self.name)
    def __eq__(self, o):
        calls.append(("eq", self.name, getattr(o, "name", o)))
        if isinstance(self.answer, BaseException):
            raise self.answer
        return self.answer
    __hash__ = None


t("it is by ==", lambda: (atexit.register(Equal("a", True)), atexit.register(Equal("b", False)), atexit.register(Equal("c", True)), atexit.unregister(5), ran(), atexit._ncallbacks(), atexit._run_exitfuncs(), ran())[3:])
t("== that raises", lambda: (atexit.register(Equal("a", True)), atexit.register(Equal("b", ValueError("no"))), atexit.register(Equal("c", True)), attempt(atexit.unregister, 5), ran(), atexit._ncallbacks(), atexit._clear())[3:])


class Meddles:
    def __init__(self, what): self.what = what
    def __call__(self): calls.append("meddler")
    def __eq__(self, o):
        self.what()
        return True
    __hash__ = None


t("== that clears", lambda: (atexit.register(note, 1), atexit.register(Meddles(atexit._clear)), atexit.register(note, 2), atexit.unregister(5), atexit._ncallbacks(), atexit._clear())[3:])
t("== that registers", lambda: (atexit.register(note, 1), atexit.register(Meddles(lambda: atexit._ncallbacks() < 6 and atexit.register(note, 9))), atexit.register(note, 2), atexit.unregister(5), atexit._ncallbacks(), atexit._run_exitfuncs(), ran())[3:])
t("== that unregisters", lambda: (atexit.register(note, 1), atexit.register(Meddles(lambda: atexit.unregister(note))), atexit.register(note, 2), atexit.unregister(5), atexit._ncallbacks(), atexit._run_exitfuncs(), ran())[3:])

print("---- what is done meanwhile")
t("registering", lambda: (atexit.register(lambda: (calls.append("outer"), atexit.register(note, "inner"))), atexit._run_exitfuncs(), ran(), atexit._ncallbacks(), atexit._run_exitfuncs(), ran())[1:])
t("clearing", lambda: (atexit.register(note, 1), atexit.register(atexit._clear), atexit.register(note, 2), atexit._run_exitfuncs(), ran(), atexit._ncallbacks())[3:])
t("unregistering", lambda: (atexit.register(note, 1), atexit.register(lambda: atexit.unregister(note)), atexit.register(note, 2), atexit._run_exitfuncs(), ran(), atexit._ncallbacks())[3:])
t("running", lambda: (atexit.register(note, 1), atexit.register(lambda: len(calls) < 5 and atexit._run_exitfuncs()), atexit.register(note, 2), atexit._run_exitfuncs(), ran(), atexit._ncallbacks())[3:])

print("---- what is raised")
sys.stdout.flush()


def raises(e):
    def raiser():
        raise e
    raiser.__qualname__ = "raiser_of_" + type(e).__name__
    return raiser


class Shown:
    def __call__(self): raise KeyError("shown")
    def __repr__(self): return "<Shown>"


class NotShown:
    def __call__(self): raise KeyError("not shown")
    def __repr__(self): raise RuntimeError("no repr")


hooked = []
sys.unraisablehook = lambda u: hooked.append((type(u.exc_value).__name__, str(u.exc_value), u.err_msg if "0x" not in (u.err_msg or "") else u.err_msg.split(" at 0x")[0], u.object))
t("it is said, and the rest are called", lambda: (atexit.register(note, 1), atexit.register(raises(ValueError("first"))), atexit.register(note, 2), atexit.register(raises(TypeError("second"))), atexit.register(Shown()), atexit.register(NotShown()), atexit.register(raises(SystemExit(3))), atexit.register(raises(KeyboardInterrupt())), atexit.register(note, 3), atexit._run_exitfuncs(), ran(), hooked[:], atexit._ncallbacks())[9:])
hooked.clear()
t("called wrongly", lambda: (atexit.register(lambda: None, 1), atexit.register(len), atexit._run_exitfuncs(), hooked[:])[2:])
hooked.clear()
sys.unraisablehook = sys.__unraisablehook__
# Where this file is has nothing to do with it.
class Relative:
    here = os.path.dirname(os.path.abspath(__file__)) + os.sep
    def write(self, text): return sys.stdout.write(text.replace(self.here, ""))
    def flush(self): sys.stdout.flush()


sys.stderr = Relative()
t("as it is shown", lambda: (atexit.register(Shown()), atexit._run_exitfuncs()))

print("---- when the program ends")
atexit.register(print, "the first registered is the last called")
atexit.register(lambda: atexit.register(print, "registered while they were being called, and never called"))
atexit.register(Shown())
atexit.register(raises(SystemExit(5)).__call__ and Shown())
atexit.register(lambda: print("sys.stdout is still there:", not sys.stdout.closed, "and so are modules:", "atexit" in sys.modules))
atexit.register(print, "with", "arguments", sep="-")
atexit.register(lambda: sys.stdout.write("what is written and not flushed is flushed afterwards\n"))
print("the last line of the program")
