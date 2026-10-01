# What a class is given without anybody writing it, and how it says that it is called.
import io, _io, collections, inspect
def attempt(label, f):
    try: r = f()
    except BaseException as e: r = "%s: %s" % (type(e).__name__, e)
    print(label, "->", r)
class B(io.BufferedReader): pass
class B2(io.BufferedReader):
    "doc"
class B3(io.BufferedReader):
    def __init__(self, a): pass
for t in (io.BufferedReader, B, B2, B3, type("D", (dict,), {}), type("L", (list,), {}), type("I", (int,), {}), type("S", (str,), {}), type("Q", (collections.deque,), {}), type("E", (ValueError,), {}), type("X", (), {}), type("T", (io.TextIOWrapper,), {}), type("BB", (B,), {})):
    attempt(t.__name__, lambda: (t.__text_signature__, t.__doc__ and t.__doc__[:20]))
    attempt("   signature", lambda: str(inspect.signature(t)))
def f():
    __firstlineno__ = 1
    __module__ = "m"
    __qualname__ = "q"
    class C:
        nonlocal __firstlineno__
    class D:
        nonlocal __module__
    class E:
        nonlocal __qualname__
    return sorted(vars(C)), sorted(vars(D)), sorted(vars(E)), __firstlineno__, __module__, __qualname__, C.__module__, D.__module__, E.__qualname__
attempt("nonlocal", f)
__firstlineno__ = "global"
class G:
    global __firstlineno__
attempt("global", lambda: (sorted(vars(G)), __firstlineno__))
for doc in ("K(a, b=1)\n--\n\nwhat it is", "K(a)\n--\n\n", "K()\n--\n\n", "K(a)\n--\n", "K(a)\n\n--\n\n", "K (a)\n--\n\n", "J(a)\n--\n\n", "K(a,\n  b)\n--\n\n", "K(a)--\n\n", "K", "K(", "", None, 5, "KK(a)\n--\n\n", "K(a)\n--\n\nK(b)\n--\n\n", "m.K(a)\n--\n\n", "K($self, a)\n--\n\n", "K(a)\n--\n\n\n\n", "K(\n\n)\n--\n\n"):
    K = type("K", (), {"__doc__": doc})
    attempt(repr(doc), lambda: (K.__text_signature__, K.__doc__))
    attempt("   signature", lambda: str(inspect.signature(K)))
K = type("K", (), {"__doc__": "K(a)\n--\n\n"}); K.__doc__ = "K(b)\n--\n\n"
class Sub(K): pass
attempt("derived", lambda: Sub.__text_signature__)
def g():
    __module__ = "outer"; __doc__ = "outer doc"; __static_attributes__ = 1; __qualname__ = "outer q"
    class C:
        "the doc"
        x = __module__
    class D:
        "the doc"
        nonlocal __doc__, __static_attributes__
        def m(self): self.a = 1
    return C.x, sorted(vars(C)), __module__, sorted(vars(D)), __doc__, __static_attributes__, D.__doc__
attempt("read, and so free", g)
print("done")
