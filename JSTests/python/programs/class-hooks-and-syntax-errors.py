def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
class Base: pass
class Mid(Base): pass
class Leaf(Mid): pass
class Other(Base): pass
class Mixin: pass
class Both(Leaf, Mixin): pass
objs = {c.__name__: c() for c in (Base, Mid, Leaf, Other, Both)}
def probe():
    out = []
    for n, o in objs.items():
        try: g = o.missing
        except AttributeError: g = "-"
        try: o.k = 1; s = "set" if "k" in vars(o) else "hooked"
        except AttributeError as e: s = "refused"
        vars(o).pop("k", None)
        out.append((n, g, s))
    return out
for _ in range(200): probe()
show("nothing", probe)
Mid.__getattr__ = lambda self, n: "mid"
show("__getattr__ on Mid", probe)
Base.__getattr__ = lambda self, n: "base"
show("__getattr__ on Base too", probe)
del Mid.__getattr__
show("Mid's deleted", probe)
del Base.__getattr__
show("Base's deleted", probe)
Mixin.__getattr__ = lambda self, n: "mixin"
show("on a second base", probe)
del Mixin.__getattr__
Base.__setattr__ = lambda self, n, v: None
show("__setattr__ on Base", probe)
Leaf.__setattr__ = object.__setattr__
show("Leaf takes object's back", probe)
del Base.__setattr__
del Leaf.__setattr__
show("both deleted", probe)
Base.__getattribute__ = lambda self, n: "all" if n == "missing" else object.__getattribute__(self, n)
show("__getattribute__ on Base", probe)
del Base.__getattribute__
show("deleted", probe)
def refuse(self, n): raise AttributeError("no deleting")
Mid.__delattr__ = refuse
show("__delattr__", lambda: [(n, (setattr(o, "d", 1), (lambda: (delattr(o, "d"), "deleted")[1])() if n in ("Base", "Other") else "kept")[1]) for n, o in objs.items()])
show("__delattr__ refuses", lambda: delattr(objs["Leaf"], "d"))
del Mid.__delattr__
show("and no more", lambda: (delattr(objs["Leaf"], "d"), hasattr(objs["Leaf"], "d")))
setattr(Base, "__getattr__", lambda self, n: "by setattr")
show("by setattr()", probe)
delattr(Base, "__getattr__")
type.__setattr__(Mid, "__getattr__", lambda self, n: "by type.__setattr__")
show("by type.__setattr__", probe)
type.__delattr__(Mid, "__getattr__")
show("a class made afterwards", lambda: (setattr(Base, "__getattr__", lambda self, n: "late"), type("New", (Leaf,), {})().missing, delattr(Base, "__getattr__"))[1])
class Meta(type): pass
class WithMeta(metaclass=Meta): pass
show("metaclass none", lambda: getattr(WithMeta, "missing", "-"))
Meta.__getattr__ = lambda cls, n: "meta"
show("metaclass hook", lambda: (WithMeta.missing, getattr(WithMeta(), "missing", "-")))
del Meta.__getattr__
show("metaclass gone", lambda: getattr(WithMeta, "missing", "-"))

# what is wrong with source, wherever it is found: in taking it apart, in working out its names, or in generating code, at any depth
for src in ['def f(:\n  pass', 'if 1:\nx', 'def f():\n  x = 1\n  global x', 'def f():\n  def g():\n    nonlocal zz', 'class C:\n  return 1', 'def f():\n  def g():\n    break', 'async def f():\n  def g():\n    await x\n', 'def f(a, a): pass', 'def f():\n  for i in range(3):\n    def g():\n      continue', 'def f():\n    from x import *']:
    try:
        compile(src, "<test>", "exec")
        print(repr(src), "compiled")
    except SyntaxError as e:
        print(repr(src), type(e).__name__, e.msg, e.lineno, e.offset, e.end_lineno, e.end_offset, repr(e.text))
