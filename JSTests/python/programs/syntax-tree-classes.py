import sys
sys.modules["warnings"] = None
import _ast, _warnings
class Said:
    def __init__(s): s.lines = []
    def write(s, t): s.lines.append(t)
    def flush(s): pass
_warnings.filters[:] = [("always", None, Warning, None, 0)]
def show(label, f):
    said = sys.stderr = Said()
    try: r = repr(f())
    except BaseException as e: r = type(e).__name__ + ": " + str(e)
    sys.stderr = sys.__stderr__
    # What is left out is gone through in the order of a set, which is any order.
    w = sorted(l.split(": ", 1)[1].strip() for l in "".join(said.lines).split("\n") if "Warning: " in l)
    print(label, "=>", r, *(["|", w] if w else []))
print(sorted(k for k in vars(_ast) if not k.startswith("__")))
print([(k, v) for k, v in vars(_ast).items() if isinstance(v, int)], _ast.__name__, _ast.__doc__, [k for k in vars(_ast) if not k.startswith("__")][:9])
for name, c in vars(_ast).items():
    if not isinstance(c, type) or name.startswith("__"): continue
    print(name, [b.__name__ for b in c.__mro__], c.__module__, c.__qualname__, c._fields, c._attributes, c.__match_args__, sorted(vars(c)), c.__dictoffset__ != 0, c.__weakrefoffset__ != 0)
    print("   ", ascii(c.__doc__))
    print("   ", vars(c).get("_field_types"), vars(c).get("_field_types") is vars(c).get("__annotations__"), {k: v for k, v in vars(c).items() if v is None})
A = _ast
show("kinds", lambda: [(k, type(v).__name__) for k, v in sorted(vars(A.AST).items())])
show("AST()", lambda: (A.AST(), vars(A.AST()), A.AST()._fields))
show("AST(1)", lambda: A.AST(1))
show("AST(x=1)", lambda: vars(A.AST(x=1)))
show("Name()", lambda: vars(A.Name()))
show("Name('x')", lambda: (A.Name("x"), vars(A.Name("x")), A.Name("x").ctx is A.Name("y").ctx, type(A.Name("x").ctx)))
show("Name('x', Store())", lambda: A.Name("x", A.Store()))
show("Name(id='x')", lambda: A.Name(id="x"))
show("Name('x', id='y')", lambda: A.Name("x", id="y"))
show("Name too many", lambda: A.Name("x", A.Load(), 3))
show("Name lineno", lambda: vars(A.Name("x", lineno=3, col_offset=4)))
show("Name unknown", lambda: vars(A.Name("x", foo=3)))
show("Module()", lambda: sorted(vars(A.Module()).items()))
show("lists are not shared", lambda: A.Module().body is A.Module().body)
show("Return()", lambda: (vars(A.Return()), A.Return().value, A.Return().end_lineno))
show("no lineno", lambda: A.Return().lineno)
show("BinOp()", lambda: vars(A.BinOp()))
show("Constant()", lambda: vars(A.Constant()))
show("Constant(1)", lambda: (A.Constant(1), vars(A.Constant(1)), A.Constant(1).kind))
show("FunctionDef('f')", lambda: sorted(vars(A.FunctionDef("f"))))
show("arguments()", lambda: sorted(vars(A.arguments()).items()))
show("Dict()", lambda: sorted(vars(A.Dict()).items()))
show("comprehension()", lambda: vars(A.comprehension()))
show("ImportFrom()", lambda: sorted(vars(A.ImportFrom()).items()))
show("MatchSingleton()", lambda: vars(A.MatchSingleton()))
show("Load() is Load()", lambda: (A.Load() is A.Load(), A.Load() == A.Load(), A.Load()))
show("stmt()", lambda: (A.stmt(), vars(A.stmt()), A.expr(1) if 0 else None))
show("mod(1)", lambda: A.mod(1))
show("setting anything", lambda: (n := A.Name("x"), setattr(n, "zzz", 1), n.zzz, vars(n))[2:])
show("deleting", lambda: (n := A.Name("x"), delattr(n, "id"), vars(n), n)[2:3])
show("repr with a field missing", lambda: (n := A.Name("x"), delattr(n, "id"), repr(n))[2])
show("repr depth", lambda: A.BinOp(A.BinOp(A.BinOp(A.BinOp(A.Constant(1), A.Add(), A.Constant(2)), A.Add(), A.Constant(3)), A.Add(), A.Constant(4)), A.Add(), A.Constant(5)))
show("repr lists", lambda: (A.List([]), A.List([A.Constant(1)]), A.List([A.Constant(1), A.Constant(2)]), A.List([A.Constant(i) for i in range(5)]), A.List((A.Constant(1), 2, 3)), A.List([1, "a", None]), A.Global(["a", "b", "c"])))
show("repr of itself", lambda: (n := A.List([]), n.elts.append(n), repr(n))[2])
show("repr of itself by a field", lambda: (n := A.Expr(None), setattr(n, "value", n), repr(n))[2])
show("reduce", lambda: (A.Name("x").__reduce__(), A.AST().__reduce__(), A.Load().__reduce__(), A.Name().__reduce__(), A.BinOp(right=1).__reduce__(), A.Name("x", lineno=1).__reduce__()))
show("reduce args", lambda: A.Name("x").__reduce__(1))
show("replace", lambda: (n := A.Name("x", lineno=1, col_offset=2), m := n.__replace__(id="y"), vars(m), m is n, vars(n.__replace__()), vars(n.__replace__(lineno=9)))[2:])
show("replace unknown", lambda: A.Name("x").__replace__(foo=1))
show("replace positional", lambda: A.Name("x").__replace__(1))
show("replace missing", lambda: A.BinOp(A.Constant(1)).__replace__())
show("replace missing one", lambda: A.BinOp(A.Constant(1), A.Add()).__replace__())
show("replace optional missing", lambda: vars(A.Return().__replace__()))
show("replace attributes missing", lambda: vars(A.Name("x").__replace__(id="z")))
class Mine(A.AST): _fields = ("a", "b")
show("derived", lambda: (Mine(), vars(Mine()), Mine(1), vars(Mine(1, 2)), Mine(1, b=2), Mine._attributes, Mine.__match_args__))
show("derived too many", lambda: Mine(1, 2, 3))
show("derived unknown", lambda: vars(Mine(c=3)))
class Typed(A.AST):
    _fields = ("a", "b", "c", "d", "e")
    _field_types = {"a": int, "b": int | None, "c": list[int], "d": A.expr_context}
