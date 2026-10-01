# _pickle: Unpickler as an object, what it reads from, and PickleBuffer.
import _pickle, pickle, io, copyreg, collections, sys, types, array, weakref
import re; address = re.compile('0x[0-9a-f]+')
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
        c = e.__context__ or e.__cause__
        if c is not None: r += " <- %s: %s" % (type(c).__name__, c)
    print(label, "->", address.sub("0x", ascii(r)))
U = _pickle.Unpickler
B = io.BytesIO
print("===== making one")
attempt("no arguments", lambda: U())
attempt("not a file", lambda: U(5))
attempt("only read", lambda: U(type("F", (), {"read": len})()))
attempt("only readline", lambda: U(type("F", (), {"readline": len})()))
attempt("getting read raises", lambda: U(type("F", (), {"read": property(lambda s: 1 / 0), "readline": len})()))
attempt("positional options", lambda: U(B(), True))
attempt("unknown keyword", lambda: U(B(), zzz=1))
for k in ({"encoding": 5}, {"encoding": None}, {"encoding": b"x"}, {"encoding": "a\0b"}, {"encoding": "\ud800"}, {"errors": 5}, {"errors": None}, {"errors": "a\0b"}, {"buffers": 5}, {"buffers": None}, {"buffers": ()}, {"buffers": iter([])}, {"fix_imports": []}, {"encoding": "nonesuch"}, {"errors": "nonesuch"}):
    attempt("Unpickler %r" % (sorted(k)[0] + " " + ascii(list(k.values())[0])[:20]), lambda: type(U(B(b"N."), **k).load()).__name__)
    attempt("   loads", lambda: _pickle.loads(b"N.", **k))
    attempt("   load", lambda: _pickle.load(B(b"N."), **k))
attempt("loads: a str", lambda: _pickle.loads("N."))
attempt("loads: an int", lambda: _pickle.loads(5))
attempt("loads: None", lambda: _pickle.loads(None))
attempt("loads: nothing", lambda: _pickle.loads())
attempt("loads: by keyword", lambda: _pickle.loads(data=b"N."))
for label, v in {"bytearray": bytearray(b"K\x05."), "memoryview": memoryview(b"K\x05."), "a slice": memoryview(b"xxK\x05.yy")[2:5], "array": array.array("B", b"K\x05."), "not contiguous": memoryview(b"K.\x05...")[::2], "PickleBuffer": _pickle.PickleBuffer(b"K\x05.")}.items():
    attempt("loads: " + label, lambda: _pickle.loads(v))
attempt("load: nothing", lambda: _pickle.load())
attempt("load: not a file", lambda: _pickle.load(5))
attempt("load: by keyword", lambda: _pickle.load(file=B(b"N.")))
class NoInit(U):
    def __init__(self): pass
attempt("__init__ not called", lambda: NoInit().load())
attempt("__new__ alone", lambda: U.__new__(U).load())
attempt("__init__ twice", lambda: (lambda u: (u.load(), u.__init__(B(b"K\x02.")), u.load()))(U(B(b"K\x01."))))
print("===== attributes")
u = U(B(b"N."))
attempt("another attribute", lambda: setattr(u, "zzz", 1))
attempt("get another", lambda: u.zzz)
attempt("persistent_load", lambda: u.persistent_load(1))
attempt("persistent_load: no argument", lambda: u.persistent_load())
attempt("set persistent_load", lambda: (setattr(u, "persistent_load", len), u.persistent_load is len)[1])
attempt("delete persistent_load", lambda: (delattr(u, "persistent_load"), u.persistent_load(1)))
attempt("find_class", lambda: u.find_class("builtins", "len"))
attempt("find_class: one", lambda: u.find_class("builtins"))
attempt("find_class: ints", lambda: u.find_class(1, 2))
attempt("find_class: name an int", lambda: u.find_class("builtins", 2))
attempt("find_class: dotted", lambda: u.find_class("collections", "OrderedDict.fromkeys"))
attempt("find_class: python 2", lambda: u.find_class("__builtin__", "unicode"))
attempt("find_class: python 2, not fixed", lambda: U(B(), fix_imports=False).find_class("__builtin__", "unicode"))
attempt("find_class: keyword", lambda: u.find_class(module_name="builtins", global_name="len"))
attempt("load: an argument", lambda: u.load(1))
hooks = []
sys.addaudithook(lambda e, a: hooks.append((e, a)) if e.startswith("pickle") else None)
attempt("audit", lambda: (_pickle.loads(b"cbuiltins\nlen\n."), list(hooks)))
print("===== the memo")
def memo():
    v = U(B(b"]q\x00Nq\x05\x8c\x01a\x94."))
    r = v.load(); m = v.memo
    return r, type(m).__name__, m.copy(), m.__reduce__(), m is v.memo
