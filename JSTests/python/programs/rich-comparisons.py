def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
ops = [("==", lambda a, b: a == b), ("!=", lambda a, b: a != b), ("<", lambda a, b: a < b), ("<=", lambda a, b: a <= b), (">", lambda a, b: a > b), (">=", lambda a, b: a >= b)]
methods = ["__eq__", "__ne__", "__lt__", "__le__", "__gt__", "__ge__"]
def attempt(f):
    try: return f()
    except Exception as e: return type(e).__name__
def table(label, values):
    for name, op in ops:
        show(label + " " + name, lambda: [[attempt(lambda: op(a, b)) for b in values] for a in values])
    show(label + " methods", lambda: [[getattr(values[0], m)(b) for b in values] for m in methods])
    show(label + " own", lambda: [m in type(values[0]).__dict__ for m in methods])
def cell(*v):
    if v:
        x = v[0]
        return (lambda: x).__closure__[0]
    def f():
        def g(): return y
        return g
        y = 1
    return f().__closure__[0]
class C:
    def m(s): pass
    def n(s): pass
    def __eq__(s, o): return True
    def __hash__(s): return 1
c1, c2 = C(), C()
table("None", [None, 0, "", NotImplemented])
table("slice", [slice(1), slice(1), slice(2), slice(1, 2), slice(1, 2, 3), slice(None), slice("a"), 1, (None, 1, None)])
table("cell", [cell(1), cell(1), cell(2), cell(), cell(), cell("a"), 1])
table("builtin", [len, len, abs, [].append, "".join, 1])
l = []
table("builtin bound", [l.append, l.append, l.pop, [].append, len])
table("method", [c1.m, c1.m, c1.n, c2.m, C.m, 1])
table("method wrapper", [(1).__add__, (1).__add__, (1).__sub__, (2).__add__, "".__add__, 1])
table("complex", [1j, 1j, 2j, 1 + 0j, 1, 1.0, True, "a"])
table("memoryview", [memoryview(b"ab"), memoryview(b"ab"), memoryview(b"ac"), b"ab", bytearray(b"ab"), "ab", 1])
def f(): pass
def g(): pass
table("code", [f.__code__, f.__code__, g.__code__, 1])
show("slice hash", lambda: (hash(slice(1)), hash(slice(1, 2, 3)), hash(slice(None)), hash(slice("a", "b")) == hash(slice("a", "b")), hash(slice(1)) == hash(slice(1)), hash(slice(1)) == hash((None, 1, None))))
show("slice hash of unhashable", lambda: hash(slice([])))
show("slice in a dict", lambda: {slice(1): "a", slice(1): "b", slice(2): "c"})
show("slice in a set", lambda: len({slice(1), slice(1), slice(1, None), slice(None, 1)}))
show("slice sorted", lambda: sorted([slice(3), slice(1), slice(2)]))
show("cell sorted", lambda: [x.cell_contents for x in sorted([cell(3), cell(1), cell(2)])])
show("cell hash", lambda: hash(cell(1)))
show("method hash", lambda: (hash(c1.m) == hash(c1.m), hash(c1.m) == hash(c2.m), hash(len) == hash(len), hash(l.append) == hash(l.append)))
show("methods of objects that are equal", lambda: (c1 == c2, c1.m == c2.m, c1.m != c2.m))
class F:
    def __call__(s): pass
    def __eq__(s, o): return True
    def __hash__(s): return 1
M = type(c1.m)
show("methods of functions that are equal", lambda: (M(F(), c1) == M(F(), c1), M(F(), c1) == M(F(), c2)))
show("None identity", lambda: (None == None, None != None, None.__eq__(None), None.__ne__(None), None.__eq__(0), None.__ne__(0), None.__lt__(None)))
show("wrong number", lambda: slice(1).__eq__())
show("wrong number 2", lambda: None.__lt__(1, 2))
