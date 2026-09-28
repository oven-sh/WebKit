import builtins
globals().update({name: value for name, value in vars(builtins).items() if isinstance(value, type)})
repr = builtins.repr; len = builtins.len; getattr = builtins.getattr
class Plain:
    kind = "plain"
    def hello(self): return "hello"
    @classmethod
    def make(cls): return cls.__name__
class Other:
    def hello(self): return "other"
class Slotted:
    __slots__ = ("a",)
class MyDict(dict): pass
class MyList(list): pass
def name_of(o): return type(o).__name__
def set_class(o, c):
    o.__class__ = c
    return type(o).__name__
list_iterator = type(iter([]))
dict_keys = type({}.keys())
function = type(name_of)
generator = type((x for x in ()))
samples = {"dict": {}, "MyDict": MyDict(), "set": set(), "tuple": (), "range": range(1), "slice": slice(1), "complex": 1j, "property": property(), "staticmethod": staticmethod(len), "list_iterator": iter([]),
           "dict_keys": {}.keys(), "ValueError": ValueError(), "Plain": Plain(), "MyList": MyList(), "bound": Plain().hello, "builtin": [].append, "memoryview": memoryview(b""), "frozenset": frozenset(), "Slotted": Slotted()}
