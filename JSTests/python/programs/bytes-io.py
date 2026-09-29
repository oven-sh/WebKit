# io.BytesIO
import sys
import _io
from _io import BytesIO


def t(label, f):
    try:
        r = f()
    except BaseException as e:
        r = type(e).__name__ + ": " + str(e)
    print(label, "=>", r if isinstance(r, str) else repr(r))


print("---- the class")
t("what it is", lambda: (BytesIO.__name__, BytesIO.__module__, [c.__name__ for c in BytesIO.__mro__], sorted(k for k in vars(BytesIO) if k != "__doc__")))
t("_BytesIOBuffer", lambda: (_io._BytesIOBuffer.__name__, _io._BytesIOBuffer.__module__, [c.__name__ for c in _io._BytesIOBuffer.__mro__]))
t("which cannot be made", lambda: _io._BytesIOBuffer())

print("---- made from")
for label, args, kwargs in (("nothing", (), {}), ("bytes", (b"abc",), {}), ("a bytearray", (bytearray(b"abc"),), {}), ("a memoryview", (memoryview(b"abcdef")[1:4],), {}), ("None", (None,), {}), ("by name", (), {"initial_bytes": b"xy"}),
                            ("a str", ("abc",), {}), ("an int", (5,), {}), ("a list", ([1, 2],), {}), ("two", (b"a", b"b"), {}), ("another name", (), {"other": b""}), ("a memoryview that skips", (memoryview(b"abcdef")[::2],), {})):
    t(label, lambda: [(b.getvalue(), b.tell(), b.read()) for b in [BytesIO(*args, **kwargs)]])

print("---- reading")
b = BytesIO(b"hello\nworld\n\nlast")
t("read", lambda: (b.read(2), b.read(0), b.tell(), b.read(100), b.read(1), b.read(), b.tell()))
t("read(None) and negative", lambda: (b.seek(3), b.read(None), b.seek(3), b.read(-1), b.seek(3), b.read(-100)))
t("read1", lambda: (b.seek(0), b.read1(3), b.read1(), b.read1(5), b.seek(0), b.read1(None), b.seek(0), b.read1(-1)))
t("readline", lambda: (b.seek(0), b.readline(), b.readline(3), b.readline(0), b.readline(100), b.readline(), b.readline(None), b.readline(), b.readline(-1)))
t("readlines", lambda: (b.seek(0), b.readlines(), b.readlines(), b.seek(0), b.readlines(1), b.readlines(7), b.seek(0), b.readlines(6), b.seek(0), b.readlines(0), b.seek(0), b.readlines(-5), b.seek(0), b.readlines(None)))
t("iteration", lambda: (b.seek(0), list(b), list(b), b.seek(6), next(b), iter(b) is b))
t("next at the end", lambda: (b.seek(0, 2), next(b)))
t("readinto", lambda: [(b.seek(0), b.readinto(x), x, b.readinto(memoryview(x)[1:3]), x, b.readinto(bytearray()), b.seek(-2, 2), b.readinto(x), x, b.readinto(x)) for x in [bytearray(4)]])
for name, args in (("read", ("a",)), ("read", (1.5,)), ("read", (1, 2)), ("read", (2 ** 70,)), ("read1", ("a",)), ("readline", ("a",)), ("readline", (1.5,)), ("readlines", ("a",)), ("readlines", (1.5,)), ("readlines", (2 ** 70,)), ("readlines", (True,)),
                   ("readinto", (b"ab",)), ("readinto", ("a",)), ("readinto", ()), ("readinto", (1,))):
    t("%s%r" % (name, args), lambda: (b.seek(0), getattr(b, name)(*args)))
t("readinto(a memoryview that cannot be written to)", lambda: b.readinto(memoryview(b"ab")))


class Index:
    def __init__(self, n): self.n = n
    def __index__(self): return self.n


t("what has __index__", lambda: (b.seek(0), b.read(Index(2)), b.readline(Index(1)), b.seek(Index(1)), b.read1(Index(1))))
t("readlines does not take it", lambda: b.readlines(Index(1)))
t("nor truncate", lambda: BytesIO(b"abc").truncate(Index(1)))

