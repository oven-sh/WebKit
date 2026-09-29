# import: the statement, __import__(), and what importlib makes of them.
import _warnings
import builtins
import posix
import sys

sys.dont_write_bytecode = True
D = posix.environ.get(b"TMPDIR", b"/tmp").decode().rstrip("/") + "/jsc-python-import-" + str(posix.getpid()) + "/"
TAG = sys.implementation.cache_tag
made = []


def mask(text):
    return text.replace(D, "D/").replace(D[:-1], "D").replace(TAG, "TAG").replace(__file__, "main.py")


def write(name, text=""):
    parts = name.split("/")
    for i in range(1, len(parts)):
        directory = D + "/".join(parts[:i])
        if directory not in made:
            posix.mkdir(directory)
            made.append(directory)
    fd = posix.open(D + name, posix.O_WRONLY | posix.O_CREAT | posix.O_TRUNC, 0o644)
    posix.write(fd, text.encode())
    posix.close(fd)
    if D + name not in made:
        made.append(D + name)


def frames(e):
    out, tb = [], e.__traceback__
    while tb:
        out.append((tb.tb_frame.f_code.co_filename, tb.tb_frame.f_code.co_name, tb.tb_lineno if tb.tb_frame.f_code.co_filename != __file__ else 0))
        tb = tb.tb_next
    return out


def show(e):
    more = ""
    if isinstance(e, ImportError):
        more = " | name=%r path=%r name_from=%r" % (e.name, e.path, getattr(e, "name_from", None))
    return type(e).__name__ + ": " + str(e) + more


