# T.__new__(S), for every class T that is written in C and has a __new__() of its own, and every class S that is derived from it. If S is made otherwise than T is, as bool is made otherwise than int, that is not allowed:
# what came of it would be laid out as one thing and be taken for another.
import builtins
import sys

MODULES = ("builtins", "_collections", "_io", "_thread", "_weakref", "itertools", "_sre", "_struct", "_random", "select", "time", "posix", "_signal", "unicodedata", "_ast", "_string", "_tokenize", "_contextvars", "_typing", "_warnings", "marshal", "errno", "atexit", "_imp", "_codecs", "math", "sys")


def attempt(f, *a):
    try:
        return f(*a)
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)


classes = {}
for name in MODULES:
    try:
        module = __import__(name)
    except ImportError:
        continue
    for x in vars(module).values():
        if isinstance(x, type):
            classes[x.__module__ + "." + x.__qualname__] = x
# And those that are not in any module
for x in (type(None), type(...), type(NotImplemented), type(iter([])), type(iter(())), type(iter({})), type({}.keys()), type({}.values()), type({}.items()), type(lambda: 0), type((lambda: 0).__code__), type(sys._getframe()), type(len), type([].append), type(int.__add__), type(str.join), type((0).__add__), type(type.__dict__["__dict__"]), type(int.real), type(sys), type(sys.flags), type(sys.version_info), type(sys.float_info), type(sys.implementation), type((x for x in ())), type(iter("")), type(iter(b"")), type(iter(range(0))), type(reversed([])), type(type.__dict__), type(int.__dict__["from_bytes"]), type(list[int]), type(int | str)):
    classes[x.__module__ + "." + x.__qualname__] = x
# What only one of them has: the class of an Error of JavaScript's, and OrderedDict, which is written in Python here.
for name in ("builtins.JSError", "collections.OrderedDict"):
    classes.pop(name, None)
ordered = sorted(classes.items())
print(len(ordered), "classes")

pairs = made = refused = 0
for tname, T in ordered:
    if "__new__" not in vars(T):
        continue
    for sname, S in ordered:
        if S is T or not issubclass(S, T):
            continue
        pairs += 1
        r = attempt(T.__new__, S)
        if isinstance(r, str):
            refused += 1
            shown = r
        else:
            made += 1
            shown = "made a " + type(r).__name__
        # object.__new__() and BaseException.__new__() are asked of a great many. Only what is out of the ordinary is shown.
        if T is object and shown in ("TypeError: object.__new__(%s) is not safe, use %s.__new__()" % (S.__name__, S.__name__), "TypeError: cannot create '%s' instances" % S.__name__, "made a " + S.__name__):
            continue
        if T is BaseException and shown == "made a " + S.__name__:
            continue
        print("%s.__new__(%s) => %s" % (tname, sname, shown))
        # And of a class of a program's that is derived from it
        P = attempt(type, "P", (S,), {})
        if isinstance(P, type):
            r = attempt(T.__new__, P)
            print("    and of what is derived from that => %s" % (r if isinstance(r, str) else "made a " + type(r).__name__))
print(pairs, "pairs:", made, "made,", refused, "refused")

print("---- what is made can be used")
for T, S, a in ((BaseException, OSError, ()), (BaseException, KeyError, ("k",)), (Exception, ValueError, (1, 2)), (OSError, FileNotFoundError, (2, "x")), (BaseException, SyntaxError, ()), (BaseException, StopIteration, (5,)), (BaseException, SystemExit, (3,)), (BaseException, UnicodeDecodeError, ()), (BaseException, ImportError, ()), (BaseException, AttributeError, ()), (BaseException, NameError, ()), (BaseException, ExceptionGroup, ()), (BaseExceptionGroup, ExceptionGroup, ("m", [ValueError()]))):
    r = attempt(T.__new__, S, *a)
    print(T.__name__, S.__name__, "=>", r if isinstance(r, str) else (type(r).__name__, attempt(repr, r), attempt(str, r), r.args, [(n, attempt(getattr, r, n)) for n in ("errno", "filename", "value", "code", "msg", "lineno", "name", "obj", "path", "reason", "start", "message", "exceptions") if hasattr(type(r), n)]))

print("---- with a __new__() of a program's in between")


class A(int):
    def __new__(cls, *a):
        return int.__new__(cls, *a)


class B(A):
    pass


class C(dict):
    def __new__(cls):
        return object.__new__(cls)


class D(list, metaclass=type):
    def __new__(cls):
        return tuple.__new__(cls)


class Mixed(A, int):
    pass


print(A(5), B(6), Mixed(7), attempt(C), attempt(D), attempt(int.__new__, B, 8), attempt(A.__new__, B, 9), attempt(float.__new__, A), attempt(int.__new__, A, "z"), attempt(object.__new__, A), attempt(int.__new__, int, 3), attempt(int.__new__), attempt(int.__new__, 5), attempt(int.__new__, str), attempt(bool.__new__, bool, 5), attempt(bool.__new__, int))
