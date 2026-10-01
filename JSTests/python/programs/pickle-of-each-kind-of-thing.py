# _pickle: what is written for each kind of thing, in each protocol, and what is read back.
import _pickle, pickle, io, collections, enum, dataclasses, fractions, datetime, functools, types, array, re, decimal, operator, sys, copyreg
import re; address = re.compile('0x[0-9a-f]+')
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
        notes = getattr(e, "__notes__", None)
        if notes: r += " | " + " | ".join(notes)
    print(label, "->", address.sub("0x", ascii(r)))
print(pickle.dumps is _pickle.dumps, pickle.loads is _pickle.loads, pickle.Pickler is _pickle.Pickler, pickle.Unpickler is _pickle.Unpickler, pickle.PickleError is _pickle.PickleError, pickle.PickleBuffer is _pickle.PickleBuffer)
print(sorted(n for n in vars(_pickle) if not n.startswith("__")), _pickle.__doc__)
class C:
    def __init__(self, a=1): self.alpha = a; self.beta = [a]
    def __eq__(self, o): return type(o) is type(self) and vars(o) == vars(self)
    def method(self): pass
    class Inner:
        def __eq__(self, o): return type(o) is type(self)
        @staticmethod
        def static(): pass
class Slots:
    __slots__ = ("x", "y")
    def __init__(self): self.x = 1; self.y = "two"
    def __eq__(self, o): return (o.x, o.y) == (self.x, self.y)
class Both(Slots):
    def __init__(self): super().__init__(); self.z = 3
    def __eq__(self, o): return (o.x, o.y, o.z) == (self.x, self.y, self.z)
class State:
    def __getstate__(self): return ("state", 5)
    def __setstate__(self, s): self.got = s
    def __eq__(self, o): return getattr(o, "got", None) == ("state", 5) or getattr(self, "got", None) == ("state", 5)
class NewArgs(int):
    def __new__(cls, v, extra=0): r = super().__new__(cls, v); r.extra = extra; return r
    def __getnewargs__(self): return (int(self), self.extra)
class NewArgsEx(int):
    def __new__(cls, v, *, extra=0): r = super().__new__(cls, v); r.extra = extra; return r
    def __getnewargs_ex__(self): return ((int(self),), {"extra": self.extra})
