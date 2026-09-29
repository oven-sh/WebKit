# `import` of a module that is JavaScript's.
import importlib
import importlib.util
import inspect
import os
import sys
import types

import js

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "importing_javascript")
sys.path.insert(0, HERE)


def t(label, f):
    try:
        result = repr(f())
    except BaseException as e:
        result = type(e).__name__ + ": " + str(e)
    print(label, "=>", result.replace(HERE, "HERE"))


def run(source, **names):
    given = dict(names)
    exec(source, names)
    return sorted((k, repr(v)) for k, v in names.items() if k != "__builtins__" and k not in given)


print("---- one of them")
t("who is asked", lambda: [f.__name__ for f in sys.meta_path])
import plain
t("what it is", lambda: (plain, type(plain), inspect.ismodule(plain)))
t("what importing says of it", lambda: (plain.__name__, plain.__file__, plain.__package__, plain.__loader__, hasattr(plain, "__path__"), hasattr(plain, "__cached__")))
t("its spec", lambda: (plain.__spec__.name, plain.__spec__.origin, plain.__spec__.has_location, plain.__spec__.parent, plain.__spec__.submodule_search_locations, plain.__spec__.cached, plain.__spec__.loader is plain.__loader__))
t("what it exports", lambda: (plain.count, plain.bump(), plain.count, plain.default("you"), plain.Thing(3).x))
t("it is the one that JavaScript has", lambda: (plain.isMe(plain), sys.modules["plain"] is plain))
t("from", lambda: run("from plain import count, bump as b, default"))
t("all of it", lambda: run("from plain import *"))
t("what it does not export", lambda: run("from plain import nothing"))
t("nor has", lambda: plain.nothing)
t("below it", lambda: run("import plain.below"))
t("it is run once", lambda: (run("import plain\nimport plain as again\nfrom plain import count"), js.runs))
t("forgotten by Python, it is not by JavaScript", lambda: (sys.modules.pop("plain") is plain, importlib.import_module("plain") is plain, js.runs))
t("reloaded", lambda: (importlib.reload(plain) is plain, js.runs))
t("dir", lambda: dir(plain))
t("what JavaScript sees of that", lambda: (list(js.Reflect.ownKeys(plain))[:-1], js.Object.isSealed(plain)))

print("---- which")
t("what is Python's comes first", lambda: __import__("both").language)
t(".mjs before .js", lambda: __import__("either").suffix)
t("neither", lambda: __import__("there_is_nothing_by_this_name"))
# There is a readline.mjs, and a _tkinter.mjs. Neither module is to be had here, and both names are the standard library's.
t("a name that is the standard library's", lambda: [(name in sys.stdlib_module_names, os.path.exists(os.path.join(HERE, name + ".mjs")), importlib.util.find_spec(name)) for name in ("readline", "_tkinter")])
t("is not found", lambda: __import__("readline"))
t("nor below it", lambda: sys.meta_path[-1].find_spec("readline.below", [HERE]))
t("find_spec", lambda: [s and s.origin for s in (importlib.util.find_spec(n) for n in ("plain", "either", "both", "there_is_nothing_by_this_name", "waits"))])
t("finding runs nothing", lambda: "waits" in sys.modules)
t("what is not a str on the path", lambda: (sys.path.insert(0, 5), sys.path.insert(0, b"bytes"), __import__("chain_four").made, sys.path.pop(0), sys.path.pop(0)))

print("---- in a package, and in a directory")
import package
t("as the package is imported", lambda: (package.beside, package.beside.where, package.found_beside, package.beside.__name__, package.beside.__package__, package.beside.__spec__.parent))
t("later", lambda: (hasattr(package, "later"), run("from package import later"), package.later.where, run("import package.later as l"), run("from package.later import where")))
t("a directory", lambda: run("import directory.leaf\nfrom directory import other\nwhere = (directory.leaf.where, other.where, directory.__path__._path if hasattr(directory.__path__, '_path') else None)"))
t("not there", lambda: run("import package.nothing"))

print("---- what goes wrong")
for name in ("throws", "throws", "throws_what_is_no_error", "throws_what_is_no_error", "throws_a_number", "wrong", "waits", "waits", "imports_what_waits", "imports_what_is_missing", "imports_what_is_not_exported"):
    t(name, lambda: __import__(name))
    t("    and it is not left behind", lambda: name in sys.modules)
def path_of_error():
    try:
        import waits
    except ImportError as e:
        return (e.path or "").replace("file://", ""), e.name
t("what it says besides", path_of_error)
t("what is waited for is there already", lambda: __import__("waits_for_what_is_ready").x)
t("or will be as soon as anything else is run", lambda: __import__("waits_a_turn").x)
# JavaScript can wait.
arrived = []
js.eval("(name) => import(name)")(HERE + "/waits.mjs").then(arrived.append, arrived.append)
js.drainMicrotasks()
t("while JavaScript is waiting for it", lambda: (arrived, __import__("waits")))
js.release("what it was waiting for")
js.drainMicrotasks()
t("once it has", lambda: (arrived, __import__("waits") is arrived[0], __import__("waits").x, __import__("imports_what_waits").y))

print("---- round and round")
import cycle_first
t("what the one of Python's saw", lambda: sys.modules["cycle_second"].first_when_this_was_run)
t("what the one of JavaScript's saw", lambda: cycle_first.secondWhenThisWasRun)
t("and afterwards", lambda: (sys.modules["cycle_second"].after, sys.modules["cycle_second"].cycle_first is cycle_first))
import chain_one
t("each as it is", lambda: [type(x).__name__ for x in chain_one.all()])
t("and the same from either end", lambda: [a is b for a, b in zip(chain_one.all(), [chain_one.made, sys.modules["chain_two"].made, sys.modules["chain_three"].made, sys.modules["chain_four"].made])])

print("---- without it")
importer = sys.meta_path.pop()
t("nothing new", lambda: run("import either_again"))
t("what has been imported has been", lambda: run("import plain"))
sys.modules.pop("either")
t("what has been forgotten is not found", lambda: run("import either"))
sys.meta_path.append(importer)
t("with it again", lambda: run("import either"))

print("---- what else asks about a module")
t("inspect", lambda: (inspect.getfile(plain), inspect.getmodulename(plain.__file__), inspect.getdoc(plain), plain.__doc__))
t("its source", lambda: inspect.getsource(plain).splitlines()[0])
t("the importer", lambda: (importer, importer.find_spec("plain").origin, importer.find_spec("plain", None, None).name, importer.find_spec("nothing"), importer.find_spec("beside", [HERE + "/package"]).origin, importer.find_spec("beside", []), importer.exec_module(plain)))
