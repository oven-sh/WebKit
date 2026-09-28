import sys, _typing
# What is written in C leaves a good deal to the typing module. This stands in for it, here and in CPython alike.
stub = type(sys)("typing")
class _GenericAlias:
    def __init__(self, origin, args): self.__origin__ = origin; self.__args__ = args
    def __mro_entries__(self, bases): return (self.__origin__,)
    def __repr__(self): return f"{self.__origin__.__name__}[{', '.join(map(repr, self.__args__))}]"
class _Unpack:
    def __getitem__(self, item): return _Unpacked(item)
class _Unpacked:
    def __init__(self, item): self.item = item
    def __repr__(self): return f"*{self.item!r}"
    def __eq__(self, other): return isinstance(other, _Unpacked) and self.item is other.item
    __hash__ = None
calls = []
stub._GenericAlias = _GenericAlias
stub.Unpack = _Unpack()
stub._generic_class_getitem = lambda cls, args: (calls.append(("getitem", cls.__name__, args)), _GenericAlias(cls, args if isinstance(args, tuple) else (args,)))[1]
stub._generic_init_subclass = lambda cls, *a, **k: calls.append(("init_subclass", cls.__name__, a, k))
stub._type_check = lambda arg, msg: (calls.append(("type_check", arg, msg)), arg)[1]
stub._typevar_subst = lambda self, arg: (calls.append(("typevar_subst", self, arg)), arg)[1]
stub._paramspec_subst = lambda self, arg: ("paramspec_subst", arg)
stub._paramspec_prepare_subst = lambda self, alias, args: args
stub._typevartuple_prepare_subst = lambda self, alias, args: args
sys.modules["typing"] = stub

def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
def took():
    r = list(calls); calls.clear(); return r

# ---- generic functions
def f[T](x: T) -> T: return x
show("function", lambda: (f(1), f.__type_params__, f.__annotations__, f.__name__, f.__qualname__, type(f.__type_params__[0])))
show("same variable", lambda: f.__annotations__["x"] is f.__type_params__[0])
def g[T: int, U: (str, bytes), *Ts, **P, V = float](a: T, *b: *Ts, **c: P.kwargs) -> U: pass
show("kinds", lambda: (g.__type_params__, [type(p).__name__ for p in g.__type_params__]))
show("bound and constraints", lambda: (g.__type_params__[0].__bound__, g.__type_params__[0].__constraints__, g.__type_params__[1].__bound__, g.__type_params__[1].__constraints__, g.__type_params__[4].__default__))
show("variance", lambda: [(p.__covariant__, p.__contravariant__, p.__infer_variance__) for p in g.__type_params__ if hasattr(p, "__covariant__")])
show("has default", lambda: [p.has_default() for p in g.__type_params__])
show("no default", lambda: [p.__default__ for p in g.__type_params__[:4]])
show("module of a variable", lambda: [p.__module__ for p in g.__type_params__])
show("annotations", lambda: g.__annotations__)
def lazy[T: Undefined1 = Undefined2](): pass
show("lazy bound", lambda: lazy.__type_params__[0].__bound__)
show("lazy default", lambda: lazy.__type_params__[0].__default__)
Undefined1 = "one"; Undefined2 = "two"
show("lazy later", lambda: (lazy.__type_params__[0].__bound__, lazy.__type_params__[0].__default__))
Undefined1 = "changed"
show("kept", lambda: lazy.__type_params__[0].__bound__)
show("evaluators", lambda: (lambda p: (p.evaluate_bound(1), p.evaluate_bound(), p.evaluate_bound.__name__, p.evaluate_bound.__qualname__, p.evaluate_default.__name__, p.evaluate_constraints, type(p.evaluate_bound).__name__, p.evaluate_bound.__defaults__, p.evaluate_bound.__code__.co_varnames[:1]))(lazy.__type_params__[0]))
show("evaluator format", lambda: lazy.__type_params__[0].evaluate_bound(3))
def defaults[T](a=1, b=[], *, c=2, d: T = 3): return (a, b, c, d)
show("defaults", lambda: (defaults(), defaults.__defaults__, defaults.__kwdefaults__, defaults(5, d=6)))
def onlykw[T](*, c=2): return c
show("only keyword defaults", lambda: (onlykw(), onlykw.__defaults__, onlykw.__kwdefaults__))
order = []
def note(x): order.append(x); return x
@note
def decorated[T: note("bound")](a=note("default")) -> note("annotation"): pass
show("order", lambda: [o if isinstance(o, str) else "function" for o in order])
def outer():
    local = "local"
    def inner[T: local](x: T, y: local): return local
    return inner
