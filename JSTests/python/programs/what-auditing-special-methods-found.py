# What was found to be wrong by giving each special method each of eighty things to do, and trying everything that would call it: audits/special-methods.py.

import _warnings

_warnings.filters.insert(0, ("ignore", None, Warning, None, 0))


def t(label, f):
    try:
        r = f()
    except BaseException as e:
        r = type(e).__name__ + ": " + str(e)
    print(label, "=>", r)


def kind(value):
    return type(value).__name__ + ":" + repr(value)


class MyStr(str):
    pass


class MyInt(int):
    pass


class MyBytes(bytes):
    pass


def make(**namespace):
    return type("X", (), namespace)()


# ---- A special method need not be a function
class Callable:
    def __call__(self, *arguments):
        return ("called with", len(arguments))


class Descriptor:
    def __get__(self, instance, owner):
        return lambda *arguments: ("got for", type(instance).__name__, owner.__name__, len(arguments))


for label, method in (("a static method", staticmethod(lambda *a: ("static", len(a)))), ("a class method", classmethod(lambda *a: ("class", a[0].__name__, len(a)))), ("something that can be called", Callable()), ("what has a __get__()", Descriptor()),
                      ("a property", property(lambda self: lambda *a: ("property", len(a)))), ("None", None), ("5", 5)):
    for name, use in (("__add__", lambda x: x + 1), ("__radd__", lambda x: 1 + x), ("__iadd__", lambda x: x.__iadd__(1)), ("__eq__", lambda x: x == 1), ("__lt__", lambda x: x < 1), ("__neg__", lambda x: -x), ("__getitem__", lambda x: x[1]), ("__call__", lambda x: x(1, 2)), ("__getattr__", lambda x: x.nope),
                      ("__getattribute__", lambda x: x.nope), ("__pow__", lambda x: pow(x, 2, 3)), ("__divmod__", lambda x: divmod(x, 1)), ("__enter__", lambda x: x.__enter__()), ("__format__", lambda x: x.__format__("")), ("__contains__", lambda x: 1 in x), ("__set_name__", lambda x: type("C", (), {"a": x}).a is x)):
        t("%s is %s" % (name, label), lambda: use(make(**{name: method})))
for label, method in (("a static method", staticmethod(lambda *a: 7)), ("a class method", classmethod(lambda *a: 7)), ("what has a __get__()", type("D", (), {"__get__": lambda s, i, o: lambda: 7})())):
    t("__hash__ is " + label, lambda: (hash(make(__hash__=method)), len({make(__hash__=method): 1}), hash((make(__hash__=method),)) == hash((7,))))
    t("__len__ is " + label, lambda: (len(make(__len__=method)), bool(make(__len__=method))))
    t("__index__ is " + label, lambda: (list(range(10))[make(__index__=method)], bin(make(__index__=method))))
t("__setattr__ and __delattr__", lambda: (setattr(make(__setattr__=staticmethod(lambda *a: print("   ", "set", a))), "a", 1), delattr(make(__delattr__=staticmethod(lambda *a: print("   ", "deleted", a))), "a")))
for name in ("__set__", "__delete__"):
    holder = type("Y", (), {"a": make(**{name: staticmethod(lambda *a: print("   ", name, len(a)))})})()
    t(name + " is a static method", lambda: setattr(holder, "a", 1) if name == "__set__" else delattr(holder, "a"))
t("__init__ is a static method", lambda: type("X", (), {"__init__": staticmethod(lambda *a: print("   ", "init", a))})(1, 2) is not None)
t("__init__ is len", lambda: type("X", (), {"__init__": len})())
t("__init__ is a class method that returns something", lambda: type("X", (), {"__init__": classmethod(lambda *a: 1)})())
t("__new__ is a class method", lambda: type("X", (), {"__new__": classmethod(lambda *a: [x.__name__ for x in a])})())
t("__new__ has a __get__()", lambda: type("X", (), {"__new__": Descriptor()})(1))
t("__buffer__ is a static method", lambda: bytes(make(__buffer__=staticmethod(lambda flags: memoryview(b"ab")))))
t("__buffer__ is len", lambda: bytes(make(__buffer__=len)))

