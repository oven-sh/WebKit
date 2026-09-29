# A class that is built in and takes no keyword arguments lets them by when they are for a class derived from it: its __init__() does if that class has its own __new__(), and its __new__() does if that class has its own
# __init__(). This tries every class that is built in and can be derived from.
import warnings

warnings.simplefilter("ignore")


def attempt(f, *a, **k):
    try:
        f(*a, **k)
        return "fine"
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)


MODULES = ("builtins", "_collections", "itertools", "array", "_random", "_io", "_struct", "_thread", "_weakref", "_abc", "types", "_contextvars", "_typing", "time", "select", "unicodedata", "binascii", "_sre", "_tokenize", "_ast")
# What each may be made from. The first of these that will do is what is used.
CANDIDATES = ((), ([1, 2],), (1,), ("b",), ("b", [1]), (len,), (len, [1]), ([1], 1), ([1], [1]), (1, 2), ("<i",), (int,), (object,), ("a", (), {}), (b"ab",), ([(1, 2)],), (None, [1]), (bool, [1]), ([1, 2], 1), (lambda: None,))
seen = set()
for module_name in MODULES:
    try:
        module = __import__(module_name)
    except ImportError:
        continue
    for name in sorted(vars(module)):
        T = getattr(module, name)
        if not isinstance(T, type) or T in seen or issubclass(T, BaseException) or T is type or name.startswith("__") or name in ("JSError", "FileIO", "OrderedDict"):  # FileIO(1) is what this prints to, and closes it. OrderedDict is not yet what CPython's is.
            continue
        seen.add(T)
        try:
            plain = type("Plain", (T,), {})
        except TypeError:
            continue
        for args in CANDIDATES:
            if attempt(plain, *args) == "fine":
                break
        else:
            print(module_name, name, "=> cannot be made from any of these")
            continue

        class WithInit(T):
            def __init__(self, *a, newarg=None):
                super().__init__(*a)

        class WithNew(T):
            def __new__(cls, *a, newarg=None):
                return super().__new__(cls, *a)

        class WithBoth(T):
            def __new__(cls, *a, newarg=None):
                return super().__new__(cls, *a)

            def __init__(self, *a, newarg=None):
                super().__init__(*a)

        class InitTakesNothing(T):
            def __init__(self, *a, **k):
                pass

        class NewTakesNothing(T):
            def __new__(cls, *a, **k):
                return super().__new__(cls)

        print(module_name, name, len(args), "=>", [attempt(k, *args, newarg=3) for k in (T, plain, WithInit, WithNew, WithBoth, InitTakesNothing)], attempt(NewTakesNothing, *args, newarg=3), attempt(InitTakesNothing, 1, 2, 3, 4, 5, 6, 7), attempt(NewTakesNothing, 1, 2, 3, 4, 5, 6, 7))

print("---- how it is put, of the class itself and of one derived from it, which are not called the same way")
for T, good in ((enumerate, ([1],)), (str, ()), (super, (int,)), (int, ()), (bytes, ()), (bytearray, ()), (dict, ()), (complex, ()), (float, ()), (tuple, ()), (list, ()), (bool, ()), (zip, ()), (map, (len, [1])), (property, ()), (memoryview, (b"",)), (slice, (1,)), (range, (1,)), (object, ()), (frozenset, ()), (set, ()), (reversed, ([1],)), (filter, (None, [1])), (type, ("a", (), {}))):
    try: S = type("S", (T,), {})
    except TypeError: S = None
    for K in (T, S):
        if K is None: continue
        print(T.__name__, "exact" if K is T else "derived", [attempt(K, *a, **k) for a, k in (((1, 2, 3, 4, 5, 6, 7), {}), (good, {"newarg": 3}), (good, {"newarg": 3, "other": 4}), ((), {"x": 1}), ((1, 2, 3, 4, 5, 6, 7), {"x": 1}))])
