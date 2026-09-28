def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
GenericAlias = type(list[int])
Union = type(int | str)

# ---- GenericAlias
show("repr", lambda: [list[int], dict[str, int], tuple[int, ...], tuple[()], list[list[int]], type[int], list[None], list[...], list["x"], list[1], dict[str, list[dict[int, str]]]])
show("types", lambda: (GenericAlias, GenericAlias.__name__, GenericAlias.__qualname__, GenericAlias.__module__, type(GenericAlias)))
show("all generic builtins", lambda: [c[int] for c in (list, dict, tuple, set, frozenset, type, enumerate, memoryview, staticmethod, classmethod, type(iter(())).__class__)])
show("not generic", lambda: int[str])
show("not generic 2", lambda: str[int])
show("not generic 3", lambda: object[int])
show("attributes", lambda: (list[int].__origin__, list[int].__args__, list[int].__parameters__, list[int].__unpacked__, list[int].__typing_unpacked_tuple_args__))
show("one argument is a tuple", lambda: (list[(int, str)].__args__, list[int,].__args__, list[((int, str),)].__args__))
show("read only", lambda: setattr(list[int], "__origin__", 1))
show("read only 2", lambda: setattr(list[int], "__parameters__", 1))
show("call", lambda: (list[int]([1, 2]), dict[str, int](a=1), tuple[int]((1,)), set[int]()))
class C:
    def __class_getitem__(cls, item): return GenericAlias(cls, item)
    def __init__(self, x=0): self.x = x
show("orig class", lambda: (C[int](5).x, C[int]().__orig_class__, C[int]))
show("no orig class on builtin", lambda: hasattr(list[int](), "__orig_class__"))
class Slotted:
    __slots__ = ()
    __class_getitem__ = classmethod(GenericAlias)
show("slotted", lambda: type(Slotted[int]()).__name__)
show("forwarded attributes", lambda: (list[int].__name__, list[int].__qualname__, list[int].__module__, list[int].append, list[int].__doc__[:20], list[int].__mro__))
show("class is its own", lambda: (list[int].__class__, type(list[int])))
show("blocked", lambda: list[int].__bases__)
show("blocked 2", lambda: list[int].__copy__)
show("missing", lambda: list[int].nope)
show("eq", lambda: (list[int] == list[int], list[int] == list[str], list[int] != list[int], list[int] != list[str], list[int] == list, list[int] == 1, list[int] == set[int]))
show("order", lambda: list[int] < list[int])
show("hash", lambda: (hash(list[int]) == hash(list[int]), hash(list[int]) == hash(list) ^ hash((int,)), len({list[int], list[int], list[str]})))
show("unhashable", lambda: hash(list[[]]))
show("isinstance", lambda: isinstance([], list[int]))
show("issubclass", lambda: issubclass(list, list[int]))
show("mro entries", lambda: list[int].__mro_entries__(()))
class L(list[int]): pass
show("subclass", lambda: (L.__bases__, L.__orig_bases__, L.__mro__, L([1])))
show("reduce", lambda: (list[int].__reduce__(), (*tuple[int],)[0].__reduce__()[0]))
show("dir", lambda: (sorted(set(dir(list[int])) - set(dir(list)) - {"__reduce__", "__reduce_ex__"}), set(dir(list)) <= set(dir(list[int]))))
show("iter", lambda: (list(tuple[int, str]), [*tuple[int]], type(iter(list[int])).__name__, (*tuple[int, str],)[0].__unpacked__, (*tuple[int, str],)[0].__typing_unpacked_tuple_args__, (*list[int],)[0].__typing_unpacked_tuple_args__))
def it():
    i = iter(list[int]); a = next(i)
    try: next(i)
    except StopIteration: return (a, "stopped", i.__reduce__(), iter(i) is i)
show("iterator", it)
show("iterator reduce", lambda: iter(list[int]).__reduce__())
show("starred eq", lambda: ((*tuple[int],)[0] == tuple[int], (*tuple[int],)[0] == (*tuple[int],)[0]))
show("new", lambda: (GenericAlias(list, int), GenericAlias(list, (int, str)), GenericAlias(1, 2), GenericAlias("a", ())))
show("new wrong", lambda: GenericAlias(list))
show("new wrong 2", lambda: GenericAlias(list, int, str))
show("new keyword", lambda: GenericAlias(origin=list, args=int))
class MyAlias(GenericAlias):
    def extra(self): return "extra"
show("derived", lambda: (MyAlias(list, int), MyAlias(list, int).extra(), type(MyAlias(list, int)).__name__, MyAlias(list, int) == list[int], isinstance(MyAlias(list, int), GenericAlias), MyAlias(list, int)()))
show("not generic alias", lambda: list[int][str])
show("callable", lambda: (callable(list[int]), callable(int | str)))
show("bool and str", lambda: (bool(list[int]), str(list[int]), f"{list[int]}"))
show("operators", lambda: list[int] + 1)
show("list argument", lambda: (GenericAlias(list, ([int, str], bool)), GenericAlias(list, ([],))))

# ---- with something that looks like a type variable
class TV:
    def __init__(self, n): self.n = n
    def __repr__(self): return "~" + self.n
    def __typing_subst__(self, arg): return arg
