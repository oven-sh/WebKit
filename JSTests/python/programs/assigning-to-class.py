import sys
def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
class A: pass
class B: pass
class A2(A): pass
class S1: __slots__ = ("a",)
class S2: __slots__ = ("a",)
class S3: __slots__ = ("b",)
class S4: __slots__ = ("a", "b")
class S0: __slots__ = ()
class S00: __slots__ = ()
class SD: __slots__ = ("__dict__",)
class SW: __slots__ = ("__weakref__",)
class SDW: __slots__ = ("__dict__", "__weakref__")
class S1sub(S1): __slots__ = ()
class S1dict(S1): pass
class L1(list): pass
class L2(list): pass
class D1(dict): pass
class I1(int): pass
class I2(int): pass
class E1(Exception): pass
class E2(ValueError): pass
class O1(OSError): pass
class M1(type(sys)): pass
class M2(type(sys)): pass
class Meta(type): pass
class WithMeta(metaclass=Meta): pass
classes = [A, B, A2, S1, S2, S3, S4, S0, S00, SD, SW, SDW, S1sub, S1dict, L1, L2, D1, I1, I2, E1, E2, O1]
def make(c): return c()
for old in classes:
    row = []
    for new in classes:
        o = make(old)
        try:
            o.__class__ = new
            row.append("+" if type(o) is new else "?")
        except TypeError:
            row.append(".")
    print(old.__name__.ljust(7), "".join(row))
show("message", lambda: setattr(A(), "__class__", S1))
show("built in", lambda: setattr(A(), "__class__", object))
show("from built in", lambda: setattr(object(), "__class__", A))
show("int", lambda: setattr(1, "__class__", I1))
show("not a class", lambda: setattr(A(), "__class__", 1))
show("delete", lambda: delattr(A(), "__class__"))
show("modules", lambda: (m := type(sys)("m"), setattr(m, "__class__", M1), type(m).__name__, setattr(m, "__class__", M2), type(m).__name__, setattr(m, "__class__", type(sys)), type(m).__name__)[2::2])
show("module to other", lambda: setattr(type(sys)("m"), "__class__", A))
show("keeps attributes", lambda: (o := A(), setattr(o, "x", 1), setattr(o, "__class__", B), o.x, vars(o), isinstance(o, B), isinstance(o, A))[3:])
show("keeps slots", lambda: (o := S1(), setattr(o, "a", 5), setattr(o, "__class__", S2), o.a, type(o).__name__)[3:])
show("class of a class", lambda: setattr(A, "__class__", Meta))
show("metaclass swap", lambda: (setattr(WithMeta, "__class__", type("Meta2", (type,), {})), type(WithMeta).__name__)[1])
