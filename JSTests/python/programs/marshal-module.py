# marshal. From version 3 on CPython marks what has more than one reference to it, which is not something that can be the same everywhere, so what is written is compared byte for byte in the versions before that, and
# in the rest by what comes of reading it.
import sys
import marshal
from marshal import dumps, loads, dump, load


def t(label, f):
    try:
        r = f()
    except BaseException as e:
        r = type(e).__name__ + ": " + str(e)
    print(label, "=>", r if isinstance(r, str) else repr(r))


def show(x, seen=None):
    "With the class of everything, and what is the same object as what."
    seen = seen if seen is not None else []
    for i, other in enumerate(seen):
        if other is x and isinstance(x, (list, dict, set)):
            return "<again %d>" % i
    if isinstance(x, (list, dict, set)):
        seen.append(x)
    if type(x) in (list, tuple):
        return type(x).__name__ + "(" + ", ".join(show(i, seen) for i in x) + ")"
    if type(x) is dict:
        return "dict(" + ", ".join(show(k, seen) + ": " + show(v, seen) for k, v in x.items()) + ")"
    if type(x) in (set, frozenset):
        return type(x).__name__ + "(" + ", ".join(sorted(show(i, seen) for i in x)) + ")"
    if type(x) is slice:
        return "slice(" + ", ".join(show(i, seen) for i in (x.start, x.stop, x.step)) + ")"
    return type(x).__name__ + ":" + repr(x)


print("---- the module")
t("what is in it", lambda: (sorted(k for k in vars(marshal) if not k.startswith("__")), marshal.version, marshal.__name__))

VALUES = [
    None, True, False, ..., StopIteration, 0, 1, -1, 255, 256, -256, 32767, 32768, 2 ** 15 - 1, 2 ** 15, 2 ** 30, 2 ** 31 - 1, 2 ** 31, -2 ** 31, -2 ** 31 - 1, 2 ** 32, 2 ** 45, 2 ** 60 - 1, 2 ** 63 - 1, 2 ** 63, -2 ** 63, 2 ** 64,
    2 ** 64 - 1, 2 ** 75 + 12345, -(2 ** 90), 10 ** 40, -(10 ** 100) + 7, 2 ** 1000, (1 << 15 * 7) - 1, 1 << 15 * 7, 1 << 128, (1 << 128) - 1,
    0.0, -0.0, 1.0, -1.5, 3.141592653589793, 1e300, 1e-300, 5e-324, 1.7976931348623157e308, float("inf"), float("-inf"), 0.1, 1 / 3, 123456789012345678.0, 1e16, 1e17, 1e-5,
    0j, 1j, -1.5 + 2.5j, complex(-0.0, -0.0), complex(float("inf"), 1e-300),
    "", "a", "abc", "hello world", "x" * 255, "x" * 256, "x" * 1000, "é", "\xff", "\x80", "日本語", "\U0001F600", "a\U0001F600b", "\ud800", "\udfff", "a\udc80b", "\x00", "a\x00b", "\x7f", "é" * 300,
    b"", b"a", b"\x00\xff", b"x" * 300, bytes(range(256)),
    (), (1,), (1, 2, 3), (None, True, (1, (2, (3,)))), tuple(range(255)), tuple(range(256)), tuple(range(300)),
    [], [1], [1, [2, [3, []]]], list(range(300)), [None] * 3,
    {}, {1: 2}, {"a": 1, "b": [2], (1, 2): {3: 4}}, {None: None}, {1.5: "x", 2j: b"y"},
    set(), {1}, {1, 2, 3}, {"a", "b", "c"}, {1, "a", 2.5, None, (1, 2), b"x", True}, frozenset(), frozenset({1}), frozenset({3, 1, 2}), frozenset({frozenset({1}), frozenset()}), {frozenset({1, 2}), 5},
    set(range(100)), {"x" * i for i in range(20)}, {-1, -2, 2 ** 40, -2 ** 40, 0},
]

print("---- written, in the versions in which nothing is marked")
for version in (0, 1, 2):
    for value in VALUES:
        t("dumps(%s, %d)" % (show(value)[:60], version), lambda: dumps(value, version).hex())

