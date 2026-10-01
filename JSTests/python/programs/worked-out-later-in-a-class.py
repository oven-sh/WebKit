# What is worked out later, and can see into a class: the value of a type alias, a bound, a default, an annotation. What the class says of a name goes for them, and so does what names are mangled.
import annotationlib

def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
    print(label, "->", r)

x = "global"
def outer(kind):
    x = "nonlocal"
    if kind == "global":
        class Cls:
            global x
            type Alias = x
            type Nested = (lambda: x)()
            type Comp = [x for _ in (1,)]
            type Gen = list(x for _ in (1,))
            def meth[T: x, U = x](self, a: x) -> x: pass
            def plain(self, a: x): return x
            class Inner[T: x]: y: x
            type Generic[T: x] = (T, x)
            attr: x
            here = x
            comp = [x for _ in (1,)]
            lam = lambda self: x
    elif kind == "global assigned":
        class Cls:
            global x
            type Alias = x
            type Nested = (lambda: x)()
            type Comp = [x for _ in (1,)]
            type Gen = list(x for _ in (1,))
            def meth[T: x, U = x](self, a: x) -> x: pass
            def plain(self, a: x): return x
            class Inner[T: x]: y: x
            type Generic[T: x] = (T, x)
            attr: x
            x = "global from class"
            here = x
            comp = [x for _ in (1,)]
            lam = lambda self: x
    elif kind == "nonlocal":
        class Cls:
            nonlocal x
            type Alias = x
            type Nested = (lambda: x)()
            type Comp = [x for _ in (1,)]
            type Gen = list(x for _ in (1,))
            def meth[T: x, U = x](self, a: x) -> x: pass
            def plain(self, a: x): return x
            class Inner[T: x]: y: x
            type Generic[T: x] = (T, x)
            attr: x
            here = x
            comp = [x for _ in (1,)]
            lam = lambda self: x
    elif kind == "bound":
        class Cls:
            x = "in the class"
            type Alias = x
            type Nested = (lambda: x)()
            type Comp = [x for _ in (1,)]
            type Gen = list(x for _ in (1,))
            def meth[T: x, U = x](self, a: x) -> x: pass
            def plain(self, a: x): return x
            class Inner[T: x]: y: x
            type Generic[T: x] = (T, x)
            attr: x
            here = x
            comp = [x for _ in (1,)]
            lam = lambda self: x
    else:
        class Cls:
            type Alias = x
            type Nested = (lambda: x)()
            type Comp = [x for _ in (1,)]
            type Gen = list(x for _ in (1,))
            def meth[T: x, U = x](self, a: x) -> x: pass
            def plain(self, a: x): return x
            class Inner[T: x]: y: x
            type Generic[T: x] = (T, x)
            attr: x
            here = x
            comp = [x for _ in (1,)]
            lam = lambda self: x
    Cls.x = "set on the class afterwards"
    return Cls

for kind in ("global", "global assigned", "nonlocal", "bound", "nothing said"):
    print("=====", kind)
    x = "global"
    C = outer(kind)
    attempt("alias", lambda: C.Alias.__value__)
    attempt("lambda in an alias", lambda: C.Nested.__value__)
    attempt("comprehension in an alias", lambda: C.Comp.__value__)
    attempt("generator in an alias", lambda: C.Gen.__value__)
    attempt("bound", lambda: C.meth.__type_params__[0].__bound__)
    attempt("default", lambda: C.meth.__type_params__[1].__default__)
    attempt("annotations of a generic method", lambda: C.meth.__annotations__)
    attempt("annotations of a method", lambda: C.plain.__annotations__)
    attempt("in a method", lambda: C().plain(1))
    attempt("bound of a class inside", lambda: C.Inner.__type_params__[0].__bound__)
    attempt("annotations of a class inside", lambda: C.Inner.__annotations__)
    attempt("bound of a generic alias", lambda: C.Generic.__type_params__[0].__bound__)
    attempt("value of a generic alias", lambda: C.Generic.__value__[1])
    attempt("annotations of the class", lambda: C.__annotations__)
    attempt("in the body", lambda: C.here)
    attempt("comprehension in the body", lambda: C.comp)
    attempt("lambda in the body", lambda: C().lam())
    attempt("the module's", lambda: x)

print("===== names that are mangled")
class Base:
    def __init_subclass__(cls, **kw): pass
def make_base(arg):
    class B: __arg__ = arg
    return B
class __X: pass
_Y__X = "mangled for Y"
_Y__T = "not the parameter"
class Y[__T: __X = __X](make_base(lambda: __X), make_base(lambda: (lambda: __X)), make_base([__X for _ in (1,)]), make_base(__X for _ in (1,)), make_base(lambda: __T), make_base([__T for _ in (1,)]),
                        make_base(lambda __a=__X: __a), make_base({__X: __X for __k in (1,)})):
    inside = __X
    lam = lambda self: __X
    param = __T
    type A = __X
    type G[__U: __X] = (__U, __X, __T)
    def m[__V: __X](self, a: __X) -> __V: return __X, __T, __V
    ann: __X
def name(v): return getattr(v, "__name__", v)
attempt("names of the parameters", lambda: [t.__name__ for t in Y.__type_params__])
attempt("bound", lambda: name(Y.__type_params__[0].__bound__))
attempt("default", lambda: name(Y.__type_params__[0].__default__))
attempt("lambda in a base", lambda: name(Y.__bases__[0].__arg__()))
attempt("lambda in a lambda", lambda: name(Y.__bases__[1].__arg__()()))
attempt("comprehension", lambda: [name(v) for v in Y.__bases__[2].__arg__])
attempt("generator", lambda: [name(v) for v in Y.__bases__[3].__arg__])
attempt("lambda that names the parameter", lambda: name(Y.__bases__[4].__arg__()))
attempt("comprehension that names the parameter", lambda: [name(v) for v in Y.__bases__[5].__arg__])
attempt("a parameter of a lambda", lambda: (name(Y.__bases__[6].__arg__()), Y.__bases__[6].__arg__.__code__.co_varnames))
attempt("dict comprehension", lambda: [(name(k), name(v)) for k, v in Y.__bases__[7].__arg__.items()])
attempt("in the body", lambda: name(Y.inside))
attempt("lambda in the body", lambda: name(Y().lam()))
attempt("the parameter in the body", lambda: name(Y.param))
attempt("alias in the body", lambda: name(Y.A.__value__))
attempt("generic alias in the body", lambda: ([t.__name__ for t in Y.G.__type_params__], name(Y.G.__type_params__[0].__bound__), [name(v) for v in Y.G.__value__]))
attempt("generic method", lambda: ([t.__name__ for t in Y.m.__type_params__], name(Y.m.__type_params__[0].__bound__), {k: name(v) for k, v in Y.m.__annotations__.items()}, [name(v) for v in Y().m(1)]))
attempt("annotations", lambda: {k: name(v) for k, v in Y.__annotations__.items()})
attempt("co_names of a lambda in a base", lambda: Y.__bases__[0].__arg__.__code__.co_names)
def in_function():
    class __Z: pass
    class W[__T: __Z](make_base(lambda: __Z), make_base(lambda: __T)):
        got = lambda self: (__Z_missing if False else 1)
    return name(W.__type_params__[0].__bound__), name(W.__bases__[0].__arg__()), name(W.__bases__[1].__arg__()), W.__bases__[0].__arg__.__code__.co_freevars, W.__bases__[1].__arg__.__code__.co_freevars
attempt("in a function", in_function)
