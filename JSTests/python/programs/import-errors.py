import sys
def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
def failing(f):
    try: f()
    except ImportError as e: return (type(e).__name__, str(e), e.name, e.name_from, e.path, e.msg, e.args)
M = type(sys)
class Spec:
    def __init__(s, **k): s.__dict__.update(k)
def module(name, **attributes):
    m = M(name); m.__dict__.update(attributes); sys.modules[name] = m; return m
def from_sys():
    from sys import versio
show("a built-in module", lambda: failing(from_sys))
module("plain")
def f():
    from plain import nothing
show("with a spec of None", lambda: failing(f))
del sys.modules["plain"].__spec__
show("with no spec", lambda: failing(f))
module("filed", __file__="/some/where.py")
def f():
    from filed import nothing
show("__file__, and a spec of None", lambda: failing(f))
module("located", __spec__=Spec(has_location=True, origin="/from/the/spec.py"), __file__="/some/where.py")
def f():
    from located import nothing
show("an origin", lambda: failing(f))
module("unlocated", __spec__=Spec(has_location=False, origin="/from/the/spec.py"), __file__="/some/where.py")
def f():
    from unlocated import nothing
show("an origin that is no location", lambda: failing(f))
module("nowhere", __spec__=Spec(has_location=False, origin="built-in"))
def f():
    from nowhere import nothing
show("nowhere", lambda: failing(f))
module("odd", __spec__=Spec(has_location=True, origin=5), __file__=6)
def f():
    from odd import nothing
show("an origin that is no string", lambda: failing(f))
module("starting", __spec__=Spec(has_location=True, origin="/a/b.py", _initializing=True))
def f():
    from starting import nothing
show("partially initialized", lambda: failing(f))
module("starting2", __spec__=Spec(_initializing=1))
def f():
    from starting2 import nothing
show("partially initialized, nowhere", lambda: failing(f))
module("empty_spec", __spec__=Spec())
def f():
    from empty_spec import nothing
show("a spec with nothing in it", lambda: failing(f))
m = module("renamed"); m.__name__ = 5
def f():
    from renamed import nothing
show("a name that is no string", lambda: failing(f))
m = module("nameless"); del m.__name__
def f():
    from nameless import nothing
show("no name", lambda: failing(f))
class NotModule: __spec__ = Spec(has_location=False); __name__ = "thing"; __file__ = "/not/looked/at.py"
sys.modules["notmodule"] = NotModule()
def f():
    from notmodule import nothing
show("what is no module", lambda: failing(f))
module("pkg"); module("pkg.sub")
def f():
    from pkg import sub
    return sub.__name__
show("found in sys.modules", f)
class Raises:
    __name__ = "raises"
    @property
    def __spec__(s): raise RuntimeError("no spec")
sys.modules["raises"] = Raises()
def f():
    from raises import nothing
show("getting the spec raises", f)
def f():
    import nothing_of_this_name
show("no such module", lambda: failing(f))
def f():
    import sys.nothing
show("no such module in what is no package", lambda: failing(f))
def f():
    from nothing_of_this_name import a
show("from no such module", lambda: failing(f))
show("made by a program", lambda: [(e.name, e.path, e.name_from, e.msg, e.args, str(e)) for e in (ImportError(), ImportError("m"), ImportError("m", name="n"), ImportError("m", name="n", path="p", name_from="f"), ImportError("a", "b"))])
