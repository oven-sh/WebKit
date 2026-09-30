# What is run often enough is compiled again, for what it has been given so far and nothing else. When it is then given something else it has to come to what it always would have. So each of these is
# run a great many times with one kind of thing, then with every kind, then with the first kind again and with every kind again, since what it makes of a surprise the second time is not what it makes of it the first.
# It is compared with the same thing written out again and given every kind from the start, which is compiled for none of them, and that is what is printed a digest of.
import warnings
from binascii import crc32

warnings.simplefilter("ignore") # ~True is on its way out, and what says so says where the file is.
inf, nan = float("inf"), float("nan")


class Index:
    def __index__(self): return 1
    def __repr__(self): return "Index()"


class Odd(list):
    def __getitem__(self, key): return ("Odd", key)
    def __setitem__(self, key, value): list.__setitem__(self, 0, ("set", key, value))
    def __bool__(self): return False


class Number:
    def __init__(self, n): self.n = n
    def __repr__(self): return "Number(%r)" % self.n
    def __add__(self, other): return Number(("add", self.n, other))
    def __radd__(self, other): return Number(("radd", self.n, other))
    def __lt__(self, other): return "less"
    def __eq__(self, other): return "equal"
    def __ne__(self, other): return ""
    def __neg__(self): return "negated"
    def __bool__(self): return self.n > 0
    __hash__ = None


VALUES = [0, 1, -1, 2, 3, -3, 7, 8, 31, 32, 33, 46341, 65536, 2**30, 2**31 - 1, -2**31, 2**31, -2**31 - 1, 2**62, 10**20,
          0.0, -0.0, 1.0, -1.0, 2.0, 0.5, -0.5, 1e300, 5e-324, 2147483647.0, 2147483648.0, -2147483648.0, inf, -inf, nan,
          True, False, None, "", "ab", "ac", (), (1, 2), (5, 6, 7), Index(), Number(1), Number(0)]
RIGHT = [0, 1, -1, 2, 3, -3, 8, 31, 32, 33, 2**31 - 1, -2**31, 2**31, 10**20, 0.0, -0.0, 2.0, 0.5, -0.5, inf, nan, True, None, "ab", (1, 2), Index(), Number(1)]
# Made afresh each time, since some of what is done changes them.
MAKERS = [lambda: [], lambda: [1], lambda: [4, 5, 6], lambda: [1.5, 2.0], lambda: ["a", None, 3], lambda: Odd([9, 8]), lambda: {1: "one", "ab": 2}, lambda: bytearray(b"xyz")]

TWO = ["a + b", "a - b", "a * b", "a / b", "a // b", "a % b", "a << b", "a >> b", "a & b", "a | b", "a ^ b",
       "a == b", "a != b", "a < b", "a <= b", "a > b", "a >= b",
       "'yes' if a < b else 'no'", "'yes' if a == b else 'no'", "'yes' if a != b else 'no'", "a[b]"]
ONE = ["-a", "+a", "~a", "not a", "'yes' if a else 'no'", "'yes' if not a else 'no'", "a + 1", "1 - a", "a * 3", "a * 0.5", "a + 0.5", "a / 2", "a / 0", "2 / a", "a // 7", "a // 8", "a // -1", "a // 0",
       "a % 7", "a % 8", "a % -7", "a % 1", "a % 0", "a << 1", "a << 3", "a << 30", "a << 31", "a >> 1", "a >> 31", "a >> 40", "a & 7", "a | 1", "a ^ -1", "a == 1", "a != 1", "a < 3", "a >= 0.5", "a == 'ab'",
       "a[0]", "a[1]", "a[-1]", "a[5]"]
WARM = {
    "ints": [(1, 2), (30, 4), (-7, 3), (1000, 9)],
    "floats": [(0.5, 1.5), (-2.25, 0.125), (1e10, 3.5)],
    "whole floats": [(2.0, 3.0), (-4.0, 1.0)],
    "an int and a float": [(1, 0.5), (7, 2.0)],
    "strings": [("ab", "ac"), ("", "x")],
    "bools": [(True, False), (True, True)],
    "None": [(None, None)],
    "a list and an int": [([4, 5, 6], 1), ([7, 8], 0)],
    "a tuple and an int": [((4, 5, 6), 1), ((7, 8), 0)],
}


def make(source):
    namespace = {}
    exec("def f(a, b): return " + source, namespace)
    return namespace["f"]


def outcome(f, a, b):
    try:
        r = f(a, b)
        return type(r).__name__, repr(r)
    except Exception as e:
        return type(e).__name__, str(e)


def is_big(x):
    return type(x) is int and abs(x) > 100


def everything(f, source, takes_two):
    results = []
    for make_a in [lambda v=v: v for v in VALUES] + MAKERS:
        if takes_two:
            for b in RIGHT:
                # There is no room for what would come of these, or no time.
                if (source == "a << b" and is_big(b)) or (source == "a * b" and (is_big(b) or is_big(make_a())) and not (isinstance(make_a(), (int, float)) and isinstance(b, (int, float)))):
                    continue
                results.append(outcome(f, make_a(), b))
        else:
            results.append(outcome(f, make_a(), None))
    return results


