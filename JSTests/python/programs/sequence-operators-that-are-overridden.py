# What + and * come to of an instance of a class that is derived from a built-in sequence and has something of its own by one of the names that they go by. What the sequence had for it as a sequence has it is then gone.
def attempt(f, *a):
    try: return f(*a)
    except BaseException as e: return type(e).__name__ + ": " + str(e)
import operator
NI = lambda *a: NotImplemented
for base, v, o in ((list, [1], [2]), (tuple, (1,), (2,)), (str, "a", "b"), (bytes, b"a", b"b"), (bytearray, b"a", b"b")):
    for names in (("__add__",), ("__radd__",), ("__iadd__",), ("__mul__",), ("__rmul__",), ("__imul__",), ("__add__", "__iadd__"), ("__mul__", "__rmul__"), ("__mul__", "__imul__", "__rmul__")):
        K = type("K", (base,), {n: NI for n in names})
        print(base.__name__, names, [attempt(f) for f in (lambda: K(v) + o, lambda: o + K(v), lambda: K(v) + K(v), lambda: operator.iadd(K(v), o), lambda: K(v) * 2, lambda: 2 * K(v), lambda: operator.imul(K(v), 2), lambda: operator.concat(K(v), o), lambda: operator.iconcat(K(v), o), lambda: operator.concat(o, K(v)))])