T = TV("T"); S = TV("S")
show("parameters", lambda: (list[T].__parameters__, dict[T, S].__parameters__, dict[T, T].__parameters__, dict[T, list[S]].__parameters__, list[[T, S]].__parameters__, tuple[(T, int), S].__parameters__))
show("substitute", lambda: (list[T][int], dict[T, S][int, str], dict[T, T][int], dict[T, list[S]][str, int], dict[str, T][int], list[[T]][str], tuple[(T, int), S][str, bytes], list[T][S], list[T][list[S]][int]))
show("too many", lambda: list[T][int, str])
show("too few", lambda: dict[T, S][int])
show("unpack in substitution", lambda: dict[T, S][*tuple[int, str]])
show("unpack unbounded", lambda: list[T][*tuple[int, ...]])
show("union parameters", lambda: ((list[T] | S.__class__).__parameters__, (list[T] | dict[S, T]).__parameters__, (list[T] | dict[S, T])[int, str]))
show("union collapses", lambda: (list[T] | list[int])[int])
class Prep(TV):
    def __typing_prepare_subst__(self, alias, args): return args + (bytes,) * (2 - len(args))
P = Prep("P")
show("prepare", lambda: dict[P, T][int])
class VT(TV):
    __typing_is_unpacked_typevartuple__ = True
    def __typing_subst__(self, arg): return (arg, arg)
show("unpacked variable", lambda: tuple[VT("V")][int])
class BadVT(VT):
    def __typing_subst__(self, arg): return arg
show("unpacked variable wrong", lambda: tuple[BadVT("V")][int])

# ---- Union
show("union repr", lambda: [int | str, int | None, None | int, int | str | bytes, list[int] | str, int | list[int] | None, type | int, int | (str | bytes), (int | str) | (bytes | int)])
show("union type", lambda: (Union, Union.__name__, Union.__qualname__, Union.__module__, type(Union)))
show("collapse", lambda: (int | int, int | int | int, (int | str) | int, (int | str) | str | int, list[int] | list[int]))
show("union attributes", lambda: ((int | str).__args__, (int | None).__args__, (int | str).__parameters__, (int | str).__origin__, (int | str).__name__, (int | str).__qualname__, (int | str).__module__))
show("union eq", lambda: (int | str == str | int, int | str == int | bytes, int | str != str | int, (int | str) == 1, (int | str) == int, int | str | None == None | str | int))
show("union hash", lambda: (hash(int | str) == hash(str | int), hash(int | str) == hash(frozenset({int, str})), len({int | str, str | int, int | bytes})))
show("union order", lambda: (int | str) < (int | str))
show("isinstance union", lambda: (isinstance(1, int | str), isinstance("a", int | str), isinstance(1.0, int | str), isinstance(None, int | None), isinstance(1, (int | str, float)), isinstance(True, str | int)))
show("issubclass union", lambda: (issubclass(int, int | str), issubclass(bool, int | str), issubclass(float, int | str), issubclass(type(None), int | None)))
show("isinstance union with alias", lambda: isinstance(1, str | list[int]))
show("isinstance union with alias found first", lambda: isinstance(1, int | list[int]))
show("bad operand", lambda: int | 1)
show("bad operand 2", lambda: 1 | int)
show("bad operand 3", lambda: int | "str")
show("bad operand 4", lambda: None | None)
show("bad operand 5", lambda: list[int] | 1)
show("cannot create", lambda: Union())
show("cannot create 2", lambda: Union(int, str))
show("class getitem", lambda: (Union[int, str], Union[int], Union[int, int], Union[int, None], Union[(int, str)], Union[int, Union[str, bytes]], Union[list[int], int]))
show("class getitem empty", lambda: Union[()])
show("cannot subclass instance", lambda: type("X", (int | str,), {}))
show("cannot subclass type", lambda: type("X", (Union,), {}))
show("union not generic", lambda: (int | str)[int])
show("union read only", lambda: setattr(int | str, "__args__", ()))
show("union no attribute", lambda: (int | str).nope)
show("union no dict", lambda: setattr(int | str, "x", 1))
show("union call", lambda: (int | str)())
class Meta(type):
    def __or__(cls, other): return "meta or"
    def __ror__(cls, other): return "meta ror"
class M(metaclass=Meta): pass
show("metaclass wins", lambda: (M | int, int | M, M | None, None | M))
class Unhashable(type):
    __hash__ = None
class U1(metaclass=Unhashable): pass
class U2(metaclass=Unhashable): pass
show("unhashable members", lambda: (int | U1, (int | U1).__args__, U1 | U1, (U1 | U2 | U1).__args__, (int | U1) == (U1 | int), (U1 | U2) == (U2 | U1), (int | U1) == (int | U2), (int | U1) == (int | str)))
show("unhashable hash", lambda: hash(int | U1))
show("user classes", lambda: (C | int, C | None, C[int] | C, (C | int).__args__))
show("in match", lambda: [("int or str" if isinstance(v, int | str) else "other") for v in (1, "a", 2.0)])
show("own attributes", lambda: (sorted(vars(GenericAlias)), sorted(vars(Union)), sorted(vars(type(iter(list[int]))))))
show("kinds", lambda: sorted({type(v).__name__ for v in vars(GenericAlias).values()} | {type(v).__name__ for v in vars(Union).values()}))
show("staticmethod is callable", lambda: (staticmethod(len)([1, 2]), callable(staticmethod(len)), callable(classmethod(len)), callable(property())))
show("annotations", lambda: (lambda f: f.__annotations__)(eval("lambda: 0") if False else __import__("builtins").eval("f", {"f": ann})))
def ann(a: list[int], b: int | None = None) -> dict[str, list[int] | None]: pass
show("annotations", lambda: ann.__annotations__)
