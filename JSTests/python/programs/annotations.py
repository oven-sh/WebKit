import sys
def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)

# ---- functions
def f(a: int, b: "s" = 1, /, c: float = 2, *d: bool, e: str, **g: bytes) -> list: pass
show("function", lambda: f.__annotations__)
show("order", lambda: list(f.__annotations__))
show("annotate", lambda: (f.__annotate__(1), f.__annotate__(2), type(f.__annotate__).__name__))
show("annotate names", lambda: (f.__annotate__.__name__, f.__annotate__.__qualname__, f.__annotate__.__code__.co_varnames[:1], f.__annotate__.__code__.co_argcount, f.__annotate__.__code__.co_posonlyargcount))
show("formats", lambda: [f.__annotate__(n) is not None for n in (0, 1, 2)])
show("format 3", lambda: f.__annotate__(3))
show("format 4", lambda: f.__annotate__(4))
show("no argument", lambda: f.__annotate__())
show("keyword", lambda: f.__annotate__(format=1))
show("cached", lambda: f.__annotations__ is f.__annotations__)
show("fresh from annotate", lambda: f.__annotate__(1) is f.__annotate__(1))
def none(): pass
show("none", lambda: (none.__annotations__, none.__annotate__, none.__annotations__ is none.__annotations__))
show("lambda", lambda: ((lambda x: x).__annotations__, (lambda x: x).__annotate__))
def lazy(a: Undefined) -> AlsoUndefined: pass
show("lazy", lambda: lazy.__annotations__)
Undefined = "now defined"; AlsoUndefined = 2
show("lazy later", lambda: lazy.__annotations__)
calls = []
def note(x): calls.append(x); return x
def counted(a: note(1), b: note(2)) -> note(3): pass
show("not evaluated at def", lambda: list(calls))
show("evaluated once", lambda: (counted.__annotations__, counted.__annotations__, list(calls)))
def outer():
    T = "closure"
    def inner(a: T, b: format): pass
    return inner
show("closure", lambda: (outer().__annotations__["a"], outer().__annotations__["b"] is format, outer().__annotate__.__closure__ is not None))
def star(*args: *[7]): pass
show("star", lambda: star.__annotations__)
def local_vars():
    x: NotEvaluated = 1
    y: AlsoNot
    return x
show("locals not evaluated", lambda: (local_vars(), local_vars.__annotations__))
async def co(a: int) -> str: pass
def gen(a: int): yield
show("async and generator", lambda: (co.__annotations__, gen.__annotations__))

# ---- setting
def s(a: int): pass
show("set annotations", lambda: (setattr(s, "__annotations__", {"z": 1}), s.__annotations__, s.__annotate__)[1:])
show("set to None", lambda: (setattr(s, "__annotations__", None), s.__annotations__, s.__annotate__)[1:])
show("set bad", lambda: setattr(s, "__annotations__", 5))
show("delete", lambda: (delattr(s, "__annotations__"), s.__annotations__)[1])
show("set annotate", lambda: (setattr(s, "__annotate__", lambda fmt: {"q": fmt}), s.__annotations__, s.__annotate__(2))[1:])
show("set annotate None", lambda: (setattr(s, "__annotate__", None), s.__annotate__, s.__annotations__)[1:])
show("set annotate bad", lambda: setattr(s, "__annotate__", 5))
show("delete annotate", lambda: delattr(s, "__annotate__"))
show("annotate returns non-dict", lambda: (setattr(s, "__annotate__", lambda fmt: 5), s.__annotations__)[1])

# ---- classes
e = type(sys)('e')
class C:
    a: int = 1
    b: "str"
    c: list[int] if False else dict
    (d): int = 2
    e.f: int if False else None
    T = "in class"
    g: T
    __private: int
    def m(self, x: T, __y: int) -> "C": pass
    def uses_global(self, x: int): pass
show("class", lambda: C.__annotations__)
show("class annotate", lambda: (C.__annotate__(1) == C.__annotations__, C.__annotate__.__name__, C.__annotate__.__qualname__))
show("class dict", lambda: sorted(k for k in C.__dict__ if "annot" in k))
show("method sees class", lambda: C.m.__annotations__)
show("method qualname", lambda: C.m.__annotate__.__qualname__)
show("instance", lambda: C().__annotations__)
class D(C): pass
show("not inherited", lambda: (D.__annotations__, D.__annotate__, sorted(k for k in D.__dict__ if "annot" in k)))
class Empty: pass
show("empty", lambda: (Empty.__annotations__, Empty.__annotate__))
show("built in", lambda: int.__annotations__)
show("built in annotate", lambda: int.__annotate__)
show("set on built in", lambda: setattr(int, "__annotations__", {}))
class Cond:
    if True:
        a: int
    else:
        b: int
    for _ in range(1):
        c: str
    try:
        d: float
    except Exception:
        e2: bytes
    with open.__class__ if False else type("M", (), {"__enter__": lambda s: s, "__exit__": lambda *a: None})():
        w: int
    while False:
        never: int
    z: complex