# ---- divmod() goes about it as an operator does


class Both:
    def __divmod__(self, other):
        return "divmod"

    def __rdivmod__(self, other):
        return "rdivmod"


class Derived(Both):
    def __rdivmod__(self, other):
        return "Derived.rdivmod"


class OnlyReflected:
    def __rdivmod__(self, other):
        return "rdivmod"


t("two of a kind that have only the reflected", lambda: divmod(OnlyReflected(), OnlyReflected()))
t("a class derived from the other's goes first", lambda: (divmod(Both(), Derived()), divmod(Derived(), Both()), divmod(Both(), Both()), divmod(1, OnlyReflected()), divmod(1.5, Both())))

# ---- What is asked how many there are of it, and what is not


class Counted:
    def __init__(self, **methods):
        self.asked = []
        self.methods = methods

    def __iter__(self):
        self.asked.append("iter")
        return iter([1, 2])

    def __len__(self):
        self.asked.append("len")
        return self.methods["len"]()


class Hinted(Counted):
    __len__ = None

    def __length_hint__(self):
        self.asked.append("hint")
        return self.methods["hint"]()


del Hinted.__len__


class OnlyHinted:
    def __init__(self, hint):
        self.asked = []
        self.hint = hint

    def __iter__(self):
        self.asked.append("iter")
        return iter([1, 2])

    def __length_hint__(self):
        self.asked.append("hint")
        return self.hint()


def raiser(error):
    def f():
        raise error
    return f


USES = (("list(x)", lambda x: list(x)), ("[*x]", lambda x: [*x]), ("(*x,)", lambda x: (*x,)), ("tuple(x)", lambda x: tuple(x)), ("f(*x)", lambda x: (lambda *a: a)(*x)), ("f(0, *x)", lambda x: (lambda *a: a)(0, *x)), ("sorted(x)", lambda x: sorted(x)), ("set(x)", lambda x: set(x)), ("[].extend(x)", lambda x: [].extend(x)),
        ("bytes(x)", lambda x: bytes(x)), ("bytearray(x)", lambda x: bytearray(x)), ("bytearray().extend(x)", lambda x: bytearray().extend(x)), ("min(x)", lambda x: min(x)), ("a, b = x", lambda x: (lambda a, b: (a, b))(*[*x][:0], *(lambda: (yield from x))())), ("dict.fromkeys(x)", lambda x: dict.fromkeys(x)))
for label, use in USES:
    x = Counted(len=lambda: 2)
    t(label, lambda: use(x))
    y = OnlyHinted(lambda: 2)
    use(y)
    print("   ", x.asked, y.asked)
for label, hint in (("None", lambda: None), ("-1", lambda: -1), ("1.5", lambda: 1.5), ("'a'", lambda: "a"), ("True", lambda: True), ("MyInt", lambda: MyInt(1)), ("NotImplemented", lambda: NotImplemented), ("2 ** 63", lambda: 2 ** 63), ("2 ** 63 - 1", lambda: 2 ** 63 - 1), ("2 ** 61", lambda: 2 ** 61),
                    ("raises TypeError", raiser(TypeError("t"))), ("raises ValueError", raiser(ValueError("v"))), ("raises StopIteration", raiser(StopIteration()))):
    t("a hint of " + label, lambda: (list(OnlyHinted(hint)), [*OnlyHinted(hint)], sorted(OnlyHinted(hint))))
    t("for bytes", lambda: bytes(OnlyHinted(hint)))
    # CPython 3.14.7 raises MemoryError for this one as it should, and then complains of the bytearray that it did not finish making.
    if label != "2 ** 61":
        t("for a bytearray", lambda: (lambda b: (b.extend(OnlyHinted(hint)), b)[1])(bytearray()))
    t("a length of " + label, lambda: list(Counted(len=hint)))