print("---- where it is")
b = BytesIO(b"0123456789")
t("seek", lambda: (b.seek(3), b.tell(), b.seek(2, 1), b.seek(-1, 1), b.seek(-3, 2), b.seek(0, 2), b.seek(5, 2), b.tell(), b.read(), b.seek(-100, 1), b.seek(-100, 2), b.seek(0, 0)))
for args in ((-1,), (-1, 0), (0, 3), (0, -1), ("a",), (0, "a"), (1.5,), (), (0, 0, 0), (2 ** 63,), (2 ** 63 - 1,), (2 ** 63 - 1, 1), (2 ** 63 - 1, 2), (None,), (0, None)):
    t("seek%r" % (args,), lambda: (b.seek(1), b.seek(*args)))
t("past the end", lambda: [(x.seek(5), x.read(), x.read(1), x.readline(), x.readlines(), x.readinto(bytearray(2)), x.getvalue(), x.write(b"Z"), x.getvalue(), x.tell()) for x in [BytesIO(b"ab")]])
t("as far as can be", lambda: [(x.seek(2 ** 63 - 1), x.read(), x.readinto(bytearray(1)), x.readline()) for x in [BytesIO(b"ab")]])
t("and written to there", lambda: [(x.seek(2 ** 63 - 1), x.write(b"a")) for x in [BytesIO(b"ab")]])

print("---- writing")
b = BytesIO()
t("write", lambda: (b.write(b"abc"), b.write(bytearray(b"de")), b.write(memoryview(b"f")), b.write(b""), b.tell(), b.getvalue()))
t("over what is there", lambda: (b.seek(1), b.write(b"XY"), b.getvalue(), b.tell(), b.seek(5), b.write(b"123"), b.getvalue()))
for arg in ("a", 1, None, [1], 1.5, memoryview(b"abcd")[::2]):
    t("write(%r)" % (arg if not isinstance(arg, memoryview) else "a memoryview that skips"), lambda: b.write(arg))
t("write()", lambda: b.write())
t("writelines", lambda: [(x.writelines([b"a", bytearray(b"b"), memoryview(b"c")]), x.writelines(()), x.writelines(iter([b"d"])), x.getvalue()) for x in [BytesIO()]])
t("writelines of bytes", lambda: BytesIO().writelines(b"ab"))
t("of what cannot be gone through", lambda: BytesIO().writelines(5))
t("with a str in it", lambda: [(attempt(lambda: x.writelines([b"a", "b", b"c"])), x.getvalue()) for x in [BytesIO()]])


def attempt(f):
    try:
        return f()
    except Exception as e:
        return type(e).__name__


t("with a str in it", lambda: [(attempt(lambda: x.writelines([b"a", "b", b"c"])), x.getvalue()) for x in [BytesIO()]])
t("truncate", lambda: [(x.seek(4), x.truncate(), x.getvalue(), x.tell(), x.truncate(2), x.getvalue(), x.tell(), x.truncate(10), x.getvalue(), x.truncate(0), x.getvalue(), x.truncate(None), x.write(b"z"), x.getvalue()) for x in [BytesIO(b"0123456789")]])
for arg in (-1, "a", 1.5, 2 ** 70, True):
    t("truncate(%r)" % (arg,), lambda: BytesIO(b"abc").truncate(arg))
t("a great deal", lambda: [(sum(x.write(b"0123456789" * 100) for i in range(300)), len(x.getvalue()), x.tell(), x.seek(299990), x.read(20), x.truncate(5), x.getvalue()) for x in [BytesIO()]])
t("byte by byte", lambda: [([x.write(bytes([i % 256])) for i in range(2000)][-1], len(x.getvalue()), x.getvalue()[:5], x.getvalue()[-3:]) for x in [BytesIO()]])

print("---- what is given out is the same thing while nothing changes")
data = b"some bytes"
t("what it was made from", lambda: [(x.getvalue() is data, x.read() is data, x.getvalue() is data) for x in [BytesIO(data)]])
t("until it is written to", lambda: [(x.write(b"S"), x.getvalue() is data, x.getvalue(), data, x.getvalue() is x.getvalue()) for x in [BytesIO(data)]])
t("read from the middle is not", lambda: [(x.read(1), x.read() is data) for x in [BytesIO(data)]])
t("what was written", lambda: [(x.write(b"abcdef"), x.getvalue() is x.getvalue(), x.seek(0), x.read() is x.getvalue()) for x in [BytesIO()]])
kept = BytesIO(b"abcdef")
value = kept.getvalue()
t("does not change", lambda: (kept.write(b"XYZ"), value, kept.getvalue(), kept.truncate(1), value))
t("nor what it was made from, if that can", lambda: [(x.write(b"Z"), source, source.__setitem__(1, 65), x.getvalue()) for source in [bytearray(b"abc")] for x in [BytesIO(source)]])

