import sys
# What is written in C is what this is about. It goes to warnings.py for what that has, if it is there.
sys.modules["warnings"] = None
import _warnings
from _warnings import warn, warn_explicit, filters
class Captured:
    directory = __file__[:__file__.rfind("/") + 1]
    def __init__(self): self.text = []
    def write(self, text): self.text.append(text.replace(self.directory, "") if self.directory else text)
    def flush(self): pass
    def take(self): r = "".join(self.text); self.text.clear(); return r
err = sys.stderr = Captured()
original = list(filters)
def show(label, f):
    filters[:] = original; globals().pop("__warningregistry__", None); _warnings._onceregistry.clear(); err.take()
    try:
        r = f(); print(label, "=>", r, "|", repr(err.take()))
    except BaseException as e:
        print(label, "!!", type(e).__name__, e, "|", repr(err.take()))
def attempt(f):
    try: return f()
    except BaseException as e: return type(e).__name__ + ": " + str(e)
def using(*fs): filters[:] = list(fs)
show("module", lambda: (_warnings.__name__, sorted(n for n in dir(_warnings) if not n.startswith("__")), _warnings.__doc__))
show("what is there to begin with", lambda: (original, _warnings._defaultaction, _warnings._onceregistry, type(_warnings._warnings_context).__name__, _warnings._warnings_context.name))
# ---- warn()
show("warn", lambda: warn("hello"))
show("each category", lambda: [warn("m", c) for c in (UserWarning, Warning, RuntimeWarning, SyntaxWarning, FutureWarning, BytesWarning, UnicodeWarning, EncodingWarning)])
show("those that are ignored", lambda: [warn("m", c) for c in (PendingDeprecationWarning, ImportWarning, ResourceWarning)])
show("DeprecationWarning in __main__", lambda: warn("old", DeprecationWarning))
show("None for the category", lambda: warn("m", None))
show("an instance", lambda: warn(RuntimeWarning("inst")))
show("an instance, and a category that is not looked at", lambda: warn(RuntimeWarning("inst"), UserWarning))
show("an instance with two arguments", lambda: warn(UserWarning("a", 2)))
show("what is no string", lambda: (warn(5), warn(None), warn(["l"])))
class MyWarning(UserWarning): pass
show("a class of one's own", lambda: warn("mine", MyWarning))
for c in (int, 1, "UserWarning", Exception, (UserWarning,)):
    show("category %r" % (c,), lambda: warn("m", c))
def twice():
    for i in range(3): warn("in a loop")
show("once for each place", twice)
def two_places():
    warn("same text")
    warn("same text")
show("two places", two_places)
show("two texts on a line", lambda: (warn("one"), warn("two")))
show("the registry", lambda: (warn("r"), sorted((k if isinstance(k, str) else (k[0], k[1].__name__, k[2] > 0), v) for k, v in __warningregistry__.items())))
def level(n): warn("level %d" % n, stacklevel=n)
def caller(n): level(n)
for n in (0, 1, 2, 3, -1, 50): show("stacklevel %d" % n, lambda: caller(n))
show("stacklevel is odd", lambda: [attempt(lambda: warn("m", stacklevel=x)) for x in ("1", None, 1.5, 1 << 70)])
show("by name", lambda: warn(message="m", category=RuntimeWarning, stacklevel=1, source=None))
show("wrong arguments", lambda: [attempt(f) for f in (lambda: warn(), lambda: warn("m", UserWarning, 1, None, ()), lambda: warn("m", x=1), lambda: warn("m", skip_file_prefixes=[]), lambda: warn("m", skip_file_prefixes=(1,)), lambda: warn("m", skip_file_prefixes="a"))])
show("skip_file_prefixes", lambda: (caller_skipping(()), caller_skipping(("/nowhere",)), caller_skipping((__file__,))))
def caller_skipping(p): warn("skipping %d" % len(p), skip_file_prefixes=p)
show("skip_file_prefixes", lambda: (caller_skipping(()), caller_skipping(("/nowhere",)), caller_skipping((__file__,))))
show("in exec", lambda: exec("warn('from exec')", {"warn": warn}))
show("in exec, with a name", lambda: exec("warn('from exec')", {"warn": warn, "__name__": "named"}))
show("in exec, with a name that is None", lambda: exec("warn('from exec')", {"warn": warn, "__name__": None}))
show("in exec, with a name that is no string", lambda: exec("warn('from exec')", {"warn": warn, "__name__": 5}))
show("in compiled code with the name of this file", lambda: exec(compile("\n\nwarn('third line')", __file__, "exec"), {"warn": warn}))
show("a registry that is no dict", lambda: exec("warn('m')", {"warn": warn, "__warningregistry__": 5}))
show("a registry that is None", lambda: exec("warn('m'); warn('m')", {"warn": warn, "__warningregistry__": None}))
# ---- actions
def three(): 
    for i in range(3): warn("text", UserWarning)
