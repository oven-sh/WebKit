# A cell is the one cell however it is come by.
import types, sys
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
    print(label, "->", r)

def outer():
    a = 1; b = 2
    def f(): return a, b
    def g(): return a
    def h():
        def i(): return a
        return i
    return f, g, h
f, g, h = outer()
print(f.__closure__ is f.__closure__, f.__closure__[0] is f.__closure__[0], f.__closure__[0] is g.__closure__[0], f.__closure__[1] is g.__closure__[0])
print(h.__closure__[0] is f.__closure__[0], h().__closure__[0] is f.__closure__[0], h().__closure__ is h().__closure__)
f2, g2, h2 = outer()
print(f2.__closure__[0] is f.__closure__[0], f2.__closure__[0] == f.__closure__[0], id(f.__closure__[0]) == id(g.__closure__[0]))
c = f.__closure__[0]
c.cell_contents = 10
print(f(), g(), h()())
attempt("attributes", lambda: setattr(c, "x", 1))
d = {id(f.__closure__[0]): "kept"}
print(d.get(id(g.__closure__[0])))

class Meta(type):
    def __new__(cls, name, bases, ns):
        Meta.seen = dict(ns)
        return super().__new__(cls, name, bases, ns)
class W(metaclass=Meta):
    def m(self): return __class__
    def n(self): return super().__init__
print(Meta.seen["__classcell__"] is W.m.__closure__[0], W.m.__closure__[0] is W.n.__closure__[0], W.m.__closure__[0].cell_contents is W)

def gen():
    x = 1
    def inner(): return x
    yield inner
    x = 2
    yield inner
it = gen(); i1 = next(it); c1 = i1.__closure__[0]; i2 = next(it)
print(i1 is i2, c1 is i2.__closure__[0], c1.cell_contents)

def loop():
    fs = []
    for i in range(2):
        def f(): return i
        fs.append(f)
    return fs
a, b = loop()
print(a.__closure__[0] is b.__closure__[0])
def comp(): return [lambda: i for i in range(2)]
a, b = comp()
print(a.__closure__[0] is b.__closure__[0], a(), b())
def params(p):
    def f(): return p
    def g(): return p
    return f, g
a, b = params(1)
print(a.__closure__[0] is b.__closure__[0])

made = types.CellType(5)
fn = types.FunctionType(f.__code__, globals(), "n", None, (made, f.__closure__[1]))
print(fn.__closure__[0] is made, fn.__closure__[1] is f.__closure__[1], fn())
copy = types.FunctionType(f.__code__, globals(), "n", None, f.__closure__)
print(copy.__closure__ is f.__closure__, copy.__closure__[0] is f.__closure__[0], copy())
def frame_cells():
    v = 1
    def inner(): return v
    return inner.__closure__[0] is inner.__closure__[0], sys._getframe().f_locals["v"]
print(frame_cells())
def annotated():
    def one(_) -> C1: pass
    type C1 = None
    return one.__annotate__.__closure__[0] is one.__annotate__.__closure__[0]
print(annotated())
print(sorted(k for k in vars(types.CellType) if not k.startswith("__")), (lambda: 0).__closure__)
