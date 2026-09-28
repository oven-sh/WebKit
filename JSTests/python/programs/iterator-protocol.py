def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
def tidy(r):
    def one(x):
        if isinstance(x, tuple): return tuple(one(y) for y in x)
        if hasattr(x, "__next__"): return "<" + type(x).__name__ + " " + str(list(x)) + ">"
        return getattr(x, "__name__", x)
    return one(r)
class Seq:
    def __repr__(s): return "<" + type(s).__name__ + ">"
    def __getitem__(s, i):
        if i > 3: raise IndexError
        return i * 10
class SeqLen(Seq):
    def __len__(s): return 4
big = 1 << 70
makers = {
    "list": lambda: iter([1, 2, 3, 4]), "list reversed": lambda: reversed([1, 2, 3, 4]), "tuple": lambda: iter((1, 2, 3, 4)), "str": lambda: iter("abcd"),
    "str wide": lambda: iter("aé\U0001F600d"), "bytes": lambda: iter(b"abcd"), "bytearray": lambda: iter(bytearray(b"abcd")), "range": lambda: iter(range(0, 40, 10)),
    "range down": lambda: iter(range(4, 0, -1)), "long range": lambda: iter(range(big, big + 4)), "dict": lambda: iter({1: "a", 2: "b", 3: "c", 4: "d"}),
    "dict values": lambda: iter({1: "a", 2: "b", 3: "c", 4: "d"}.values()), "dict items": lambda: iter({1: "a", 2: "b", 3: "c", 4: "d"}.items()),
    "dict reversed": lambda: reversed({1: "a", 2: "b", 3: "c", 4: "d"}), "dict keys reversed": lambda: reversed({1: "a", 2: "b", 3: "c", 4: "d"}.keys()),
    "dict values reversed": lambda: reversed({1: "a", 2: "b", 3: "c", 4: "d"}.values()), "dict items reversed": lambda: reversed({1: "a", 2: "b", 3: "c", 4: "d"}.items()),
    "set": lambda: iter({1, 2, 3, 4}), "frozenset": lambda: iter(frozenset({1, 2, 3, 4})), "sequence": lambda: iter(Seq()), "sequence with len": lambda: iter(SeqLen()),
    "reversed": lambda: reversed(SeqLen()), "enumerate": lambda: enumerate("abcd"), "enumerate from": lambda: enumerate("abcd", 5), "enumerate big": lambda: enumerate("abcd", big),
    "zip": lambda: zip("abcd", [1, 2, 3, 4]), "zip strict": lambda: zip("abcd", [1, 2, 3, 4], strict=True), "map": lambda: map(str, [1, 2, 3, 4]),
    "map two": lambda: map(pow, [1, 2, 3, 4], [2, 2, 2, 2]), "map strict": lambda: map(pow, [1, 2, 3, 4], [2, 2, 2, 2], strict=True), "filter": lambda: filter(None, [1, 0, 2, 3, 4]),
    "memoryview": lambda: iter(memoryview(b"abcd")),
}
count = [0]
def counter():
    count[0] += 1; return count[0]
makers["callable"] = lambda: (count.__setitem__(0, 0), iter(counter, 5))[1]
def advanced(make, n):
    i = make()
    for _ in range(n): next(i, None)
    return i
for name, make in makers.items():
    show(name + " type", lambda: (type(make()).__name__, list(make()), iter(make()) is not None))
    show(name + " hint", lambda: [getattr(advanced(make, n), "__length_hint__", lambda: "none")() for n in (0, 1, 4, 5)])
    if "__reduce__" not in type(make()).__dict__:
        print(name, "has no __reduce__ of its own")
        continue
    for n in (0, 2, 4, 5):
        show(name + " reduce after " + str(n), lambda: tidy(advanced(make, n).__reduce__()))
    def rebuilt(n):
        r = advanced(make, n).__reduce__()
        i = r[0](*r[1])
        if len(r) > 2 and r[2] is not None: i.__setstate__(r[2])
        return list(i)
    if name != "callable":
        show(name + " rebuilt", lambda: [rebuilt(n) for n in (0, 1, 3, 4, 5)])
    if hasattr(make(), "__setstate__"):
        def state(v, n=0):
            i = advanced(make, n); i.__setstate__(v); return list(i)
        show(name + " setstate", lambda: [state(v) for v in (0, 1, 3, 4, 5, 100, -1, -5, True)])
        show(name + " setstate later", lambda: [state(v, 2) for v in (0, 1, 3)])
        show(name + " setstate when done", lambda: [state(v, 5) for v in (0, 1)])
        for bad in ("x", 1.5, None, 1 << 80, -(1 << 80)):
            show(name + " setstate " + repr(bad), lambda: state(bad))
        show(name + " setstate nothing", lambda: make().__setstate__())
    show(name + " is not left where it was", lambda: (lambda i: (i.__reduce__() and None, list(i)))(advanced(make, 1)))