def elsewhere(): warn("text", UserWarning)
for action in ("error", "ignore", "always", "all", "default", "module", "once", "bogus", ""):
    show("action " + repr(action), lambda: (using((action, None, Warning, None, 0)), three(), elsewhere()))
show("error with an instance", lambda: (using(("error", None, Warning, None, 0)), warn(MyWarning("x", 1))))
show("the first that matches", lambda: (using(("ignore", None, UserWarning, None, 0), ("error", None, Warning, None, 0)), warn("u"), attempt(lambda: warn("r", RuntimeWarning))))
show("no filters", lambda: (using(), three()))
show("by module", lambda: (using(("error", None, Warning, "__main__", 0)), attempt(lambda: warn("m")), exec("warn('other')", {"warn": warn, "__name__": "other"})))
show("by text", lambda: (using(("error", "exactly", Warning, None, 0)), attempt(lambda: warn("exactly")), warn("exactly not")))
class Matcher:
    def __init__(s, r): s.r = r; s.seen = []
    def match(s, text): s.seen.append(text); return s.r
show("what has match()", lambda: (lambda a, b: (using(("error", a, Warning, b, 0)), attempt(lambda: warn("m")), a.seen, b.seen))(Matcher(1), Matcher("yes")))
show("match() says no", lambda: (lambda a: (using(("error", a, Warning, None, 0)), warn("m"), a.seen))(Matcher(0)))
class BadMatcher:
    def match(s, text): raise KeyError("match")
show("match() raises", lambda: (using(("error", BadMatcher(), Warning, None, 0)), warn("m")))
show("no match()", lambda: (using(("error", 5, Warning, None, 0)), warn("m")))
show("by line", lambda: (using(("error", None, Warning, None, 1), ("ignore", None, Warning, None, 0)), warn("m")))
for bad in (5, (), ("error",), ("error", None, Warning, None), ("error", None, Warning, None, 0, 0), ["error", None, Warning, None, 0], (5, None, Warning, None, 0), ("error", None, 5, None, 0), ("error", None, Warning, None, "0"), ("error", None, (Warning, int), None, 0)):
    show("filter %r" % (bad,), lambda: (using(bad), warn("m")))
class L(list): pass
# ---- warn_explicit()
show("warn_explicit", lambda: warn_explicit("m", UserWarning, "no_such_file.py", 3))
show("of this file", lambda: warn_explicit("m", UserWarning, __file__, 1))
show("of a file of the same name as one that modules are looked for beside", lambda: warn_explicit("m", UserWarning, "/no/such/directory/" + __file__.rpartition("/")[2], 1))
show("of a line that there is not", lambda: warn_explicit("m", UserWarning, __file__, 100000))
show("line 0 and below", lambda: (warn_explicit("m", UserWarning, __file__, 0), warn_explicit("n", UserWarning, __file__, -1)))
show("twice, with no registry", lambda: (warn_explicit("m", UserWarning, "no_such_file.py", 3), warn_explicit("m", UserWarning, "no_such_file.py", 3)))
def with_registry():
    r = {}; warn_explicit("m", UserWarning, "no_such_file.py", 3, registry=r); warn_explicit("m", UserWarning, "no_such_file.py", 3, registry=r); return sorted((k if isinstance(k, str) else (k[0], k[1].__name__, k[2]), v) for k, v in r.items())
show("twice, with a registry", with_registry)
show("the module is worked out", lambda: [(using(("error", None, Warning, m, 0)), attempt(lambda: warn_explicit("t", UserWarning, f, 1)))[1] for f, m in (("a/b.py", "a/b"), ("c", "c"), ("", "<unknown>"), (".py", ""), ("x.pyc", "x.pyc"))])
show("a module that is None", lambda: (using(("error", None, Warning, None, 0)), warn_explicit("m", UserWarning, "no_such_file.py", 1, None)))
show("a module that is given", lambda: (using(("error", None, Warning, "given", 0)), attempt(lambda: warn_explicit("m", UserWarning, "no_such_file.py", 1, "given"))))
for r in (5, "r", [], ()): show("registry %r" % (r,), lambda: warn_explicit("m", UserWarning, "no_such_file.py", 1, registry=r))
show("an instance", lambda: warn_explicit(RuntimeWarning("inst"), UserWarning, "no_such_file.py", 1))
show("a category that is no class", lambda: [attempt(lambda: warn_explicit("m", c, "no_such_file.py", 1)) for c in (5, None, int, "c")])
show("wrong arguments", lambda: [attempt(f) for f in (lambda: warn_explicit(), lambda: warn_explicit("m", UserWarning, "f"), lambda: warn_explicit("m", UserWarning, 5, 1), lambda: warn_explicit("m", UserWarning, "f", "1"), lambda: warn_explicit("m", UserWarning, "f", 1.5), lambda: warn_explicit("m", UserWarning, "f", 1 << 40), lambda: warn_explicit("m", UserWarning, "f", 1, module_globals=5), lambda: warn_explicit("m", UserWarning, "f", 1, x=1))])
class Loader:
    def get_source(s, name): return "first of %s\n   second\nthird" % name
