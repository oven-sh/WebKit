# f(*values) can be given any number of them. In this engine what is given to a call goes on the stack, of which there is only so much, unless there is a great deal of it.


def t(label, f):
    try:
        r = f()
    except BaseException as e:
        r = type(e).__name__ + ": " + str(e)[:150]
    print(label, "=>", r)


def brief(values):
    values = list(values)
    return len(values), values[:2], values[-2:]


def rest(*a): return brief(a)
def first_and_rest(x, *a): return x, brief(a)
def with_defaults(x, y=5, *a, z=6): return x, y, z, brief(a)
def with_keywords(*a, **k): return brief(a), brief(k.items())
def keyword_only(*a, z): return z, brief(a)
def two(x, y): return x, y
def none(): return "none"
def positional_only(x, /, *a): return x, brief(a)
def generator(*a): yield brief(a)
async def coroutine(*a): return brief(a)
lam = lambda *a: brief(a)
five = 5
nothing = None


def run(c):
    try:
        c.send(None)
    except StopIteration as e:
        return e.value


class Plain:
    def __init__(self, *a): self.a = brief(a)
    def method(self, *a): return brief(a)
    def fixed(self, x): return x
    def __call__(self, *a): return "called", brief(a)
    @staticmethod
    def static(*a): return brief(a)
    @classmethod
    def of_class(cls, *a): return cls.__name__, brief(a)


class WithNew:
    def __new__(cls, *a):
        self = object.__new__(cls)
        self.a = brief(a)
        return self


class Meta(type):
    def __call__(cls, *a): return "Meta", cls.__name__, brief(a)


class OfMeta(metaclass=Meta):
    pass


class Sink:
    def __init__(self): self.count = 0
    def write(self, text): self.count += 1


class MyTuple(tuple):
    def __new__(cls, *a): return tuple.__new__(cls, a)


for n in (0, 3, 1000, 70000, 300000, 1200000):
    print("----", n)
    l = list(range(n))
    p = Plain()
    for label, f in (("f(*l)", lambda: rest(*l)), ("f(*tuple)", lambda: rest(*tuple(l))), ("f(*range)", lambda: rest(*range(n))), ("f(*generator)", lambda: rest(*(i for i in l))), ("f(-1, *l)", lambda: rest(-1, *l)), ("f(*l, -1)", lambda: rest(*l, -1)), ("f(*l, *l)", lambda: rest(*l, *l)),
                     ("f(x, *a)", lambda: first_and_rest(*l)), ("with defaults", lambda: with_defaults(*l)), ("and a keyword", lambda: with_defaults(*l, z=1)), ("*a, **k", lambda: with_keywords(*l, k=1)), ("keyword only", lambda: keyword_only(*l, z=1)), ("which is missing", lambda: keyword_only(*l)),
                     ("that takes two", lambda: two(*l)), ("that takes none", lambda: none(*l)), ("positional only", lambda: positional_only(*l)), ("a lambda", lambda: lam(*l)), ("a generator", lambda: next(generator(*l))), ("a coroutine", lambda: run(coroutine(*l))),
                     ("a class", lambda: Plain(*l).a), ("with __new__", lambda: WithNew(*l).a), ("with a metaclass", lambda: OfMeta(*l)), ("derived from tuple", lambda: brief(MyTuple(*l))), ("object(*l)", lambda: type(object(*l)).__name__), ("a method", lambda: p.method(*l)), ("that takes one", lambda: p.fixed(*l)),
                     ("from the class", lambda: Plain.method(p, *l)), ("an instance", lambda: p(*l)), ("a static method", lambda: p.static(*l)), ("a class method", lambda: p.of_class(*l)), ("a bound method kept", lambda: (lambda m: m(*l))(p.method)), ("type.__call__", lambda: type.__call__(Plain, *l).a),
                     ("__call__ of a function", lambda: rest.__call__(*l)), ("of a method", lambda: p.method.__call__(*l)), ("list[int](*l)", lambda: list[int](*l)), ("max", lambda: max(*l)), ("min", lambda: min(*l)), ("max with a key", lambda: max(*l, key=lambda x: -x)), ("max with a default", lambda: max(*l, default=1)),
                     ("print", lambda: (lambda s: (print(*l, file=s), s.count)[1])(Sink())), ("print with sep", lambda: (lambda s: (print(*l, sep="", end="", file=s), s.count)[1])(Sink())), ("zip", lambda: brief(zip(*[(i, -i) for i in l]))[0]), ("zip of them", lambda: brief(next(zip(*[(i, -i) for i in l]), ()))),
                     ("map", lambda: brief(map(lambda *a: len(a), *[[i] for i in l]))), ("format", lambda: "{0}{%d}" .replace("%d", str(max(n - 1, 0))).format(*l)), ("len", lambda: len(*l)), ("abs", lambda: abs(*l)), ("int", lambda: int(*l)), ("str", lambda: str(*l)), ("range", lambda: range(*l)), ("slice", lambda: slice(*l)),
                     ("isinstance", lambda: isinstance(*l)), ("dict", lambda: dict(*l)), ("list", lambda: list(*l)), ("tuple", lambda: tuple(*l)), ("set().union", lambda: len(set().union(*[[i] for i in l]))), ("set.intersection", lambda: set(l[:3]).intersection(*[l[:2] for i in l])), ("''.join", lambda: "".join(*l)),
                     ("[].append", lambda: [].append(*l)), ("dict.update", lambda: {}.update(*l)), ("getattr", lambda: getattr(*l)), ("pow", lambda: pow(*l)), ("sum", lambda: sum(*l)), ("sorted", lambda: sorted(*l)), ("type", lambda: type(*l)), ("super", lambda: super(*l)), ("ValueError", lambda: brief(ValueError(*l).args)),
                     ("what is not callable", lambda: five(*l)), ("None", lambda: nothing(*l))):
        t(label, f)