different = 0
for takes_two, sources in ((True, TWO), (False, ONE)):
    for source in sources:
        expected = everything(make(source), source, takes_two)
        print(source, "=>", len(expected), crc32(repr(expected).encode()), sum(1 for kind, said in expected if kind.endswith("Error")), "errors")
        for kind, pairs in WARM.items():
            f = make(source)
            for again in range(2):
                for i in range(600 // len(pairs)):
                    for a, b in pairs:
                        outcome(f, a, b)
                got = everything(f, source, takes_two)
                if got != expected:
                    different += 1
                    where = [i for i in range(len(got)) if got[i] != expected[i]]
                    print("    DIFFERENT after", kind, "the", ("first", "second")[again], "time:", len(where), "of them, the first being number", where[0], got[where[0]], "and not", expected[where[0]])
print("different:", different)

print("---- giving something a place in a list")
STORES = ["a[b] = c", "a[0] = c", "a[-1] = c", "a[1] = 5", "a[1] = 2.0"]
STORE_WARM = {
    "ints in a list of ints": [(lambda: [1, 2, 3], 1, 7)],
    "floats in a list of floats": [(lambda: [1.5, 2.5, 3.5], 1, 0.5)],
    "strings in a list of anything": [(lambda: ["a", None, 3], 2, "z")],
}
STORED = [0, 5, 2**31, 2.0, 0.5, -0.0, nan, None, "s", (1,), True]


def make_store(source):
    namespace = {}
    exec("def f(a, b, c):\n    " + source + "\n    return a", namespace)
    return namespace["f"]


def store_outcome(f, a, b, c):
    try:
        r = f(a, b, c)
        return type(r).__name__, repr(r), [type(x).__name__ for x in r] if isinstance(r, list) else None
    except Exception as e:
        return type(e).__name__, str(e), None


def every_store(f):
    return [store_outcome(f, make_a(), b, c) for make_a in MAKERS + [lambda: (1, 2), lambda: None, lambda: 3] for b in (0, 1, 2, -1, -3, -4, 3, 2**31, 1.0, True, None, "ab", Index()) for c in STORED]


different = 0
for source in STORES:
    expected = every_store(make_store(source))
    print(source, "=>", len(expected), crc32(repr(expected).encode()))
    for kind, triples in STORE_WARM.items():
        f = make_store(source)
        for again in range(2):
            for i in range(600):
                for make_a, b, c in triples:
                    store_outcome(f, make_a(), b, c)
            got = every_store(f)
            if got != expected:
                different += 1
                where = [i for i in range(len(got)) if got[i] != expected[i]]
                print("    DIFFERENT after", kind, "the", ("first", "second")[again], "time:", len(where), "of them, the first being number", where[0], got[where[0]], "and not", expected[where[0]])
print("different:", different)

print("---- in a loop, where what a variable has changes as it goes")


def sums(values):
    total = values[0]
    for v in values[1:]:
        total = total + v
    return total


def products(values):
    total = values[0]
    for v in values[1:]:
        total *= v
    return total


def counts(values):
    n = 0
    for v in values:
        if v:
            n += 1
    return n


def largest(values):
    best = values[0]
    for v in values:
        if v > best:
            best = v
    return best


for f in (sums, products, counts, largest):
    for label, values in (("ints", [1, 2, 3] * 700), ("that get too big", [2**20] * 2100), ("and then a float", [1] * 2000 + [0.5] + [1] * 100), ("whole floats", [1.0, 2.0] * 1000), ("a whole float at the end", [1] * 2000 + [2.0]),
                          ("bools", [True, False] * 1000), ("ints again", [3, 2, 1] * 700), ("a NaN among them", [1.5] * 2000 + [nan] + [2.5] * 10)):
        r = outcome(lambda a, b: f(a), values, None)
        print(f.__name__, label, "=>", r[0], r[1] if len(r[1]) < 60 else (len(r[1]), crc32(r[1].encode())))

print("---- taking apart what has only just been put together")


def swap(n):
    a, b = 0, 1
    for i in range(n):
        a, b = b, a + b & 0xFFFF
    return a, b


def rotate(n):
    a, b, c = 1, 2.0, "three"
    for i in range(n):
        a, b, c = b, c, a
    return a, b, c


def in_order(n):
    log = []
    def note(x):
        log.append(x)
        return x
    for i in range(n):
        a, b = note(i), note(-i)
    return a, b, len(log), log[-4:]


def wrong_number(n):
    made = 0
    for i in range(n):
        try:
            if i == n - 1:
                a, b = i, i, i
            else:
                a, b = i, i
            made += 1
        except ValueError as e:
            return made, str(e)


def starred(n):
    for i in range(n):
        a, *b = i, i + 1, i + 2
        *c, d = a, b
    return a, b, c, d


def kept(n):
    for i in range(n):
        t = i, i + 1
        a, b = t
    return t, a, b, t is t


for f in (swap, rotate, in_order, wrong_number, starred, kept):
    print(f.__name__, "=>", f(3000), f(3001))