t("added to a list that has something in it", lambda: (lambda l: (l.extend(OnlyHinted(lambda: 2 ** 63 - 1)), l)[1])([0]))
t("it is the iterator that join() asks", lambda: (lambda x: ("".join(map(str, x)), x.asked))(Counted(len=raiser(ValueError("not asked")))))
t("and a slice that is assigned to", lambda: (lambda l, x: (l.__setitem__(slice(None), x), l, x.asked)[1:])([0], Counted(len=raiser(ValueError("not asked")))))
t("what goes by __getitem__() is asked by way of its iterator", lambda: (lambda l: (l.__setitem__(slice(None), make(__getitem__=lambda s, i: [1, 2][i], __len__=raiser(ValueError("asked")))), l))([0]))
t("what a slice says is asked before what is put there is gone through", lambda: (lambda log: ([0, 1].__setitem__(slice(make(__index__=lambda s: log.append("index") or 0), None), make(__iter__=lambda s: log.append("iter") or iter([9]))), log)[1])([]))

# ---- What is said of what cannot be gone through, and what is not lost
for label, x in (("nothing to go by", make()), ("__iter__ raises TypeError", make(__iter__=raiser(TypeError("its own")))), ("__iter__ returns None", make(__iter__=lambda s: None)), ("__iter__ is None", make(__iter__=None)), ("__getitem__ raises TypeError", make(__getitem__=raiser(TypeError("its own")))),
                 ("__next__ raises TypeError", make(__iter__=lambda s: s, __next__=raiser(TypeError("its own")))), ("__iter__ raises ValueError", make(__iter__=raiser(ValueError("its own"))))):
    for what, use in (("a, b = x", lambda: (lambda: [0 for a, b in [x]])()), ("[*x]", lambda: [*x]), ("f(*x)", lambda: print(*x)), ("f(0, *x)", lambda: print(0, *x)), ("zip(x)", lambda: type(zip(x)).__name__), ("''.join(x)", lambda: "".join(x)), ("b''.join(x)", lambda: b"".join(x)), ("bytes(x)", lambda: bytes(x)),
                      ("bytearray(x)", lambda: bytearray(x)), ("1 in x", lambda: 1 in x), ("l[:] = x", lambda: [].__setitem__(slice(None), x)), ("l[::2] = x", lambda: [].__setitem__(slice(None, None, 2), x)), ("reversed(x)", lambda: reversed(x)), ("list(x)", lambda: list(x)), ("for", lambda: [y for y in x])):
        t("%s: %s" % (label, what), use)

# ---- StopIteration is how it is said that there is no more, whoever says it
stop = raiser(StopIteration("from within"))
t("from what map() calls", lambda: list(map(lambda v: stop() if v == 2 else v, [1, 2, 3])))
t("from what filter() calls", lambda: list(filter(lambda v: stop() if v == 2 else True, [1, 2, 3])))
t("from __bool__() in filter()", lambda: list(filter(None, [1, make(__bool__=stop), 3])))
t("from what iter() calls", lambda: list(iter(stop, 1)))
t("from __getitem__()", lambda: list(make(__getitem__=lambda s, i: stop() if i == 1 else i)))
t("in zip()", lambda: list(zip([1, 2], map(lambda v: stop() if v == 2 else v, [1, 2]))))
t("but not from a generator", lambda: list((stop() if v == 2 else v) for v in [1, 2, 3]))
t("next() gives it as it was raised", lambda: next(make(__next__=raiser(StopIteration(5)))))


def value_of_stop(x):
    try:
        next(x)
    except StopIteration as e:
        return e.value, e.args


t("with what it has to say", lambda: value_of_stop(make(__next__=raiser(StopIteration(5)))))
t("and with something to give instead it is not raised", lambda: next(make(__next__=raiser(StopIteration(5))), "instead"))
t("from map()", lambda: value_of_stop(map(raiser(StopIteration(6)), [1])))

