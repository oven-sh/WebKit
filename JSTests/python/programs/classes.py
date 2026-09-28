class Animal:
    kind = "animal"
    count = 0
    def __init__(self, name):
        self.name = name
        Animal.count += 1
    def speak(self): return f"{self.name} makes a sound"
    def __repr__(self): return f"{type(self).__name__}({self.name!r})"
    def __eq__(self, o): return isinstance(o, Animal) and self.name == o.name
    def __hash__(self): return hash(self.name)
class Dog(Animal):
    kind = "dog"
    def __init__(self, name, tricks=()):
        super().__init__(name)
        self.tricks = list(tricks)
    def speak(self): return super().speak() + ": woof"
a, d = Animal("cat"), Dog("rex", ["sit"])
print(a, d, a.speak(), d.speak(), a.kind, d.kind, Dog.kind, Animal.count, d.count, d.tricks, isinstance(d, Animal), isinstance(a, Dog), issubclass(Dog, Animal), type(d) is Dog, type(d).__name__)
print(a == Animal("cat"), a != d, a in [Animal("cat")], {a: 1}[Animal("cat")], Dog.__mro__, Dog.__bases__, Dog.__name__, d.__class__ is Dog, isinstance(d, (int, Animal)), isinstance(1, int), isinstance(True, int), isinstance(1.0, int))
m = d.speak; print(m(), Animal.speak(d), Dog.speak(d), m.__self__ is d, callable(m))
d.extra = 5; print(d.extra, hasattr(d, "extra"), hasattr(d, "nope"), getattr(d, "nope", "dflt"), getattr(d, "name"), sorted(d.__dict__)); del d.extra; print(hasattr(d, "extra")); setattr(d, "z", 1); print(d.z)
d.none = None; print(d.none, hasattr(d, "none"))
d.kind = "mine"; print(d.kind, Dog.kind); Animal.kind = "changed"; print(a.kind, Dog("x").kind); Animal.new_method = lambda self: "added " + self.name; print(a.new_method(), d.new_method())
class P:
    def __init__(self): self._v = 1
    @property
    def v(self): return self._v
    @v.setter
    def v(self, x): self._v = x * 2
    @property
    def ro(self): return "ro"
    @staticmethod
    def s(x): return x + 1
    @classmethod
    def c(cls, x): return (cls.__name__, x)
class Q(P): pass
p = P(); p.v = 5; print(p.v, p.ro, P.s(1), p.s(2), P.c(3), p.c(4), Q.c(5), Q().c(6), Q().v)
class V:
    def __init__(self, *c): self.c = list(c)
    def __add__(s, o): return V(*[a + b for a, b in zip(s.c, o.c)]) if isinstance(o, V) else NotImplemented
    def __radd__(s, o): return V(*[o + a for a in s.c])
    def __mul__(s, k): return V(*[a * k for a in s.c])
    def __rmul__(s, k): return s * k
    def __neg__(s): return V(*[-a for a in s.c])
    def __sub__(s, o): return s + -o
    def __len__(s): return len(s.c)
    def __getitem__(s, i): return s.c[i]
    def __setitem__(s, i, v): s.c[i] = v
    def __contains__(s, x): return x in s.c
    def __iter__(s): return iter(s.c)
    def __bool__(s): return any(s.c)
    def __lt__(s, o): return sum(s.c) < sum(o.c)
    def __call__(s, x): return [a * x for a in s.c]
    def __str__(s): return "V" + str(tuple(s.c))
    def __repr__(s): return "V(" + ", ".join(map(repr, s.c)) + ")"
v, w = V(1, 2), V(3, 4)
print(v + w, v - w, v * 3, 3 * v, -v, 1 + v, len(v), v[0], v[-1], 2 in v, 9 in v, list(v), bool(v), bool(V(0, 0)), v < w, w > v, sorted([w, v]), v(10), str(v), repr(v), f"{v} {v!r}", [v], max(v, w), sum([v, w], V(0, 0)))
v[0] = 9; x, y = v; print(v, x, y, [i * 2 for i in v], "yes" if V(0) else "no", not V(0))
class A:
    def who(self): return "A"
    def chain(self): return ["A"]
class B(A):
    def who(self): return "B"
    def chain(self): return ["B"] + super().chain()
class C(A):
    def who(self): return "C"
    def chain(self): return ["C"] + super().chain()
class D(B, C):
    def chain(self): return ["D"] + super().chain()
print(D().who(), D().chain(), [c.__name__ for c in D.__mro__], isinstance(D(), C), issubclass(D, (B, int)))
class Ctx:
    def __init__(self, swallow): self.swallow = swallow
    def __enter__(self): print("enter"); return "res"
    def __exit__(self, t, e, tb): print("exit", t.__name__ if t else None, e); return self.swallow
with Ctx(False) as r: print("body", r)
with Ctx(True): raise ValueError("swallowed")
try:
    with Ctx(False): raise KeyError("k")
except KeyError as e: print("propagated", e)
class G:
    def __getattr__(self, n): return "dyn_" + n
print(G().anything, G().x)
class Slots:
    x = 1
    y = x + 1
    def f(self): return self.y
    z = [i for i in range(3)]
print(Slots.y, Slots().f(), Slots.z)
class It:
    def __init__(self, n): self.i, self.n = 0, n
    def __iter__(self): return self
    def __next__(self):
        if self.i >= self.n: raise StopIteration
        self.i += 1
        return self.i
print(list(It(3)), sum(It(4)), [x for x in It(2)], next(It(1)), next(iter([]), "empty"), max(It(3)), 2 in It(3), dict(zip(It(2), "ab")))
for bad in (lambda: a.nope, lambda: Animal.nope, lambda: Animal(), lambda: a + 1, lambda: a < a, lambda: len(a), lambda: a[0], lambda: a(), lambda: iter(a), lambda: setattr(p, "ro", 1), lambda: v + "s"):
    try: bad()
    except Exception as e: print(type(e).__name__, e)