show("derived with types", lambda: sorted(vars(Typed()).items()))
class MyName(A.Name): pass
show("derived from Name", lambda: (MyName("x"), vars(MyName("x")), MyName.__mro__[1].__name__, MyName()))
class Bad(A.AST): _fields = 5
show("_fields is no sequence", lambda: Bad())
class Bad2(A.AST): _fields = (1, 2)
show("_fields of ints", lambda: Bad2(1))
show("no _fields", lambda: (delattr(Mine, "_fields"), Mine(), Mine(1))[1:])
show("match", lambda: [(lambda n: (m := None, [m for _ in [0]]) and None)(0)])
def match(n):
    match n:
        case A.Name("x"): return "x"
        case A.Name(id, ctx=A.Store()): return ("store", id)
        case A.BinOp(l, A.Add(), r): return ("add", l, r)
        case A.expr(): return "expr"
        case _: return "?"
show("patterns", lambda: [match(n) for n in (A.Name("x"), A.Name("y", A.Store()), A.Name("y"), A.BinOp(1, A.Add(), 2), A.BinOp(1, A.Sub(), 2), A.Pass())])
show("isinstance", lambda: (isinstance(A.Name("x"), A.expr), isinstance(A.Name("x"), A.AST), isinstance(A.Name("x"), A.stmt), issubclass(A.Add, A.operator), issubclass(A.arg, A.AST)))
show("classes can be changed", lambda: (setattr(A.Name, "zz", 1), A.Name.zz, delattr(A.Name, "zz"), setattr(A.AST, "zz", 2), A.Pass.zz, delattr(A.AST, "zz"))[1::3])
show("hash and eq", lambda: (A.Name("x") == A.Name("x"), (n := A.Name("x")) == n, isinstance(hash(n), int)))
show("bool", lambda: bool(A.Module()))
show("weakref slot", lambda: ("__weakref__" in vars(A.AST), "__weakref__" in vars(A.mod), "__weakref__" in vars(A.Module), "__dict__" in vars(A.AST), "__dict__" in vars(A.mod)))
show("__init__ again", lambda: (n := A.Name("x"), n.__init__("y"), vars(n))[2])
show("__new__", lambda: (vars(A.Name.__new__(A.Name)), A.AST.__new__(A.Name, 1, 2, x=3)))
show("__dict__ set", lambda: (n := A.Name("x"), setattr(n, "__dict__", {"id": "q"}), n.id, vars(n))[2:])