# ---- len(), bool() and in
for label, value in (("-1", -1), ("-2 ** 70", -2 ** 70), ("2 ** 63", 2 ** 63), ("2 ** 63 - 1", 2 ** 63 - 1), ("1.5", 1.5), ("True", True), ("MyInt", MyInt(3)), ("None", None), ("what has __index__()", make(__index__=lambda s: 4))):
    t("__len__ returns " + label, lambda: (len(make(__len__=lambda s: value)), bool(make(__len__=lambda s: value))))
t("__bool__ is None", lambda: bool(make(__bool__=None)))
t("__bool__ is None, in an if", lambda: 1 if make(__bool__=None) else 2)
t("__contains__ is None", lambda: 1 in make(__contains__=None, __iter__=lambda s: iter([1])))
t("__reversed__ is None", lambda: reversed(make(__reversed__=None, __getitem__=lambda s, i: i, __len__=lambda s: 1)))
t("reversed() of what has no length", lambda: reversed(make(__getitem__=lambda s, i: i)))
t("reversed() of a dict's own", lambda: reversed(type("D", (dict,), {"__reversed__": None})()))

# ---- str() and repr() give what was returned
x = make(__str__=lambda s: MyStr("S"), __repr__=lambda s: MyStr("R"), __format__=lambda s, spec: MyStr("F" + spec))
for label, f in (("str(x)", lambda: str(x)), ("repr(x)", lambda: repr(x)), ("ascii(x)", lambda: ascii(x)), ("format(x)", lambda: format(x)), ("format(x, 'q')", lambda: format(x, "q")), ("f'{x}'", lambda: f"{x}"), ("f'{x!s}'", lambda: f"{x!s}"), ("f'{x!r}'", lambda: f"{x!r}"), ("f'{x}{x}'", lambda: f"{x}{x}"), ("f'{x:q}'", lambda: f"{x:q}"),
                 ("'%s' % x", lambda: "%s" % x), ("'%r' % x", lambda: "%r" % x), ("'%a' % x", lambda: "%a" % x), ("'%s' % (x,)", lambda: "%s" % (x,)), ("'%s%s' % ('', x)", lambda: "%s%s" % ("", x)), ("'%*s' % (0, x)", lambda: "%*s" % (0, x)), ("'% s' % x", lambda: "% s" % x), ("'%+s' % x", lambda: "%+s" % x), ("'%1s' % x", lambda: "%1s" % x),
                 ("'%2s' % x", lambda: "%2s" % x), ("'%(a)s'", lambda: "%(a)s" % {"a": x}), ("'a%s' % x", lambda: "a%s" % x), ("'{}'", lambda: "{}".format(x)), ("'{}{}' with nothing first", lambda: "{}{}".format("", x)), ("'{}{}' with nothing last", lambda: "{}{}".format(x, "")), ("'{!s}'", lambda: "{!s}".format(x)),
                 ("'{!s:1}'", lambda: "{!s:1}".format(x)), ("'{!r:1}'", lambda: "{!r:1}".format(x)), ("'{:q}'", lambda: "{:q}".format(x)), ("'{a}'", lambda: "{a}".format(a=x)), ("'{a}' from a map", lambda: "{a}".format_map({"a": x})), ("MyStr(x)", lambda: MyStr(x)), ("str([x])", lambda: str([x])), ("'a{}'", lambda: "a{}".format(x))):
    t(label, lambda: kind(f()))
t("print() takes them", lambda: print(x, x, sep=MyStr("|"), end=MyStr("!\n")))
for value in (None, 5, b"a", MyInt(1), ["a"]):
    t("__repr__ returns %r" % (value,), lambda: repr(make(__repr__=lambda s: value)))
    t("and str() has it that __str__ did", lambda: str(make(__repr__=lambda s: value)))
    t("__str__ returns %r" % (value,), lambda: str(make(__str__=lambda s: value)))
    t("__format__ returns %r" % (value,), lambda: format(make(__format__=lambda s, spec: value)))
    t("in a str.format()", lambda: "{}".format(make(__format__=lambda s, spec: value)))
