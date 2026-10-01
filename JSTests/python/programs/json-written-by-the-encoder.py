# The encoder of _json, asked directly and by way of json.
import _json, json, json.encoder, decimal, enum, collections, math, sys, fractions, types
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
        notes = getattr(e, "__notes__", None)
        if notes: r += " | " + " | ".join(notes)
    print(label, "->", ascii(r))
class S(str): pass
def default(o): raise TypeError("default: " + type(o).__name__)
def make(markers=None, default=default, encoder=_json.encode_basestring_ascii, indent=None, key_separator=": ", item_separator=", ", sort_keys=False, skipkeys=False, allow_nan=True):
    return _json.make_encoder(markers, default, encoder, indent, key_separator, item_separator, sort_keys, skipkeys, allow_nan)
e = make({})
print(type(e).__name__, e.markers, e.default is default, e.encoder is _json.encode_basestring_ascii, e.indent, repr(e.key_separator), repr(e.item_separator), e.sort_keys, e.skipkeys, hasattr(e, "allow_nan"))
attempt("set a member", lambda: setattr(e, "indent", 1))
attempt("markers: a list", lambda: make([]))
attempt("markers: a dict subclass", lambda: make(collections.OrderedDict())([1], 0))
attempt("key_separator: bytes", lambda: make(key_separator=b":"))
attempt("key_separator: None", lambda: make(key_separator=None))
attempt("item_separator: int", lambda: make(item_separator=1))
attempt("separators of a subclass", lambda: make(key_separator=S("="), item_separator=S(";"))({"a": [1, 2]}, 0))
attempt("flags are anything", lambda: (lambda x: (x.sort_keys, x.skipkeys))(make(sort_keys=[1], skipkeys="")))
class Bad:
    def __bool__(self): raise ZeroDivisionError("bool")
attempt("a flag that raises", lambda: make(sort_keys=Bad()))
attempt("eight arguments", lambda: _json.make_encoder(None, default, str, None, ":", ",", 0, 0))
attempt("by keyword", lambda: _json.make_encoder(markers=None, default=default, encoder=str, indent=None, key_separator=":", item_separator=",", sort_keys=0, skipkeys=0, allow_nan=0)([1, "a"], 0))
attempt("call: none", lambda: e())
attempt("call: one", lambda: e(1))
attempt("call: three", lambda: e(1, 0, 0))
attempt("call: keywords", lambda: e(obj=[1], _current_indent_level=0))
attempt("call: unknown keyword", lambda: e([1], level=0))
for level in (0, 1, 3, -1, 2**70, 1.5, "0", None, True):
    attempt("level %r" % (level,), lambda: e([1, [2]], level))
    attempt("level %r, indented" % (level,), lambda: make(indent="  ")([1, [2], {"a": {}}], level))
print("===== values")
class I(int): pass
class I2(int):
    def __repr__(self): return "I2!"
    __str__ = __repr__
class F(float): pass
class F2(float):
    def __repr__(self): return "F2!"
class L(list): pass
class T(tuple): pass
class D(dict): pass
class D2(dict):
    def items(self): return [("x", 1), ("y", 2)]
class D3(dict):
    def items(self): return [("x", 1, 2)]
class D4(dict):
    def items(self): return 5
class D5(dict):
    def items(self): return iter([["x", 1]])
class D6(dict):
    items = None
class L2(list):
    def __iter__(self): return iter([9, 9])
    def __len__(self): return 7