show("closure", lambda: (outer()(1, 2), outer().__type_params__[0].__bound__, outer().__annotations__["y"], outer().__qualname__, outer().__type_params__[0].evaluate_bound.__qualname__))
async def co[T](): pass
def gen[T](): yield 1
show("async and generator", lambda: (co.__type_params__, list(gen()), gen.__type_params__))
show("variable is not visible outside", lambda: T)
def shadows[int](x: int): return int
show("shadows", lambda: (shadows(1), shadows.__annotations__))
def refers[T, U: T](): pass
show("one refers to another", lambda: refers.__type_params__[1].__bound__ is refers.__type_params__[0])
show("set type params", lambda: (setattr(f, "__type_params__", (1,)), f.__type_params__)[1])
show("set type params wrong", lambda: setattr(f, "__type_params__", [1]))
show("delete type params", lambda: delattr(f, "__type_params__"))
show("plain function", lambda: (show.__type_params__, (lambda: 0).__type_params__))
show("lambda in a bound", lambda: (lambda h: (h.__type_params__[0].__bound__.__qualname__, h.__type_params__[0].__bound__()))(eval("h", {"h": lam})))
def lam[T: (lambda: "called")](): pass
show("lambda in a bound", lambda: (lam.__type_params__[0].__bound__.__qualname__, lam.__type_params__[0].__bound__()))
def comp[T: [i for i in range(3)]](): pass
show("comprehension in a bound", lambda: comp.__type_params__[0].__bound__)

# ---- the type statement
type A = int
show("alias", lambda: (A, repr(A), type(A), A.__name__, A.__value__, A.__type_params__, A.__parameters__, A.__module__))
type B[T] = list[T]
show("generic alias", lambda: (B, B.__value__, B.__type_params__, B.__parameters__, B[int], B[int].__origin__ is B, B[int].__args__, type(B[int]).__name__))
show("not generic", lambda: A[int])
type L = NotYet
show("lazy alias", lambda: L.__value__)
NotYet = 5
show("lazy alias later", lambda: L.__value__)
NotYet = 6
show("alias kept", lambda: L.__value__)
type R = list[R] | int
show("recursive", lambda: (R.__value__, R.__value__.__args__[0].__args__[0] is R))
show("alias in a union", lambda: (A | str, str | A, A | None, (A | str).__args__, A | A))
show("alias evaluate", lambda: (A.evaluate_value(1), A.evaluate_value(), A.evaluate_value.__name__, A.evaluate_value.__qualname__))
show("alias evaluate format", lambda: A.evaluate_value(3))
show("alias reduce", lambda: A.__reduce__())
show("alias read only", lambda: setattr(A, "__value__", 1))
show("alias no attributes", lambda: setattr(A, "x", 1))
show("alias not callable", lambda: A())
show("alias isinstance", lambda: isinstance(1, A))
show("alias iter", lambda: list(B))
type Many[T, *Ts, **P] = tuple[T, *Ts]
show("alias parameters", lambda: (Many.__type_params__, Many.__parameters__, Many.__value__))
type D[T = int, U = str] = dict[T, U]
show("alias defaults", lambda: [p.__default__ for p in D.__type_params__])
type S[*Ts = *tuple[int, str]] = tuple[*Ts]
show("starred default", lambda: S.__type_params__[0].__default__)
def alias_in_function():
    v = "local"
    type X = v
    type Y[T: v] = T
    return X, Y
show("alias in function", lambda: (alias_in_function()[0].__value__, alias_in_function()[1].__type_params__[0].__bound__, alias_in_function()[0].evaluate_value.__qualname__))
type = 5
show("type is still a name", lambda: type + 1)
del type
show("made by hand", lambda: (_typing.TypeAliasType("H", int), _typing.TypeAliasType("H", int).__value__, _typing.TypeAliasType("H", int).__module__, _typing.TypeAliasType("H", int).evaluate_value, _typing.TypeAliasType("H", list, type_params=(f.__type_params__ and B.__type_params__[0],)).__type_params__))
show("by hand wrong", lambda: _typing.TypeAliasType("H", int, type_params=[1]))
show("by hand wrong 2", lambda: _typing.TypeAliasType("H", int, type_params=(1,)))
show("by hand wrong 3", lambda: _typing.TypeAliasType(1, int))
show("by hand wrong 4", lambda: _typing.TypeAliasType("H"))
show("by hand wrong 5", lambda: _typing.TypeAliasType("H", int, type_params=(D.__type_params__[0], B.__type_params__[0])))

