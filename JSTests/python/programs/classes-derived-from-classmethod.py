# Classes derived from classmethod, staticmethod and property, which abc.abstractclassmethod and the like are; those that have been made and not initialized; and a metaclass that is derived from something besides type.
def attempt(f, *a):
    try: return f(*a)
    except BaseException as e: return type(e).__name__ + ": " + str(e)[:80]
class CM(classmethod):
    pass
class SM(staticmethod):
    pass
class PR(property):
    pass
class C:
    @CM
    def foo(cls): return "C.foo of " + cls.__name__
    @SM
    def bar(): return "C.bar"
    @PR
    def baz(self): return "C.baz"
print(attempt(lambda: C.foo()), attempt(lambda: C().foo()), attempt(lambda: C.bar()), attempt(lambda: C().bar()), attempt(lambda: C().baz), attempt(lambda: type(C.baz).__name__))
print(attempt(getattr, C, "foo", None) is not None, type(C.__dict__["foo"]).__name__, attempt(lambda: C.__dict__["foo"].__get__(None, C)()), attempt(lambda: C.__dict__["foo"].__func__(int)))
class D(C):
    @classmethod
    def foo(cls): return "D then " + super().foo()
    @staticmethod
    def bar(): return "D then " + super(D, D).bar()
print(attempt(lambda: D.foo()), attempt(lambda: D().foo()), attempt(lambda: D.bar()))
import abc
class E(metaclass=abc.ABCMeta):
    @abc.abstractclassmethod
    def foo(cls): return cls.__name__
    @abc.abstractstaticmethod
    def bar(): return "bar"
    @abc.abstractproperty
    def baz(self): return "baz"
print(sorted(E.__abstractmethods__), attempt(E), attempt(lambda: E.foo()), attempt(lambda: E.bar()))
class WithGet(classmethod):
    def __get__(self, obj, cls=None):
        return "mine, then " + super().__get__(obj, cls)()
class G:
    @WithGet
    def foo(cls): return cls.__name__
print(attempt(lambda: G.foo), attempt(lambda: G().foo))
class A(metaclass=abc.ABCMeta):
    @abc.abstractmethod
    def x(self): pass
print(attempt(lambda: type("meta", (type, A), {"x": lambda self: 1})), attempt(lambda: type("m2", (type,), {})("K", (), {}).__mro__))
class M(type):
    def mro(cls): return [cls, object]
class N(M): pass
print(attempt(lambda: N("K", (int,), {}).__mro__), attempt(lambda: type("MM", (M, type), {}).__mro__), attempt(lambda: type.mro(int)), attempt(lambda: int.mro()), attempt(lambda: type.mro()))
raw_cm, raw_sm = classmethod.__new__(classmethod), staticmethod.__new__(staticmethod)
class H:
    a = raw_cm
    b = raw_sm
    c = CM.__new__(CM)
    d = SM.__new__(SM)
print([attempt(getattr, x, n) for x in (H, H()) for n in "abcd"])
print([attempt(o.__get__, *a) for o in (raw_cm, raw_sm, classmethod(len), staticmethod(len), CM(len), SM(len)) for a in ((), (None,), (None, None), (1,), (1, None), (None, int), (1, int), (1, 5), (None, 5), (1, int, 2))])
print(attempt(raw_sm), attempt(repr, raw_cm).split(" at ")[0], attempt(repr, raw_sm).split(" at ")[0], attempt(getattr, raw_cm, "__func__"), attempt(getattr, raw_sm, "__wrapped__"), attempt(getattr, raw_cm, "__isabstractmethod__"))
