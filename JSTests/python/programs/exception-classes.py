import builtins
def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
def attempt(f):
    try: return f()
    except BaseException as e: return type(e).__name__ + ": " + str(e)
def fields(e, *names): return tuple(attempt(lambda: getattr(e, n)) for n in names)
U = ("encoding", "object", "start", "end", "reason", "args")

# ---- the Unicode errors
E, D, T = UnicodeEncodeError, UnicodeDecodeError, UnicodeTranslateError
show("encode", lambda: (fields(E("utf-8", "abc", 1, 2, "why"), *U), str(E("utf-8", "abc", 1, 2, "why")), repr(E("utf-8", "abc", 1, 2, "why"))))
show("decode", lambda: (fields(D("utf-8", b"abc", 1, 2, "why"), *U), str(D("utf-8", b"abc", 1, 2, "why"))))
show("translate", lambda: (fields(T("abc", 1, 2, "why"), *U), str(T("abc", 1, 2, "why")), repr(T("abc", 1, 2, "why"))))
for label, s in [("latin", "a\xe9c"), ("bmp", "a€c"), ("astral", "a\U0001F600c")]:
    show("character " + label, lambda: (str(E("x", s, 1, 2, "r")), str(T(s, 1, 2, "r"))))
for a, b in [(0, 1), (0, 2), (0, 3), (2, 3), (3, 4), (1, 1), (2, 1), (-1, 0), (0, 0), (5, 6), (0, 100), (-5, -4)]:
    show("range %d %d" % (a, b), lambda: (str(E("x", "abc", a, b, "r")), str(D("x", b"abc", a, b, "r")), str(T("abc", a, b, "r"))))
show("empty object", lambda: (str(E("x", "", 0, 1, "r")), str(D("x", b"", 0, 1, "r")), str(T("", 0, 1, "r"))))
for cls, good in [(E, ("x", "abc", 1, 2, "r")), (D, ("x", b"abc", 1, 2, "r")), (T, ("abc", 1, 2, "r"))]:
    n = cls.__name__
    show(n + " none", lambda: cls())
    show(n + " too few", lambda: cls(*good[:-1]))
    show(n + " too many", lambda: cls(*good, 1))
    show(n + " keywords", lambda: cls(*good, x=1))
    for i in range(len(good)):
        for bad in (None, 1.5, [], b"b" if isinstance(good[i], str) else "s"):
            show(n + " argument %d is %r" % (i + 1, bad), lambda: cls(*good[:i], bad, *good[i + 1:]))
    show(n + " big", lambda: cls(*[1 << 70 if isinstance(g, int) else g for g in good]))
    show(n + " index", lambda: fields(cls(*[True if isinstance(g, int) else g for g in good]), "start", "end"))
    show(n + " without init", lambda: (fields(cls.__new__(cls), *U), str(cls.__new__(cls))))
    show(n + " without init, with arguments", lambda: (fields(cls.__new__(cls, 1, 2), *U), str(cls.__new__(cls, 1, 2))))
    show(n + " args are set even so", lambda: after_failure(cls))
    show(n + " own", lambda: sorted(k for k in vars(cls) if k != "__doc__"))
    show(n + " kinds", lambda: [type(vars(cls)[k]).__name__ for k in ("encoding", "object", "start", "end", "reason")])
    def changed(name, value):
        e = cls(*good); setattr(e, name, value); return (getattr(e, name), attempt(lambda: str(e)))
    for name in ("encoding", "object", "reason"):
        for value in (None, 5, "new", b"new"):
            show(n + " set " + name + " to " + repr(value), lambda: changed(name, value))
        show(n + " delete " + name, lambda: (lambda e: (delattr(e, name), getattr(e, name), attempt(lambda: str(e))))(cls(*good)))
    for name in ("start", "end"):
        for value in (0, 2, -1, True, 1.5, "1", None, 1 << 70):
            show(n + " set " + name + " to " + repr(value), lambda: changed(name, value))
        show(n + " delete " + name, lambda: delattr(cls(*good), name))
def after_failure(cls):
    e = cls.__new__(cls)
    try: e.__init__(1, 2)
    except TypeError: pass
    return e.args
class S(str): pass
class B(bytes): pass
show("derived strings", lambda: (fields(E(S("x"), S("abc"), 1, 2, S("r")), *U[:5]), type(E(S("x"), S("abc"), 1, 2, S("r")).object).__name__, str(E(S("x"), S("abc"), 1, 2, S("r")))))
show("decode of other buffers", lambda: [(type(D("x", o, 0, 1, "r").object).__name__, D("x", o, 0, 1, "r").object, str(D("x", o, 0, 1, "r"))) for o in (bytearray(b"ab"), memoryview(b"ab"), B(b"ab"))])
show("decode keeps bytes", lambda: (lambda b: D("x", b, 0, 1, "r").object is b)(b"ab"))
show("decode copies a bytearray", lambda: (lambda b: (lambda e: (b.clear(), e.object))(D("x", b, 0, 1, "r")))(bytearray(b"ab")))
show("real ones", lambda: [attempt(f) for f in (lambda: "\xe9".encode("ascii"), lambda: b"\xe9".decode("ascii"), lambda: "\ud800".encode("utf-8"), lambda: b"\xff\xfe".decode("utf-8"))])
def caught(f):
    try: f()
    except UnicodeError as e: return fields(e, *U[:5])
show("real ones, fields", lambda: [caught(f) for f in (lambda: "a\xe9\xe9b".encode("ascii"), lambda: b"a\xe9b".decode("ascii"))])
show("hierarchy", lambda: [c.__mro__[1].__name__ for c in (E, D, T, UnicodeError)])