# ---- generic classes
took()
class C[T]:
    x: T
    def m(self, a: T) -> T: return a
show("class", lambda: (C, C.__type_params__, C.__bases__, C.__orig_bases__, C.__mro__, C.__annotations__, C.m.__annotations__, C().m(1)))
show("what the library was asked", took)
show("class dict order", lambda: [k for k in C.__dict__ if not k.startswith("__a")])
show("subscript", lambda: (C[int], took()))
class E[T, *Ts, **P](dict, metaclass=type): pass
show("class with bases", lambda: (E.__bases__, E.__orig_bases__, E.__type_params__, took()))
class Base: pass
class F[T: Base = Base](Base, *[]): pass
show("class bound", lambda: (F.__type_params__[0].__bound__, F.__type_params__[0].__default__, F.__bases__))
class Nested:
    K = "in class"
    def method[T: K](self, a: K, b: T): pass
    type Alias = K
    type GenericAlias[T: K] = K
    class Inner[T: K]:
        y: K if False else T
show("in a class", lambda: (Nested.method.__type_params__[0].__bound__, Nested.method.__annotations__["a"], Nested.Alias.__value__, Nested.GenericAlias.__value__, Nested.GenericAlias.__type_params__[0].__bound__, Nested.Inner.__type_params__[0].__bound__))
show("names in a class", lambda: (Nested.method.__qualname__, Nested.Inner.__qualname__, Nested.method.__type_params__[0].evaluate_bound.__qualname__, Nested.Alias.evaluate_value.__qualname__))
class Mangled[__T]:
    def m(self) -> __T: pass
    v: __T
show("mangled", lambda: (Mangled.__type_params__, Mangled.m.__annotations__, Mangled.__annotations__))
class Changes:
    T = int
    x: T
    type Al = T
Changes.T = str
show("the class as it is now", lambda: (Changes.__annotations__, Changes.Al.__value__))
show("type params of a class", lambda: (int.__type_params__, type.__type_params__, Base.__type_params__))
show("set on a class", lambda: (setattr(Base, "__type_params__", (1,)), Base.__type_params__, "__type_params__" in Base.__dict__)[1:])
show("set on a built-in class", lambda: setattr(int, "__type_params__", ()))
show("delete on a class", lambda: delattr(Base, "__type_params__"))
show("Generic", lambda: (_typing.Generic, _typing.Generic.__module__, _typing.Generic.__bases__, _typing.Generic[int], took()))
class Sub(_typing.Generic): pass
show("derived from Generic", lambda: (Sub.__mro__, took(), type(Sub()).__name__))

# ---- by hand
TypeVar, ParamSpec, TypeVarTuple, NoDefault = _typing.TypeVar, _typing.ParamSpec, _typing.TypeVarTuple, _typing.NoDefault
show("TypeVar", lambda: (TypeVar("T"), TypeVar("T", covariant=True), TypeVar("T", contravariant=True), TypeVar("T", infer_variance=True), TypeVar("T", int, str).__constraints__, TypeVar("T", bound=int).__bound__, TypeVar("T", bound=None).__bound__, TypeVar("T", default=None).__default__, TypeVar(name="T"), took()))
show("TypeVar module", lambda: (TypeVar("T").__module__, ParamSpec("P").__module__, TypeVarTuple("Ts").__module__))
for label, make in [("no name", lambda: TypeVar()), ("name", lambda: TypeVar(1)), ("bivariant", lambda: TypeVar("T", covariant=True, contravariant=True)), ("infer", lambda: TypeVar("T", covariant=True, infer_variance=True)), ("one constraint", lambda: TypeVar("T", int)), ("both", lambda: TypeVar("T", int, str, bound=int)), ("keyword", lambda: TypeVar("T", nope=1)),
        ("ParamSpec name", lambda: ParamSpec(1)), ("ParamSpec positional", lambda: ParamSpec("P", int)), ("TypeVarTuple name", lambda: TypeVarTuple(None)), ("NoDefault", lambda: type(NoDefault)(1))]:
    show("wrong: " + label, make)