t("what % with a tuple written out is compiled as", lambda: (lambda log: ("%s and %r" % (make(__str__=lambda s: log.append("str") or "S", __format__=lambda s, f: log.append("format") or "F"), make(__repr__=lambda s: log.append("repr") or "R")), log))([]))
t("which a class derived from str can tell", lambda: (lambda C: ("%s" % (make(__str__=lambda s: C("S")),), "%s" % make(__str__=lambda s: C("S"))))(type("C", (str,), {"__format__": lambda s, f: "formatted"})))
t("not with a star, or too few, or too many", lambda: ("%s %s" % (*[1], 2), ))
t("too few", lambda: "%s %s" % (1,))
t("too many", lambda: "%s" % (1, 2))
t("with how wide", lambda: ("%5s|%-5s|%.1s|%5.1r|%%" % ("ab", "ab", "ab", "ab"), "%05s" % ("ab",), "%d %s" % (1, 2)))

# ---- int(), float() and complex()
for name in ("__int__", "__index__", "__trunc__", "__float__"):
    for label, value in (("5", 5), ("True", True), ("MyInt", MyInt(5)), ("5.0", 5.0), ("None", None), ("'5'", "5")):
        x = make(**{name: lambda s: value})
        for g in (int, float, complex):
            t("%s returns %s: %s(x)" % (name, label, g.__name__), lambda: kind(g(x)))
for label, x in (("__index__ returns None", make(__index__=lambda s: None)), ("__index__ raises TypeError", make(__index__=raiser(TypeError("its own")))), ("__index__ raises ValueError", make(__index__=raiser(ValueError("its own")))), ("__int__ returns None", make(__int__=lambda s: None)),
                 ("__int__ raises TypeError", make(__int__=raiser(TypeError("its own")))), ("__index__ returns 65", make(__index__=lambda s: 65)), ("__int__ returns 65", make(__int__=lambda s: 65)), ("__float__ returns 1.5", make(__float__=lambda s: 1.5)), ("nothing", make()), ("__index__ is None", make(__index__=None))):
    for spec in ("%d", "%i", "%x", "%o", "%c", "%f", "%s"):
        t("%s: %r %% x" % (label, spec), lambda: (spec % x)[:12])
    t(label + ": bytes(x)", lambda: bytes(x)[:3])
    t(label + ": bytearray(x)", lambda: bytearray(x)[:3])
t("a str that has __int__()", lambda: int(type("S", (str,), {"__int__": lambda s: 7})("12")))
t("a bytes that has __index__()", lambda: (int(type("B", (bytes,), {"__index__": lambda s: 7})(b"12")), bytes(type("B", (bytes,), {"__index__": lambda s: 2})(b"12"))))
t("what __bytes__() returns is what bytes() gives", lambda: [kind(v) for v in (bytes(make(__bytes__=lambda s: MyBytes(b"a"))), MyBytes(make(__bytes__=lambda s: b"a")), b"%b" % make(__bytes__=lambda s: MyBytes(b"a")))])
for value in (None, bytearray(b"a"), "a", 5, memoryview(b"a")):
    t("__bytes__ returns %s" % type(value).__name__, lambda: bytes(make(__bytes__=lambda s: value)))
    t("for %b", lambda: b"%b" % make(__bytes__=lambda s: value))
t("__bytes__() before its bytes, for %b", lambda: b"%b" % make(__bytes__=lambda s: b"by the method", __buffer__=lambda s, flags: memoryview(b"by its bytes")))

# ---- What is lost when its bytes cannot be got at
for label, error in (("TypeError", TypeError("its own")), ("ValueError", ValueError("its own")), ("KeyboardInterrupt", KeyboardInterrupt())):
    x = make(__buffer__=raiser(error))
    for what, use in (("int(x)", lambda: int(x)), ("float(x)", lambda: float(x)), ("compile(x)", lambda: compile(x, "f", "eval")), ("eval(x)", lambda: eval(x)), ("str(x, 'ascii')", lambda: str(x, "ascii")), ("bytearray().extend(x)", lambda: bytearray().extend(x)), ("b''.startswith(x)", lambda: b"".startswith(x)),
                      ("b''.endswith(x)", lambda: b"".endswith(x)), ("bytes(x)", lambda: bytes(x)), ("memoryview(x)", lambda: memoryview(x)), ("b'' + x", lambda: b"" + x), ("b''.join([x])", lambda: b"".join([x])), ("b''.find(x)", lambda: b"".find(x)), ("x in b''", lambda: x in b""), ("b'' == x", lambda: b"" == x)):
        t("__buffer__ raises %s: %s" % (label, what), use)
