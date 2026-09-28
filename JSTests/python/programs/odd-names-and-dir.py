import sys
def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
class C: pass
o = C()
show("set and get", lambda: (setattr(o, "0", "zero"), setattr(o, "a", 1), setattr(o, "10", "ten"), setattr(o, "b", 2), getattr(o, "0"), getattr(o, "10"), hasattr(o, "0"), hasattr(o, "1"))[4:])
show("in the dict", lambda: (sorted(vars(o).items()), o.__dict__["0"], "10" in o.__dict__, len(o.__dict__), sorted(o.__dict__), sorted(dir(o))[:2]))
show("through the dict", lambda: (o.__dict__.__setitem__("7", "seven"), getattr(o, "7"), o.__dict__.pop("7"), hasattr(o, "7"))[1:])
show("delete", lambda: (delattr(o, "0"), hasattr(o, "0"), sorted(vars(o))))
show("delete missing", lambda: delattr(o, "0"))
show("odd names", lambda: [(setattr(o, n, n), getattr(o, n))[1] for n in ("00", "01", "-1", "1.5", "4294967295", "4294967294", "99999999999999999999", "", " ", "1e3", "٣")])
show("class", lambda: (setattr(C, "5", "five"), getattr(C, "5"), getattr(o, "5"), "5" in vars(C), vars(C)["5"], delattr(C, "5"), hasattr(C, "5"))[1:])
show("type()", lambda: (t := type("T", (), {"3": "three", "x": 1}), getattr(t, "3"), getattr(t(), "3"), "3" in vars(t))[1:])
show("module", lambda: (m := sys.modules[__name__], setattr(m, "2", "two"), getattr(m, "2"), globals()["2"], "2" in globals(), globals().__setitem__("4", "four"), getattr(m, "4"), delattr(m, "2"), "2" in globals())[2:])
show("function", lambda: (setattr(show, "1", "one"), getattr(show, "1"), show.__dict__)[1:])
show("exec", lambda: (d := {"1": "one"}, exec("x = 1", d), d["1"], d["x"], sorted(k for k in d if k != "__builtins__"))[2:])
show("exception", lambda: (e := ValueError(), setattr(e, "0", 1), getattr(e, "0"), vars(e))[2:])
show("set dict", lambda: (p := C(), setattr(p, "__dict__", {"0": "z", "k": 1}), getattr(p, "0"), p.k, sorted(vars(p)))[2:])
show("keywords", lambda: (lambda **k: sorted(k.items()))(**{"0": 1, "a": 2}))
class Base:
    b = 1
class Mixin:
    m = 2
class Derived(Base, Mixin):
    d = 3
    def __init__(self): self.i = 4
pub = lambda names: [n for n in names if not n.startswith("__")]
show("dir instance", lambda: pub(dir(Derived())))
show("__dir__ unsorted", lambda: pub(Derived().__dir__()))
show("dir class", lambda: (pub(dir(Derived)), pub(type.__dir__(Derived))))
show("dir with slots", lambda: pub(dir(type("S", (), {"__slots__": ("z", "y")})())))
class Own:
    def __dir__(self): return ["q", "p"]
show("own __dir__", lambda: dir(Own()))
class Fake:
    __class__ = property(lambda self: Derived)
show("__class__ that lies", lambda: pub(dir(Fake())))
M = type(sys)
m = M("m"); m.x = 1; m.a = 2
show("dir module", lambda: (pub(dir(m)), pub(m.__dir__())))
m.__dir__ = lambda: ["only", "these"]
show("module __dir__", lambda: dir(m))
show("dir of builtins", lambda: (pub(dir(1))[:4], pub(dir([]))[:4], pub(dir(show))))
show("dir no arguments", lambda: (lambda zz, aa: dir())(1, 2))
show("dir bad", lambda: dir(type("B", (), {"__dir__": lambda s: 1})()))
