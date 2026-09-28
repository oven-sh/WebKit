import sys, math
def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
def noaddr(s): return s.split(" at 0x")[0] + (">" + s.split(">", 1)[1] if " at 0x" in s else "")

# what the callables that are built in are
things = {"len": len, "[].append": [].append, "list.append": list.append, "int.__add__": int.__add__, "(1).__add__": (1).__add__, "dict.fromkeys": dict.fromkeys, "dict.__dict__['fromkeys']": dict.__dict__["fromkeys"],
          "int.__new__": int.__new__, "math.sqrt": math.sqrt, "str.maketrans": bytes.maketrans, "True.__and__": True.bit_length, "{}.__contains__": {}.__contains__, "list.__getitem__": list.__getitem__, "object.__init__": object.__init__}
for name, f in things.items():
    show(name, lambda: (type(f).__name__, noaddr(repr(f)), f.__name__, f.__qualname__, f.__text_signature__, (f.__doc__ or "")[:40]))
    show(name + " self", lambda: f.__self__.__name__ if type(f.__self__) is type(sys) else noaddr(repr(f.__self__)))
    show(name + " objclass", lambda: f.__objclass__)
    show(name + " module", lambda: f.__module__)
    if type(f).__name__ != "classmethod_descriptor":
        show(name + " reduce", lambda: noaddr(repr(f.__reduce__())))
l = [1]
show("equal", lambda: (l.append == l.append, l.append is l.append, l.append == [1].append, l.append != l.pop, hash(l.append) == hash(l.append), {l.append: 1}[l.append], (1).__add__ == (1).__add__, (1).__add__ == (2).__add__, len == len, len != abs, l.append == 1))
show("ordering", lambda: len < abs)
show("read only", lambda: setattr(len, "__name__", "x"))
show("no dict", lambda: setattr(len, "x", 1))
show("__get__", lambda: (noaddr(repr(list.append.__get__([], list))), list.append.__get__(None, list) is list.append, noaddr(repr(int.__add__.__get__(1))), noaddr(repr(dict.__dict__["fromkeys"].__get__(None, dict))), noaddr(repr(dict.__dict__["fromkeys"].__get__({})))))
show("__get__ nothing", lambda: dict.__dict__["fromkeys"].__get__(None, None))
show("__get__ wrong", lambda: dict.__dict__["fromkeys"].__get__(None, int))
show("__get__ not a type", lambda: dict.__dict__["fromkeys"].__get__(None, 1))

# the arguments
for label, f in {
    "no arguments": lambda: [].clear(1), "one argument": lambda: [].append(), "one argument, two": lambda: [].append(1, 2), "positional few": lambda: [].index(), "positional many": lambda: [].index(1, 2, 3, 4), "exactly two": lambda: "".replace("a"),
    "no keywords": lambda: [].append(x=1), "no keywords 2": lambda: [].index(value=1), "no keywords 3": lambda: [].clear(x=1), "wrapper": lambda: (1).__add__(), "wrapper many": lambda: (1).__add__(1, 2), "wrapper keywords": lambda: (1).__add__(x=1),
    "wrapper none": lambda: (1).__neg__(1), "pow": lambda: (1).__pow__(), "setitem": lambda: [].__setitem__(1), "get": lambda: property().__get__(), "unbound": lambda: list.append(), "unbound wrapper": lambda: int.__add__(),
    "wrong self": lambda: list.append(1, 2), "wrong self wrapper": lambda: int.__add__("a", 1), "inherited": lambda: True.bit_length(1), "inherited bound": lambda: (lambda m: m(1))(True.bit_length), "class method": lambda: dict.fromkeys(),
    "class method many": lambda: dict.fromkeys(1, 2, 3), "function": lambda: len(), "function many": lambda: len(1, 2), "function keywords": lambda: len(obj=1), "module function": lambda: math.sqrt(), "module function 2": lambda: math.sqrt(1, 2),
    "sort positional": lambda: [].sort(1), "sort unknown": lambda: [].sort(keys=1), "sort unknown far": lambda: [].sort(zzz=1), "sort fine": lambda: [3, 1].sort(key=None, reverse=True), "sorted none": lambda: sorted(), "sorted two": lambda: sorted([], 1),
    "sorted unknown": lambda: sorted([], revers=True), "sorted by name": lambda: sorted(iterable=[]), "print unknown": lambda: print(sepp=""), "print far": lambda: print(zzz=""), "round": lambda: round(), "round by name": lambda: round(number=1.5, ndigits=0),
    "round twice": lambda: round(1.5, number=1), "round many": lambda: round(1, 2, 3), "round unknown": lambda: round(1, digits=1), "int base": lambda: int("11", base=2), "int many": lambda: int(1, 2, 3), "int unknown": lambda: int("1", bas=2),
    "str.split": lambda: "a b".split(sep=" ", maxsplit=1), "str.split many": lambda: "a".split(1, 2, 3), "str.split unknown": lambda: "a".split(seps=1), "str.split twice": lambda: "a".split(" ", sep=" "), "encode": lambda: "a".encode(encoding="utf-8", errors="strict"),
    "encode unknown": lambda: "a".encode(error="x"), "enumerate": lambda: list(enumerate("a", start=1)), "enumerate by name": lambda: list(enumerate(iterable="a")), "enumerate unknown": lambda: enumerate("a", strt=1), "sum": lambda: sum([1], start=2), "sum unknown": lambda: sum([1], star=2),
    "pow": lambda: pow(2, 3, mod=5), "pow names": lambda: pow(base=2, exp=3), "pow missing": lambda: pow(2), "pow missing name": lambda: pow(exp=2), "divmod": lambda: divmod(1), "isinstance": lambda: isinstance(1), "getattr": lambda: getattr(1), "max": lambda: max(), "min unknown": lambda: min(1, 2, ky=1),
    "expandtabs": lambda: "\t".expandtabs(tabsize=2), "splitlines": lambda: "a\n".splitlines(keepends=True), "to_bytes": lambda: (1).to_bytes(length=2, byteorder="big", signed=False), "to_bytes unknown": lambda: (1).to_bytes(2, "big", sign=1), "from_bytes": lambda: int.from_bytes(b"\1", byteorder="big"),
    "bytes.hex": lambda: b"ab".hex(sep=":", bytes_per_sep=1), "decode": lambda: b"a".decode(encoding="ascii"), "prepare": lambda: type.__prepare__(), "zip strict": lambda: list(zip("a", strict=True)), "zip unknown": lambda: zip("a", strct=1),
}.items():
    show(label, f)

# what things are for
show("type docs", lambda: [(t.__name__, (t.__doc__ or "?").split("\n")[0][:50]) for t in (int, list, dict, str, ValueError, type(None), type(len), range, object)])
show("descriptor docs", lambda: (int.real.__doc__, type(int.real).__name__, complex.real.__doc__, type(complex.real).__name__, range.start.__doc__, type(range.start).__name__, type.__dict__["__dict__"].__doc__, BaseException.args.__doc__))
show("module docs", lambda: (math.__doc__, sys.__doc__[:40], __builtins__.__doc__[:40], type(sys)("x").__doc__, type(sys)("math").__doc__))
show("class docs", lambda: (type("C", (), {}).__doc__, type("C", (int,), {}).__doc__, type("C", (), {"__doc__": "d"}).__doc__))