t("it is asked for them once", lambda: (lambda log: (int(make(__buffer__=lambda s, flags: log.append(flags) or memoryview(b"12"))), float(make(__buffer__=lambda s, flags: log.append(flags) or memoryview(b"1.5"))), log))([]))

# ---- What something says that it is


class Proxy:
    __class__ = int


class Lies:
    @property
    def __class__(self):
        return str


class Raises:
    @property
    def __class__(self):
        raise ValueError("its own")


def matched(value):
    match value:
        case int():
            return "an int"
        case str():
            return "a str"
        case _:
            return "neither"


t("isinstance()", lambda: (isinstance(Proxy(), int), isinstance(Proxy(), Proxy), isinstance(Proxy(), (str, int)), isinstance(Lies(), str), isinstance(Lies(), Lies), isinstance(Lies(), int), type(Proxy()) is Proxy, isinstance(Proxy(), int | str), issubclass(type(Proxy()), int)))
t("match", lambda: (matched(Proxy()), matched(Lies()), matched(object())))
t("what is raised in saying", lambda: isinstance(Raises(), int))
t("is not asked if there is no need", lambda: isinstance(Raises(), Raises))
t("by __getattribute__()", lambda: isinstance(make(__getattribute__=lambda s, name: int), int))
t("what is not a class", lambda: isinstance(make(__getattribute__=lambda s, name: 5), int))

# ---- What a class is given by a name that it would have had anyway
C = type("C", (), {"__dict__": lambda self: "its own", "__weakref__": 5, "__doc__": staticmethod(lambda: "a static method")})
t("__dict__ and __weakref__", lambda: (C().__dict__(), vars(C())(), C().__weakref__, C.__doc__(), C().__doc__()))
t("and can still be given attributes", lambda: (lambda c: (setattr(c, "a", 1), c.a)[1])(C()))
t("__class_getitem__ is None", lambda: type("C", (), {"__class_getitem__": None})[int])
t("__fspath__ is None", lambda: compile("1", make(__fspath__=None), "eval"))
t("__fspath__ returns one of a class derived from str", lambda: compile("1", make(__fspath__=lambda s: MyStr("named")), "eval").co_filename == "named")
t("a mode of a class derived from str", lambda: eval(compile(MyStr("1 + 1"), MyStr("f"), MyStr("eval"))))
for label, namespace in (("both of the other", {"__aenter__": lambda s: 1, "__aexit__": lambda s, *a: 1}), ("one of them None", {"__aenter__": None, "__aexit__": lambda s, *a: 1}), ("one of them 5", {"__aenter__": lambda s: 1, "__aexit__": 5}), ("neither", {})):
    def use():
        with make(**namespace):
            pass
    t("with, of what has " + label, use)
t("ellipsis with arguments", lambda: type(...)(1))
t("a class derived from int with too many", lambda: MyInt(1, 2, 3))
t("int with too many", lambda: int(1, 2, 3))

# ---- dir()
t("of what says that its class is something else", lambda: [n for n in dir(make(__getattribute__=lambda s, name: {"__dict__": {"own": 1}, "__class__": type("K", (), {"of_the_class": 1})}[name])) if not n.startswith("__")])
t("of what says that everything is itself", lambda: dir(make(__getattribute__=lambda s, name: s)))
K = make(__getattribute__=lambda s, name: {"__dict__": make(keys=lambda s: ["from_keys"], __getitem__=lambda s, k: 1), "__bases__": make(__len__=lambda s: 0)}[name])
t("of what has for its class something that only looks like one", lambda: dir(make(__getattribute__=lambda s, name: {"__dict__": 5, "__class__": K}[name])))
