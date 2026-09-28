import js, sys
seen = {}
class Base:
    def __new__(cls, *a):
        seen["__new__ cls"] = cls
        return super().__new__(cls)
    def __init_subclass__(cls):
        seen["__init_subclass__ cls"] = cls
    @classmethod
    def cm(cls): return cls
    def me(self): return __class__
    def sup(self): return super()
class Named:
    def __set_name__(self, owner, name): seen["__set_name__ owner"] = owner
class BaseError(Exception): pass
named = Named()
C = js.eval("(function (B, n) { class C extends B { static s() { return this; } m() { return 1; } } return C; })")(Base, named)
E = js.eval("(function (B) { return class E extends B {}; })")(BaseError)
class P(C):
    def mine(self): return __class__
known = [Base, C, P, E, BaseError, Exception, BaseException, object, type, js.Object, js.Map, js.Function]
def which(v):
    for k in known:
        if v is k:
            return k.__name__
    return "LEAK " + repr(v)
def show(label, f):
    try:
        r = f()
        print(label, "=>", [which(v) for v in r] if isinstance(r, (list, tuple)) else which(r))
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
d = C(); p = P()
show("type()", lambda: [type(d), type(p), type(C), type(js.Map.new()), type(js.Map)])
show("__class__", lambda: [d.__class__, p.__class__, C.__class__, js.Map.new().__class__])
show("__mro__", lambda: C.__mro__ + P.__mro__ + js.Map.__mro__)
show("mro()", lambda: C.mro() + P.mro())
show("type.mro", lambda: type.mro(C))
show("__bases__", lambda: C.__bases__ + P.__bases__ + js.Map.__bases__)
show("__base__", lambda: [C.__base__, P.__base__, js.Map.__base__, js.Object.__base__])
show("__subclasses__", lambda: Base.__subclasses__() + C.__subclasses__() + type.__subclasses__(C))
show("seen", lambda: [seen[k] for k in sorted(seen)])
show("classmethod", lambda: [C.cm(), d.cm(), P.cm(), p.cm(), C.cm.__self__, d.cm.__self__])
show("static this", lambda: [C.s(), P.s()])
show("__class__ cell", lambda: [d.me(), p.me(), p.mine()])
show("super()", lambda: [d.sup().__thisclass__, d.sup().__self_class__, p.sup().__self_class__, super(C, d).__thisclass__, super(C, d).__self_class__, super(P, p).__thisclass__, super(C, P).__self_class__])
show("__objclass__", lambda: [C.__init__.__objclass__, js.Map.__init__.__objclass__])
show("__new__.__self__", lambda: [C.__new__.__self__, js.Map.__new__.__self__])
def exc():
    try:
        raise E("x")
    except BaseError as e:
        return [type(e), e.__class__, sys.exc_info()[0]]
show("exceptions", exc)
def exc2():
    try:
        raise E
    except E as e:
        return [type(e)]
show("raise the class", exc2)
show("constructor, which is JavaScript's way of asking", lambda: [js.eval("o => o.constructor")(d), js.eval("o => o.constructor")(p), js.eval("o => o.constructor")(Base())])
show("type(name, bases, ns)", lambda: type("T", (C,), {}).__mro__[1:])
show("__class__ assignment", lambda: (lambda o: (setattr(o, "__class__", C), type(o))[1])(P()))
show("and back", lambda: (lambda o: (setattr(o, "__class__", P), type(o))[1])(C()))
def plain(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
plain("names", lambda: (C.__name__, C.__qualname__, C.__module__, C.__doc__, repr(C), str(C), repr(js.Map)))
plain("equal and hash", lambda: (C == C, C != P, hash(C) == hash(C), {C: 1}[type(d)], C in {C, P}, type(d) == C))
plain("isinstance", lambda: (isinstance(d, C), isinstance(d, (int, C)), isinstance(p, C), isinstance(d, P), issubclass(P, (int, C)), isinstance(C, type), type.__instancecheck__(C, d), type.__subclasscheck__(C, P)))
plain("dir", lambda: [n for n in dir(C) if not n.startswith("__")])
plain("dir of instance", lambda: [n for n in dir(d) if not n.startswith("__")])
plain("vars", lambda: sorted(k for k in vars(C) if not k.startswith("__")))
plain("__dict__", lambda: (type(C.__dict__).__name__, "m" in C.__dict__, "s" in C.__dict__))
plain("callable", lambda: (callable(C), callable(d)))
plain("flags", lambda: (C.__basicsize__, C.__dictoffset__, C.__flags__ == P.__flags__))