# ---- OSError
O = ("errno", "strerror", "filename", "filename2", "args", "characters_written")
for a in [(), (1,), (1, "s"), (1, "s", "f"), (1, "s", "f", 0), (1, "s", "f", 0, "g"), (1, "s", "f", 0, "g", 6), (1, "s", None), (1, "s", None, None, "g"), (1, "s", "f", None, None), ("x", "y"), (2, "s"), (2, "s", "f"), (None, None), (1 << 70, "s")]:
    show("OSError" + repr(a), lambda: (type(OSError(*a)).__name__, fields(OSError(*a), *O), str(OSError(*a)), repr(OSError(*a))))
show("OSError keywords", lambda: OSError(1, "s", x=1))
show("by number", lambda: [type(OSError(n, "s")).__name__ for n in (1, 2, 3, 4, 10, 11, 13, 17, 20, 21, 32, 35, 36, 54, 60, 61, 9999, -1, True)])
show("not for derived classes", lambda: type(type("X", (OSError,), {})(2, "s")).__name__)
show("not for one that is specific", lambda: type(PermissionError(2, "s")).__name__)
for a in [(1, "s", 5), (1, "s", 5.5), (1, "s", "f"), (1, "s", True), (1, "s", 1 << 70), (1, "s", 5, 0, "g"), (1, "s", None), (1, "s")]:
    show("BlockingIOError" + repr(a), lambda: (fields(BlockingIOError(*a), *O), str(BlockingIOError(*a))))
show("only BlockingIOError itself", lambda: (fields(type("X", (BlockingIOError,), {})(1, "s", 5), *O), fields(OSError(35, "s", 5), *O), type(OSError(35, "s", 5)).__name__))
def written(v):
    e = OSError(); e.characters_written = v; return e.characters_written
show("characters_written", lambda: [attempt(lambda: written(v)) for v in (0, 5, -1, -2, True, 1.5, "1", None, 1 << 70)])
show("characters_written delete", lambda: (lambda e: (setattr(e, "characters_written", 3), delattr(e, "characters_written"), attempt(lambda: e.characters_written)))(OSError()))
show("characters_written delete twice", lambda: delattr(OSError(), "characters_written"))
class OI(OSError):
    def __init__(s, a, b): s.mine = (a, b)
class OI2(OSError):
    def __init__(s, a, b): super().__init__(a, b, "file")
class ON(OSError):
    def __new__(cls, a): return super().__new__(cls, a, "from new")
class ONI(OSError):
    def __new__(cls, *a): return super().__new__(cls, 7, "from new")
    def __init__(s, *a): super().__init__(8, "from init")
show("own init", lambda: (fields(OI(1, 2), *O[:5]), OI(1, 2).mine, str(OI(1, 2))))
show("own init that calls", lambda: (fields(OI2(1, 2), *O[:5]), str(OI2(1, 2))))
show("own new", lambda: (fields(ON(1), *O[:5]), str(ON(1))))
show("own both", lambda: (fields(ONI(1), *O[:5]), str(ONI(1))))
show("own init keywords", lambda: OI(a=1, b=2).mine)
show("init again does nothing", lambda: (lambda e: (e.__init__(5, "t"), fields(e, *O[:5])))(OSError(1, "s")))
show("aliases", lambda: (IOError is OSError, EnvironmentError is OSError))
show("set fields", lambda: (lambda e: (setattr(e, "errno", 9), setattr(e, "filename", "z"), str(e), e.args))(OSError(1, "s")))

# ---- SyntaxError
Y = ("msg", "filename", "lineno", "offset", "text", "end_lineno", "end_offset", "_metadata", "print_file_and_line", "args")
for a in [(), ("m",), ("m", ("f", 1, 2, "t")), ("m", ("f", 1, 2, "t", 3, 4)), ("m", ("f", 1, 2, "t", 3, 4, "meta")), ("m", ["f", 1, 2, "t"]), ("m", ("f", 1, 2)), ("m", ("f", 1, 2, "t", 3)), ("m", ("f", 1, 2, "t", 3, 4, 5, 6)), ("m", 1), ("m", "abcd"), ("m", ("f", 1, 2, "t"), 3)]:
    show("SyntaxError" + repr(a), lambda: (fields(SyntaxError(*a), *Y), str(SyntaxError(*a))))
show("metadata", lambda: (lambda e: (setattr(e, "_metadata", 5), e._metadata, delattr(e, "_metadata"), e._metadata))(SyntaxError()))
show("incomplete input", lambda: (_IncompleteInputError.__mro__[1].__name__, _IncompleteInputError.__name__, _IncompleteInputError.__module__, _IncompleteInputError.__doc__, str(_IncompleteInputError("m", ("f", 1, 2, "t"))), "_IncompleteInputError" in vars(builtins)))

# ---- __new__
# All but the class of an Error of JavaScript's, which CPython has not
classes = sorted((n for n, c in vars(builtins).items() if isinstance(c, type) and issubclass(c, BaseException) and n != "JSError"))
show("which have a __new__ of their own", lambda: [n for n in classes if "__new__" in vars(getattr(builtins, n))])
show("which have an __init__ of their own", lambda: [n for n in classes if "__init__" in vars(getattr(builtins, n))])
show("new of another", lambda: (type(ValueError.__new__(TypeError)).__name__, type(BaseException.__new__(KeyError, 1)).__name__, BaseException.__new__(KeyError, 1).args))
show("new of what is not one", lambda: ValueError.__new__(int))
show("new of nothing", lambda: ValueError.__new__())
show("new of a value", lambda: ValueError.__new__(1))
show("new is not safe", lambda: object.__new__(ValueError))
show("new with keywords", lambda: ValueError.__new__(ValueError, x=1).args)