show("conditional", lambda: Cond.__annotations__)
class LazyC:
    a: NotYet
show("lazy class", lambda: LazyC.__annotations__)
NotYet = 5
show("lazy class later", lambda: LazyC.__annotations__)
class SetC:
    a: int
show("set class annotations", lambda: (setattr(SetC, "__annotations__", {"n": 1}), SetC.__annotations__, SetC.__annotate__, sorted(k for k in SetC.__dict__ if "annot" in k))[1:])
show("delete class annotations", lambda: (delattr(SetC, "__annotations__"), SetC.__annotations__)[1])
show("delete twice", lambda: (delattr(SetC, "__annotations__"), delattr(SetC, "__annotations__")))
show("set class annotate", lambda: (setattr(SetC, "__annotate__", lambda fmt: {"k": fmt}), SetC.__annotations__, sorted(k for k in SetC.__dict__ if "annot" in k))[1:])
show("set class annotate bad", lambda: setattr(SetC, "__annotate__", 1))
show("delete class annotate", lambda: delattr(SetC, "__annotate__"))
def make():
    V = "enclosing"
    class Inner:
        a: V
        V2 = "own"
        b: V2
    return Inner
show("class in function", lambda: make().__annotations__)
class Meta(type):
    x: int
class WithMeta(metaclass=Meta):
    y: str
show("metaclass", lambda: (Meta.__annotations__, WithMeta.__annotations__))
class Comp:
    a: [i for i in range(2)]
    b: (lambda: 1)
show("comprehension and lambda", lambda: (Comp.__annotations__["a"], Comp.__annotations__["b"]()))

# ---- modules
mx: int = 1
my: "str"
if True:
    mz: float
else:
    mnever: int
show("module", lambda: (__annotations__ if "__annotations__" in globals() else "not a global", sys.modules[__name__].__annotations__))
show("module annotate", lambda: (__annotate__(1), __annotate__.__name__, __annotate__.__qualname__))
show("conditional set", lambda: sorted(__conditional_annotations__))
m = type(sys)("m")
show("plain module", lambda: (m.__annotations__, m.__annotate__, sorted(k for k in vars(m) if "annot" in k)))
show("set module annotate", lambda: (setattr(m, "__annotate__", lambda fmt: {"a": fmt}), m.__annotations__)[1])
show("set module annotations", lambda: (setattr(m, "__annotations__", {"b": 1}), m.__annotations__, "__annotate__" in vars(m))[1:])
show("delete module annotations", lambda: (delattr(m, "__annotations__"), m.__annotations__)[1])
ns = {}
exec("a: int = 1\nb: str", ns)
show("exec", lambda: (ns["__annotate__"](1), "__annotations__" in ns))
show("exec partial", lambda: (lambda n: (exec("a: int\nraise ValueError\nb: str", n) if False else None, n)[1])({}))
def partial():
    n = {}
    try: exec("a: int\nraise ValueError\nb: str", n)
    except ValueError: pass
    return n["__annotate__"](1)
show("module that stopped part way", partial)
show("errors", lambda: [compile_error(src) for src in ["def f(a: (yield)): pass", "def f(a: (x := 1)): pass", "async def f(a: await x): pass", "class C:\n a: (yield)", "x: (y := 1)", "def f():\n global x\n x: int", "(a, b): int", "[a]: int", "a, b: int", "f(): int"]])
def compile_error(src):
    try: compile(src, "<s>", "exec"); return "compiled"
    except SyntaxError as e: return e.msg
show("errors", lambda: [compile_error(src) for src in ["def f(a: (yield)): pass", "def f(a: (x := 1)): pass", "async def f(a: await x): pass", "class C:\n a: (yield)", "x: (y := 1)", "def f():\n global x\n x: int", "(a, b): int", "[a]: int", "a, b: int", "f(): int"]])
class W:
    @classmethod
    def c(cls, a: int) -> str: pass
    @staticmethod
    def s(a: float): pass
show("classmethod and staticmethod", lambda: (W.__dict__["c"].__annotations__, W.__dict__["s"].__annotations__, W.__dict__["c"].__annotate__(1), W.c.__annotations__, W.s.__annotations__, sorted(vars(W.__dict__["c"]))))
show("set on classmethod", lambda: (setattr(W.__dict__["c"], "__annotations__", {"z": 1}), W.__dict__["c"].__annotations__, W.c.__annotations__)[1:])
show("delete on classmethod", lambda: (delattr(W.__dict__["c"], "__annotations__"), W.__dict__["c"].__annotations__)[1])
show("delete on staticmethod", lambda: (delattr(staticmethod(len), "__annotations__")))
show("nested qualname", lambda: (outer().__annotate__.__qualname__, make().__annotate__.__qualname__, C.__annotate__.__qualname__))
