# What is being handled after `finally`, what float(), round() and int.from_bytes() make of what has something of its own to say, and what formatting makes of a class derived from str and of a format that is not quite one.
import sys, operator
def attempt(label, f):
    try: print(label, "=>", repr(f()))
    except BaseException as e: print(label, "=>", type(e).__name__, e, "| cause", repr(e.__cause__), "| context", repr(e.__context__))
print("---- what is being handled in finally")
def a():
    try:
        raise TypeError("foo")
    except:
        try:
            raise KeyError("caught")
        finally:
            raise
attempt("raise in finally", a)
def b():
    seen = []
    try:
        try:
            raise KeyError("k")
        finally:
            seen.append(repr(sys.exception()))
    except KeyError:
        seen.append(repr(sys.exception()))
    seen.append(repr(sys.exception()))
    return seen
attempt("sys.exception() in finally", b)
def c():
    seen = []
    try:
        pass
    finally:
        seen.append(repr(sys.exception()))
    try:
        raise ValueError("outer")
    except ValueError:
        try:
            pass
        finally:
            seen.append(repr(sys.exception()))
        try:
            try:
                raise KeyError("inner")
            finally:
                seen.append(repr(sys.exception()))
        except KeyError:
            pass
        seen.append(repr(sys.exception()))
    return seen
attempt("with nothing raised, and inside a handler", c)
def d():
    class M:
        def __enter__(self): return self
        def __exit__(self, *a):
            seen.append((repr(a[1]), repr(sys.exception())))
            return True
    seen = []
    with M():
        raise KeyError("in with")
    seen.append(repr(sys.exception()))
    return seen
attempt("in __exit__", d)
def e():
    try:
        try:
            raise KeyError("first")
        finally:
            raise ValueError("second")
    except ValueError as x:
        return repr(x.__context__)
attempt("context of what finally raises", e)
def f():
    def g():
        try:
            yield 1
        finally:
            seen.append(repr(sys.exception()))
    seen = []
    it = g(); next(it)
    try: it.throw(KeyError("thrown"))
    except KeyError: pass
    return seen
attempt("in a generator", f)
import warnings
with warnings.catch_warnings():
    # That there is a `continue` there is warned of when it is compiled.
    warnings.simplefilter("ignore", SyntaxWarning)
    exec("""def h():
    for i in range(2):
        try:
            try:
                raise KeyError(i)
            finally:
                if i:
                    raise
                continue
        except KeyError as x:
            return repr(x)
""")
attempt("continue in finally, then raise", h)
print("---- numbers")
class Foo2(float):
    def __float__(self): return 42.
class Foo3(float):
    def __new__(cls, value=0.): return float.__new__(cls, 2 * value)
    def __float__(self): return self
class Foo4(float):
    def __float__(self): return 42
class FooStr(str):
    def __float__(self): return float(str(self)) + 1
class I2(int):
    def __int__(self): return 42
    def __index__(self): return 43
    def __trunc__(self): return 44
class C2(complex):
    def __complex__(self): return 42j
import warnings; warnings.simplefilter("ignore")
for label, fn in {"float(Foo2())": lambda: float(Foo2()), "float(Foo3(21))": lambda: float(Foo3(21)), "float(Foo4(42))": lambda: float(Foo4(42)), "float(FooStr('8'))": lambda: float(FooStr("8")), "int(I2(1))": lambda: int(I2(1)), "operator.index(I2(1))": lambda: operator.index(I2(1)), "complex(C2(1))": lambda: complex(C2(1)), "Foo2() + 1": lambda: Foo2() + 1, "math.sqrt(Foo2(4))": lambda: __import__("math").sqrt(Foo2(4)), "round(Foo2(1.5))": lambda: round(Foo2(1.5)), "'%f' % Foo2(1)": lambda: "%f" % Foo2(1), "'%d' % I2(1)": lambda: "%d" % I2(1), "[0,1,2][I2(1)]": lambda: [0, 1, 2][I2(1)], "float.__float__(Foo2(3))": lambda: float.__float__(Foo2(3)), "complex(Foo2(1))": lambda: complex(Foo2(1)), "int(Foo2(7.5))": lambda: int(Foo2(7.5)), "range(I2(2))": lambda: list(range(I2(2))), "bin(I2(5))": lambda: bin(I2(5)), "chr(I2(65))": lambda: chr(I2(65))}.items():
    attempt(label, fn)