show("a loader has the source", lambda: [warn_explicit("m%d" % n, UserWarning, "no_such_file.py", n, module_globals={"__loader__": Loader(), "__name__": "mod"}) for n in (1, 2, 3)])
show("a loader, and a line that there is not", lambda: warn_explicit("m", UserWarning, "no_such_file.py", 9, module_globals={"__loader__": Loader(), "__name__": "mod"}))
class NoSource:
    def get_source(s, name): return None
show("a loader with no source", lambda: warn_explicit("m", UserWarning, "no_such_file.py", 1, module_globals={"__loader__": NoSource(), "__name__": "mod"}))
show("a loader with no get_source()", lambda: warn_explicit("m", UserWarning, "no_such_file.py", 1, module_globals={"__loader__": 5, "__name__": "mod"}))
show("globals with nothing in them", lambda: warn_explicit("m", UserWarning, "no_such_file.py", 1, module_globals={}))
# ---- the version of the filters
show("the lock", lambda: (attempt(_warnings._filters_mutated_lock_held), attempt(_warnings._release_lock), _warnings._acquire_lock(), _warnings._acquire_lock(), _warnings._filters_mutated_lock_held(), _warnings._release_lock(), _warnings._release_lock(), attempt(_warnings._release_lock)))
def mutated():
    def f(): warn("again")
    f(); f(); _warnings._acquire_lock(); _warnings._filters_mutated_lock_held(); _warnings._release_lock(); f(); f()
show("what has been warned of is forgotten", mutated)
# ---- what is in a context
class Ctx:
    def __init__(s, f): s._filters = f
def in_context(f, g):
    t = _warnings._warnings_context.set(f)
    try: return g()
    finally: _warnings._warnings_context.reset(t)
show("filters of the context", lambda: in_context(Ctx([("error", None, Warning, None, 0)]), lambda: attempt(lambda: warn("m"))))
show("the others are not looked at then", lambda: (using(("error", None, Warning, None, 0)), in_context(Ctx([]), lambda: warn("m"))))
show("_filters is no list", lambda: in_context(Ctx(()), lambda: warn("m")))
show("no _filters", lambda: in_context(5, lambda: warn("m")))
show("a bad one in the context", lambda: in_context(Ctx([5]), lambda: warn("m")))
# ---- what warnings.py has is gone by, if it is there
M = type(sys)
def with_module(**attributes):
    m = M("warnings"); m.__dict__.update(attributes); sys.modules["warnings"] = m
def without_module(f):
    try: return f()
    finally: sys.modules["warnings"] = None
shown = []
class Message:
    def __init__(s, *a): s.a = a
show("_showwarnmsg", lambda: without_module(lambda: (with_module(_showwarnmsg=lambda m: shown.append(m.a), WarningMessage=Message), warn("via python"), [(type(a[0]).__name__, str(a[0]), a[1].__name__, a[2].rpartition("/")[2], a[3] > 0, a[4:]) for a in shown])))
show("_showwarnmsg is not callable", lambda: without_module(lambda: (with_module(_showwarnmsg=5, WarningMessage=Message), warn("m"))))
show("no WarningMessage", lambda: without_module(lambda: (with_module(_showwarnmsg=print), warn("m"))))
show("_showwarnmsg raises", lambda: without_module(lambda: (with_module(_showwarnmsg=lambda m: 1 / 0, WarningMessage=Message), warn("m"))))
show("its filters", lambda: without_module(lambda: (with_module(filters=[("error", None, Warning, None, 0)]), attempt(lambda: warn("m")))))
show("its filters are no list", lambda: without_module(lambda: (with_module(filters=()), warn("m"))))
show("its defaultaction", lambda: without_module(lambda: (with_module(filters=[], defaultaction="error"), attempt(lambda: warn("m")))))
show("its defaultaction is no string", lambda: without_module(lambda: (with_module(filters=[], defaultaction=5), warn("m"))))
def its_onceregistry():
    r = {}; with_module(filters=[("once", None, Warning, None, 0)], onceregistry=r); warn("m"); warn("m"); return sorted((k if isinstance(k, str) else (k[0], k[1].__name__), v) for k, v in r.items())
show("its onceregistry", lambda: without_module(its_onceregistry))
show("its onceregistry is no dict", lambda: without_module(lambda: (with_module(filters=[("once", None, Warning, None, 0)], onceregistry=5), warn("m"))))
filters[:] = original
_warnings._defaultaction
# ---- sys.stderr
def no_stderr():
    global err
    saved = sys.stderr
    try:
        sys.stderr = None; a = warn("to nowhere")
        class Bad:
            def write(s, t): raise KeyError("write")
        sys.stderr = Bad(); b = warn("to what raises")
        return (a, b)
    finally: sys.stderr = saved
show("sys.stderr is None, or raises", no_stderr)
