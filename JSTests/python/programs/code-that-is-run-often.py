# What is run often is compiled, and what is compiled does for itself what is most often wanted: of numbers, of lists and tuples, of going through them. It is to come to the same thing as when it is left to what is
# written out in full. So everything here is done many times over, with what is for the one and what is for the other by turns.
import collections, warnings

warnings.simplefilter("ignore") # ~True is on its way out, and what says so says where the file is.
inf, nan = float("inf"), float("nan")
VALUES = [0, 1, -1, 2, 3, -3, 7, 31, 32, 33, 64, 46341, 65536, 2**30, 2**31 - 1, -2**31, 2**31, -2**31 - 1, 2**53, 2**62, -2**63, 10**20,
          0.0, -0.0, 1.0, -1.0, 2.0, 0.5, -0.5, 1e300, 5e-324, 2147483647.0, 2147483648.0, -2147483648.0, -2147483649.0, inf, -inf, nan,
          True, False, None, "", "ab", (), (1, 2), [], [1]]

def add(a, b): return a + b
def sub(a, b): return a - b
def mul(a, b): return a * b
def div(a, b): return a / b
def floordiv(a, b): return a // b
def mod(a, b): return a % b
def lshift(a, b): return a << b
def rshift(a, b): return a >> b
def and_(a, b): return a & b
def or_(a, b): return a | b
def xor(a, b): return a ^ b
def iadd(a, b): a += b; return a
def isub(a, b): a -= b; return a
def imul(a, b): a *= b; return a
def idiv(a, b): a /= b; return a
def ifloordiv(a, b): a //= b; return a
def imod(a, b): a %= b; return a
def ilshift(a, b): a <<= b; return a
def irshift(a, b): a >>= b; return a
def iand(a, b): a &= b; return a
def ior(a, b): a |= b; return a
def ixor(a, b): a ^= b; return a
def eq(a, b): return a == b
def ne(a, b): return a != b
def lt(a, b): return a < b
def le(a, b): return a <= b
def gt(a, b): return a > b
def ge(a, b): return a >= b
def is_(a, b): return a is b
def is_not(a, b): return a is not b
def if_lt(a, b): return "yes" if a < b else "no"
def if_eq(a, b): return "yes" if a == b else "no"
def while_ne(a, b):
    n = 0
    while a != b and n < 2: n += 1
    return n
# With one of them written out.
def add_1(a, b): return a + 1
def sub_1(a, b): return a - 1
def rsub_1(a, b): return 1 - a
def mul_3(a, b): return a * 3
def and_7(a, b): return a & 7
def mod_7(a, b): return a % 7
def mod_minus_7(a, b): return a % -7
def floordiv_7(a, b): return a // 7
def floordiv_minus_1(a, b): return a // -1
def lshift_1(a, b): return a << 1
def lshift_31(a, b): return a << 31
def rshift_1(a, b): return a >> 1
def add_half(a, b): return a + 0.5
def mul_2_0(a, b): return a * 2.0
def div_2(a, b): return a / 2
def lt_0(a, b): return a < 0
def eq_1_0(a, b): return a == 1.0
def is_none(a, b): return a is None
def is_not_none(a, b): return a is not None
def neg(a, b): return -a
def pos(a, b): return +a
def invert(a, b): return ~a
def not_(a, b): return not a
def truth(a, b): return "yes" if a else "no"
def both(a, b): return a and b
def either(a, b): return a or b

BINARY = [add, sub, mul, div, floordiv, mod, lshift, rshift, and_, or_, xor, iadd, isub, imul, idiv, ifloordiv, imod, ilshift, irshift, iand, ior, ixor, eq, ne, lt, le, gt, ge, is_, is_not, if_lt, if_eq, while_ne, both, either]
UNARY = [add_1, sub_1, rsub_1, mul_3, and_7, mod_7, mod_minus_7, floordiv_7, floordiv_minus_1, lshift_1, lshift_31, rshift_1, add_half, mul_2_0, div_2, lt_0, eq_1_0, is_none, is_not_none, neg, pos, invert, not_, truth]

def shown(f, a, b):
    if f in (lshift, ilshift) and type(b) is int and b > 100: return "too far"
    if f in (mul, imul) and (type(a) is int and abs(a) > 100 and type(b) in (str, tuple, list) and b or type(b) is int and abs(b) > 100 and type(a) in (str, tuple, list) and a): return "too many"
    # A list is changed by += and *=, so each has its own.
    if type(a) is list: a = list(a)
    try: r = f(a, b)
    except Exception as e: return type(e).__name__
    return type(r).__name__ + " " + repr(r)

def summed(text, total):
    for c in text: total = (total * 31 + ord(c)) % 1000000007
    return total

for turn in range(3):
    lines = []
    for f in BINARY:
        for a in VALUES:
            total = 0
            for b in VALUES: total = summed(shown(f, a, b), total)
            lines.append("%s %r => %d" % (f.__name__, a, total))
    for f in UNARY:
        lines.append("%s => %s" % (f.__name__, " | ".join(shown(f, a, None) for a in VALUES)))
    if turn and lines != before: print("it was otherwise the time before")
    before = lines
for line in lines: print(line)

# ---- What is in a list or a tuple

class Backwards(list):
    def __getitem__(self, i): return ("mine", i)
    def __setitem__(self, i, v): list.__setitem__(self, 0, ("set", i, v))
class Pair(tuple):
    def __getitem__(self, i): return ("mine", i)
class Plain(list): pass
Named = collections.namedtuple("Named", "x y")
class Index:
    def __index__(self): return 1
    def __repr__(self): return "Index()"