for x, n in ((1.6e308, -308), (-1.7e308, -308), (1.5e308, -308), (5e307, -308), (1e308, -400), (1.0, 400), (1.23456, 3), (float("inf"), -1), (float("nan"), 2), (123.456, -1), (123.456, -3), (2.5, 0), (1e16, -16), (0.5e-323, 323)):
    attempt("round(%r, %r)" % (x, n), lambda: round(x, n))
class ValidBytes:
    def __bytes__(self): return b"\x01"
class InvalidBytes:
    def __bytes__(self): return "abc"
class RaisingBytes:
    def __bytes__(self): 1 / 0
for v in (ValidBytes(), InvalidBytes(), RaisingBytes(), object(), [1, 2], (255,), bytearray(b"\x02"), memoryview(b"\x03"), "a", 5, iter([1]), range(3), None):
    attempt("int.from_bytes(%s)" % type(v).__name__, lambda: int.from_bytes(v))
print("---- formatting")
class S(str):
    def __str__(self): return "__str__ overridden"
class S2(str):
    def __format__(self, spec): return "__format__ overridden"
s = S("xxx")
for label, fn in {"'%s' % s": lambda: "%s" % s, "'{}'.format(s)": lambda: "{}".format(s), "format(s)": lambda: format(s), "format(s, '')": lambda: format(s, ""), "format(s, '5')": lambda: format(s, "5"), "f'{s}'": lambda: f"{s}", "f'{s!s}'": lambda: f"{s!s}", "f'{s:}'": lambda: f"{s:}", "f'{s:5}'": lambda: f"{s:5}", "str(s)": lambda: str(s), "'{:}'.format(s)": lambda: "{:}".format(s), "'{!s}'.format(s)": lambda: "{!s}".format(s), "S2": lambda: (f"{S2('a')}", "{}".format(S2("a")), format(S2("a"))), "str.__format__(s, '')": lambda: str.__format__(s, ""), "print": lambda: print(s), "''.join([s])": lambda: "".join([s]), "s + ''": lambda: s + ""}.items():
    attempt(label, fn)
class PseudoFloat:
    def __init__(self, v): self.v = v
    def __float__(self): return self.v
class PseudoInt:
    def __index__(self): return 65
for fmt, arg in (("%c", PseudoFloat(3.14)), ("%c", PseudoInt()), ("%c", 3.14), ("%c", None), ("%c", "ab"), ("%c", ""), ("%c", -1), ("%c", 0x110000), ("%c", 2**70), ("%d", PseudoFloat(3.14)), ("%d", "a"), ("%x", 3.14), ("%x", PseudoFloat(1.0)), ("%f", "a"), ("%f", PseudoFloat(2.5)), ("%i", None), ("%o", 1.0), ("%e", None), ("% %s", 1), ("%%s", 1), ("%% %s", 1), ("%", 1), ("%5", 1), ("%z", 1), ("%(a", {"a": 1}), ("%(a)", {"a": 1}), ("%(a)s", 1), ("%s %s", 1), ("%s", (1, 2)), ("%*d", (1,)), ("%*d", ("a", 1)), ("%.*f", (2, 1.0)), ("%-5%|", ()), ("%5%|", ()), ("%\x00", 1), ("%é", 1), ("abc", 1), ("abc", ()), ("abc", {}), ("abc", []), ("%s", {}), ("%s", [])):
    attempt("%r %% %s" % (fmt, type(arg).__name__ if not isinstance(arg, (int, float, str, tuple, dict, list, type(None))) else repr(arg)), lambda: fmt % arg)
    if all(ord(ch) < 128 for ch in fmt):
        attempt("bytes %r" % fmt, lambda: fmt.encode() % arg)
