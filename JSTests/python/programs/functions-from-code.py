import sys
def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
def attempt(f):
    try: return f()
    except BaseException as e: return type(e).__name__ + ": " + str(e)
Function = type(show); Cell = type((lambda x: lambda: x)(1).__closure__[0])
# ---- cells
show("cell", lambda: (Cell(1).cell_contents, Cell("a").cell_contents, Cell(None).cell_contents, attempt(lambda: Cell().cell_contents), Cell.__name__))
show("cell set", lambda: (lambda c: (setattr(c, "cell_contents", 5), c.cell_contents, delattr(c, "cell_contents"), attempt(lambda: c.cell_contents), setattr(c, "cell_contents", 6), c.cell_contents))(Cell()))
show("cell compare", lambda: (Cell(1) == Cell(1), Cell(1) < Cell(2), Cell() == Cell(), Cell() < Cell(1), Cell(1) != Cell()))
show("cell repr", lambda: (repr(Cell()).split(":")[1], repr(Cell(1)).split(":")[1].split(" at ")[0]))
show("cell two", lambda: Cell(1, 2))
show("cell keyword", lambda: Cell(contents=1))
show("cell derived", lambda: type("X", (Cell,), {}))
# ---- functions from code
def plain(a, b=2): return (a, b, n)
n = "the module's n"
g = Function(plain.__code__, {"n": "another n"})
show("other globals", lambda: (g(1, 5), attempt(lambda: g(1)), plain(1), g.__name__, g.__qualname__, g.__module__, g.__defaults__, g.__globals__["n"], g.__code__ is plain.__code__, g is not plain, type(g).__name__))
show("all arguments", lambda: (lambda h: (h(), h(1), h.__name__, h.__qualname__, h.__defaults__, h.__kwdefaults__))(Function(plain.__code__, globals(), "renamed", (7, 8))))
def kw(*, k): return k
show("kwdefaults", lambda: (Function(kw.__code__, {}, None, None, None, {"k": 9})(), Function(kw.__code__, {}, kwdefaults={"k": 3})()))
show("module name", lambda: (Function(plain.__code__, {"__name__": "somewhere"}).__module__, Function(plain.__code__, {}).__module__))
def documented():
    "the doc"
show("doc", lambda: (Function(documented.__code__, {}).__doc__, Function(plain.__code__, {}).__doc__))
show("builtins", lambda: (Function((lambda: len("ab")).__code__, {})(), attempt(lambda: Function((lambda: len("ab")).__code__, {"__builtins__": {}})()), Function((lambda: len("ab")).__code__, {"__builtins__": {"len": lambda x: "mine"}})()))
show("globals are shared", lambda: (lambda d: (Function((lambda: x).__code__, d), d.__setitem__("x", 1), Function((lambda: x).__code__, d)())[2])({}))
def sets_global():
    global made
    made = 1
show("globals are written", lambda: (lambda d: (Function(sets_global.__code__, d)(), sorted(k for k in d if k != "__builtins__")))({}))
# ---- closures
def outer(x, y):
    def inner(): return (x, y)
    return inner
inner = outer(1, 2)
show("free variables", lambda: (inner.__code__.co_freevars, [c.cell_contents for c in inner.__closure__]))
show("with cells", lambda: Function(inner.__code__, {}, None, None, (Cell("X"), Cell("Y")))())
show("with its own cells", lambda: Function(inner.__code__, globals(), None, None, inner.__closure__)())
show("cells are kept", lambda: (lambda a, b: (lambda f: (f.__closure__[0] is a, f.__closure__[1] is b, f.__closure__ == (a, b)))(Function(inner.__code__, {}, None, None, (a, b))))(Cell(1), Cell(2)))
def shared():
    a = Cell(1); f = Function(inner.__code__, {}, None, None, (a, Cell(0))); h = Function(inner.__code__, {}, None, None, (Cell(0), a))
    r = [f(), h()]; a.cell_contents = 5; r += [f(), h()]; return r
show("a cell in two functions, under two names", shared)
def counter():
    n = 0
    def bump():
        nonlocal n
        n += 1
        return n
    return bump
bump = counter()
def rebump():
    c = Cell(10); f = Function(bump.__code__, {}, None, None, (c,)); return (f(), f(), c.cell_contents, bump(), bump())
show("nonlocal", rebump)
def aliased():
    b = counter(); f = Function(b.__code__, {}, None, None, b.__closure__); return (b(), f(), b(), f(), b.__closure__[0].cell_contents)
show("the same variable as another function's", aliased)
show("empty cell", lambda: Function(inner.__code__, {}, None, None, (Cell(), Cell(1)))())
def deleter():
    v = 1
    def d():
        nonlocal v
        del v
    def r(): return v
    return d, r
def deleting():
    d, r = deleter(); c = Cell(5); f = Function(d.__code__, {}, None, None, (c,)); f(); return (attempt(lambda: c.cell_contents), attempt(f), r())
show("del", deleting)
def deep(x):
    def middle():
        def innermost(): return x
        return innermost
    return middle
show("passed on to what is defined in it", lambda: (lambda c: (lambda m: (m()(), c.__setattr__("cell_contents", "changed"), m()(), m().__closure__[0] is c))(Function(deep(0).__code__, {}, None, None, (c,))))(Cell("given")))
def with_class(x):
    def make():
        class K:
            y = x
            def m(self): return x
        return K
    return make
show("a class in it", lambda: (lambda K: (K.y, K().m()))(Function(with_class(0).__code__, {"__name__": "m", "__build_class__": __build_class__}, None, None, (Cell("in class"),))()))
def with_gen(x):
    def gen():
        yield x
        yield x
    return gen