print("---- and read back, in all of them")
for version in (0, 1, 2, 3, 4, 5, 6, 100, -1):
    wrong = []
    for value in VALUES:
        back = loads(dumps(value, version))
        if show(back) != show(value):
            wrong.append((show(value)[:50], show(back)[:50]))
    t("version %d" % version, lambda: wrong)
t("nan", lambda: [(repr(loads(dumps(float("nan"), v))), dumps(float("nan"), v).hex()) for v in (0, 2)])

print("---- slices")
for value in (slice(None), slice(1, 2, 3), slice("a", (1,), [2]), slice(slice(1), None, None)):
    t("%r" % (value,), lambda: [show(loads(dumps(value, v))) for v in (5, 6)])
    t("%r, before there were any" % (value,), lambda: dumps(value, 4))

print("---- what is the same thing is still")
shared = [1, 2]
text = "a string that is not a name"
big = 2 ** 100
for version in (2, 3, 4, 5):
    def again():
        back = loads(dumps([shared, shared, text, text, big, big, (shared,), {"k": shared}], version))
        return back[0] is back[1], back[2] is back[3], back[4] is back[5], back[6][0] is back[0], back[7]["k"] is back[0]
    t("version %d" % version, again)
loop = []
loop.append(loop)
t("a list that is in itself", lambda: show(loads(dumps(loop))))
t("before version 3", lambda: dumps(loop, 2))
loop_dict = {}
loop_dict["self"] = loop_dict
t("a dict", lambda: show(loads(dumps(loop_dict))))
in_tuple = []
holder = (in_tuple,)
in_tuple.append(holder)
t("a tuple, by way of a list", lambda: (lambda b: (type(b).__name__, b[0][0] is b))(loads(dumps(holder))))
in_slice = []
a_slice = slice(in_slice)
in_slice.append(a_slice)
t("a slice, by way of a list", lambda: dumps(a_slice))

print("---- how deep")


def nested(n, make=lambda x: [x]):
    x = None
    for i in range(n):
        x = make(x)
    return x


for n in (100, 1000, 1998, 1999, 2000, 2001, 5000):
    t("%d lists" % n, lambda: len(dumps(nested(n), 2)))
    t("%d tuples" % n, lambda: len(dumps(nested(n, lambda x: (x,)), 2)))
for n in (1998, 1999, 2000, 2001):
    t("%d lists read" % n, lambda: type(loads(b"[\x01\x00\x00\x00" * n + b"N")).__name__)
    t("%d tuples read" % n, lambda: type(loads(b")\x01" * n + b"N")).__name__)

print("---- what cannot be written")


class I(int): pass
class S(str): pass
class L(list): pass
class D(dict): pass
class T(tuple): pass
class B(bytes): pass
class F(float): pass
class FS(frozenset): pass


for label, value in (("object()", object()), ("a class", int), ("a function", t), ("a module", sys), ("derived from int", I(1)), ("from str", S("a")), ("from list", L()), ("from dict", D()), ("from tuple", T()),
                     ("from float", F(1.0)), ("from frozenset", FS()), ("a range", range(3)), ("NotImplemented", NotImplemented), ("an exception", ValueError()), ("ValueError", ValueError), ("in a list", [1, object()]),
                     ("in a dict", {1: object()}), ("in a set", {object()}), ("a generator", (x for x in ())), ("an iterator", iter([]))):
    t(label, lambda: dumps(value))
for label, value in (("derived from bytes", B(b"ab")), ("a bytearray", bytearray(b"ab")), ("a memoryview", memoryview(b"ab")), ("a memoryview of part", memoryview(b"abcdef")[1:3])):
    t(label + ", which is written as bytes", lambda: (dumps(value, 2).hex(), show(loads(dumps(value)))))
t("a memoryview that is not all together", lambda: dumps(memoryview(b"abcdef")[::2]))

