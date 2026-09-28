def f(a, b=2, *, c=3):
    "the doc"
    return (a, b, c)
f.custom = 1
def g(): pass
class C:
    "class doc"
    def m(self, x): return ("m", x)
    @staticmethod
    def s(): return "s"
    @classmethod
    def k(cls): return cls.__name__
    attr = 5
class M(type):
    def __repr__(cls): return "<M class " + cls.__name__ + ">"
class D(metaclass=M): pass
lam = lambda x: x
def gen(): yield 1
async def co(): return 1
c = C()
builtin = len
bound_builtin = [].append
def attrs(fn): return sorted(fn.__dict__.items())
def doc(fn): return fn.__doc__
def name(fn): return fn.__name__
