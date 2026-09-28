import sys
sys.modules["warnings"] = None
import _warnings
class Count:
    n = 0
    def write(s, t): s.n += t.count("returned non-")
    def flush(s): pass
c = sys.stderr = Count()
_warnings.filters[:] = [("always", None, Warning, None, 0)]
class I(int): pass
class F(float): pass
class R:
    def __index__(s): return I(2)
class RF:
    def __float__(s): return F(2.0)
class RI:
    def __int__(s): return I(2)
import math
cases = [("hex", lambda: hex(R())), ("bin", lambda: bin(R())), ("oct", lambda: oct(R())), ("range1", lambda: range(R())), ("range3", lambda: range(R(), R(), R())), ("str*", lambda: "ab" * R()), ("list*", lambda: [1] * R()), ("*list", lambda: R() * [1]), ("bytes*", lambda: b"a" * R()), ("index", lambda: [1, 2, 3][R()]), ("slice", lambda: [1, 2, 3][R():R()]), ("str index", lambda: "abc"[R()]),
    ("tuple index", lambda: (1, 2, 3)[R()]), ("bytes index", lambda: b"abc"[R()]), ("setitem", lambda: [1, 2, 3].__setitem__(R(), 0)), ("int()", lambda: int(R())), ("int(RI)", lambda: int(RI())), ("float()", lambda: float(R())), ("float(RF)", lambda: float(RF())), ("complex(R)", lambda: complex(R())), ("complex(RF)", lambda: complex(RF())), ("chr", lambda: chr(R())), ("round n", lambda: round(1.234, R())),
    ("pow", lambda: pow(2, 3, 5)), ("bytes(R)", lambda: bytes(R())), ("bytearray(R)", lambda: bytearray(R())), ("list.insert", lambda: [].insert(R(), 1)), ("list.pop", lambda: [1, 2, 3].pop(R())), ("str.center", lambda: "a".center(R())), ("str.zfill", lambda: "a".zfill(R())), ("format d", lambda: "%d" % R()), ("format x", lambda: "%x" % R()), ("format c", lambda: "%c" % R()), ("format f", lambda: "%f" % RF()),
    ("int.from_bytes", lambda: (5).to_bytes(R(), "big")), ("math.sqrt", lambda: math.sqrt(RF())), ("math.floor", lambda: math.floor(RF())), ("math.factorial", lambda: math.factorial(R())), ("math.gcd", lambda: math.gcd(R(), R())), ("enumerate", lambda: enumerate([], R())), ("slice.indices", lambda: slice(R()).indices(R())), ("operator <<", lambda: 1 << 2), ("divmod", lambda: divmod(RF().__float__(), 1)), ("sum", lambda: sum([1], 2)),
    ("str.split", lambda: "a b".split(None, R())), ("str.expandtabs", lambda: "a".expandtabs(R())), ("str.find", lambda: "abc".find("c", R())), ("memoryview", lambda: memoryview(b"abc")[R()]), ("range index", lambda: range(5)[R()]), ("isinstance", lambda: isinstance(R().__index__(), int)), ("int base", lambda: int("11", R())), ("float+", lambda: 1.0 + RF().__float__())]
for n, f in cases:
    c.n = 0
    try: f()
    except BaseException as e: print(n, "!!", type(e).__name__, e); continue
    print(n, c.n)
