# One str for a name: what CPython calls interned.
import warnings; warnings.simplefilter('ignore')
import sys, marshal, pickle, types, collections, enum, dataclasses, ast, inspect
def same(label, a, b): print(label, a is b, a == b)
I = sys._is_interned
made = "".join(["alpha", "_", "beta"])
print(I("alpha_beta"), I(made), I("alpha beta"), I(""), I("a"), I("\xe9"), I("€"), I("alpha-beta"), I("_"), I("9"), I("a9_"), I("x" * 100), I("\xe9t\xe9"))
same("intern of what was made", sys.intern(made), "alpha_beta")
print(I(made), I(sys.intern(made)))
other = "".join(["gamma", " ", "delta"]); again = "".join(["gamma ", "delta"])
same("not a name: the first to be interned", sys.intern(other), other)
same("and the next is that", sys.intern(again), other)
print(I(other), I(again))
class C:
    alpha_beta = 1
    __slots__ = ()
class D:
    def __init__(self): self.alpha_beta = 1; self.second_one = 2
    def alpha_method(self): pass
    @property
    def alpha_property(self): pass
d = D(); e = D()
same("keys of two instances", list(vars(d))[0], list(vars(e))[0])
same("a key and the literal", list(vars(d))[1], "second_one")
same("asked twice", list(d.__dict__)[0], list(d.__dict__)[0])
same("items", list(d.__dict__.items())[0][0], "alpha_beta")
same("iter", next(iter(d.__dict__)), "alpha_beta")
same("reversed", next(reversed(d.__dict__)), "second_one")
same("popitem", dict(d.__dict__).popitem()[0], "second_one")
same("copy", list(d.__dict__.copy())[0], "alpha_beta")
same("a class", [k for k in vars(D) if k == "alpha_method"][0], "alpha_method")
same("dir", [k for k in dir(d) if k == "second_one"][0], "second_one")
same("dir of a class", [k for k in dir(D) if k == "alpha_method"][0], "alpha_method")
same("dir of a module", [k for k in dir(sys) if k == "intern"][0], "intern")
same("globals", [k for k in globals() if k == "made"][0], "made")
same("vars of a module", [k for k in vars(sys) if k == "intern"][0], "intern")
def f(first_one, *rest_of, key_only=1, **others_too):
    local_one = 1
    return locals(), others_too
same("__name__ of a function", f.__name__, "f")
same("__name__ of a method", D.alpha_method.__name__, "alpha_method")
same("__name__ of a class", D.__name__, "D")
same("co_name", f.__code__.co_name, "f")
for i, n in enumerate(("first_one", "key_only", "rest_of", "others_too", "local_one")): same("co_varnames " + n, f.__code__.co_varnames[i], n)
same("co_names", (lambda: some_global.some_attribute).__code__.co_names[0], "some_global")
same("co_names, an attribute", (lambda: some_global.some_attribute).__code__.co_names[1], "some_attribute")
same("co_consts", (lambda: "a_constant").__code__.co_consts[-1], "a_constant")
same("co_consts, in a tuple", (lambda: (1, "in_a_tuple")).__code__.co_consts[-1][1], "in_a_tuple")
same("co_freevars", (lambda free_one: lambda: free_one)(1).__code__.co_freevars[0], "free_one")
same("co_cellvars", (lambda free_one: lambda: free_one).__code__.co_cellvars[0], "free_one")
l, k = f(1, extra_key=2)
same("locals()", [x for x in l if x == "local_one"][0], "local_one")
same("**kwargs", list(k)[0], "extra_key")
same("**kwargs out of a dict", list(f(1, **{"from_a_dict": 1})[1])[0], "from_a_dict")
same("__kwdefaults__", list(f.__kwdefaults__)[0], "key_only")
def g(annotated_one: int = 0): pass
same("__annotations__", list(g.__annotations__)[0], "annotated_one")
same("a signature", list(inspect.signature(g).parameters)[0], "annotated_one")
setattr(d, "".join(["set", "_by_", "name"]), 1)
same("setattr", [x for x in vars(d) if x == "set_by_name"][0], "set_by_name")
same("across code", (lambda: "shared_literal")(), "shared_literal")
same("across functions", (lambda: "shared_literal")(), (lambda: "shared_literal")())
same("exec", eval("'shared_literal'"), "shared_literal")
ns = {}; exec("def h(): return 'shared_literal'", ns)
same("a function made by exec", ns["h"](), "shared_literal")
same("the key that exec made", list(k for k in ns if k == "h")[0], "h")
same("not a name, in one code", "two words", "two words")
same("worked out", "shared" + "_literal", "shared_literal")
same("marshal", marshal.loads(marshal.dumps("shared_literal")), "shared_literal")
same("marshal, of what was made", marshal.loads(marshal.dumps(made)), "alpha_beta")
same("a namedtuple", collections.namedtuple("N", "field_one field_two")._fields[0], "field_one")
class Color(enum.Enum): RED_ONE = 1
same("an enum", Color.RED_ONE.name, "RED_ONE")
same("__members__", list(Color.__members__)[0], "RED_ONE")
@dataclasses.dataclass
class P: data_field: int = 0
same("a dataclass", dataclasses.fields(P)[0].name, "data_field")
same("asdict", list(dataclasses.asdict(P()))[0], "data_field")
same("ast", ast.parse("name_in_tree").body[0].value.id, "name_in_tree")
same("ast, an attribute", ast.parse("a.attribute_in_tree").body[0].value.attr, "attribute_in_tree")
same("ast, a constant", ast.parse("'constant_in_tree'").body[0].value.value, "constant_in_tree")
same("SimpleNamespace", list(vars(types.SimpleNamespace(space_key=1)))[0], "space_key")
same("__slots__", type("S", (), {"__slots__": ("slot_one",)}).__slots__[0], "slot_one")
try: d.missing_attribute
except AttributeError as err: same("AttributeError.name", err.name, "missing_attribute")
try: missing_global
except NameError as err: same("NameError.name", err.name, "missing_global")
class G:
    def __getattr__(self, name): return name
    def __setattr__(self, name, value): seen.append(name)
seen = []
same("__getattr__", G().asked_for_this, "asked_for_this")
G().set_this = 1
same("__setattr__", seen[0], "set_this")
class SN:
    def __set_name__(self, owner, name): seen.append(name)
class Owner: owned_name = SN()
same("__set_name__", seen[1], "owned_name")
for p in range(6):
    print(p, pickle.dumps([D(), D()], p) == pickle._dumps([D(), D()], p), len(pickle.dumps([D() for _ in range(50)], p)))
