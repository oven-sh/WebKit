# What a name means in something that is inside a class that is inside a function. What the class itself has by the name, or says of it, is nothing to what is inside the class.
import symtable
def attempt(label, f):
    try: print(label, "=>", f())
    except BaseException as e: print(label, "=>", type(e).__name__, e)

# testClassAndGlobal
def test(x):
    class Foo:
        global x
        def __call__(self, y):
            return x + y
    return Foo()
x = 0
attempt("global in the class, free in the method", lambda: test(6)(2))

# testFreeVarInMethod
def test2():
    method_and_var = "var"
    class Test:
        def method_and_var(self):
            return "method"
        def test(self):
            return method_and_var
        def actual_global(self):
            return str("global")
        def str(self):
            return str(self)
    return Test()
t = test2()
attempt("a method of the same name", lambda: (t.test(), t.method_and_var(), t.actual_global()))
method_and_var = "var"
class Test:
    def method_and_var(self):
        return "method"
    def test(self):
        return method_and_var
attempt("at the top", lambda: (Test().test(), Test().method_and_var()))

# testNonLocalClass
def f(x):
    class c:
        nonlocal x
        x += 1
        def get(self):
            return x
    return c()
c = f(0)
attempt("nonlocal in the class", lambda: (c.get(), "x" in c.__class__.__dict__))

def g(x):
    class c:
        x = x + 10 if False else 5
        def get(self):
            return x
    return c()
attempt("bound in the class", lambda: (g(1).get(), g(1).x))

def h(x):
    class c:
        y = x
        x = 7
        z = x
        def get(self):
            return x
    return c()
attempt("read and then bound in the class", lambda: (h(1).y, h(1).x, h(1).z, h(1).get()))
src = '''
def test(x):
    class Foo:
        global x
        def __call__(self, y):
            return x + y
    return Foo()
'''
def walk(t, depth=0):
    print("  " * depth, t.get_type(), t.get_name(), [(s.get_name(), "G" if s.is_global() else "", "L" if s.is_local() else "", "F" if s.is_free() else "") for s in t.get_symbols()])
    for c in t.get_children(): walk(c, depth + 1)
walk(symtable.symtable(src, "s", "exec"))

print("---- what the code objects say")


def g2(x):
    class c:
        x = 5

        def get(self):
            return x
    return c


C = g2(1)
body = [k for k in g2.__code__.co_consts if hasattr(k, "co_name")][0]
print(g2.__code__.co_cellvars, body.co_name, body.co_freevars, C.get.__code__.co_freevars, [cell.cell_contents for cell in C.get.__closure__])

print("---- deeper")


def outer(a, b, c):
    class One:
        a = "One.a"
        global b
        nonlocal c
        c = c + "!"

        class Two:
            a = "Two.a"
            b = "Two.b"

            def get(self):
                return a, b, c

            class Three:
                def get(self):
                    return a, b, c
                got = (a, b, c)

        def get(self):
            return a, b, c
        got = (a, b, c)
    return One


b = "global b"
One = outer("a", "b", "c")
print(One().get(), One.got, One.Two().get(), One.Two.Three().get(), One.Two.Three.got, sorted(k for k in One.__dict__ if len(k) == 1))


def deleted(x):
    class c:
        nonlocal x
        del x

        def get(self):
            return x
    return c()


attempt("deleted by the class", lambda: deleted(1).get())


def lambdas(x):
    class c:
        x = 2
        f = lambda self: x
        g = staticmethod(lambda: [x for _ in "a"])
        h = property(lambda self, x=x: x)
    return c()


attempt("lambdas", lambda: (lambdas(1).f(), lambdas(1).g(), lambdas(1).h))


def generators(x):
    class c:
        x = 2
        g = list(x for _ in "a")
        h = list(y for y in [x])
        i = list((x, y) for y in [x])
    return c


attempt("generator expressions", lambda: (generators(1).g, generators(1).h, generators(1).i))

print("---- a comprehension in a class does not see what the class has")
y = "global y"
z = "global z"


class InModule:
    y = "the class's y"
    first = [y for _ in "a"]
    iterable = [v for v in [y]]
    both = [(v, y) for v in [y]]
    nested = [[(v, w, y) for w in [y]] for v in [y]]
    conditions = [v for v in "a" if y == "global y"]
    aSet = {y for _ in "a"}
    aDict = {y: y for _ in "a"}
    inALambda = [(lambda: y)() for _ in "a"]


for name in ("first", "iterable", "both", "nested", "conditions", "aSet", "aDict", "inALambda"):
    print("   ", name, getattr(InModule, name))


def inFunction():
    y = "the function's y"

    class C:
        y = "the class's y"
        first = [y for _ in "a"]
        both = [(v, y) for v in [y]]
        nested = [[(v, w, y, z) for w in [y]] for v in [y]]
    return C.first, C.both, C.nested


print("   ", inFunction())


def withGlobal():
    y = "the function's y"

    class C:
        global y
        y = "set by the class"
        locals()["y"] = "in the namespace"
        values = [y for _ in "a"]
        plain = y
    return C.values, C.plain


print("   ", withGlobal(), y)
y = "global y"


def withNonlocal():
    y = "the function's y"

    class C:
        nonlocal y
        y = "set by the class"
        locals()["y"] = "in the namespace"
        values = [y for _ in "a"]
        plain = y
    return C.values, C.plain, y


print("   ", withNonlocal())


class Named:
    __class__ = 5
    values = [v for v in "a"]

    def which(self):
        return __class__


print("   ", Named().which() is Named, Named.values)

print("---- the variable of one comprehension is nothing to the next")
x = "global x"


def reuse():
    [x for x in [1]]
    return [x for _ in [1]]


def reuseWithLocal():
    x = "local x"
    [x for x in [1]]
    return [x for _ in [1]], x


class Reuse:
    x = "the class's x"
    [x for x in [1]]
    values = [x for _ in [1]]


attempt("in a function", reuse)
attempt("with a variable of the name", reuseWithLocal)
attempt("in a class", lambda: (Reuse.values, Reuse.x))
[x for x in [1]]
attempt("in the module", lambda: ([x for _ in [1]], x))
namespace = {"x": 3}
exec("[x for x in [1]]\ny = [x for _ in [1]]", namespace)
attempt("in exec()", lambda: namespace["y"])
namespace = {"z": 0}
exec("class C:\n    z = 1\n    items = [a for a in [1] if [x for x in [1] if z]]", namespace)
attempt("nested, in a class", lambda: namespace["C"].items)
