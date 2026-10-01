# _pickle: PickleBuffer, buffers that are not in the pickle, pickles cut short, and pickles with a byte changed.
import _pickle, pickle, io, array, weakref, gc, sys, hashlib
import re; address = re.compile('0x[0-9a-f]+')
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
        c = e.__context__ or e.__cause__
        if c is not None: r += " <- %s: %s" % (type(c).__name__, c)
    print(label, "->", address.sub("0x", ascii(r)))
PB = _pickle.PickleBuffer
def show(m): return (m.tobytes(), m.format, m.itemsize, m.ndim, m.shape, m.strides, m.readonly, m.nbytes, m.c_contiguous, m.f_contiguous)
print("===== PickleBuffer")
attempt("no arguments", lambda: PB())
attempt("two", lambda: PB(b"a", b"b"))
attempt("keyword", lambda: PB(buffer=b"a"))
for label, v in {"int": 5, "str": "a", "None": None, "list": [1]}.items(): attempt("of " + label, lambda: PB(v))
things = {"bytes": lambda: b"abcdef", "bytearray": lambda: bytearray(b"abcdef"), "memoryview": lambda: memoryview(b"abcdef"), "a slice": lambda: memoryview(bytearray(b"abcdef"))[1:4], "array of ints": lambda: array.array("i", [1, 2, 3]), "every other": lambda: memoryview(b"abcdef")[::2], "backwards": lambda: memoryview(b"abcdef")[::-1],
          "two by three": lambda: memoryview(b"abcdef").cast("B", (2, 3)), "empty": lambda: b"", "another PickleBuffer": lambda: PB(bytearray(b"abc")), "cast": lambda: memoryview(bytearray(8)).cast("d"), "no dimensions": lambda: memoryview(b"abcd").cast("i", ())}
for label, make in things.items():
    print("--", label)
    attempt("memoryview", lambda: show(memoryview(PB(make()))))
    attempt("raw", lambda: show(PB(make()).raw()))
    attempt("bytes", lambda: bytes(PB(make())))
    attempt("__buffer__", lambda: show(PB(make()).__buffer__(0)))
    attempt("__buffer__, writable", lambda: show(PB(make()).__buffer__(1)))
    for proto in (4, 5):
        attempt("dumps %d" % proto, lambda: _pickle.dumps(PB(make()), proto))
    attempt("and back", lambda: (lambda r: (type(r).__name__, bytes(r)))(_pickle.loads(_pickle.dumps(PB(make()), 5))))
    def out_of_band():
        bufs = []; d = _pickle.dumps([PB(make())], 5, buffer_callback=bufs.append)
        r = _pickle.loads(d, buffers=bufs)
        return d, [type(b).__name__ for b in bufs], type(r[0]).__name__, show(memoryview(r[0])), r[0] is bufs[0]
    attempt("out of band", out_of_band)
    attempt("as Python has it", lambda: _pickle.dumps(PB(make()), 5) == pickle._dumps(PB(make()), 5))
for v in (b"a", bytearray(b"a"), array.array("b", b"a")): attempt("whose they are", lambda: (memoryview(PB(v)).obj is v, PB(v).raw().obj is v))
print("-- released")
def released(): p = PB(b"abc"); p.release(); return p
attempt("release", lambda: PB(b"abc").release())
attempt("twice", lambda: (lambda p: (p.release(), p.release()))(PB(b"abc")))
attempt("raw", lambda: released().raw())
attempt("memoryview", lambda: memoryview(released()))
attempt("bytes", lambda: bytes(released()))
attempt("dumps", lambda: _pickle.dumps(released(), 5))
attempt("dumps 4", lambda: _pickle.dumps(released(), 4))
attempt("a view outlasts it", lambda: (lambda p: (lambda m: (p.release(), m.tobytes()))(p.raw()))(PB(b"abc")))
attempt("weak reference", lambda: (lambda p: weakref.ref(p)() is p)(PB(b"abc")))
attempt("attribute", lambda: setattr(PB(b"a"), "x", 1))
attempt("derive", lambda: type("X", (PB,), {}))
attempt("hash, eq", lambda: (lambda p: (hash(p) == hash(p), p == p, p == PB(b"a")))(PB(b"a")))
attempt("writes through", lambda: (lambda b: (PB(b).raw().__setitem__(0, 65), b)[1])(bytearray(b"abc")))
print("===== buffer_callback")
for label, cb in {"None": lambda b: None, "True": lambda b: True, "0": lambda b: 0, "raises": lambda b: 1 / 0, "a list": lambda b: [1]}.items():
    attempt("gives " + label, lambda: _pickle.dumps([PB(b"ab"), PB(bytearray(b"cd"))], 5, buffer_callback=cb))
class Bad:
    def __bool__(self): raise ZeroDivisionError("bool")