def generating():
    c = Cell(1); f = Function(with_gen(0).__code__, {}, None, None, (c,)); i = f(); a = next(i); c.cell_contents = 2; return (a, next(i))
show("a generator", generating)
def with_comprehension(x):
    def comp(): return [x + i for i in range(2)], {k: x for k in "a"}, list(x for _ in "a")
    return comp
show("comprehensions", lambda: Function(with_comprehension(0).__code__, {"range": range, "list": list}, None, None, (Cell(10),))())
def with_lambda(x):
    def f(): return (lambda: x)()
    return f
show("a lambda", lambda: Function(with_lambda(0).__code__, {}, None, None, (Cell("l"),))())
class Base:
    def who(self): return "Base"
class Derived(Base):
    def who(self): return "Derived then " + super().who()
    def cls(self): return __class__
show("__class__", lambda: (Derived.who.__code__.co_freevars, Function(Derived.who.__code__, globals(), None, None, Derived.who.__closure__)(Derived()), Function(Derived.cls.__code__, {}, None, None, (Cell(int),))(None)))
def with_locals(x):
    def f():
        z = 1
        return sorted(locals().items())
    def uses(): return (x, sorted(locals().items()), sorted(sys._getframe().f_locals.items()))
    return uses
show("locals", lambda: Function(with_locals(0).__code__, {"sorted": sorted, "locals": locals, "sys": sys}, None, None, (Cell("seen"),))())
# ---- what is wrong
for label, a in [("no arguments", ()), ("no globals", (plain.__code__,)), ("not code", (1, {})), ("None code", (None, {})), ("not a dict", (plain.__code__, 1)), ("None globals", (plain.__code__, None)), ("bad name", (plain.__code__, {}, 1)),
        ("bad defaults", (plain.__code__, {}, None, [1])), ("bad closure", (plain.__code__, {}, None, None, [1])), ("closure wanted", (inner.__code__, {})), ("closure too short", (inner.__code__, {}, None, None, (Cell(),))),
        ("closure not wanted", (plain.__code__, {}, None, None, (Cell(),))), ("empty closure", (plain.__code__, {}, None, None, ())), ("not cells", (inner.__code__, {}, None, None, (1, 2))), ("bad kwdefaults", (plain.__code__, {}, None, None, None, [1])), ("too many", (plain.__code__, {}, None, None, None, None, 1))]:
    show(label, lambda: type(Function(*a)).__name__)
show("keywords", lambda: Function(code=plain.__code__, globals={"n": 0}, name="k", argdefs=(1, 2), closure=None, kwdefaults=None)())
show("derived", lambda: type("X", (Function,), {}))
class DD(dict): pass
show("derived dict", lambda: Function(plain.__code__, DD(n="dd"))(1))
# ---- f.__code__ = ...
def one(): return "one"
def two(): return "two"
def swapping():
    def f(): return "f"
    r = [f()]; f.__code__ = one.__code__; r.append(f()); f.__code__ = two.__code__; r.append(f()); return (r, f.__name__, f.__qualname__, f.__code__ is two.__code__, f.__code__.co_name)
show("code set", swapping)
def hot():
    def f(): return 1
    def caller(): return f()
    t = sum(caller() for _ in range(3000)); f.__code__ = (lambda: 2).__code__; return (t, sum(caller() for _ in range(3000)))
show("code set while it is being called", hot)
def keeps():
    def f(a, b=5):
        "f's doc"
        return (a, b)
    def h(x, y=0, z=0):
        "h's doc"
        return ("h", x, y, z)
    f.attr = 1; f.__code__ = h.__code__; return (f(1), f.__doc__, f.__defaults__, f.attr, f.__name__, f.__module__)
show("what it keeps", keeps)
def with_closure():
    b = counter(); other = counter()
    def double():
        nonlocal_value = 0
    def make(m):
        def times():
            nonlocal m
            m *= 2
            return m
        return times
    b(); b(); b.__code__ = make(0).__code__; return (b(), b(), b.__closure__[0].cell_contents, b.__code__.co_freevars)
show("code set with a closure", with_closure)
show("code set wrong closure", lambda: setattr(counter(), "__code__", one.__code__))
show("code set wrong closure 2", lambda: (lambda f: setattr(f, "__code__", bump.__code__))(lambda: 0))
show("code set wrong", lambda: (lambda f: setattr(f, "__code__", 1))(lambda: 0))
show("code delete", lambda: (lambda f: delattr(f, "__code__"))(lambda: 0))
def globals_stay():
    f = Function((lambda: n).__code__, {"n": "kept"}); f.__code__ = (lambda: (n, n)).__code__; return f()
show("code set keeps the globals", globals_stay)
def method_swap():
    class K:
        def m(self): return "m"
    k = K(); b = k.m; r = [b()]; K.m.__code__ = (lambda self: "swapped").__code__; return r + [b(), k.m(), K().m()]
show("code set of a method", method_swap)
def frame_writes():
    c = Cell(1)
    def uses_frame(x):
        def f():
            sys._getframe().f_locals["x"] = "written"
            return x
        return f
    return (Function(uses_frame(0).__code__, {"sys": sys}, None, None, (c,))(), c.cell_contents)
show("f_locals writes through", frame_writes)
def recursive_swap():
    def f(n):
        if n == 0:
            f.__code__ = (lambda n: "new").__code__
            return "old"
        return f(n - 1)
    return (f(3), f(3))
show("code set from within", recursive_swap)
events = []
sys.addaudithook(lambda e, a: events.append((e, len(a))) if e in ("function.__new__", "object.__setattr__") else None)
show("audited", lambda: (Function(one.__code__, {}).__name__, (lambda f: setattr(f, "__code__", one.__code__))(lambda: 0), events))