print("---- by name")
for n in (0, 3, 70000, 600000):
    d = {"k%d" % i: i for i in range(n)}
    for label, f in (("f(**d)", lambda: with_keywords(**d)), ("f(*l, **d)", lambda: with_keywords(*range(n), **d)), ("that takes none", lambda: none(**d)), ("that takes two", lambda: two(**d)), ("a class", lambda: type("K", (), {"__init__": lambda s, **k: setattr(s, "k", len(k))})(**d).k),
                     ("a method", lambda: type("K", (), {"m": lambda s, **k: len(k)})().m(**d)), ("dict(**d)", lambda: len(dict(**d))), ("dict.update(**d)", lambda: (lambda x: (x.update(**d), len(x))[1])({})), ("print(**d)", lambda: print(**d)), ("len(**d)", lambda: len(**d)), ("format(**d)", lambda: "{k0}".format(**d)),
                     ("int(**d)", lambda: int(**d)), ("twice", lambda: with_keywords(**d, **d))):
        t("%d %s" % (n, label), f)

print("---- from far down")


def down(depth, values):
    if depth:
        return down(depth - 1, values)
    return rest(*values), max(*values), Plain(*values).a, Plain().method(*values)


def down_with_them(depth, *values):
    return down_with_them(depth - 1, *values) if depth else brief(values)


for depth in (0, 100, 900):
    for n in (10, 5000, 400000):
        t("%d arguments, %d down" % (n, depth), lambda: down(depth, range(n)))
class Down:
    def method(self, depth, *values): return self.method(depth - 1, *values) if depth else brief(values)
    def __call__(self, depth, *values): return self(depth - 1, *values) if depth else brief(values)
    def __init__(self, depth=0, *values): self.a = Down(depth - 1, *values).a if depth else brief(values)
    @classmethod
    def of_class(cls, depth, *values): return cls.of_class(depth - 1, *values) if depth else brief(values)
    @staticmethod
    def static(depth, *values): return Down.static(depth - 1, *values) if depth else brief(values)
    def by_name(self, depth, *values, k): return self.by_name(depth - 1, *values, k=k) if depth else (k, brief(values))


for depth, n in ((10, 400000), (200, 20000), (900, 3000), (900, 100)):
    t("%d arguments at each of %d" % (n, depth), lambda: down_with_them(depth, *range(n)))
    t("to a method", lambda: Down().method(depth, *range(n)))
    t("to an instance", lambda: Down()(depth, *range(n)))
    t("to a class method", lambda: Down.of_class(depth, *range(n)))
    t("to a static method", lambda: Down.static(depth, *range(n)))
    t("and one by name", lambda: Down().by_name(depth, *range(n), k=7))
    # Two frames at each.
    t("to a class", lambda: Down(depth // 3, *range(n)).a)

print("---- what never ends")


class Itself:
    pass


Itself.__call__ = Itself()
# What CPython says of it has in it how much stack there was.
def kind(f):
    try:
        return f()
    except RecursionError as e:
        return type(e).__name__


t("__call__ that is an instance", lambda: kind(lambda: Itself()(*range(3))))
t("with a great many", lambda: kind(lambda: Itself()(*range(300000))))


def forever(*values): return forever(*values)
t("a function", lambda: forever(*range(5000)))