print("---- getbuffer")
b = BytesIO(b"abcdef")
v = b.getbuffer()
t("a view", lambda: (type(v).__name__, bytes(v), v.readonly, v.format, v.nbytes, v.shape, type(v.obj).__name__, len(v)))
t("that can be written through", lambda: (v.__setitem__(0, 65), v.__setitem__(slice(1, 3), b"XY"), b.getvalue(), b.read()))
v.release()
t("and when it has been released", lambda: (b.seek(3), b.write(b"1234"), b.getvalue()))
t("getbuffer(1)", lambda: b.getbuffer(1))
t("of what it was made from", lambda: [(w.__setitem__(0, 90), x.getvalue(), source) for source in [b"abc"] for x in [BytesIO(source)] for w in [x.getbuffer()]])
t("getvalue is another each time, with one", lambda: [(w[0], x.getvalue() is x.getvalue(), w.release()) for x in [BytesIO(b"abcdef")] for w in [x.getbuffer()]])
t("of nothing", lambda: bytes(BytesIO().getbuffer()))
t("with", lambda: [len(w) for x in [BytesIO(b"abc")] for w in [x.getbuffer().__enter__()]])

print("---- closed")
b = BytesIO(b"abc")
t("close", lambda: (b.closed, b.close(), b.closed, b.close(), b.closed))
for name, args in (("read", ()), ("read1", ()), ("readline", ()), ("readlines", ()), ("readinto", (bytearray(1),)), ("write", (b"a",)), ("write", (b"",)), ("write", ("a",)), ("writelines", ([],)), ("seek", (0,)), ("tell", ()), ("truncate", ()), ("getvalue", ()),
                   ("getbuffer", ()), ("flush", ()), ("isatty", ()), ("readable", ()), ("writable", ()), ("seekable", ()), ("__next__", ()), ("__enter__", ()), ("__getstate__", ()), ("fileno", ()), ("detach", ()), ("read", ("a",)), ("seek", ("a",))):
    t("%s%r when closed" % (name, args), lambda: getattr(b, name)(*args))
t("__iter__ when closed", lambda: iter(b) is b)
t("with", lambda: [(x.closed, x.__exit__(None, None, None), x.closed) for x in [BytesIO().__enter__()]])
t("closed cannot be set", lambda: setattr(b, "closed", False))
t("open", lambda: [(x.readable(), x.writable(), x.seekable(), x.isatty(), x.flush()) for x in [BytesIO()]])
t("fileno", lambda: BytesIO().fileno())
t("detach", lambda: BytesIO().detach())
t("__init__ of one that is closed", lambda: [(x.close(), x.__init__(b"new"), x.closed, x.read()) for x in [BytesIO(b"old")]])
t("with nothing", lambda: [(x.close(), x.__init__(), x.closed) for x in [BytesIO(b"old")]])
t("with a bytearray", lambda: [(x.close(), x.__init__(bytearray(b"new"))) for x in [BytesIO(b"old")]])
t("__init__ again", lambda: [(x.read(2), x.__init__(b"new"), x.tell(), x.read(), x.__init__(), x.getvalue(), x.__init__(bytearray(b"more")), x.getvalue(), x.tell()) for x in [BytesIO(b"old")]])
t("__new__ alone", lambda: [(x.closed, x.getvalue(), x.write(b"a"), x.getvalue()) for x in [BytesIO.__new__(BytesIO)]])

print("---- state")
b = BytesIO(b"abcdef")
b.seek(2)
t("__getstate__", lambda: b.__getstate__())
b.extra = [1]
t("with attributes", lambda: (b.__getstate__(), b.__getstate__()[2] is not b.__dict__, b.__dict__))
t("__setstate__", lambda: [(x.__setstate__((b"xyz", 1, None)), x.getvalue(), x.tell(), x.__setstate__((bytearray(b"pq"), 5, {"a": 1})), x.getvalue(), x.tell(), x.a, x.__setstate__((b"", 0, {"b": 2}, "more")), sorted(vars(x))) for x in [BytesIO(b"old")]])
for state in ((), (b"", 0), [b"", 0, None], None, ("a", 0, None), (b"", "a", None), (b"", -1, None), (b"", 2 ** 70, None), (b"", 1.5, None), (b"", 0, []), (b"", 0, 5), (b"", True, None)):
    t("__setstate__(%r)" % (state,), lambda: [(x.__setstate__(state), x.getvalue(), x.tell()) for x in [BytesIO(b"old")]])