print("---- read from what is given here")
for label, data in (
    ("None", "4e"), ("marked, which None is not", "ce"), ("true", "54"), ("false", "46"), ("StopIteration", "53"), ("...", "2e"), ("int", "6901000000"), ("negative", "69ffffffff"), ("int64", "490100000000000000"),
    ("int64, negative", "49ffffffffffffffff"), ("int64, large", "49ffffffffffffff7f"), ("long 0", "6c00000000"), ("long 1", "6c010000000100"), ("long -1", "6cffffffff0100"), ("long 2**15", "6c0200000000000100"),
    ("long, small enough for an int", "6c020000000100" + "0100"), ("float as text", "6603312e35"), ("float inf", "6603696e66"), ("float nan", "66036e616e"), ("float 1e5", "6603316535"), ("binary float", "67000000000000f83f"),
    ("complex as text", "780131" + "0132"), ("binary complex", "79000000000000f03f0000000000000040"), ("bytes", "73020000006162"), ("unicode", "7502000000c3a9"), ("interned", "74020000006162"), ("ascii", "61020000006162"),
    ("ascii, interned", "41020000006162"), ("short ascii", "7a026162"), ("short, interned", "5a026162"), ("ascii that is not", "7a02e9ff"), ("a lone surrogate", "7503000000eda080"), ("empty unicode", "7500000000"),
    ("tuple", "28020000004e54"), ("small tuple", "29024e54"), ("list", "5b020000004e54"), ("dict", "7b4e5430"), ("dict with no value for the last", "7b4e544630"), ("empty dict", "7b30"), ("set", "3c02000000" + "6901000000" * 2),
    ("frozenset", "3e01000000" + "6901000000"), ("empty frozenset", "3e00000000"), ("slice", "3a4e69010000004e"),
    ("a reference", "5b02000000" + "e901000000" + "7200000000"), ("to a list, in itself", "db01000000" + "7200000000"), ("to a str", "2902" + "da0161" + "7200000000"), ("two", "2904" + "e901000000" + "e902000000" + "7201000000" + "7200000000"),
    ("more after it", "4e4e4e"),
):
    t(label, lambda: show(loads(bytes.fromhex(data))))

print("---- and what is wrong with it")
for label, data in (
    ("nothing", ""), ("null", "30"), ("marked null", "b0"), ("unknown", "3f"), ("no such type", "21"), ("another", "00"), ("0xff", "ff"), ("short int", "690100"), ("short int64", "4901"), ("short long", "6c01"), ("long with no digits", "6c01000000"),
    ("long with half a digit", "6c0100000001"), ("digit out of range", "6c01000000ffff"), ("digit that is 2**15", "6c010000000080"), ("in the middle", "6c02000000" + "0080" + "0100"), ("negative digit", "6c020000000180" + "0100"),
    ("unnormalized", "6c02000000" + "0100" + "0000"), ("a bad digit and too short", "6c05000000" + "ffff"), ("long of size -2**31", "6c00000080"), ("long of a great many", "6cffffff7f0100"),
    ("float with no length", "66"), ("float too short", "660531"), ("float that is none", "66026162"), ("empty float", "6600"), ("float with a space", "660320312e"), ("with an underscore", "6603315f31"), ("with a zero in it", "660331003a"),
    ("hex float", "6603307831"), ("short binary float", "6700"), ("complex with half", "780131"), ("short binary complex", "79000000000000f03f"),
    ("bytes too short", "730500000061"), ("bytes of negative size", "73ffffffff"), ("bytes with no size", "7301"), ("bytes of a great many", "73ffffff7f61"), ("unicode too short", "750500000061"), ("of negative size", "75ffffffff"),
    ("bad utf-8", "7501000000ff"), ("cut short", "7501000000c3"), ("overlong", "7502000000c080"), ("ascii too short", "610500000061"), ("of negative size", "61ffffffff"), ("short ascii too short", "7a0561"), ("with no length", "7a"),
    ("tuple too short", "28020000004e"), ("of negative size", "28ffffffff"), ("of a great many", "28ffffff7f4e"), ("of a great many, and something wrong", "28ffffff7f4e21"), ("of a great many, that refers to itself", "a8ffffff7f" + "7200000000" + "21"),
    ("with a null", "290130"), ("small tuple too short", "2902" + "4e"), ("with no length", "29"), ("list too short", "5b020000004e"), ("of negative size", "5bffffffff"), ("of a great many", "5bffffff7f4e"), ("with a null", "5b0100000030"),
    ("dict with no end", "7b4e54"), ("with no end after a key", "7b4e"), ("with a key that cannot be hashed", "7b5b000000004e30"), ("with something wrong for a value", "7b4e21"), ("set too short", "3c020000004e"), ("of negative size", "3cffffffff"),
    ("of a great many", "3cffffff7f4e"), ("with a null", "3c0100000030"), ("with what cannot be hashed", "3c010000005b00000000"), ("frozenset with what cannot be hashed", "3e010000005b00000000"),
    ("slice too short", "3a4e4e"), ("with a null", "3a304e4e"), ("with a null at the end", "3a4e4e30"), ("in a list", "5b01000000" + "3a4e4e30"),
    ("reference to nothing", "7200000000"), ("negative", "72ffffffff"), ("too short", "7200"), ("past the end", "2902" + "e901000000" + "7201000000"), ("to a frozenset from inside it", "be01000000" + "7200000000"),
    ("to a slice from inside it", "ba" + "7200000000" + "4e4e"), ("to a tuple from inside it", "a901" + "7200000000"),
):
    t(label, lambda: show(loads(bytes.fromhex(data))))

