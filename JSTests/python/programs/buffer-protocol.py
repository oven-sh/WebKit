def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
def attempt(f):
    try: return f()
    except BaseException as e: return type(e).__name__ + ": " + str(e)
log = []
def took():
    r = list(log); log.clear(); return r
class B:
    def __init__(s, data=b"xyz"): s.data = data
    def __buffer__(s, flags): log.append(("get", flags)); return memoryview(s.data)
class R(B):
    def __release_buffer__(s, mv): log.append(("release", bytes(mv)))
# ---- wherever bytes are wanted
uses = {
    "memoryview": lambda x: through_memoryview(x), "bytes": lambda x: bytes(x), "bytearray": lambda x: bytearray(x), "join": lambda x: b"-".join([x, x]), "find": lambda x: b"axyzb".find(x),
    "rfind": lambda x: b"axyzb".rfind(x), "index": lambda x: b"axyzb".index(x), "count": lambda x: b"xyzxyz".count(x), "add": lambda x: b"a" + x, "bytearray add": lambda x: bytearray(b"a") + x,
    "iadd": lambda x: iadd(x), "startswith": lambda x: b"xyzw".startswith(x), "endswith": lambda x: b"wxyz".endswith(x), "startswith tuple": lambda x: b"xyzw".startswith((b"q", x)),
    "in": lambda x: x in b"axyz", "in bytearray": lambda x: x in bytearray(b"axyz"), "decode": lambda x: str(x, "ascii"), "split": lambda x: b"1xyz2".split(x), "rsplit": lambda x: b"1xyz2".rsplit(x),
    "partition": lambda x: named(b"1xyz2".partition(x)), "rpartition": lambda x: named(b"1xyz2".rpartition(x)), "bytearray partition": lambda x: bytearray(b"1xyz2").partition(x), "replace": lambda x: b"1xyz2".replace(x, b"-"), "replace with": lambda x: b"1-2".replace(b"-", x),
    "strip": lambda x: b"xy1zy".strip(x), "lstrip": lambda x: b"xy1zy".lstrip(x), "removeprefix": lambda x: b"xyz1".removeprefix(x), "removesuffix": lambda x: b"1xyz".removesuffix(x),
    "translate delete": lambda x: b"axbycz".translate(None, x), "maketrans": lambda x: bytes.maketrans(x, b"123")[119:123], "extend": lambda x: extend(x), "slice assign": lambda x: assign(x),
    "percent b": lambda x: b"<%b>" % x, "percent s": lambda x: b"<%s>" % x, "fromhex": lambda x: bytes.fromhex(B(b"6162")), "eq bytes": lambda x: b"xyz" == x, "eq bytearray": lambda x: bytearray(b"xyz") == x,
    "lt bytearray": lambda x: bytearray(b"xya") < x, "eq memoryview": lambda x: memoryview(b"xyz") == x, "ne memoryview": lambda x: memoryview(b"xyz") != x, "from_bytes": lambda x: int.from_bytes(x, "big"),
    "compile": lambda x: eval(B(b"1 + 1")), "exec": lambda x: exec(B(b"pass")), "decode error": lambda x: UnicodeDecodeError("a", x, 0, 1, "r").object,
}
def through_memoryview(x):
    # In CPython it would be released as soon as nothing referred to it.
    with memoryview(x) as m: return bytes(m)
def named(t): return tuple(type(x).__name__ if type(x).__name__ == "_buffer_wrapper" else x for x in t)
def iadd(x):
    b = bytearray(b"a"); b += x; return b
def extend(x):
    b = bytearray(b"a"); b.extend(x); return b
def assign(x):
    b = bytearray(b"abc"); b[1:2] = x; return b
for name, use in uses.items():
    show(name, lambda: (use(B()), took()))
    log.clear()
    show(name + ", released", lambda: (use(R()), took()))
    log.clear()
show("not on the left", lambda: B() + b"a")
show("center fill", lambda: b"a".center(3, B(b"-")))
show("hex sep", lambda: b"ab".hex(B(b"-")))
show("percent c", lambda: b"%c" % B(b"a"))
show("int", lambda: [int(x) for x in (b"12", bytearray(b" 12 "), memoryview(b"-7"), B(b"12"), b"1_000")])
show("int with base", lambda: [int(b"ff", 16), int(bytearray(b"11"), 2), int(b"0x1f", 0)])
show("int with base of others", lambda: int(memoryview(b"11"), 2))
show("int with base of a program's", lambda: int(B(b"11"), 2))
for bad in (b"x", b"", b"1\x002", b"\xff", bytearray(b"1.5"), memoryview(b"z"), b"1" * 300 + b"z"):
    show("int of " + repr(bytes(bad))[:20], lambda: int(bad))
show("int of a program's, bad", lambda: int(B(b"q")))
show("float", lambda: [float(x) for x in (b"1.5", bytearray(b" 2 "), memoryview(b"-7e1"), B(b"1.5"), b"inf", b"1_0.5")])
for bad in (b"x", b"", b"1\x002", b"\xff", bytearray(b"1.5.5"), memoryview(b"z")):
    show("float of " + repr(bytes(bad)), lambda: attempt(lambda: float(bad)).replace(hex(id(bad)), "0x"))
