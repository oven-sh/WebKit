# What an AttributeError says of which attribute it was and of what, which is what is gone by in suggesting another; what __init__() again does to an exception that has attributes; and str() of a
# UnicodeError that has been meddled with.
import types, sys
class A:
    __slots__ = ("s",)
class B: pass
class G:
    def __getattr__(self, n): raise AttributeError("mine")
class M(type): pass
class C(metaclass=M): pass
def show(label, f):
    try:
        f()
        print(label, "nothing raised")
    except AttributeError as e:
        print(label, "|", e, "|", repr(e.name), type(e.obj).__name__ if e.obj is not None else None)
    except BaseException as e:
        print(label, type(e).__name__, e)
a, b = A(), B()
mod = types.ModuleType("m")
for label, f in {
    "b.x": lambda: b.x, "getattr": lambda: getattr(b, "x"), "object.__getattribute__": lambda: object.__getattribute__(b, "x"), "type.__getattribute__": lambda: type.__getattribute__(B, "x"), "B.x": lambda: B.x, "C.x": lambda: C.x,
    "slot": lambda: a.s, "a.x": lambda: a.x, "del b.x": lambda: delattr(b, "x"), "del a.s": lambda: delattr(a, "s"), "a.x = 1": lambda: setattr(a, "x", 1), "object().x = 1": lambda: setattr(object(), "x", 1), "del object().x": lambda: delattr(object(), "x"),
    "object.__setattr__": lambda: object.__setattr__(a, "x", 1), "object.__delattr__": lambda: object.__delattr__(b, "x"), "module": lambda: mod.x, "module getattribute": lambda: types.ModuleType.__getattribute__(mod, "x"), "del module": lambda: delattr(mod, "x"),
    "int": lambda: (1).x, "int set": lambda: setattr(1, "x", 1), "str": lambda: "a".x, "None": lambda: None.x, "function": lambda: show.x, "builtin": lambda: len.x, "method": lambda: b.__init__.x, "super": lambda: super(B, b).x, "own": lambda: G().x,
    "list method": lambda: [].x, "dict": lambda: {}.x, "type set": lambda: setattr(int, "x", 1), "type del": lambda: delattr(B, "x"), "int del": lambda: delattr(int, "real"), "property": lambda: setattr(type("P", (), {"p": property(lambda s: 1)})(), "p", 2),
    "property del": lambda: delattr(type("P", (), {"p": property(lambda s: 1)})(), "p"), "property get": lambda: type("P", (), {"p": property()})().p, "operator.attrgetter": lambda: __import__("operator").attrgetter("x")(b), "generator": lambda: (i for i in ()).x, "frame": lambda: sys._getframe().x, "code": lambda: show.__code__.x,
}.items():
    show(label, f)
print("---- ImportError")
for cls in (ImportError, ModuleNotFoundError):
    e = cls("test", name="name", path="path", name_from="nf")
    print(e.args, e.msg, e.name, e.path, e.name_from)
    e.__init__(); print(e.args, e.msg, e.name, e.path, e.name_from)
    e.__init__("a", "b"); print(e.args, e.msg, e.name, e.path)
    e.__init__("c", name="n"); print(e.args, e.msg, e.name, e.path)
for cls, args in ((AttributeError, dict(name="n", obj=1)), (NameError, dict(name="n")), (StopIteration, {}), (SystemExit, {}), (OSError, {}), (SyntaxError, {}), (KeyError, {}), (UnicodeDecodeError, {})):
    try:
        e = cls("m", **args) if cls is not UnicodeDecodeError else cls("utf-8", b"a", 0, 1, "r")
        before = {k: getattr(e, k, "-") for k in ("args", "name", "obj", "value", "code", "errno", "strerror", "filename", "msg", "lineno", "reason", "start")}
        try: e.__init__()
        except Exception as x: print(cls.__name__, "__init__()", type(x).__name__, x)
        after = {k: getattr(e, k, "-") for k in before}
        print(cls.__name__, before, after)
    except Exception as x: print(cls.__name__, type(x).__name__, x)
print("---- UnicodeError")
def make(cls):
    return cls("utf-8", "abc", 0, 1, "reason") if cls is UnicodeEncodeError else cls("utf-8", b"abc", 0, 1, "reason") if cls is UnicodeDecodeError else cls("abc", 0, 1, "reason")
for cls in (UnicodeEncodeError, UnicodeDecodeError, UnicodeTranslateError):
    for attribute in ("object", "encoding", "reason", "start", "end"):
        if attribute == "encoding" and cls is UnicodeTranslateError: continue
        for value in ("deleted", None, 5, "x", b"x", -1, 100, 2**70):
            e = make(cls)
            try:
                if value == "deleted": delattr(e, attribute)
                else: setattr(e, attribute, value)
            except Exception as x:
                print(cls.__name__, attribute, repr(value), "cannot:", type(x).__name__, x); continue
            try: print(cls.__name__, attribute, repr(value), "str:", str(e))
            except Exception as x: print(cls.__name__, attribute, repr(value), "str raises", type(x).__name__, x)
class Evil(str):
    def __str__(self):
        del exc.object
        return "evil"
class EvilNone(str):
    def __str__(self):
        exc.object = None
        return "evil"
for cls in (UnicodeEncodeError, UnicodeDecodeError, UnicodeTranslateError):
    for evil in (Evil, EvilNone):
        for attribute in ("encoding", "reason"):
            if attribute == "encoding" and cls is UnicodeTranslateError: continue
            exc = make(cls); 
            try:
                setattr(exc, attribute, evil("e"))
                print(cls.__name__, evil.__name__, attribute, str(exc))
            except Exception as x: print(cls.__name__, evil.__name__, attribute, type(x).__name__, x)
print("---- whoever raised it")
class G:
    def __getattr__(self, n): raise AttributeError("mine")
class H:
    def __getattribute__(self, n): raise AttributeError("mine")
class P:
    @property
    def p(self): raise AttributeError("mine")
for label, f in (("G().x", lambda: G().x), ("getattr", lambda: getattr(G(), "x")), ("H().x", lambda: H().x), ("P().p", lambda: P().p), ("G().x()", lambda: G().x()), ("hasattr then", lambda: hasattr(G(), "x") or G().y)):
    try: f()
    except AttributeError as e: print(label, e, repr(e.name), type(e.obj).__name__)