print("---- arguments")
t("dumps()", lambda: dumps())
t("dumps(1, 2, 3)", lambda: dumps(1, 2, 3))
t("dumps(1, version=2)", lambda: dumps(1, version=2))
t("dumps(1, 'a')", lambda: dumps(1, "a"))
t("dumps(1, 2 ** 40)", lambda: dumps(1, 2 ** 40))
t("dumps(1, 1.5)", lambda: dumps(1, 1.5))
t("dumps(1, allow_code=[])", lambda: dumps(1, allow_code=[]).hex()[-8:])
t("dumps(1, other=1)", lambda: dumps(1, other=1))
t("loads()", lambda: loads())
t("loads('a')", lambda: loads("a"))
t("loads(1)", lambda: loads(1))
t("loads(None)", lambda: loads(None))
t("loads(b'N', 1)", lambda: loads(b"N", 1))
t("loads(bytes=b'N')", lambda: loads(bytes=b"N"))
t("loads of other things that have bytes", lambda: (loads(bytearray(b"T")), loads(memoryview(b"F")), loads(memoryview(b"xNx")[1:2])))
t("loads of a memoryview that is not all together", lambda: loads(memoryview(b"NxTx")[::2]))

print("---- files")


class Out:
    def __init__(self): self.data = []
    def write(self, b):
        self.data.append(b)
        return "what write() gives"


class In:
    def __init__(self, data, log=None):
        self.data, self.at, self.log = data, 0, log

    def read(self, n):
        if self.log is not None:
            self.log.append(("read", n))
        return b""

    def readinto(self, view):
        n = len(view)
        if self.log is not None:
            self.log.append(("readinto", type(view).__name__, n, view.readonly, view.format))
        piece = self.data[self.at:self.at + n]
        view[:len(piece)] = piece
        self.at += len(piece)
        return len(piece)


out = Out()
t("dump", lambda: (dump([1, "a"], out, 2), [type(b).__name__ + ":" + b.hex() for b in out.data]))
t("dump(1)", lambda: dump(1))
t("dump to what cannot be written to", lambda: dump(1, object()))
t("dump of what cannot be written", lambda: (dump(object(), out), len(out.data)))
t("dump(1, out, 2, allow_code=False)", lambda: dump(1, out, 2, allow_code=False))
t("dump(1, out, version=2)", lambda: dump(1, out, version=2))
log = []
t("load", lambda: (show(load(In(dumps([1, "abc", (2.5, b"xy"), 2 ** 70], 2), log))), log))
several = In(dumps(1, 2) + dumps("two", 2) + dumps([3], 2))
t("one after another", lambda: (load(several), load(several), load(several)))
t("and then no more", lambda: load(several))
t("cut short", lambda: load(In(dumps([1, 2, 3], 2)[:-2])))
t("load()", lambda: load())
t("load(1)", lambda: load(1))