class L(list): pass
class D(dict): pass
class T(tuple): pass
class S(str): pass
class B(bytes): pass
class St(set): pass
class Color(enum.Enum): RED = 1; GREEN = "g"
class Flag(enum.IntFlag): A = 1; B = 2
N = collections.namedtuple("N", "x y")
@dataclasses.dataclass
class P: x: int; y: list
def function(): pass
l = L([1, 2]); l.attr = "a"
d = D(a=1); d.attr = "b"
values = {"None": None, "True": True, "False": False, "0": 0, "1": 1, "255": 255, "256": 256, "65535": 65535, "65536": 65536, "-1": -1, "-128": -128, "-129": -129, "2**31-1": 2**31 - 1, "2**31": 2**31, "-2**31": -2**31, "-2**31-1": -2**31 - 1, "2**63": 2**63, "-2**63": -2**63,
          "2**64": 2**64, "2**100": 2**100, "-2**100": -2**100, "-2**7": -2**7, "-2**15": -2**15, "-2**23": -2**23, "-2**39": -2**39, "-2**63-1": -2**63 - 1, "2**2040": 2**2040, "-2**2047": -2**2047, "2**2048": 2**2048, "10**30": 10**30, "0.0": 0.0, "-0.0": -0.0, "1.5": 1.5, "1e100": 1e100,
          "inf": float("inf"), "-inf": float("-inf"), "1e-320": 1e-320, "0.1": 0.1, "1j": 1j, "str": "abc", "empty str": "", "latin": "caf\xe9", "wide": "€", "astral": "\U0001f600", "surrogate": "\ud800", "newline": "a\nb\\c\r\x00\x1a", "255 chars": "x" * 255, "256 chars": "x" * 256, "256 wide": "\xe9" * 128,
          "bytes": b"abc", "empty bytes": b"", "high bytes": bytes(range(256)), "256 bytes": b"x" * 256, "bytearray": bytearray(b"abc"), "empty bytearray": bytearray(), "()": (), "(1,)": (1,), "(1,2)": (1, 2), "(1,2,3)": (1, 2, 3), "(1,2,3,4)": (1, 2, 3, 4), "[]": [], "[1]": [1], "[1,2]": [1, 2], "{}": {}, "{1:2}": {1: 2},
          "{1:2,3:4}": {1: 2, 3: 4}, "set()": set(), "{1}": {1}, "{1,2,3}": {1, 2, 3}, "frozenset()": frozenset(), "frozenset": frozenset({1, 2}), "nested": [1, (2, [3, {4: (5, {6})}]), {"k": [frozenset({7})]}], "1000 items": list(range(1000)), "1001 items": list(range(1001)), "2000 items": list(range(2000)),
          "999 keys": dict.fromkeys(range(999)), "1000 keys": dict.fromkeys(range(1000)), "1001 keys": dict.fromkeys(range(1001)), "1000 in a set": set(range(1000)), "1001 in a set": set(range(1001)), "shared": (lambda x: [x, x, (x, x)])([1]), "shared str": (lambda x: [x, x])("shared" * 2), "shared bytes": (lambda x: [x, x])(bytes(5)),
          "C": C(), "list of C": [C(1), C(2), C(3)], "Inner": C.Inner(), "Slots": Slots(), "Both": Both(), "State": State(), "NewArgs": NewArgs(5, 6), "NewArgsEx": NewArgsEx(5, extra=6), "L": l, "D": d, "T": T((1, 2)), "S": S("s"), "B": B(b"b"), "St": St({1}), "enum": Color.RED, "enum str": Color.GREEN, "flag": Flag.A | Flag.B,
          "namedtuple": N(1, 2), "dataclass": P(1, [2]), "class": C, "inner class": C.Inner, "function": function, "builtin": len, "method of a class": C.method, "static": C.Inner.static, "bound builtin": [].append.__self__.__class__, "type": type, "int": int, "NoneType": type(None), "ellipsis type": type(...),
          "NotImplementedType": type(NotImplemented), "Ellipsis": ..., "NotImplemented": NotImplemented, "range": range(1, 10, 2), "slice": slice(1, 2, 3), "complex": 1 + 2j, "Fraction": fractions.Fraction(1, 3), "date": datetime.date(2020, 1, 2), "datetime": datetime.datetime(2020, 1, 2, 3, 4, 5, 6), "timedelta": datetime.timedelta(1, 2, 3),
          "OrderedDict": collections.OrderedDict(a=1, b=2), "defaultdict": collections.defaultdict(list, a=[1]), "deque": collections.deque([1, 2], 5), "Counter": collections.Counter("aab"), "partial": None, "array": array.array("i", [1, 2]), "pattern": re.compile("a+", re.I),
          "Decimal": decimal.Decimal("1.5"), "itemgetter": operator.itemgetter(1), "attrgetter": operator.attrgetter("a.b"), "methodcaller": operator.methodcaller("m", 1, k=2), "SimpleNamespace": types.SimpleNamespace(a=1), "exception": ValueError("x", 1), "OSError": OSError(2, "no"), "KeyError": KeyError("k"),
          "exception with attributes": (lambda e: (setattr(e, "extra", 1), e)[1])(RuntimeError("r")), "ExceptionGroup": ExceptionGroup("g", [ValueError(1)]), "bound method": C(7).method, "dict keys type": type({}.keys()), "mappingproxy type": types.MappingProxyType, "sys.flags type": type(sys.flags), "generic alias": list[int], "union": int | str}
def same(a, b):
    # Not by vars(): an exception that has been asked for its __dict__ is written with it from then on, though there is nothing in it.
    if isinstance(a, BaseException): return type(a) is type(b) and a.args == b.args and getattr(a, "extra", None) == getattr(b, "extra", None)
    if isinstance(a, (operator.itemgetter, operator.attrgetter, operator.methodcaller)): return repr(a) == repr(b)
    if isinstance(a, types.MethodType): return a.__func__ is b.__func__ and a.__self__ == b.__self__
    return a == b and type(a) is type(b)
for label, v in values.items():
    for p in range(6):
        def go():
            data = _pickle.dumps(v, p)
            back = _pickle.loads(data)
            return (data if len(data) < 150 else (len(data), data[:40], data[-40:])), data == pickle._dumps(v, p), same(v, back), same(v, pickle._loads(data))
        attempt("%s %d" % (label, p), go)
