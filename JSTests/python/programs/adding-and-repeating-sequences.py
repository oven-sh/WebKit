def attempt(f):
    try: return f()
    except BaseException as e: return type(e).__name__ + ": " + str(e)
class X:
    def __init__(s, v): s.v = v
    def __index__(s): return s.v
class R:
    def __radd__(s, o): return ("radd", type(o).__name__)
    def __rmul__(s, o): return ("rmul", type(o).__name__)
    def __add__(s, o): return ("add", type(o).__name__)
    def __mul__(s, o): return ("mul", type(o).__name__)
    def __index__(s): return 2
class N:
    def __radd__(s, o): return NotImplemented
    def __rmul__(s, o): return NotImplemented
    def __add__(s, o): return NotImplemented
    def __mul__(s, o): return NotImplemented
class NI(N):
    def __index__(s): return 2
class BadIndex:
    def __index__(s): raise KeyError("index")
class StrIndex:
    def __index__(s): return "a"
def each():
    class L(list): pass
    class T(tuple): pass
    class S(str): pass
    class B(bytes): pass
    class BA(bytearray): pass
    return [("str", lambda: "ab"), ("list", lambda: [1, 2]), ("tuple", lambda: (1, 2)), ("bytes", lambda: b"ab"), ("bytearray", lambda: bytearray(b"ab")), ("S", lambda: S("ab")), ("L", lambda: L([1, 2])), ("T", lambda: T((1, 2))), ("B", lambda: B(b"ab")), ("BA", lambda: BA(b"ab")),
            ("range", lambda: range(2)), ("memoryview", lambda: memoryview(b"ab")), ("dict", lambda: {1: 2}), ("set", lambda: {1})]
def kind(v): return (type(v).__name__, v) if not isinstance(v, str) or not (": " in v) else v
others = [("2", lambda: 2), ("True", lambda: True), ("-1", lambda: -1), ("0", lambda: 0), ("X(2)", lambda: X(2)), ("X(-1)", lambda: X(-1)), ("X(big)", lambda: X(1 << 70)), ("X(-big)", lambda: X(-(1 << 70))), ("big", lambda: 1 << 70), ("1.5", lambda: 1.5), ("None", lambda: None), ("'a'", lambda: "a"),
          ("R", R), ("N", N), ("NI", NI), ("BadIndex", BadIndex), ("StrIndex", StrIndex), ("[3]", lambda: [3]), ("(3,)", lambda: (3,)), ("b'c'", lambda: b"c"), ("bytearray", lambda: bytearray(b"c")), ("memoryview", lambda: memoryview(b"c")), ("range", lambda: range(1)), ("1j", lambda: 1j)]
def imul(a, b): a *= b; return a
def iadd(a, b): a += b; return a
for name, make in each():
    for other, value in others:
        print(name, other, "|", kind(attempt(lambda: make() * value())), "|", kind(attempt(lambda: value() * make())), "|", kind(attempt(lambda: imul(make(), value()))), "|", kind(attempt(lambda: make() + value())), "|", kind(attempt(lambda: value() + make())), "|", kind(attempt(lambda: iadd(make(), value()))))
for name, make in each():
    for m in ("__mul__", "__rmul__", "__imul__", "__add__", "__radd__", "__iadd__"):
        if not hasattr(make(), m): print(name, m, "has none"); continue
        print(name, m, type(getattr(type(make()), m)).__name__, [kind(attempt(lambda: getattr(make(), m)(v()))) for _, v in others if _ in ("2", "X(2)", "1.5", "None", "R", "NI", "[3]", "(3,)", "'a'", "b'c'", "X(big)", "BadIndex", "StrIndex")], attempt(lambda: getattr(make(), m)()), attempt(lambda: getattr(make(), m)(1, 2)))
def same(make, op, v):
    a = make(); b = op(a, v); return b is a
print("in place is the same object", [(n, same(m, imul, 2), same(m, iadd, m())) for n, m in each() if n in ("list", "bytearray", "L", "BA", "str", "tuple", "bytes")])
class Own(list):
    def __mul__(s, o): return "own mul"
    def __rmul__(s, o): return "own rmul"
    def __add__(s, o): return "own add"
    def __imul__(s, o): return "own imul"
print("a subclass with its own", Own([1]) * 2, 2 * Own([1]), Own([1]) + [1], [1] + Own([1]), [1] * Own([1]) if False else "-", imul(Own([1]), 2), attempt(lambda: [1] * Own([1])), "ab" * X(2), attempt(lambda: Own([1]) * R()), attempt(lambda: R() * Own([1])))
class Sub(list): pass
print("list and a subclass", type([1] + Sub([2])).__name__, type(Sub([1]) + [2]).__name__, type(Sub([1]) * 2).__name__, type(2 * Sub([1])).__name__, type(imul(Sub([1]), 2)).__name__, type(iadd(Sub([1]), [2])).__name__)
print("*= with the sequence on the right", [kind(attempt(lambda: imul(v(), s()))) for _, v in others if _ in ("2", "True", "X(2)", "R", "N", "NI", "1.5", "None", "1j") for s in (lambda: "ab", lambda: [1], lambda: b"ab")])
print("+= with the sequence on the right", [kind(attempt(lambda: iadd(v(), s()))) for _, v in others if _ in ("2", "X(2)", "R", "N", "None") for s in (lambda: "ab", lambda: [1], lambda: b"ab")])
print("too big", attempt(lambda: [1] * (1 << 62))[:40], attempt(lambda: "ab" * (1 << 62))[:40], attempt(lambda: (1,) * (1 << 62))[:40], attempt(lambda: b"ab" * (1 << 62))[:40], [] * (1 << 62), "" * (1 << 62), () * (1 << 62), b"" * (1 << 62))