attempt("gives what cannot say", lambda: _pickle.dumps(PB(b"ab"), 5, buffer_callback=lambda b: Bad()))
attempt("not callable", lambda: _pickle.dumps(PB(b"ab"), 5, buffer_callback=5))
attempt("the same twice", lambda: (lambda p, l: (_pickle.dumps([p, p], 5, buffer_callback=l.append), len(l)))(PB(b"ab"), []))
attempt("the same twice, in band", lambda: (lambda p: _pickle.dumps([p, p], 5))(PB(b"ab")))
attempt("releases it meanwhile", lambda: (lambda p: _pickle.dumps(p, 5, buffer_callback=lambda b: (p.release(), True)[1]))(PB(bytearray(b"abcd"))))
print("===== buffers")
d = _pickle.dumps([PB(b"ab"), PB(bytearray(b"cd"))], 5, buffer_callback=lambda b: None)
attempt("none given", lambda: _pickle.loads(d))
attempt("None", lambda: _pickle.loads(d, buffers=None))
attempt("too few", lambda: _pickle.loads(d, buffers=[b"ab"]))
attempt("none at all", lambda: _pickle.loads(d, buffers=[]))
attempt("too many", lambda: _pickle.loads(d, buffers=[b"ab", bytearray(b"cd"), b"ef"]))
attempt("an iterator", lambda: _pickle.loads(d, buffers=iter([b"ab", bytearray(b"cd")])))
attempt("not buffers", lambda: _pickle.loads(d, buffers=[1, 2]))
attempt("writable where it was not", lambda: (lambda r: [(type(x).__name__, memoryview(x).readonly) for x in r])(_pickle.loads(d, buffers=[bytearray(b"ab"), bytearray(b"cd")])))
attempt("not writable where it was", lambda: (lambda r: [(type(x).__name__, memoryview(x).readonly) for x in r])(_pickle.loads(d, buffers=[b"ab", b"cd"])))
attempt("raises", lambda: _pickle.loads(d, buffers=(1 / 0 for _ in [1])))
print("===== cut short")
class C:
    def __init__(self): self.a = [1, "two", 3.0]; self.b = {"k": (None, True, b"bytes")}
    def __eq__(self, o): return vars(self) == vars(o)
value = [C(), {1, 2}, frozenset({3}), 10**30, "caf\xe9", bytearray(b"ba"), (1, 2, 3, 4), -5, 70000]
for proto in range(6):
    data = _pickle.dumps(value, proto)
    seen = {}
    for n in range(len(data)):
        for how, f in (("bytes", lambda: _pickle.loads(data[:n])), ("file", lambda: _pickle.load(io.BytesIO(data[:n]))), ("buffered", lambda: _pickle.load(io.BufferedReader(io.BytesIO(data[:n]))))):
            try: r = "no error"; f()
            except Exception as e: r = "%s: %s" % (type(e).__name__, e)
            seen.setdefault((how, r), []).append(n)
    for (how, r), where in sorted(seen.items()): print(proto, how, ascii(r), len(where), where[:8])
print("===== a byte changed")
class Safe(_pickle.Unpickler):
    def find_class(self, m, n):
        if (m, n) in (("__main__", "C"), ("builtins", "set"), ("builtins", "frozenset"), ("builtins", "bytearray"), ("copy_reg", "_reconstructor"), ("__builtin__", "object"), ("__builtin__", "set"), ("__builtin__", "frozenset"), ("__builtin__", "bytearray"), ("_codecs", "encode"), ("builtins", "object")): return super().find_class(m, n)
        raise pickle.UnpicklingError("not allowed: %s.%s" % (ascii(m)[:30], ascii(n)[:30]))
state = 12345
def rand(n):
    global state
    state = (state * 1103515245 + 12345) & 0x7fffffff
    return (state >> 8) % n
def normal(r):
    # A set is gone through in no particular order.
    if isinstance(r, (set, frozenset)):
        try: return (type(r).__name__, sorted(r, key=repr))
        except Exception: return r
    if isinstance(r, list): return [normal(x) for x in r]
    return r
def describe(f):
    try: r = f()
    except RecursionError: return "RecursionError"
    except MemoryError: return "not all there"
    except Exception as e:
        # Whether room can be made for a great many bytes, before it is found that they are not there, is up to the system.
        if str(e) in ("pickle exhausted before end of frame", "pickle data was truncated"): return "not all there"
        return address.sub("0x", "%s: %s" % (type(e).__name__, e))[:150]
    try: return address.sub("0x", ascii(normal(r)))[:150]
    except Exception as e: return "repr: " + type(e).__name__
total = {}
for proto in range(6):
    data = _pickle.dumps(value, proto)
    lines = []
    for i in range(len(data)):
        for v in (0, 1, 0x28, 0x2e, 0x30, 0x7f, 0x80, 0xff, data[i] ^ 1, (data[i] + 1) & 0xff, rand(256)):
            d = bytearray(data); d[i] = v
            lines.append("%d %d %d %s" % (proto, i, v, describe(lambda: Safe(io.BytesIO(bytes(d))).load())))
    # What came of each is a good many lines. Given an argument they are all printed, and otherwise what they come to, so many at a time.
    if len(sys.argv) > 1:
        for l in lines: print(l)
    for start in range(0, len(lines), 500):
        print(proto, start, hashlib.md5("\n".join(lines[start:start + 500]).encode()).hexdigest())