class Color(enum.IntEnum): RED = 1
class Name(enum.StrEnum): A = "a"
class Fl(enum.IntFlag): X = 1; Y = 2
N = collections.namedtuple("N", "a b")
values = {"None": None, "True": True, "False": False, "0": 0, "-1": -1, "big": 2**100, "-big": -2**100, "2**31": 2**31, "2**63": 2**63, "I": I(5), "I2": I2(5), "IntEnum": Color.RED, "IntFlag": Fl.X | Fl.Y, "StrEnum": Name.A, "1.0": 1.0, "-0.0": -0.0, "1e100": 1e100, "1e-7": 1e-7, "0.1": 0.1, "1e16": 1e16,
          "1e22": 1e22, "F": F(1.5), "F2": F2(1.5), "nan": math.nan, "inf": math.inf, "-inf": -math.inf, "F nan": F(math.nan), "str": "a\n\xe9€\U0001f600", "S": S("s"), "[]": [], "()": (), "{}": {}, "L()": L(), "T()": T(), "D()": D(), "[1]": [1], "(1,)": (1,), "L": L([1, 2]), "T": T((1, 2)), "N": N(1, 2),
          "L2": L2([1, 2, 3]), "nested": [1, [2, [3, [], {}]], {"a": [], "b": {}}], "dict": {"a": 1, "b": [2]}, "D": D(a=1), "D2": D2(q=1), "D2 empty": D2(), "D3": D3(q=1), "D4": D4(q=1), "D5": D5(q=1), "D6": D6(q=1), "OrderedDict": collections.OrderedDict(b=1, a=2), "defaultdict": collections.defaultdict(int, a=1),
          "Counter": collections.Counter("aab"), "set": {1}, "bytes": b"a", "complex": 1j, "Decimal": decimal.Decimal("1.5"), "Fraction": fractions.Fraction(1, 2), "object": object, "range": range(3), "generator": (i for i in ()), "deque": collections.deque([1]), "UserDict": collections.UserDict(a=1),
          "mappingproxy": types.MappingProxyType({"a": 1}), "in a list": [1, {2}], "in a dict": {"k": [1, {"j": {3}}]}, "in a tuple": (1, (2, b"x")), "Ellipsis": ..., "NotImplemented": NotImplemented, "bytearray": bytearray(b"a"), "dict keys": {}.keys()}
settings = {"plain": {}, "indent": {"indent": "  "}, "not ascii": {"encoder": _json.encode_basestring}, "no nan": {"allow_nan": False}, "markers": {"markers": {}}, "sorted": {"sort_keys": True}, "compact": {"key_separator": ":", "item_separator": ","}, "tabs, sorted": {"indent": "\t", "sort_keys": True, "item_separator": ","},
            "empty indent": {"indent": ""}, "own encoder": {"encoder": lambda s: "<" + s + ">"}}
for sl, s in settings.items():
    print("=====", sl)
    for vl, v in values.items():
        attempt(vl, lambda: make(**s)(v, 0))
print("===== keys")
keys = {"str": "a", "S": S("a"), "StrEnum": Name.A, "int": 1, "big": 2**70, "I2": I2(1), "IntEnum": Color.RED, "float": 1.5, "F2": F2(1.5), "nan": math.nan, "inf": math.inf, "True": True, "False": False, "None": None, "tuple": (1, 2), "bytes": b"a", "frozenset": frozenset(), "complex": 1j, "object": object}
for kl, k in keys.items():
    attempt(kl, lambda: make()({k: 1}, 0))
    attempt(kl + ", skipped", lambda: make(skipkeys=True)({k: 1, "z": 2}, 0))
    attempt(kl + ", skipped and indented", lambda: make(skipkeys=True, indent=" ")({k: 1}, 0))
    attempt(kl + ", no nan", lambda: make(allow_nan=False)({k: 1}, 0))
attempt("all skipped", lambda: make(skipkeys=True)({(1,): 1, (2,): 2}, 0))
attempt("all skipped, indented", lambda: make(skipkeys=True, indent="  ")({(1,): 1, (2,): 2}, 0))
attempt("first skipped", lambda: make(skipkeys=True, indent="  ")({(1,): 1, "a": 2, (2,): 3, "b": 4}, 0))
attempt("sorted, mixed", lambda: make(sort_keys=True)({1: 1, "a": 2}, 0))
attempt("sorted, ints", lambda: make(sort_keys=True)({10: 1, 9: 2, 1.5: 3, True: 4}, 0))
attempt("sorted by value when the keys are equal", lambda: make(sort_keys=True)(D2(q=1), 0))
attempt("1 and '1'", lambda: make()({1: "a", "1": "b", 1.0: "c"}, 0))