attempt("copy", memo)
attempt("clear", lambda: (lambda v: (v.load(), v.memo.clear(), v.memo.copy()))(U(B(b"]q\x00."))))
attempt("hash", lambda: hash(u.memo))
attempt("len", lambda: len(u.memo))
attempt("delete", lambda: delattr(u, "memo"))
attempt("set to a list", lambda: setattr(u, "memo", []))
attempt("set: keys no ints", lambda: setattr(u, "memo", {"a": 1}))
attempt("set: negative", lambda: setattr(u, "memo", {-1: 1}))
attempt("set: too large", lambda: setattr(u, "memo", {2**70: 1}))
attempt("set: True", lambda: setattr(u, "memo", {True: 1}))
attempt("set to a dict", lambda: (lambda v: (setattr(v, "memo", {0: "zero", 3: "three"}), v.memo.copy()))(U(B(b"h\x00."))))
attempt("set to a dict, and read", lambda: (lambda v: (setattr(v, "memo", {0: "zero", 3: "three"}), v.load()))(U(B(b"h\x00."))))
def primed():
    a = U(B(b"]q\x00.")); x = a.load(); b = U(B(b"h\x00.")); b.memo = a.memo; return b.load() is x, b.memo.copy()
attempt("set to a proxy", primed)
attempt("kept from one load to the next", lambda: (lambda v: (v.load(), v.load()))(U(B(b"]q\x00.h\x00."))))
attempt("MEMOIZE goes on counting", lambda: (lambda v: (v.load(), v.load(), v.memo.copy()))(U(B(b"K\x01\x94.K\x02\x94."))))
print("===== in use")
class Re:
    pass
def reenter(what):
    v = None
    class V(U):
        def find_class(self, m, n): return what(self)
    v = V(B(b"cbuiltins\nlen\n."))
    return v.load()
attempt("load inside load", lambda: reenter(lambda v: v.load()))
attempt("__init__ inside load", lambda: reenter(lambda v: v.__init__(B())))
attempt("memo inside load", lambda: reenter(lambda v: v.memo.copy()))
attempt("after it failed", lambda: (lambda v: ([attempt("  first", v.load)], v.load()))(U(B(b"Z.K\x05."))))
print("===== what it reads from")
class File:
    def __init__(self, data, *methods, chunk=None): self.f = B(data); self.log = []; self.methods = methods; self.chunk = chunk
    def __getattr__(self, n):
        if n not in self.methods: raise AttributeError(n)
        def call(*a):
            r = getattr(self.f, n)(*a) if n != "peek" else self.f.getvalue()[self.f.tell():self.f.tell() + (self.chunk or a[0])]
            self.log.append((n, a[0] if a and isinstance(a[0], int) else len(a[0]) if a else None, r if isinstance(r, int) else len(r)))
            return r
        return call
big = pickle.dumps([b"x" * 100, "y" * 100, list(range(50)), bytearray(b"z" * 70000), b"w" * 70000, "v" * 70000], 5)
small = pickle.dumps({"a": [1, 2.5, "three"], "b": (None, True)}, 4)
old = pickle.dumps({"a": [1, 2.5, "three", 10**30], "b": (None, True)}, 0)
for label, data in (("small", small), ("old", old), ("big", big), ("two", small + old)):
    for methods in (("read", "readline"), ("read", "readline", "readinto"), ("read", "readline", "peek"), ("read", "readline", "readinto", "peek")):
        def go(chunk=None):
            f = File(data, *methods, chunk=chunk); v = U(f); r = v.load()
            return r == pickle._loads(data), f.log if len(f.log) < 40 else (len(f.log), f.log[:6], f.log[-6:]), f.f.tell()
        attempt("%s %s" % (label, "+".join(methods[2:]) or "plain"), go)
        if "peek" in methods: attempt("   peek gives little", lambda: go(3))
attempt("two, one after the other", lambda: (lambda f: (_pickle.load(f), f.tell(), _pickle.load(f), f.tell()))(B(small + old)))
attempt("buffered", lambda: (lambda f: (_pickle.load(f), f.tell(), _pickle.load(f), f.tell()))(io.BufferedReader(B(small + old))))
def once(data):
    f = B(data)
    return lambda s, n: f.read(n)
for label, make in {"read gives a str": lambda: {"read": lambda s, n: "N.", "readline": lambda s: "N."}, "read gives None": lambda: {"read": lambda s, n: None, "readline": lambda s: None}, "read gives too much": lambda: {"read": lambda s, n: b"N.N.N.", "readline": lambda s: b""},
                    "read gives nothing": lambda: {"read": lambda s, n: b"", "readline": lambda s: b""}, "read raises": lambda: {"read": lambda s, n: 1 / 0, "readline": lambda s: b""}, "read gives a bytearray": lambda: {"read": (lambda r: lambda s, n: bytearray(r(s, n)))(once(b"N.")), "readline": lambda s: b""},
                    "peek raises NotImplementedError": lambda: {"read": once(b"N."), "readline": lambda s: b"", "peek": lambda s, n: (_ for _ in ()).throw(NotImplementedError)}, "peek raises": lambda: {"read": once(b"N."), "readline": lambda s: b"", "peek": lambda s, n: 1 / 0},
                    "peek gives a str": lambda: {"read": once(b"N."), "readline": lambda s: b"", "peek": lambda s, n: "x"}, "peek gives nothing": lambda: {"read": once(b"N."), "readline": lambda s: b"", "peek": lambda s, n: b""}}.items():
    attempt(label, lambda: _pickle.load(type("F", (), make())()))
