# __class__, named in the body of a class, is a name like any other there. It is what is defined in the class that means the class by it.
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
    print(label, "->", r)
def name(v): return getattr(v, "__name__", v)

class A:
    def f(self): return "A"

class Outer:
    def assigned(self):
        class X(A):
            def f(self): return super().f()
            __class__ = 413
        x = X()
        return x.f(), x.__class__, type(x).__name__, X.__dict__["__class__"]
    def read(self):
        class X:
            x = __class__
            def f(): return __class__
        return name(X.x), name(X.f())
    def read_only(self):
        class X:
            x = __class__
        return name(X.x)
    def said_global(self):
        class X:
            global __class__
            __class__ = 42
            def f(): return __class__
        r = globals()["__class__"], "__class__" in X.__dict__, name(X.f())
        del globals()["__class__"]
        return r
    def said_nonlocal(self):
        class X:
            nonlocal __class__
            __class__ = 42
            def f(): return __class__
        return __class__, "__class__" in X.__dict__, name(X.f())
    def deleted(self):
        class X:
            __class__ = 1
            del __class__
            y = __class__
            def f(): return __class__
        return name(X.y), name(X.f())
    def nested(self):
        class X:
            class Y:
                z = __class__
                def f(): return __class__
            w = __class__
            def g(): return __class__
        return name(X.Y.z), name(X.Y.f()), name(X.w), name(X.g())
    def in_comprehension(self):
        class X:
            c = [__class__ for _ in (1,)]
            g = list(__class__ for _ in (1,))
            l = (lambda: __class__)()
        return [name(v) for v in X.c], [name(v) for v in X.g], name(X.l)
    def with_super(self):
        class X(A):
            s = super
            outer = __class__
            def f(self): return super().f() + "X"
        return name(X.outer), X().f()
    def freevars(self):
        class X:
            x = __class__
            def f(): return __class__
        return X.f.__code__.co_freevars
    def classdict(self):
        class X:
            a = 1
            type T = a
            d = "__classdict__" in dir()
        return X.T.__value__, X.d
    def annotated(self):
        class X:
            v: __class__
            def f(a: __class__): pass
        return {k: name(t) for k, t in X.__annotations__.items()}, {k: name(t) for k, t in X.f.__annotations__.items()}
o = Outer()
for m in ("assigned", "read", "read_only", "said_global", "deleted", "nested", "in_comprehension", "with_super", "freevars", "classdict", "annotated", "said_nonlocal"):
    attempt(m, getattr(o, m))
attempt("at the top of a module", lambda: exec("class X:\n    __class__\n    def f():\n        __class__", globals(), {}))
attempt("at the top, with one there", lambda: (lambda ns: (exec("__class__ = 'the module has one'\nclass X:\n    x = __class__\n    def f(): return __class__", ns), ns["X"].x, name(ns["X"].f()))[1:])({}))
def function():
    class X:
        x = __class__
    return X
attempt("in a function that is in no class", function)
def function2():
    __class__ = "a variable of the function"
    class X:
        x = __class__
        def f(): return __class__
    return X.x, name(X.f())
attempt("in a function that has a variable of the name", function2)

print("== what is called super")
class B(A):
    def alias(self):
        s = super
        return s
    def by_alias(self):
        s = super
        try:
            return s().f()
        except RuntimeError as e:
            return "RuntimeError: %s" % e
    def by_alias_with_class(self):
        __class__
        s = super
        return s().f()
    def explicit(self): return super(B, self).f()
class MySuper(super): pass
class C(A):
    def f(self):
        __class__
        return MySuper().f() + "C"
    def g(self):
        try:
            return MySuper().f()
        except RuntimeError as e:
            return "RuntimeError: %s" % e
attempt("alias", lambda: B().by_alias())
attempt("alias, where __class__ is named", lambda: B().by_alias_with_class())
attempt("explicit", lambda: B().explicit())
attempt("a class derived from super", lambda: C().f())
attempt("and where __class__ is not named", lambda: C().g())