class BadRead(In):
    def read(self, n): return "text"


class NoReadInto:
    def read(self, n): return b""


class TooMuch(In):
    def readinto(self, view): return len(view) + 1


class NotANumber(In):
    def readinto(self, view): return "a"


class Huge(In):
    def readinto(self, view): return 2 ** 70


class Raises(In):
    def readinto(self, view): raise KeyError("from readinto")


for kind in (BadRead, TooMuch, NotANumber, Huge, Raises):
    t(kind.__name__, lambda: load(kind(b"N")))
t("NoReadInto", lambda: load(NoReadInto()))

print("---- code")
SOURCE = '''
import sys
X = 10
def plain(a, b=2, *args, c, d=4, **kwargs):
    "a docstring"
    return (a, b, args, c, d, sorted(kwargs.items()), X)
def outer(n):
    total = [n]
    def inner(m):
        total[0] += m
        return total[0]
    return inner
def generator(n):
    for i in range(n):
        yield i * i
class C:
    attribute = "of the class"
    def method(self): return __class__.__name__, self.attribute
    @staticmethod
    def static(): return [x * 2 for x in range(3)]
async def coroutine(): return "from a coroutine"
squares = {k: v for k, v in zip("abc", (1, 4, 9))}
constants = (1, 2.5, "text", b"bytes", None, ..., (1, (2, 3)), frozenset({1, 2}), 2 ** 100, 1j)
try:
    raise ValueError("raised")
except ValueError as e:
    caught = (type(e).__name__, e.__traceback__.tb_lineno)
'''


def run(code):
    namespace = {"__name__": "loaded"}
    exec(code, namespace)
    inner = namespace["outer"](5)
    c = namespace["coroutine"]()
    try:
        c.send(None)
    except StopIteration as e:
        awaited = e.value
    return (namespace["plain"](1, c=3), namespace["plain"](1, 2, 3, c=4, e=5), inner(1), inner(2), list(namespace["generator"](4)), namespace["C"]().method(), namespace["C"].static(), awaited, namespace["squares"],
            namespace["constants"], namespace["caught"], namespace["plain"].__doc__, namespace["plain"].__code__.co_filename, namespace["plain"].__code__.co_firstlineno, namespace["C"].method.__qualname__)


code = compile(SOURCE, "the file", "exec")
t("as it is", lambda: run(code))
for version in (0, 2, 3, 4, 5):
    t("written and read, version %d" % version, lambda: run(loads(dumps(code, version))) == run(code))
back = loads(dumps(code))
ATTRIBUTES = ("co_argcount", "co_posonlyargcount", "co_kwonlyargcount", "co_nlocals", "co_flags", "co_names", "co_varnames", "co_freevars", "co_cellvars", "co_filename", "co_name", "co_qualname", "co_firstlineno")


def walk(a, b, path="module"):
    different = [(path, name) for name in ATTRIBUTES if getattr(a, name) != getattr(b, name)]
    if list(a.co_lines()) != list(b.co_lines()):
        different.append((path, "co_lines"))
    if len(a.co_consts) != len(b.co_consts):
        return different + [(path, "co_consts")]
    for x, y in zip(a.co_consts, b.co_consts):
        if type(x) is type(code):
            different += walk(x, y, path + "." + x.co_name)
        elif show(x) != show(y):
            different.append((path, "constant", show(x)))
    return different


t("it is the same all through", lambda: (walk(code, back), back == code, hash(back) == hash(code), back is code))
t("a function's", lambda: (lambda f: (loads(dumps(f.__code__)) == f.__code__, type(f)(loads(dumps(f.__code__)), {"X": 1})(1, c=2)))(run.__globals__["t"].__class__(code.co_consts[[getattr(c, "co_name", None) for c in code.co_consts].index("plain")], {"X": 0})))
t("an expression's", lambda: eval(loads(dumps(compile("1 + x * 2", "<e>", "eval"))), {"x": 5}))
t("from a tree", lambda: eval(loads(dumps(compile(compile("[y for y in range(x)]", "<a>", "eval", 0x400), "<t>", "eval"))), {"x": 3}))
t("in a tuple, twice", lambda: (lambda b: (b[0] == code, b[0] is b[1]))(loads(dumps((code, code)))))
t("not allowed to be written", lambda: dumps(code, allow_code=False))
t("in a list", lambda: dumps([1, code], allow_code=False))
t("in a set", lambda: dumps({code}, allow_code=False))
t("not allowed to be read", lambda: loads(dumps(code), allow_code=False))
t("nor from a file", lambda: load(In(dumps(code)), allow_code=False))
t("nor to one", lambda: dump(code, Out(), allow_code=False))
def attempt(data):
    try:
        return loads(data)
    except Exception as e:
        return e