def item(s, i): return s[i]
def set_item(s, i, v): s[i] = v; return s
def literal(i, v):
    s = [1, 2, 3]
    s[i] = v
    return s
def literal_of_all_sorts(i, v):
    s = [1, "two", 3.0]
    s[i] = v
    return s
SEQUENCES = [lambda: [], lambda: [10], lambda: [10, 20, 30], lambda: [1.5, 2.5], lambda: ["a", None, 3], lambda: (), lambda: (10,), lambda: (10, 20, 30), lambda: Backwards([1, 2]), lambda: Pair((1, 2)), lambda: Plain([1, 2]), lambda: Named(1, 2),
             lambda: "abc", lambda: b"abc", lambda: bytearray(b"abc"), lambda: range(5), lambda: {0: "zero", 1: "one", -1: "minus one", True: "true"}, lambda: None, lambda: 5, lambda: collections.deque([1, 2, 3]), lambda: [0] * 3, lambda: [[]] * 2]
KEYS = [0, 1, 2, 3, -1, -2, -3, -4, 2**31 - 1, -2**31, 2**31, 2**70, -2**70, True, False, 1.0, 0.0, None, "a", Index(), slice(1, None), (0,)]
def attempt(f, *a):
    try: return f(*a)
    except Exception as e: return type(e).__name__
for turn in range(30):
    lines = []
    for make in SEQUENCES:
        lines.append("%r: %r" % (make(), [attempt(item, make(), k) for k in KEYS]))
        for v in (7, "x", 1.0, 2.5, 2**40, None, True):
            lines.append("%r, set to %r: %r" % (make(), v, [attempt(set_item, make(), k, v) for k in KEYS[:16]]))
    for v in (7, "x", 1.0, 2.5, 2**40, None):
        lines.append("what is written out, with %r: %r %r" % (v, [attempt(literal, k, v) for k in KEYS[:8]], [attempt(literal_of_all_sorts, k, v) for k in KEYS[:8]]))
    if turn and lines != before: print("it was otherwise the time before")
    before = lines
for line in lines: print(line)
print([type(x).__name__ for x in set_item([1, 2, 3, 4], 1, 1.0)], [type(x).__name__ for x in set_item(set_item([1, 2], 0, 1.0), 1, True)])

# ---- Going through them

def gone_through(s):
    got = []
    for x in s: got.append(x)
    return got
def added_to(n):
    s = [1, 2, 3]
    for x in s:
        if len(s) < n: s.append(x * 10)
    return s
def taken_from():
    s = [1, 2, 3, 4, 5, 6]
    got = []
    for x in s:
        got.append(x)
        s.pop()
    return got, s
def emptied():
    s = [1, 2, 3]
    i = iter(s)
    got = [next(i)]
    s.clear()
    got.append(next(i, "no more"))
    s.extend([7, 8, 9])
    got.append(next(i, "still no more"))
    return got
def replaced():
    s = [1, 2, 3]
    got = []
    for x in s:
        got.append(x)
        s[-1] = "another"
        s[1] = 2.5
    return got
def twice(s):
    i = iter(s)
    return [x for x in i], [x for x in i], next(i, "no more")
def nested(n):
    return [(a, b) for a in range(n) for b in (a, -a) if a != 1]
def counted(*a):
    total = n = 0
    for i in range(*a):
        total += i
        n += 1
        if n > 6: break
    return n, total, (i if n else None), type(i if n else 0).__name__
RANGES = [(0,), (1,), (5,), (-5,), (2, 5), (5, 2), (5, 2, -1), (0, 10, 3), (10, 0, -3), (2**31 - 3, 2**31 + 3), (-2**31 + 3, -2**31 - 3, -1), (2**31 - 1, 2**31), (0, 2**33, 2**31), (2**31, 2**31 + 2), (2**62, 2**62 + 3), (2**63 - 2, 2**63 + 2), (2**70, 2**70 + 2),
          (0, 2**31 - 1, 2**30), (-2**31, 2**31 - 1, 2**31 - 1), (2**31 - 1, -2**31, -2**31), (True,), (0, 3, True)]
for turn in range(40):
    lines = [repr([gone_through(make()) if make() is not None and make() != 5 else None for make in SEQUENCES])]
    lines += [repr(f()) for f in (taken_from, emptied, replaced)] + [repr([added_to(n) for n in range(2, 9)]), repr([twice(make()) for make in SEQUENCES[:12]]), repr(nested(4))]
    lines += ["range%r: %r" % (a, counted(*a)) for a in RANGES]
    if turn and lines != before: print("it was otherwise the time before")
    before = lines
for line in lines: print(line)

# ---- Taking them apart

def two(s): a, b = s; return b, a
def three(s): a, b, c = s; return c, b, a
def one(s): (a,) = s; return a
def none(s): () = s; return "nothing"
def swapped(a, b): a, b = b, a; return a, b
def starred(s): a, *b = s; return a, b
class Yields(tuple):
    def __iter__(self): return iter(("mine", "too"))
for turn in range(60):
    lines = []
    for f in (none, one, two, three, starred):
        lines.append("%s: %r" % (f.__name__, [attempt(f, s) for s in ((), (1,), (1, 2), (1, 2, 3), (1, 2, 3, 4), [], [1], [1, 2], [1, 2, 3], "ab", "abc", Named(1, 2), Yields((1, 2, 3)), Pair((1, 2)), Backwards([1, 2]), {1: 2, 3: 4}, range(2), None, 5, iter((1, 2)))]))
    lines.append(repr((swapped(1, 2), swapped("a", None), swapped(1.0, 2**40))))
    if turn and lines != before: print("it was otherwise the time before")
    before = lines
for line in lines: print(line)
