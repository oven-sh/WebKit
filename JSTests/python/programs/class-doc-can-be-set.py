# C.__doc__ = ..., which is for type's own descriptor to see to.
import sys
def attempt(f):
    try: return f()
    except Exception as e: return type(e).__name__ + ": " + str(e)
class A: pass
class B: "doc"
class M(type): pass
class C(metaclass=M): pass
class T(tuple): pass
D = type("D", (tuple,), {"__doc__": "d", "__slots__": ()})
E = type("E", (), {})
for c in (A, B, C, T, D, E, int, type, M):
    print(c.__name__, c.__doc__ if c not in (int, type) else "...", attempt(lambda: setattr(c, "__doc__", "new")), c.__doc__ if c not in (int, type) else "...", "__doc__" in vars(c), attempt(lambda: delattr(c, "__doc__")), (c.__doc__ or "")[:10] if c not in (int, type) else "...")
print(type(type.__dict__["__doc__"]).__name__, attempt(lambda: type.__dict__["__doc__"].__set__(A, "via the descriptor")), A.__doc__)


class Descriptor:
    def __get__(self, instance, owner): return "from __get__ of %s, %s" % (type(instance).__name__, owner.__name__)


for value in (None, 5, ["a"], Descriptor(), property(lambda self: "p")):
    A.__doc__ = value
    print(type(value).__name__, A.__doc__ if not isinstance(value, property) else type(A.__doc__).__name__, type(vars(A)["__doc__"]).__name__, A().__doc__)


class Sub(A): pass
print(Sub.__doc__, "__doc__" in vars(Sub))
told = []
listening = [True]
sys.addaudithook(lambda event, args: told.append((event, args[0].__name__, args[1], args[2])) if listening[0] and event == "object.__setattr__" else None)
A.__doc__ = "told"
A.__name__ = "Renamed"
A.__qualname__ = "Q"
attempt(lambda: setattr(int, "__doc__", "x"))
attempt(lambda: delattr(A, "__doc__"))
listening[0] = False
print(told)
