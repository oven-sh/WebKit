log = []
class Animal:
    kind = "animal"
    def __init__(self, name):
        log.append("Animal.__init__")
        self.name = name
    def speak(self): return f"{self.name} says {self.sound()}"
    def sound(self): return "..."
    def __repr__(self): return f"<{type(self).__name__} {self.name}>"
    @classmethod
    def make(cls, name): return cls(name)
    @property
    def loud(self): return self.sound().upper()
def names(c): return [k.__name__ for k in c.__mro__]
def describe(a):
    return [type(a).__name__, names(type(a)), isinstance(a, Animal), a.speak(), repr(a), sorted(vars(a)), a.loud]
def type_of(a): return type(a)
def call(a, name, *args): return getattr(a, name)(*args)
def take_log():
    out = list(log); log.clear(); return out
# py on top of js
def derive(Base):
    class Middle(Base):
        def __init__(self, x):
            log.append("Middle.__init__")
            super().__init__(x)
            self.middle = x
        def who(self): return "middle:" + super().who()
        def only_middle(self): return "only middle " + str(self.base)
    return Middle
def derive_again(Base):
    class Bottom(Base):
        def who(self): return "bottom:" + super().who()
    return Bottom
import builtins
isinstance = builtins.isinstance; issubclass = builtins.issubclass; getattr = builtins.getattr; setattr = builtins.setattr
def make(C, *args): return C(*args)