class Into:
    def __init__(self, result, fill=True): self.first = True; self.result = result; self.fill = fill; self.seen = None
    def read(self, n):
        if self.first: self.first = False; return b"B"[:n]
        return (b"\x05\x00\x00\x00" if n == 4 else b".")[:n]
    def readline(self): return b""
    def readinto(self, b):
        self.seen = (type(b).__name__, len(b), b.readonly, b.format)
        if self.fill: b[:] = b"abcde"
        return self.result
for label, r in {"5": 5, "4": 4, "6": 6, "-1": -1, "None": None, "a str": "5", "True": True, "2**70": 2**70}.items():
    attempt("readinto gives " + label, lambda: (lambda f: (_pickle.load(f), f.seen))(Into(r)))
class NonBytes(Into):
    readinto = None
    def read(self, n):
        if n == 5: return self.result
        return Into.read(self, n)
for label, r in {"bytes": b"abcde", "too few": b"abc", "too many": b"abcdefg", "a bytearray": bytearray(b"abcde"), "a str": "abcde", "None": None}.items():
    attempt("no readinto, and read gives " + label, lambda: _pickle.load(type("N", (), {"__init__": NonBytes.__init__, "read": NonBytes.read, "readline": NonBytes.readline})(r)))
print("===== strings of Python 2")
for k in ({}, {"encoding": "latin-1"}, {"encoding": "bytes"}, {"encoding": "utf-8"}, {"encoding": "ascii", "errors": "replace"}, {"encoding": "ascii", "errors": "ignore"}, {"encoding": "BYTES"}, {"encoding": "nonesuch"}, {"errors": "nonesuch"}):
    for label, data in (("STRING", b"S'a\\xe9'\n."), ("BINSTRING", b"T\x02\x00\x00\x00a\xe9."), ("SHORT_BINSTRING", b"U\x02a\xe9."), ("ascii", b"U\x01a.")):
        attempt("%s %r" % (label, sorted(k.items())), lambda: _pickle.loads(data, **k))
print("===== persistent_load")
class PL(U):
    def persistent_load(self, pid): return ("loaded", pid)
attempt("PERSID", lambda: PL(B(b"Pabc\n.")).load())
attempt("PERSID not ASCII", lambda: PL(B(b"P\xe9\n.")).load())
attempt("BINPERSID", lambda: PL(B(b"(K\x01K\x02tQ.")).load())
attempt("set on the instance", lambda: (lambda v: (setattr(v, "persistent_load", lambda p: ("set", p)), v.load())[1])(U(B(b"K\x01Q."))))
attempt("raises", lambda: (lambda v: (setattr(v, "persistent_load", lambda p: 1 / 0), v.load())[1])(U(B(b"K\x01Q."))))
attempt("not callable", lambda: (lambda v: (setattr(v, "persistent_load", 5), v.load())[1])(U(B(b"K\x01Q."))))
print("===== find_class")
class FC(U):
    def find_class(self, m, n): return ("found", m, n, type(m).__name__)
for label, data in {"GLOBAL": b"ca\nb\n.", "STACK_GLOBAL": b"\x8c\x01a\x8c\x01b\x93.", "INST": b"(ia\nb\n.", "GLOBAL utf-8": b"c\xc3\xa9\n\xe2\x82\xac\n."}.items():
    attempt(label, lambda: FC(B(data)).load())
attempt("forbidden", lambda: type("F", (U,), {"find_class": lambda s, m, n: (_ for _ in ()).throw(pickle.UnpicklingError("forbidden %s.%s" % (m, n)))})(B(b"cos\nsystem\n.")).load())
copyreg.add_extension("collections", "OrderedDict", 240)
attempt("EXT1", lambda: (_pickle.loads(b"\x82\xf0."), _pickle.dumps(collections.OrderedDict, 2), _pickle.dumps(collections.OrderedDict, 1), FC(B(b"\x82\xf0.")).load()))
copyreg.remove_extension("collections", "OrderedDict", 240)
attempt("EXT1, removed", lambda: _pickle.loads(b"\x82\xf0."))
copyreg._inverted_registry[241] = "not a pair"
attempt("EXT1, no pair", lambda: _pickle.loads(b"\x82\xf1."))
copyreg._inverted_registry[241] = (1, 2)
attempt("EXT1, no strs", lambda: _pickle.loads(b"\x82\xf1."))
del copyreg._inverted_registry[241]
copyreg._extension_registry[("collections", "deque")] = 0
attempt("a code of 0", lambda: _pickle.dumps(collections.deque, 2))
copyreg._extension_registry[("collections", "deque")] = "x"
attempt("a code that is a str", lambda: _pickle.dumps(collections.deque, 2))
copyreg._extension_registry[("collections", "deque")] = 70000
attempt("EXT4", lambda: _pickle.dumps(collections.deque, 2))
copyreg._extension_registry[("collections", "deque")] = 300
attempt("EXT2", lambda: _pickle.dumps(collections.deque, 2))
del copyreg._extension_registry[("collections", "deque")]