def attempt(f, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return show(e)


def t(label, f):
    r = attempt(f)
    print(mask(label), "=>", mask(r if isinstance(r, str) else ascii(r)))


def run(source, **names):
    "Runs some statements as a module of no name would, and gives what they bound."
    space = {"__name__": "runner", "__builtins__": builtins}
    space.update(names)
    exec(source, space)
    return {k: (v.__name__ if isinstance(v, type(sys)) else v) for k, v in space.items() if k not in names and k not in ("__name__", "__builtins__")}


def forget(*prefixes):
    for name in list(sys.modules):
        if name.split(".")[0] in prefixes:
            del sys.modules[name]


def warnings_as_errors(category):
    _warnings._acquire_lock()
    _warnings.filters.insert(0, ("error", None, category, None, 0))
    _warnings._filters_mutated_lock_held()
    _warnings._release_lock()


posix.mkdir(D)
sys.path.insert(0, D[:-1])
log = []
builtins.log = log

print("---- what does the importing")
t("sys.meta_path", lambda: [f.__name__ for f in sys.meta_path if getattr(f, "__module__", "").startswith("_frozen_importlib")])
t("sys.path_hooks", lambda: [getattr(h, "__qualname__", h) for h in sys.path_hooks if getattr(h, "__name__", "") != "zipimporter"])
t("what is written in C", lambda: [(name, name in sys.builtin_module_names) for name in ("sys", "builtins", "_imp", "marshal", "_io", "_thread", "_warnings", "_weakref", "posix", "os", "nothing")])
t("it is a tuple, in order", lambda: (type(sys.builtin_module_names).__name__, list(sys.builtin_module_names) == sorted(sys.builtin_module_names)))
for name in ("sys", "builtins", "_imp", "marshal"):
    t(name, lambda: [(repr(m), m.__name__, m.__package__, m.__loader__.__name__, m.__spec__.name, m.__spec__.origin, m.__spec__.parent, m.__spec__.has_location, m.__spec__.cached, m.__spec__.submodule_search_locations, hasattr(m, "__file__"), hasattr(m, "__path__")) for m in [__import__(name)]])

print("---- modules in files")
write("plain.py", "log.append(('plain', __name__, __package__, __file__, __cached__, __spec__.name, type(__loader__).__name__, __builtins__ is __import__('builtins').__dict__))\nvalue = 1\n_hidden = 2\n")
t("import plain", lambda: (run("import plain"), log[-1]))
t("it is run once", lambda: (len(log), run("import plain"), run("import plain as p"), len(log)))
t("what it has", lambda: [(repr(m), sorted(vars(m)), m.__spec__.origin, m.__spec__.cached, m.__spec__.has_location, m.__spec__.parent, m.__spec__.submodule_search_locations, repr(m.__spec__).replace(repr(m.__loader__), "LOADER"), m.__loader__.name, m.__loader__.path) for m in [sys.modules["plain"]]])
t("from", lambda: (run("from plain import value"), run("from plain import value as v, _hidden"), run("from plain import *")))
t("what is not there", lambda: (attempt(run, "from plain import nothing"), attempt(run, "import plain.nothing"), attempt(run, "import nothing_at_all"), attempt(run, "import nothing_at_all.more"), attempt(run, "from nothing_at_all import x"), attempt(run, "import plain.value")))
t("an attribute that is not there", lambda: sys.modules["plain"].nothing)

print("---- packages")
write("pkg/__init__.py", "log.append(('pkg', __name__, __package__, __path__, __file__, __spec__.submodule_search_locations))\n")
write("pkg/one.py", "log.append(('pkg.one', __name__, __package__))\nname = 'one'\n")
write("pkg/two.py", "from . import one\nfrom .one import name\nfrom .sub import deep\nfrom .sub.deep import up, upper, same\n")
write("pkg/sub/__init__.py", "log.append(('pkg.sub', __name__, __package__, __path__))\n")
write("pkg/sub/deep.py", "from .. import one as up\nfrom ..one import name as upper\nfrom . import __name__ as same\n")
del log[:]
t("import pkg", lambda: (run("import pkg"), log, [n for n in sorted(sys.modules) if n.startswith("pkg")]))
del log[:]
t("import pkg.one", lambda: (run("import pkg.one"), log, sys.modules["pkg"].one is sys.modules["pkg.one"]))
t("import pkg.sub.deep", lambda: (run("import pkg.sub.deep"), [n for n in sorted(sys.modules) if n.startswith("pkg")]))
t("as", lambda: (run("import pkg.one as a"), run("import pkg.sub.deep as b"), run("import pkg.sub as c"), run("import pkg as d")))
t("from a package", lambda: (run("from pkg import one"), run("from pkg import sub"), run("from pkg.sub import deep"), run("from pkg import two"), run("from pkg import one as x, two as y")))
t("relative", lambda: {k: getattr(v, "__name__", v) for k, v in vars(sys.modules["pkg.two"]).items() if not k.startswith("__")})
t("what a package has", lambda: [(repr(m), m.__package__, m.__path__, m.__spec__.parent, m.__spec__.submodule_search_locations, m.__spec__.cached) for m in [sys.modules["pkg"], sys.modules["pkg.sub"], sys.modules["pkg.sub.deep"]]])
t("of a package, what is not there", lambda: (attempt(run, "from pkg import nothing"), attempt(run, "import pkg.nothing"), attempt(run, "import pkg.one.nothing"), attempt(run, "from pkg.nothing import x"), attempt(run, "import pkg.sub.nothing.more")))
t("as, of what is not an attribute", lambda: [(delattr(sys.modules["pkg"], "one"), run("import pkg.one as a"), attempt(lambda: sys.modules["pkg"].one), sys.modules.pop("pkg.one") and None, attempt(run, "import pkg.one as a"), hasattr(sys.modules["pkg"], "one"))])

print("---- from where")
inside = {"__name__": "pkg.sub.deep", "__package__": "pkg.sub"}
for source in ("from . import deep", "from .deep import up", "from .. import one", "from ..one import name", "from ... import x", "from .... import x", "from .nothing import x", "from . import nothing", "from .. import sub", "from ..sub import deep", "from ..sub.deep import same"):
    t(source, lambda: run(source, **inside))
for label, names in (("no package", {}), ("__package__ = ''", {"__package__": ""}), ("__package__ = None", {"__package__": None}), ("__package__ = 5", {"__package__": 5}), ("a name with a dot", {"__name__": "pkg.one"}), ("a name with a dot, and a path", {"__name__": "pkg.sub", "__path__": []}), ("a name that is not a str", {"__name__": 5}),
                     ("a package that is not imported", {"__package__": "absent"}), ("a package that is not one", {"__package__": "plain"})):
    t(label, lambda: run("from . import one", **names))


class Spec:
    def __init__(self, parent): self.parent = parent


for label, names in (("a spec", {"__spec__": Spec("pkg")}), ("a spec and no package", {"__spec__": Spec("pkg"), "__package__": None}), ("a spec whose parent is not a str", {"__spec__": Spec(5)}), ("a spec that has no parent", {"__spec__": object()}), ("a spec that agrees", {"__spec__": Spec("pkg"), "__package__": "pkg"}), ("__spec__ = None", {"__spec__": None, "__package__": "pkg"})):
    t(label, lambda: run("from . import one", **names))
seen = []
_warnings._acquire_lock()
_warnings.filters.insert(0, ("always", None, Warning, None, 0))
_warnings._filters_mutated_lock_held()
_warnings._release_lock()
sys.modules["warnings"] = type(sys)("warnings")
sys.modules["warnings"]._showwarnmsg = lambda m: seen.append((m.category.__name__, str(m.message), m.lineno))
sys.modules["warnings"].filters = _warnings.filters
sys.modules["warnings"].defaultaction = "default"
sys.modules["warnings"].onceregistry = {}
t("a spec that disagrees", lambda: (run("from . import one", __spec__=Spec("pkg.sub"), __package__="pkg"), seen))
del seen[:]
t("neither", lambda: (run("from . import one", __name__="pkg.two"), seen))
del sys.modules["warnings"]
_warnings._acquire_lock()
del _warnings.filters[0]
_warnings._filters_mutated_lock_held()
_warnings._release_lock()

print("---- from module import *")
write("stars.py", "a = 1\n_b = 2\n__c__ = 3\ndef f(): pass\nimport sys\n")
write("stars_all.py", "__all__ = ['a', '_b', 'later']\na = 1\n_b = 2\nc = 3\ndef __getattr__(name):\n    if name == 'later': return 'from __getattr__'\n    raise AttributeError(name)\n")
t("all that does not begin with _", lambda: sorted(run("from stars import *")))
t("__all__", lambda: run("from stars_all import *"))
for label, value in (("a tuple", "('a',)"), ("[]", "[]"), ("a str", "'ab'"), ("what is not there", "['a', 'zz']"), ("an int in it", "['a', 5]"), ("bytes in it", "[b'a']"), ("an int", "5"), ("None", "None"), ("a dict", "{'a': 1}"), ("a set", "{'a'}"), ("a generator", "(x for x in 'a')"), ("derived from str", "[type('S', (str,), {})('a')]")):
    write("stars_odd.py", "a = 1\nb = 2\n__all__ = " + value + "\n")
    forget("stars_odd")
    t("__all__ = " + label, lambda: run("from stars_odd import *"))
write("stars_pkg/__init__.py", "__all__ = ['inner', 'x']\nx = 1\n")
write("stars_pkg/inner.py", "")
t("of a package, whose modules are imported for it", lambda: run("from stars_pkg import *"))


class Odd:
    pass


t("of what has neither", lambda: [(sys.modules.__setitem__("odd", 5), attempt(run, "from odd import *"), sys.modules.__setitem__("odd", Odd()), setattr(sys.modules["odd"], "x", 1), setattr(sys.modules["odd"], "_y", 1), run("from odd import *"), vars(sys.modules["odd"]).__setitem__(5, 1), attempt(run, "from odd import *"), setattr(sys.modules["odd"], "__name__", "odd"),
                                   attempt(run, "from odd import *"), setattr(sys.modules["odd"], "__name__", 7), attempt(run, "from odd import *"))][0][1::2])
t("into the locals", lambda: [(exec("from stars import *", {"__builtins__": builtins}, space), sorted(space)) for space in [{}]])


class Mapping:
    def __init__(self): self.set = []
    def __setitem__(self, k, v): self.set.append(k)
    def __getitem__(self, k): raise KeyError(k)


t("which can be any mapping", lambda: [(exec("from stars_all import *\nimport plain\nfrom plain import value as v", {"__builtins__": builtins}, space), space.set) for space in [Mapping()]])

print("---- round in a circle")
write("circle_a.py", "import circle_b\nx = 1\n")
write("circle_b.py", "import circle_a\ntry:\n    circle_a.x\nexcept AttributeError as e:\n    log.append(str(e))\ntry:\n    from circle_a import x\nexcept ImportError as e:\n    log.append((str(e), e.name, e.path, e.name_from))\n")
del log[:]
t("what has not been run to its end", lambda: (run("import circle_a"), log))
write("ring/__init__.py", "from . import first\n")
write("ring/first.py", "from . import second\nvalue = 'first'\n")
write("ring/second.py", "from . import first\nimport ring.first as again\nlog.append((first.__name__, again is first, hasattr(__import__('ring'), 'first')))\ntry:\n    import ring\n    ring.first\nexcept AttributeError as e:\n    log.append(str(e))\n")
del log[:]
t("a module of a package that is not yet an attribute of it", lambda: (run("import ring"), log))

print("---- modules that fail")
write("fails.py", "log.append('fails is run')\nx = 1\nraise ValueError('from the module')\n")
write("fails_import.py", "import nothing_here_either\n")
write("fails_syntax.py", "x = 1\ny = = 2\n")
write("fails_deep.py", "import fails\n")
write("fails_from.py", "from plain import nothing\n")
write("fails_exit.py", "raise SystemExit(3)\n")
write("failing_pkg/__init__.py", "from . import good\nfrom . import bad\n")
write("failing_pkg/good.py", "")
write("failing_pkg/bad.py", "1 / 0\n")
for name in ("fails", "fails_import", "fails_syntax", "fails_deep", "fails_from", "fails_exit", "failing_pkg"):
    def go():
        try:
            __import__(name)
        except BaseException as e:
            return show(e), frames(e), [n for n in sorted(sys.modules) if n.startswith("fail")]
    t("import " + name, go)
del log[:]
t("it is run again each time", lambda: (attempt(run, "import fails"), attempt(run, "import fails"), log))

def _catch(name):
    try:
        __import__(name)
    except BaseException as e:
        return e


t("SyntaxError", lambda: [(e.filename, e.lineno, e.offset, e.text, e.msg) for e in [_catch("fails_syntax")]])
write("replaces.py", "import sys\nsys.modules[__name__] = 'something else'\n")
write("removes.py", "import sys\ndel sys.modules[__name__]\n")
t("one that puts something else in its place", lambda: (run("import replaces"), sys.modules["replaces"]))
t("one that takes itself out", lambda: [(show(e), frames(e)) for e in [_catch("removes")]])

print("---- sys.modules")
t("None", lambda: (sys.modules.__setitem__("halted", None), attempt(run, "import halted"), attempt(run, "from halted import x"), attempt(run, "import halted.more"), sys.modules.pop("halted")))
t("anything at all", lambda: (sys.modules.__setitem__("anything", 5), run("import anything"), attempt(run, "from anything import real"), attempt(run, "from anything import nothing"), attempt(run, "import anything.more"), sys.modules.pop("anything")))
t("taken out, it is run again", lambda: (log.clear(), sys.modules.pop("plain") and None, run("import plain"), len(log)))
t("a package taken out, and not its modules", lambda: (sys.modules.pop("pkg") and None, log.clear(), run("import pkg.one"), [x[0] for x in log], hasattr(sys.modules["pkg"], "one")))
t("sys.modules is another dict", lambda: [(setattr(sys, "modules", dict(old)), sys.modules.pop("plain") and None, log.clear(), run("import plain"), len(log), "plain" in sys.modules, setattr(sys, "modules", old)) for old in [sys.modules]])
t("a module by a name that it has not", lambda: (sys.modules.__setitem__("alias", sys.modules["plain"]), run("import alias"), run("from alias import value"), sys.modules.pop("alias") and None))

print("---- __import__()")
for args in (("plain",), ("pkg.one",), ("pkg.sub.deep",), ("pkg.one", None, None, ("name",)), ("pkg.one", None, None, ["name"]), ("pkg", None, None, ("one", "two")), ("pkg", None, None, ("nothing",)), ("pkg", None, None, ("*",)), ("pkg.one", None, None, ()), ("pkg.one", None, None, None), ("pkg.one", None, None, 0), ("pkg.one", None, None, 1),
             ("pkg.one", None, None, "x"), ("pkg", None, None, (5,)), ("pkg", None, None, (b"one",)), ("plain", None, None, (5,)), ("pkg.one", {}, {}, (), 0), ("one", {"__package__": "pkg"}, None, (), 1), ("", {"__package__": "pkg"}, None, (), 1), ("", {"__package__": "pkg.sub"}, None, (), 2), ("sub.deep", {"__package__": "pkg"}, None, (), 1),
             ("sub.deep", {"__package__": "pkg"}, None, ("up",), 1), ("one", {"__package__": "pkg.sub"}, None, (), 2), ("one", {"__package__": "pkg.sub"}, None, (), 3), ("one", None, None, (), 1), ("one", 5, None, (), 1), ("one", [], None, (), 1), ("one", {}, None, (), 1), ("",), ("", None, None, (), 0), ("plain", None, None, (), -1), ("plain", None, None, (), True),
             ("plain", None, None, (), 1.5), ("plain", None, None, (), "1"), ("plain", None, None, (), 2 ** 40), ("plain", None, None, (), 2 ** 70), (5,), (None,), (b"plain",), (), ("plain", 1, 2, 3, 0, 5), (".",), ("plain.",), (".plain",), ("pkg..one",), ("pkg.",), ("a\0b",), ("\ud800",), (" plain",)):
    t("__import__%r" % (args,), lambda: (lambda m: getattr(m, "__name__", m))(__import__(*args)))
t("by name", lambda: (__import__(name="pkg.one", globals=None, locals=None, fromlist=("name",), level=0).__name__, attempt(lambda: __import__("plain", other=1))))


class Name(str):
    pass


t("a name of a class derived from str", lambda: (__import__(Name("plain")).__name__, __import__(Name("pkg.one")).__name__, __import__("pkg", None, None, (Name("one"),)).__name__))

print("---- another __import__")
original = builtins.__import__
calls = []


def recording(name, globals=None, locals=None, fromlist=(), level=0):
    calls.append((name, None if globals is None else globals.get("__name__"), "the globals" if locals is globals else locals if locals is None else type(locals).__name__, fromlist, level))
    return original(name, globals, locals, fromlist, level)


builtins.__import__ = recording
SOURCE = """
import plain
import pkg.one
import pkg.sub.deep as d
from pkg import one, two as t
from pkg.one import *
def f():
    import plain
    from pkg import one
    import pkg.one as o
f()
class C:
    import plain
    from pkg import one
"""
t("what it is given", lambda: (sorted(run(SOURCE)), calls))
del calls[:]
t("from within a package", lambda: (run("from . import one\nfrom .one import name\nfrom .. import pkg", __package__="pkg.sub", __name__="pkg.sub.x") and None, calls))
del calls[:]
t("with locals of its own", lambda: (exec("import plain", {"__name__": "g", "__builtins__": builtins}, {}), calls))
builtins.__import__ = lambda *a: "whatever it returns"
t("what it returns is what is bound", lambda: (run("import plain"), run("import pkg.one"), attempt(run, "import pkg.one as x"), sorted(run("from plain import upper")), attempt(run, "from plain import nothing")))
builtins.__import__ = 5
t("one that cannot be called", lambda: run("import plain"))
del builtins.__import__
t("none", lambda: (attempt(run, "import plain"), attempt(run, "from plain import value")))
builtins.__import__ = original
t("the builtins of the globals", lambda: [(exec("import plain\nfrom pkg import one", {"__builtins__": {"__import__": lambda *a: seen.append(a[0]) or sys}}), seen) for seen in [[]]])
t("which have none", lambda: exec("import plain", {"__builtins__": {}}))

print("---- finders and loaders")


class Finder:
    def __init__(self, names): self.names, self.asked = names, []

    def find_spec(self, name, path, target=None):
        self.asked.append((name, path, target))
        if name in self.names:
            return type(sys.__spec__)(name, self, origin="from the finder", is_package=self.names[name])

    def create_module(self, spec):
        self.asked.append(("create", spec.name))

    def exec_module(self, module):
        self.asked.append(("exec", module.__name__))
        module.made_by = "the loader"


finder = Finder({"found": False, "found_pkg": True, "found_pkg.inner": False})
sys.meta_path.insert(0, finder)
t("a finder", lambda: (run("import found"), finder.asked, [(repr(m), m.made_by, m.__loader__ is finder, m.__spec__.origin, m.__package__, hasattr(m, "__file__"), hasattr(m, "__path__")) for m in [sys.modules["found"]]]))
del finder.asked[:]
t("of a package", lambda: (run("import found_pkg.inner"), finder.asked, sys.modules["found_pkg"].__path__, sys.modules["found_pkg.inner"].__package__))
del finder.asked[:]
t("what it does not find goes on to the next", lambda: (forget("plain"), run("import plain"), finder.asked))
sys.meta_path.remove(finder)
for label, value in (("has no find_spec", object()), ("raises", type("F", (), {"find_spec": lambda self, *a: 1 / 0})()), ("raises ImportError", type("F", (), {"find_spec": lambda self, *a: (_ for _ in ()).throw(ImportError("from the finder"))})()), ("returns something else", type("F", (), {"find_spec": lambda self, *a: 5})())):
    sys.meta_path.insert(0, value)
    t("a finder that " + label, lambda: [(show(e), frames(e)) for e in [_catch("sought")]])
    sys.meta_path.pop(0)
saved = sys.meta_path[:]
del sys.meta_path[:]
seen = []
t("no finders", lambda: attempt(run, "import sought"))
sys.meta_path[:] = saved
sys.meta_path, saved = None, sys.meta_path
t("sys.meta_path = None", lambda: attempt(run, "import sought"))
sys.meta_path = saved
for label, members in (("has no exec_module", {}), ("creates something else", {"create_module": lambda self, spec: Odd(), "exec_module": lambda self, m: setattr(m, "x", 1)}), ("raises in create_module", {"create_module": lambda self, spec: 1 / 0, "exec_module": lambda self, m: None}), ("raises in exec_module", {"create_module": lambda self, spec: None, "exec_module": lambda self, m: 1 / 0}),
                       ("has exec_module and not create_module", {"exec_module": lambda self, m: None})):
    loader = type("L", (), members)()
    one = type("F", (), {"find_spec": lambda self, name, path, target=None: type(sys.__spec__)(name, loader) if name == "loaded" else None})()
    sys.meta_path.insert(0, one)
    forget("loaded")
    t("a loader that " + label, lambda: [(type(r).__name__, sorted(k for k in vars(r) if k != "__builtins__")) if not isinstance(r, BaseException) else (show(r), frames(r), "loaded" in sys.modules) for r in [_catch("loaded") or sys.modules["loaded"]]])
    sys.meta_path.pop(0)

print("---- sys.path")
write("elsewhere/only_there.py", "where = 'elsewhere'\n")
write("elsewhere/plain.py", "where = 'elsewhere'\n")
t("not on it", lambda: attempt(run, "import only_there"))
sys.path.append(D + "elsewhere")
t("on it", lambda: (run("import only_there"), sys.modules["only_there"].__file__))
t("the first that has it", lambda: (forget("plain"), run("import plain"), sys.modules["plain"].__file__))
t("what has been asked of each", lambda: sorted((k, type(v).__name__) for k, v in sys.path_importer_cache.items() if k.startswith(D[:-1])))
for entry in (5, None, b"bytes", D + "absent", D + "plain.py", ""):
    sys.path.insert(0, entry)
    forget("plain")
    t("with %r on it" % (entry,), lambda: (run("import plain"), sys.modules["plain"].__file__, type(sys.path_importer_cache.get(entry, "not there")).__name__))
    sys.path.pop(0)
hooked = []


def hook(path):
    hooked.append(path)
    if path != "a hook's":
        raise ImportError
    return type("PF", (), {"find_spec": lambda self, name, target=None: hooked.append(name)})()


sys.path_hooks.insert(0, hook)
sys.path.insert(0, "a hook's")
sys.path_importer_cache.clear()
t("a hook", lambda: (forget("plain"), run("import plain"), [h for h in hooked if h == "a hook's" or h.startswith(D[:-1]) or h == "plain"]))
sys.path_hooks.pop(0)
sys.path.pop(0)
sys.path_importer_cache.clear()
write("late.py", "")
t("a file that is written afterwards", lambda: run("import late"))
saved, sys.path = sys.path, [D[:-1]]
t("sys.path is another list", lambda: (forget("plain"), run("import plain")))
sys.path = ()
t("or nothing", lambda: (forget("plain"), attempt(run, "import plain")))
sys.path = None
t("or None", lambda: (forget("plain"), attempt(run, "import plain")))
sys.path = saved

print("---- packages that are in no one place")
write("part1/spread/a.py", "")
write("part2/spread/b.py", "")
sys.path[:0] = [D + "part1", D + "part2"]
t("a directory with no __init__.py", lambda: (run("import spread.a, spread.b"), [(repr(m), type(m.__path__).__name__, list(m.__path__), m.__spec__.origin, m.__spec__.loader.__class__.__name__, hasattr(m, "__file__"), m.__spec__.submodule_search_locations is m.__path__) for m in [sys.modules["spread"]]]))
write("part3/spread/c.py", "")
sys.path.insert(0, D + "part3")
t("it is looked for again when sys.path is not what it was", lambda: (run("import spread.c"), list(sys.modules["spread"].__path__)))
del sys.path[:3]
write("both/__init__.py", "")
write("both.py", "")
t("a package before a module of the same name", lambda: (run("import both"), sys.modules["both"].__file__))
t("__path__ that a program set", lambda: (forget("pkg"), run("import pkg"), setattr(sys.modules["pkg"], "__path__", [D + "elsewhere"]), run("import pkg.only_there"), sys.modules["pkg.only_there"].__file__, attempt(run, "import pkg.one")))
forget("pkg")

print("---- who is told")
told = []
listening = [True]
sys.addaudithook(lambda event, args: told.append((args[0], args[1], args[2] is sys.path, args[3] is sys.meta_path, args[4] is sys.path_hooks)) if listening[0] and event == "import" else None)
t("import", lambda: (forget("pkg", "plain"), run("import plain\nimport plain\nimport pkg.sub.deep\nimport marshal"), attempt(run, "import nothing_at_all"), told))
listening[0] = False

del builtins.log
for path in reversed(made):
    (posix.unlink if not posix.stat(path).st_mode & 0o40000 else posix.rmdir)(path)
posix.rmdir(D)
