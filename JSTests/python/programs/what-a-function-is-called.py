# What a function is called, and its docstring, is the same object however often it is asked for, and a generator goes by the name of the function that made it.
import functools
def f(): pass
class C:
    def m(self): pass
    @staticmethod
    def s(): pass
    @classmethod
    def c(cls): pass
lam = lambda: 0
def gen(): yield
async def co(): pass
w = functools.wraps(f)(lambda: 0)
print(w.__name__ is f.__name__, w.__qualname__ is f.__qualname__, w.__module__ is f.__module__, w.__doc__ is f.__doc__)
f.__name__ = "g"; print(f.__name__, f.__qualname__, f.__name__ is f.__name__)
f.__qualname__ = "h"; print(f.__name__, f.__qualname__, f.__qualname__ is f.__qualname__)
g = gen(); print(g.__name__ is g.__name__, g.__qualname__ is g.__qualname__, g.__name__ is gen.__name__)
c = co(); print(c.__name__ is c.__name__, c.__qualname__ is c.__qualname__); c.close()
print(f.__code__.co_name is f.__code__.co_name, f.__code__.co_qualname is f.__code__.co_qualname, f.__code__.co_filename is f.__code__.co_filename)
for x in (f, C.m, C.s, C.__dict__["s"], C.__dict__["c"], lam, gen, co, C, functools.wraps(f)(lambda: 0)):
    print(getattr(x, "__qualname__", None), x.__name__ is x.__name__, x.__qualname__ is x.__qualname__)


def documented():
    "This is a test"


print(documented.__doc__ is documented.__doc__, functools.wraps(documented)(lambda: 0).__doc__ is documented.__doc__)


def renamed():
    yield 1


g = renamed()
print(g.__name__, g.__qualname__)
g.__name__ = "name"
g.__qualname__ = "qualname"
print(g.__name__, g.__qualname__, repr(g).split(" at ")[0])
renamed.__qualname__ = "func_qualname"
renamed.__name__ = "func_name"
g = renamed()
print(g.__name__, g.__qualname__, renamed.__code__.co_name, (x for x in ()).__name__, (x for x in ()).__qualname__)


async def renamedCoroutine():
    pass


renamedCoroutine.__name__ = "other"
c = renamedCoroutine()
print(c.__name__, c.__qualname__)
c.close()