t("of one that is closed", lambda: [(x.close(), x.__setstate__((b"a", 0, None))) for x in [BytesIO()]])
t("__reduce_ex__(2)", lambda: [(r[0].__name__, r[1], r[2]) for r in [BytesIO(b"ab").__reduce_ex__(2)]] if "copyreg" in sys.modules else "no copyreg")

print("---- derived from")


class Sub(BytesIO):
    def __init__(self, data, tag):
        super().__init__(data)
        self.tag = tag

    def write(self, b):
        self.written = b
        return super().write(b)


s = Sub(b"ab", "t")
t("it", lambda: (s.tag, s.read(), s.write(b"c"), s.written, s.getvalue(), isinstance(s, _io._BufferedIOBase), vars(s)))
t("writelines does not go by way of its write", lambda: (setattr(s, "written", None), s.writelines([b"d"]), s.written, s.getvalue()))
t("attributes", lambda: [(setattr(x, "a", 1), x.a, x.__dict__) for x in [BytesIO()]])
t("weakly referred to", lambda: [__import__("_weakref").ref(x)() is x for x in [BytesIO()]])
t("no hash of its own, nor ==", lambda: [(x == x, x == BytesIO(), hash(x) == hash(x)) for x in [BytesIO()]])

print("---- how large it says it is")
BASE = BytesIO().__sizeof__()


def sizes():
    out = []
    x = BytesIO()
    for n in (0, 1, 2, 5, 8, 9, 10, 20, 100, 101, 110, 130, 1000):
        x.seek(0)
        x.write(b"a" * n)
        out.append(x.__sizeof__() - BASE)
    for n in (999, 600, 499, 100, 10, 0):
        x.truncate(n)
        out.append(x.__sizeof__() - BASE)
    return out


t("as it grows and shrinks", sizes)
t("closed", lambda: [(x.close(), BASE - x.__sizeof__()) for x in [BytesIO()]])

print("---- at random, against a bytearray")
state = [88172645463325252]


def random(n):
    x = state[0]
    x ^= (x << 13) & 0xFFFFFFFFFFFFFFFF
    x ^= x >> 7
    x ^= (x << 17) & 0xFFFFFFFFFFFFFFFF
    state[0] = x
    return x % n


x, model, at = BytesIO(), bytearray(), 0
wrong = []
for step in range(20000):
    what = random(9)
    if what == 0:
        data = bytes(random(256) for i in range(random(12)))
        x.write(data)
        if data:
            if at > len(model):
                model.extend(bytes(at - len(model)))
            model[at:at + len(data)] = data
            at += len(data)
    elif what == 1:
        n = random(15) - 2
        got = x.read(n)
        want = bytes(model[at:] if n < 0 else model[at:at + n])
        at = min(max(at, len(model)) if n < 0 else at + len(want), max(at, len(model)))
        if got != want:
            wrong.append((step, "read", got, want))
    elif what == 2:
        at = random(len(model) + 6)
        x.seek(at)
    elif what == 3:
        n = random(len(model) + 4)
        x.truncate(n)
        del model[n:]
    elif what == 4:
        if x.getvalue() != bytes(model):
            wrong.append((step, "getvalue"))
    elif what == 5:
        got = x.readline()
        end = model.find(b"\n", at)
        want = bytes(model[at:end + 1] if end >= 0 else model[at:])
        at += len(want)
        if got != want:
            wrong.append((step, "readline", got, want))
    elif what == 6:
        target = bytearray(random(8))
        n = x.readinto(target)
        want = bytes(model[at:at + len(target)])
        at += len(want)
        if n != len(want) or bytes(target[:n]) != want:
            wrong.append((step, "readinto"))
    elif what == 7 and random(20) == 0:
        view = x.getbuffer()
        if bytes(view) != bytes(model):
            wrong.append((step, "getbuffer"))
        if len(view):
            i = random(len(view))
            view[i] = model[i] = random(256)
        view.release()
    if x.tell() != at:
        wrong.append((step, "tell", x.tell(), at))
        break
t("what went wrong", lambda: (wrong[:3], len(model), sum(model) % 1000))