t("cut short", lambda: [type(attempt(dumps(code, 2)[:n])).__name__ for n in (1, 5, 21, 22, 30)])

print("---- who is told")
told = []
sys.addaudithook(lambda event, args: told.append((event, tuple(a if isinstance(a, (int, bytes)) else type(a).__name__ for a in args))) if event.startswith(("marshal.", "code.")) else None)
t("dumps", lambda: (dumps([1], 2), told[:], told.clear()))
t("of a set, for each thing in it", lambda: (dumps({1, 2}, 2), told[:], told.clear()))
t("loads", lambda: (loads(b"N"), told[:], told.clear()))
t("dump", lambda: (dump(1, Out(), 2), told[:], told.clear()))
t("load", lambda: (load(In(b"N")), told[:], told.clear()))
t("loads of code", lambda: (type(loads(dumps(compile("1", "f", "eval")))).__name__, [e for e, a in told], told.clear()))

print("---- what is right, with something changed")
state = [2463534242]


def random(n):
    x = state[0]
    x ^= (x << 13) & 0xFFFFFFFF
    x ^= x >> 17
    x ^= (x << 5) & 0xFFFFFFFF
    state[0] = x
    return x % n


SEEDS = [dumps(v, version) for version in (0, 2) for v in ([1, "abc", (2.5, b"xy", None), {"k": [True, 2 ** 70], 3: {4, 5}}, frozenset({"a", 1j}), -2 ** 40], {"a": (1, 2), "b": "é日", (): []}, (1.5, 2j, "x" * 300, [[[]]]))]
SEEDS += [bytes.fromhex("db03000000" + "e901000000" + "da03616263" + "a902" + "7200000000" + "7201000000"), bytes.fromhex("ba" + "e901000000" + "7201000000" + "4e"), bytes.fromhex("fb" + "da0161" + "7200000000" + "30")]
outcomes = {}
lines = []
for i in range(6000):
    data = bytearray(SEEDS[random(len(SEEDS))])
    for change in range(1 + random(3)):
        what = random(4)
        at = random(len(data))
        if what == 0:
            data[at] = random(256)
        elif what == 1:
            data[at] ^= 1 << random(8)
        elif what == 2:
            del data[at:at + 1 + random(4)]
        else:
            data[at:at] = bytes([b"0NFTS.gyls([{u?<>:taAzZi)xfIr"[random(29)] | (0x80 if random(4) == 0 else 0)])
        if not data:
            data = bytearray(b"N")
    # CPython makes room for as many as a tuple or a list is said to have before it finds that they are not there, which takes it a while.
    if any(data[p] & 0x7F in b"([" and 100000 <= int.from_bytes(data[p + 1:p + 5], "little") < 2 ** 31 for p in range(len(data) - 4)):
        outcomes["passed over"] = outcomes.get("passed over", 0) + 1
        continue
    try:
        result = show(loads(bytes(data)))
    except RecursionError:
        result = "RecursionError"
    except Exception as e:
        result = type(e).__name__ + ": " + str(e)
    kind = result.split(":")[0] if not result.startswith(("list", "dict", "tuple")) else "read"
    outcomes[kind] = outcomes.get(kind, 0) + 1
    lines.append(result)
t("how it went", lambda: sorted(outcomes.items()))
digest = 0
for n, line in enumerate(lines):
    for ch in line:
        digest = (digest * 31 + ord(ch)) % 1000000007
    if n % 250 == 249:
        print("  after", n + 1, digest)