class IB(int): pass
class FB(float): pass
show("derived numbers", lambda: (type(IB(b"5")).__name__, IB(b"5"), type(FB(b"5")).__name__, FB(b"5")))
show("complex", lambda: complex(b"1"))
# ---- what __buffer__ may do
show("not a memoryview", lambda: bytes(type("X", (), {"__buffer__": lambda s, f: b"x"})()))
show("not a memoryview, for memoryview", lambda: memoryview(type("X", (), {"__buffer__": lambda s, f: b"x"})()))
show("None", lambda: bytes(type("X", (), {"__buffer__": lambda s, f: None})()))
show("raises", lambda: bytes(type("X", (), {"__buffer__": lambda s, f: 1 / 0})()))
show("raises in find", lambda: b"a".find(type("X", (), {"__buffer__": lambda s, f: 1 / 0})()))
show("raises in eq", lambda: (bytearray(b"a") == type("X", (), {"__buffer__": lambda s, f: 1 / 0})(), memoryview(b"a") == type("X", (), {"__buffer__": lambda s, f: 1 / 0})()))
show("set to None", lambda: bytes(type("X", (), {"__buffer__": None})()))
show("on the instance means nothing", lambda: bytes(inst()))
def inst():
    class X: pass
    x = X(); x.__buffer__ = lambda f: memoryview(b"q"); return x
show("on the instance means nothing", lambda: bytes(inst()))
def released_view():
    m = memoryview(b"a"); m.release(); return m
show("a released one", lambda: bytes(type("X", (), {"__buffer__": lambda s, f: released_view()})()))
show("one that skips", lambda: b"ace".find(type("X", (), {"__buffer__": lambda s, f: memoryview(b"abcdef")[::2]})()))
show("one that skips, for bytes", lambda: bytes(type("X", (), {"__buffer__": lambda s, f: memoryview(b"abcdef")[::2]})()))
show("one that skips, for memoryview", lambda: memoryview(type("X", (), {"__buffer__": lambda s, f: memoryview(b"abcdef")[::2]})()).tolist())
show("of another format", lambda: (lambda m: (m.format, m.itemsize, m.tolist(), len(m), m.nbytes))(memoryview(type("X", (), {"__buffer__": lambda s, f: memoryview(bytearray(8)).cast("i")})())))
show("inherited", lambda: (bytes(type("Y", (B,), {})()), took()))
class BB(bytes):
    def __buffer__(s, flags): log.append("BB"); return memoryview(b"other")
class BA(bytearray):
    def __buffer__(s, flags): log.append("BA"); return super().__buffer__(flags)
show("partition keeps what it was given", lambda: [(lambda s: b"1x2".partition(s)[1] is s)(s) for s in (b"x", bytearray(b"x"), memoryview(b"x"))])
show("raises in join", lambda: b"".join([type("X", (), {"__buffer__": lambda s, f: 1 / 0})()]))
show("raises in add", lambda: b"" + type("X", (), {"__buffer__": lambda s, f: 1 / 0})())
show("raises in iadd", lambda: iadd(type("X", (), {"__buffer__": lambda s, f: 1 / 0})()))
show("a class derived from bytes", lambda: (b"-".join([BB(b"mine")]), bytes(memoryview(BB(b"mine"))), took()))
show("a class derived from bytearray", lambda: (b"-".join([BA(b"mine")]), bytes(memoryview(BA(b"mine"))), took()))
# ---- releasing
show("release memoryview", lambda: (lambda m: (took(), m.release(), took(), m.release(), took()))(memoryview(R())))
def withit():
    with memoryview(R()) as m: a = took()
    return (a, took())
show("release with", withit)
def two():
    m = memoryview(R()); n = m[1:]; o = memoryview(m); a = took()
    m.release(); b = took(); n.release(); c = took(); o.release(); return (a, b, c, took())
show("the last of them releases", two)
def cast():
    m = memoryview(R()); n = m.cast("B"); r = m.toreadonly(); m.release(); n.release(); a = took(); r.release(); return (a, took())