# ---- changes under way
def changed(make_container, change, it=iter):
    c = make_container(); i = it(c); next(i); change(c)
    return (i.__length_hint__(), attempt(lambda: tidy(i.__reduce__())), attempt(lambda: list(i)))
def attempt(f):
    try: return f()
    except Exception as e: return type(e).__name__ + ": " + str(e)
show("list grown", lambda: changed(lambda: [1, 2, 3], lambda c: c.append(4)))
show("list shrunk", lambda: changed(lambda: [1, 2, 3], lambda c: c.clear()))
show("list reversed shrunk", lambda: changed(lambda: [1, 2, 3], lambda c: c.clear(), reversed))
show("dict grown", lambda: changed(lambda: {1: 1, 2: 2}, lambda c: c.__setitem__(3, 3)))
show("set grown", lambda: changed(lambda: {1, 2}, lambda c: c.add(3)))
show("bytearray shrunk", lambda: changed(lambda: bytearray(b"abc"), lambda c: c.clear()))
show("memoryview released", lambda: (lambda m: (lambda i: (next(i), m.release(), attempt(lambda: next(i))))(iter(m)))(memoryview(b"ab")))

# ---- strict
show("map strict shorter", lambda: list(map(pow, [1, 2], [1], strict=True)))
show("map strict longer", lambda: list(map(pow, [1], [1, 2], strict=True)))
show("map strict third shorter", lambda: list(map(lambda *a: a, [1, 2], [1, 2], [1], strict=True)))
show("map strict third longer", lambda: list(map(lambda *a: a, [1], [1], [1, 2], strict=True)))
show("map not strict", lambda: list(map(pow, [1, 2], [1])))
show("map strict of one", lambda: list(map(str, [1, 2], strict=True)))
show("map made strict", lambda: (lambda m: (m.__setstate__(True), list(m)))(map(pow, [1, 2], [1])))
show("zip made strict", lambda: (lambda z: (z.__setstate__(1), list(z)))(zip([1, 2], [1])))
show("zip made lax", lambda: (lambda z: (z.__setstate__(0), list(z)))(zip([1, 2], [1], strict=True)))
show("map bad keyword", lambda: map(str, [1], nope=1))
show("map too few", lambda: map(str))

# ---- classes derived from them
class E(enumerate): pass
class Z(zip): pass
class M(map): pass
class F(filter): pass
class R(reversed): pass
show("derived", lambda: [(type(x).__name__, list(x)) for x in (E("ab"), Z("ab", "cd"), M(str, [1]), F(None, [0, 1]), R([1, 2]))])
show("derived reduce", lambda: [tidy(x.__reduce__()) for x in (E("ab"), Z("ab", "cd"), M(str, [1]), F(None, [0, 1]), R(SeqLen()))])
show("derived attributes", lambda: (lambda e: (setattr(e, "x", 1), e.x, e.__dict__))(E("ab")))
show("derived isinstance", lambda: (isinstance(E("a"), enumerate), issubclass(E, enumerate), E.__mro__[1].__name__))
class E2(enumerate):
    def __next__(s): return "mine"
show("derived next", lambda: (next(E2("ab")), next(iter(E2("ab")))))
show("cannot be derived from", lambda: [attempt(lambda: type("X", (type(m()),), {})) for n, m in makers.items() if n in ("list", "tuple", "str", "range", "dict", "set", "sequence", "callable", "memoryview")])
show("cannot be made", lambda: [attempt(lambda: type(m())()) for n, m in makers.items() if n in ("list", "tuple", "str", "range", "dict", "set", "sequence", "callable", "memoryview")])
show("builtins replaced", lambda: replaced())
def replaced():
    import builtins
    old = builtins.iter
    builtins.iter = "not iter"
    try: return old([1]).__reduce__()[0]
    finally: builtins.iter = old
show("builtins replaced", replaced)
def deleted():
    import builtins
    old = builtins.iter
    del builtins.iter
    try: return old([1]).__reduce__()[0]
    finally: builtins.iter = old
show("builtins deleted", deleted)
