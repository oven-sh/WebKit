# What gc.is_tracked() says of each kind of thing
import gc, collections, datetime, io, types, zoneinfo, functools, itertools, re, array
class C: pass
class S: __slots__ = ()
class SS: __slots__ = ("a",)
def f(): pass
def g(): yield
things = {"None": None, "int": 1, "big int": 10**30, "float": 1.5, "complex": 1j, "bool": True, "str": "a", "bytes": b"a", "bytearray": bytearray(), "Ellipsis": ..., "NotImplemented": NotImplemented, "list": [], "list of one": [1], "dict": {}, "dict of atoms": {"a": 1}, "dict of a list": {"a": []}, "tuple": (), "tuple of atoms": (1, 2), "tuple of a list": ([],),
          "set": set(), "set of one": {1}, "frozenset": frozenset(), "frozenset of one": frozenset({1}), "range": range(3), "slice": slice(1), "instance": C(), "no slots": S(), "slots": SS(), "class": C, "int the class": int, "type": type, "object the class": object, "object()": object(), "function": f, "builtin": len, "method": C().__init__, "bound": [].append, "generator": g(),
          "module": gc, "code": f.__code__, "lambda": lambda: 0, "iter of a list": iter([]), "iter of a str": iter(""), "memoryview": memoryview(b""), "property": property(), "staticmethod": staticmethod(f), "deque": collections.deque(), "OrderedDict": collections.OrderedDict(), "defaultdict": collections.defaultdict(), "Counter": collections.Counter(), "namedtuple": collections.namedtuple("N", "a")(1),
          "date": datetime.date(1, 1, 1), "datetime": datetime.datetime(1, 1, 1), "aware": datetime.datetime(1, 1, 1, tzinfo=datetime.UTC), "timedelta": datetime.timedelta(1), "timezone": datetime.UTC, "date the class": datetime.date, "ZoneInfo the class": zoneinfo.ZoneInfo, "BytesIO": io.BytesIO(), "StringIO": io.StringIO(), "partial": functools.partial(f), "count": itertools.count(), "pattern": re.compile("a"),
          "array": array.array("b"), "exception": ValueError(), "exception the class": ValueError, "SimpleNamespace": types.SimpleNamespace(), "mappingproxy": types.MappingProxyType({}), "dict keys": {}.keys(), "cell": (lambda x: lambda: x)(1).__closure__[0], "enumerate": enumerate([]), "zip": zip(), "map": map(f, []), "weakref": __import__("weakref").ref(C), "GenericAlias": list[int], "union": int | str, "deque the class": collections.deque,
          "OrderedDict the class": collections.OrderedDict, "io.BytesIO the class": io.BytesIO, "super": super(C, C()), "classmethod": classmethod(f), "reversed": reversed([]), "str iterator": iter("a"), "range iterator": iter(range(1)), "wrapper descriptor": int.__add__, "method descriptor": str.join, "getset": type.__dict__["__dict__"], "member": SS.a}
for n, v in things.items(): print(n, gc.is_tracked(v))
print("done")