show("cast and toreadonly count", cast)
show("release after an error", lambda: (attempt(lambda: b"a".find(R(), "x")), took()))
show("obj", lambda: (type(memoryview(B()).obj).__name__, type(memoryview(B())[1:].obj).__name__, memoryview(b"a").obj))
show("wrapper", lambda: (lambda w: (type(w).__module__, type(w).__qualname__, sorted(n for n in vars(type(w)) if n != "__doc__"), attempt(lambda: type(w)())))(memoryview(B()).obj))
# ---- the built-in ones
show("builtin", lambda: (bytes(b"ab".__buffer__(0)), type(b"ab".__buffer__(0)).__name__, b"ab".__buffer__(0).readonly, bytearray(b"ab").__buffer__(1).readonly, bytearray(b"ab").__buffer__(0).readonly, memoryview(b"ab").__buffer__(0).tobytes(), memoryview(bytearray(b"ab")).__buffer__(1).readonly))
show("builtin obj", lambda: (lambda b: b.__buffer__(0).obj is b)(b"ab"))
show("builtin writable", lambda: b"ab".__buffer__(1))
show("builtin writable memoryview", lambda: memoryview(b"ab").__buffer__(1))
show("builtin wrong", lambda: b"ab".__buffer__("a"))
show("builtin big", lambda: b"ab".__buffer__(1 << 40))
show("builtin huge", lambda: b"ab".__buffer__(1 << 70))
show("builtin none", lambda: b"ab".__buffer__())
show("builtin released", lambda: released_view().__buffer__(0))
show("bytearray release", lambda: (lambda b: (lambda m: (b.__release_buffer__(m), attempt(lambda: m.tobytes()), b.__release_buffer__(m)))(b.__buffer__(0)))(bytearray(b"ab")))
show("bytearray release other", lambda: bytearray(b"ab").__release_buffer__(memoryview(b"ab")))
show("bytearray release wrong", lambda: bytearray(b"ab").__release_buffer__(1))
show("bytearray release none", lambda: bytearray(b"ab").__release_buffer__())
show("who has", lambda: [(t.__name__, "__buffer__" in vars(t), "__release_buffer__" in vars(t)) for t in (bytes, bytearray, memoryview, str, object)])
show("kinds", lambda: (type(vars(bytes)["__buffer__"]).__name__, type(vars(bytearray)["__release_buffer__"]).__name__))
show("memoryview arguments", lambda: [attempt(f) for f in (lambda: b"ace".find(memoryview(b"abcdef")[::2]), lambda: b"a".find(released_view()), lambda: bytes(released_view()), lambda: b"".join([released_view()]), lambda: b"a" + memoryview(b"abcd")[::2], lambda: bytes(memoryview(b"abcdef")[::2]), lambda: bytearray(memoryview(b"abcdef")[::-1]))])
# ---- what is changed while it is being looked at
class Shrinks:
    def __init__(s, target): s.target = target
    def __index__(s): s.target.clear(); return 1
    def __buffer__(s, flags): s.target.clear(); return memoryview(b"b")
def shrinking(f):
    b = bytearray(b"abc" * 2000); return f(b, Shrinks(b))
# CPython refuses to resize a bytearray that is being looked at. Here it is resized, and what is looked at is what is there afterwards. Either will do: it is not to crash.
for f in (lambda b, s: b.find(s), lambda b, s: b.count(s), lambda b, s: b * s, lambda b, s: b.hex("-", s), lambda b, s: b + s, lambda b, s: b.startswith(s), lambda b, s: b.replace(s, b"x"),
        lambda b, s: b.split(s), lambda b, s: b.rsplit(s), lambda b, s: b.center(s), lambda b, s: b.strip(s), lambda b, s: b.find(b"c", s), lambda b, s: b[s:], lambda b, s: b[s], lambda b, s: b[::s],
        lambda b, s: b.pop(s), lambda b, s: b.insert(s, 1), lambda b, s: b.__setitem__(s, 1), lambda b, s: b.__setitem__(5000, s), lambda b, s: b.__setitem__(slice(s, None), b"x"), lambda b, s: b.__delitem__(s),
        lambda b, s: b.partition(s), lambda b, s: b.join([s, s]), lambda b, s: b.extend(s), lambda b, s: b.__iadd__(s), lambda b, s: b.removeprefix(s), lambda b, s: b.translate(None, s), lambda b, s: b.expandtabs(s),
        lambda b, s: b.zfill(s), lambda b, s: b.ljust(s), lambda b, s: b.endswith((s, s)), lambda b, s: s in b, lambda b, s: b == s, lambda b, s: memoryview(b).__setitem__(5000, s), lambda b, s: memoryview(b)[s],
        lambda b, s: memoryview(b)[s:], lambda b, s: memoryview(b).index(99, s), lambda b, s: memoryview(b).hex("-", s), lambda b, s: memoryview(b).__setitem__(slice(0, 1), s), lambda b, s: b.remove(s), lambda b, s: b.append(s),
        lambda b, s: b.index(s), lambda b, s: b.rfind(s, s, s), lambda b, s: b.splitlines(s), lambda b, s: b.__imul__(s), lambda b, s: int.from_bytes(b, "big", signed=s) and 0, lambda b, s: b.decode("ascii", s) and 0):
    attempt(lambda: shrinking(f))
class ListShrinks:
    def __init__(s, target): s.target = target
    def __index__(s): s.target.clear(); return 1
def list_shrinking(f):
    l = list(range(5000)); return f(l, ListShrinks(l))
for f in (lambda l, s: l[s:], lambda l, s: l[s], lambda l, s: l[::s], lambda l, s: l[4000:s:-1], lambda l, s: l.__setitem__(slice(s, None), [1]), lambda l, s: l.__delitem__(slice(s, 4000)), lambda l, s: l.pop(s), lambda l, s: l.insert(s, 1), lambda l, s: l * s, lambda l, s: l.index(4999, s), lambda l, s: l.__imul__(s)):
    attempt(lambda: list_shrinking(f))
print("survived")