show("evaluators by hand", lambda: (TypeVar("T", bound=int).evaluate_bound, TypeVar("T", bound=int).evaluate_bound(1), TypeVar("T", bound=int).evaluate_bound(4), TypeVar("T", int, str).evaluate_constraints(4), TypeVar("T").evaluate_bound, TypeVar("T").evaluate_default, TypeVar("T", default=list[int]).evaluate_default(4)))
show("const evaluator wrong", lambda: TypeVar("T", bound=int).evaluate_bound())
show("const evaluator wrong 2", lambda: TypeVar("T", bound=int).evaluate_bound(format=1))
show("const evaluator wrong 3", lambda: TypeVar("T", bound=int).evaluate_bound("a"))
show("attributes", lambda: (lambda t: (setattr(t, "x", 1), t.x))(TypeVar("T")))
show("read only", lambda: setattr(TypeVar("T"), "__name__", "U"))
show("reduce", lambda: (TypeVar("T").__reduce__(), ParamSpec("P").__reduce__(), TypeVarTuple("Ts").__reduce__(), NoDefault.__reduce__()))
show("cannot subclass", lambda: [cannot(x) for x in (TypeVar("T"), ParamSpec("P"), TypeVarTuple("Ts"), ParamSpec("P").args, ParamSpec("P").kwargs)])
def cannot(x):
    try:
        class Z(x): pass
    except TypeError as e: return str(e)
show("cannot subclass", lambda: [cannot(x) for x in (TypeVar("T"), ParamSpec("P"), TypeVarTuple("Ts"), ParamSpec("P").args, ParamSpec("P").kwargs)])
show("cannot subclass the classes", lambda: [cannot(x) for x in (TypeVar, ParamSpec, TypeVarTuple, _typing.TypeAliasType, _typing.ParamSpecArgs, type(NoDefault))])
took()
show("or", lambda: (TypeVar("T") | int, int | TypeVar("T"), TypeVar("T") | None, ParamSpec("P") | int, took()))
show("ParamSpec", lambda: (lambda p: (p, p.args, p.kwargs, p.args.__origin__ is p, p.args == p.args, p.args == p.kwargs, p.args != p.args, p.args == 1, p.__bound__, p.__default__, p.has_default()))(ParamSpec("P")))
show("ParamSpec parts are unhashable", lambda: hash(ParamSpec("P").args))
show("names in a generator expression", lambda: next((lambda: 0).__qualname__ for _ in [1]))
def in_function(): return (next((lambda: 0).__qualname__ for _ in [1]), [(lambda: 0).__qualname__ for _ in [1]][0])
show("names in a generator expression in a function", in_function)
show("ParamSpec parts", lambda: (_typing.ParamSpecArgs(1), _typing.ParamSpecKwargs("a"), _typing.ParamSpecArgs(origin=2).__origin__))
show("ParamSpec parts order", lambda: ParamSpec("P").args < ParamSpec("P").args)
show("TypeVarTuple", lambda: (lambda t: (t, list(t), [*t], t.__default__, t.has_default(), TypeVarTuple("Ts", default=()).has_default()))(TypeVarTuple("Ts")))
show("bare TypeVarTuple", lambda: TypeVarTuple("Ts").__typing_subst__(1))
show("NoDefault", lambda: (NoDefault, type(NoDefault), type(NoDefault)() is NoDefault, type(NoDefault).__module__, bool(NoDefault)))
X = TypeVar("X"); Y = TypeVar("Y", default=bytes)
took()
show("substitution", lambda: (list[X][int], dict[X, Y][int, str], dict[X, Y][int], dict[X, list[Y]][int], took()))
show("too few", lambda: dict[X, TypeVar("Z")][int])
show("prepare", lambda: (Y.__typing_prepare_subst__(dict[X, Y], (int,)), Y.__typing_prepare_subst__(dict[X, Y], (int, str))))
show("prepare not there", lambda: X.__typing_prepare_subst__(list[Y], ()))
show("idfunc", lambda: _typing._idfunc(5))
def compile_error(src):
    try: compile(src, "<s>", "exec"); return "compiled"
    except SyntaxError as e: return e.msg
show("errors", lambda: [compile_error(s) for s in ["def f[T = int, U](): pass", "class C[T = int, *Ts]: pass", "type A[**P = [int], T] = int", "def f[T, T](): pass", "def f[T: (yield)](): pass", "type A = (yield)", "type A[T: (x := 1)] = T", "def f[__classdict__](): pass", "class C[T](x := 1): pass", "def f[T](): \n nonlocal T", "type A = await x", "def f[*Ts: int](): pass", "def f[**P: int](): pass", "def f[](): pass", "type A[] = int", "def outer():\n def f[T = int, U](): pass"]])
