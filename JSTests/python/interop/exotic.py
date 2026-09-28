log = []

class Plain:
    def __init__(self, x):
        self.x = x
    kind = "plain"
    def method(self):
        return ("method", self.x)
    @classmethod
    def make(cls, x):
        return cls(x)
    @staticmethod
    def helper(a):
        return a + 1

class Temperature:
    def __init__(self, c):
        self.__dict__["celsius"] = c
    @property
    def celsius(self):
        log.append("get")
        return self.__dict__["celsius"]
    @celsius.setter
    def celsius(self, value):
        log.append("set")
        if value < -273:
            raise ValueError("too cold")
        self.__dict__["celsius"] = value
    @property
    def fahrenheit(self):
        return self.celsius * 9 / 5 + 32
    @property
    def gone(self):
        raise AttributeError("gone")

class Frozen:
    def __init__(self, x):
        object.__setattr__(self, "x", x)
    def __setattr__(self, name, value):
        raise AttributeError(f"cannot assign to field {name!r}")
    def __delattr__(self, name):
        raise AttributeError(f"cannot delete field {name!r}")

class Slotted:
    __slots__ = ("a",)
    def __init__(self):
        self.a = 1

class Dynamic:
    def __getattr__(self, name):
        if name.startswith("dyn_"):
            return name[4:]
        raise AttributeError(name)

class Spy:
    def __init__(self):
        object.__setattr__(self, "real", 1)
    def __getattribute__(self, name):
        log.append(("spy", name))
        if name == "hidden":
            raise AttributeError(name)
        return object.__getattribute__(self, name)

class A:
    def who(self): return "A"
class B(A):
    def who(self): return "B" + super().who()
class C(A):
    def who(self): return "C" + super().who()
    only_c = "c"
class D(B, C):
    pass

class Meta(type):
    def __instancecheck__(cls, obj):
        return obj == 42
class Odd(metaclass=Meta):
    pass

class AttrDict(dict):
    def __getattr__(self, name):
        try:
            return self[name]
        except KeyError:
            raise AttributeError(name)
    def __setattr__(self, name, value):
        self[name] = value

def add_property(cls, name):
    setattr(cls, name, property(lambda self: "from property", lambda self, v: log.append(("prop set", v))))
def add_setattr(cls):
    cls.__setattr__ = lambda self, n, v: log.append(("late setattr", n, v))
def add_getattribute(cls):
    cls.__getattribute__ = lambda self, n: "intercepted"
def take_log():
    out = list(log)
    log.clear()
    return out
def attrs(o):
    return sorted(vars(o).items())
class Late1(Plain): pass
class Late2(Plain): pass
class Late3(Plain): pass
class LateBase(Plain): pass
class LateDerived(LateBase): pass

# The same holds whatever kind of thing the class is derived from.
def guarded(base):
    class Guarded(base):
        def __init__(self, *args):
            object.__setattr__(self, "tag", "own")
        def __setattr__(self, name, value):
            log.append(("guarded set", name))
        @property
        def prop(self):
            return "computed"
    Guarded.__name__ = "Guarded" + base.__name__
    return Guarded
guards = [guarded(b) for b in (list, dict, set, tuple, int, str, float, bytes, bytearray, frozenset)]
def make_guards():
    return [g() for g in guards]
