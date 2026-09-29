# What can be stored through a memoryview, for each kind of item: not only a number, but what can be made one by __index__() or __float__(), and what is said of what cannot.
def attempt(f, *a):
    try: return f(*a)
    except BaseException as e: return type(e).__name__ + ": " + str(e)
class Index:
    def __init__(s, v): s.v = v
    def __index__(s): return s.v
class Float:
    def __init__(s, v): s.v = v
    def __float__(s): return s.v
class Raises:
    def __index__(s): raise KeyError("k")
    def __float__(s): raise KeyError("k")
    def __bool__(s): raise KeyError("k")
class RaisesValue:
    def __index__(s): raise ValueError("v")
    def __float__(s): raise OverflowError("o")
for f in "bBhHiIlLqQnNPefd?c":
    b = bytearray(8); m = memoryview(b).cast(f)
    print(f, [(attempt(m.__setitem__, 0, v), bytes(b[:m.itemsize])) for v in (Index(5), Index(-1), Index(2 ** 70), Index("a"), Float(1.5), Float("a"), True, 1.5, 5, "a", None, Raises(), RaisesValue(), b"a", type("I", (int,), {})(3), type("F", (float,), {})(2.5))])
b = bytearray(8); m = memoryview(b).cast("P")
print([(attempt(m.__setitem__, 0, v), bytes(b)) for v in (-1, -2 ** 63, -2 ** 63 - 1, 2 ** 64 - 1, 2 ** 64, 2 ** 63)], m[0])
