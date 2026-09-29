# io.BufferedReader, BufferedWriter, BufferedRandom and BufferedRWPair, over raw streams that take note of all that is asked of them.
import sys
import _io
from _io import BufferedReader, BufferedWriter, BufferedRandom, BufferedRWPair, BytesIO, _RawIOBase, UnsupportedOperation

# What is let go of with something still to be sent on tries to send it when it is found that nothing refers to it, and when that is is not the same everywhere. Some of what is here cannot, and says so then.
sys.unraisablehook = lambda unraisable: None


def nowhere(text):
    out, i = "", 0
    while True:
        j = text.find("0x", i)
        if j < 0:
            return out + text[i:]
        k = j + 2
        while k < len(text) and text[k] in "0123456789abcdef":
            k += 1
        out += text[i:j] + "0x"
        i = k


def t(label, f):
    try:
        r = f()
    except BaseException as e:
        r = type(e).__name__ + ": " + str(e)
    print(label, "=>", nowhere(r if isinstance(r, str) else repr(r)))


def attempt(f):
    try:
        return f()
    except BaseException as e:
        return type(e).__name__ + ": " + nowhere(str(e))


class Raw(_RawIOBase):
    "Gives out what it has so many bytes at a time, and takes what it is given likewise. None in `script` is that it would block."

    def __init__(self, data=b"", chunk=None, script=None, seekable=True, readable=True, writable=True):
        self.data, self.at, self.chunk, self.script, self.log = bytearray(data), 0, chunk, script, []
        self.can = (seekable, readable, writable)

    def seekable(self): return self.can[0]
    def readable(self): return self.can[1]
    def writable(self): return self.can[2]

    def limit(self, n):
        if self.script is not None:
            if not self.script:
                return n
            step = self.script.pop(0)
            return step if step is None else min(step, n)
        return n if self.chunk is None else min(n, self.chunk)

    def readinto(self, view):
        n = self.limit(len(view))
        self.log.append(("readinto", len(view), n))
        if n is None:
            return None
        piece = self.data[self.at:self.at + n]
        view[:len(piece)] = piece
        self.at += len(piece)
        return len(piece)

    def write(self, view):
        n = self.limit(len(view))
        self.log.append(("write", bytes(view), n))
        if n is None:
            return None
        if self.at > len(self.data):
            self.data.extend(bytes(self.at - len(self.data)))
        self.data[self.at:self.at + n] = bytes(view[:n])
        self.at += n
        return n

    def seek(self, pos, whence=0):
        self.log.append(("seek", pos, whence))
        self.at = pos if whence == 0 else self.at + pos if whence == 1 else len(self.data) + pos
        return self.at

    def tell(self):
        self.log.append(("tell",))
        return self.at

    def truncate(self, size=None):
        self.log.append(("truncate", size))
        size = self.at if size is None else size
        del self.data[size:]
        return size

    def took(self):
        log, self.log[:] = self.log[:], []
        return log


print("---- the classes")
for c in (BufferedReader, BufferedWriter, BufferedRandom, BufferedRWPair):
    t(c.__name__, lambda: (c.__module__, [b.__name__ for b in c.__mro__], sorted(k for k in vars(c) if k != "__doc__")))

print("---- reading")
r = Raw(b"abcdefghijklmnopqrstuvwxyz")
b = BufferedReader(r, 8)
t("made", lambda: (r.took(), b.raw is r, b.closed, b.readable(), b.seekable(), b.isatty(), repr(b), b._finalizing))
t("read(3)", lambda: (b.read(3), r.took(), b.tell(), r.took()))
t("what is in the buffer", lambda: (b.read(2), b.read(0), b.read(3), r.took()))
t("more than the buffer holds", lambda: (b.read(10), r.took(), b.tell(), r.took()))
t("the rest", lambda: (b.read(), r.took(), b.read(), b.read(5), r.took()))
for size, wanted in ((8, (1, 7, 8, 9, 16, 17, 100)), (5, (1, 4, 5, 6, 10, 11, 12)), (1, (1, 2, 5))):
    for n in wanted:
        def go():
            raw = Raw(bytes(range(65, 91)))
            x = BufferedReader(raw, size)
            raw.took()
            return x.read(n), raw.took(), x.read(2), raw.took()
        t("read(%d) with a buffer of %d" % (n, size), go)
t("a raw stream that gives a little at a time", lambda: [(x.read(7), raw.took(), x.read(20), raw.took()) for raw in [Raw(b"abcdefghijklmnop", chunk=3)] for x in [BufferedReader(raw, 4)]])
t("read(None) and read(-1)", lambda: [(x.read(None), y.read(-1)) for x in [BufferedReader(Raw(b"abc"))] for y in [BufferedReader(Raw(b"abc"))]])
for arg in (-2, "a", 1.5, 2 ** 70):
    t("read(%r)" % (arg,), lambda: BufferedReader(Raw(b"abc")).read(arg))
t("peek", lambda: [(x.peek(), raw.took(), x.peek(1), x.peek(100), x.read(2), x.peek(), x.tell(), x.read(2), x.peek(), raw.took()) for raw in [Raw(b"abcdefghij")] for x in [BufferedReader(raw, 4)] if raw.took() or True])
t("peek at the end", lambda: [(x.read(), x.peek(), x.peek(5)) for x in [BufferedReader(Raw(b"ab"), 4)]])
t("peek('a')", lambda: BufferedReader(Raw(b"")).peek("a"))
t("peek(None)", lambda: BufferedReader(Raw(b"")).peek(None))
t("read1", lambda: [(raw.took() and None, x.read1(2), raw.took(), x.read1(100), raw.took(), x.read1(100), raw.took(), x.read1(), raw.took(), x.read1(0), x.read1(-1), raw.took(), x.read1(), x.read1(5)) for raw in [Raw(b"abcdefghijklmnopqrstuvwxyz", chunk=6)] for x in [BufferedReader(raw, 4)]])
t("read1(None)", lambda: BufferedReader(Raw(b"")).read1(None))
t("readinto", lambda: [(raw.took() and None, x.readinto(v), bytes(v), raw.took(), x.readinto(w), bytes(w), raw.took(), x.readinto(bytearray()), x.readinto(w), bytes(w), raw.took(), x.readinto(w)) for raw in [Raw(b"abcdefghijklmnopqrstuvwxyz")] for x in [BufferedReader(raw, 4)] for v in [bytearray(3)] for w in [bytearray(11)]])
t("readinto1", lambda: [(raw.took() and None, x.readinto1(v), bytes(v), raw.took(), x.readinto1(w), bytes(w), raw.took(), x.readinto1(w), bytes(w), raw.took(), x.readinto1(v), bytes(v), raw.took()) for raw in [Raw(b"abcdefghijklmnopqrstuvwxyz", chunk=5)] for x in [BufferedReader(raw, 4)] for v in [bytearray(3)] for w in [bytearray(11)]])
for arg in (b"ab", "a", 1, memoryview(b"ab")):
    t("readinto(%s)" % type(arg).__name__, lambda: BufferedReader(Raw(b"abc")).readinto(arg))
t("readline", lambda: [(raw.took() and None, x.readline(), raw.took(), x.readline(), raw.took(), x.readline(2), x.readline(0), x.readline(100), x.readline(None), x.readline(-1), x.readline(), x.readline(), raw.took()) for raw in [Raw(b"ab\ncdefghijk\nlm\nn\n\nop")] for x in [BufferedReader(raw, 4)]])
t("readline with a limit that falls at the end of the buffer", lambda: [(x.readline(4), x.readline(8), x.readline(3), x.readline()) for x in [BufferedReader(Raw(b"abcdefghijklmnopq\nr"), 4)]])
t("readline('a')", lambda: BufferedReader(Raw(b"")).readline("a"))
t("iteration", lambda: (list(BufferedReader(Raw(b"a\nbb\n\nccc"), 2)), next(BufferedReader(Raw(b"x\ny"))), BufferedReader(Raw(b"a\nb\nc")).readlines(), BufferedReader(Raw(b"a\nb\nc")).readlines(2)))
t("next at the end", lambda: next(BufferedReader(Raw(b""))))

print("---- where it is")
t("seek within the buffer", lambda: [(x.read(2), raw.took() and None, x.seek(0), x.seek(3), x.seek(1, 1), x.seek(-2, 1), x.tell(), raw.took(), x.read(3)) for raw in [Raw(b"abcdefghijklmnop")] for x in [BufferedReader(raw, 8)]])
t("out of it", lambda: [(x.read(2), raw.took() and None, x.seek(10), raw.took(), x.read(2), raw.took(), x.seek(-1, 2), raw.took(), x.read(), x.seek(0), x.read(1), x.seek(20, 1), raw.took(), x.read()) for raw in [Raw(b"abcdefghijklmnop")] for x in [BufferedReader(raw, 4)]])
for args in ((0, 3), (0, -1), (0, 5), ("a",), (0, "a"), (1.5,), (), (2 ** 70,), (-1,), (None,)):
    t("seek%r" % (args,), lambda: BufferedReader(Raw(b"abc")).seek(*args))
t("of what cannot", lambda: BufferedReader(Raw(b"abc", seekable=False)).seek(0))
class NoTell(Raw):
    def tell(self): raise OSError("no telling")


t("tell of what cannot", lambda: [(x.read(1), x.tell()) for x in [BufferedReader(NoTell(b"abc"))]])
for label, value in (("negative", -5), ("a str", "a"), ("None", None), ("huge", 2 ** 70), ("a float", 1.5)):
    class OddTell(Raw):
        def tell(self): return value
        def seek(self, *a): return value
    t("tell that returns " + label, lambda: BufferedReader(OddTell(b"abc")).tell())
    t("seek that returns " + label, lambda: BufferedReader(OddTell(b"abc")).seek(1))
t("truncate", lambda: BufferedReader(Raw(b"abc")).truncate())
t("flush", lambda: [(raw.took() and None, x.flush(), raw.took()) for raw in [Raw(b"abc")] for x in [BufferedReader(raw)]])

print("---- a raw stream that would block, or is wrong")
t("read", lambda: [(x.read(5), x.read(5), x.read(5), x.read(5), raw.took()[1:]) for raw in [Raw(b"abcdefgh", script=[None, 2, None, 3, None, None])] for x in [BufferedReader(raw, 4)]])
t("read()", lambda: [(x.read(), x.read()) for raw in [Raw(b"abcdefgh", script=[None, 2, None])] for x in [BufferedReader(raw, 4)]])
t("read1, peek, readinto, readline", lambda: [(BufferedReader(Raw(b"ab", script=[None]), 4).read1(2), BufferedReader(Raw(b"ab", script=[None]), 4).peek(), BufferedReader(Raw(b"ab", script=[None]), 4).readinto(bytearray(2)), BufferedReader(Raw(b"ab", script=[None]), 4).readinto(bytearray(9)),
                                               BufferedReader(Raw(b"ab", script=[None]), 4).readline(), BufferedReader(Raw(b"ab\n", script=[1, None]), 4).readline(), BufferedReader(Raw(b"abcdef", script=[1, None]), 4).readinto(bytearray(3)))])
for label, value in (("too many", 100), ("negative", -1), ("a str", "a"), ("huge", 2 ** 70), ("a float", 1.5)):
    class OddRead(Raw):
        def readinto(self, view): return value
    for name, args in (("read", (2,)), ("read1", (2,)), ("peek", ()), ("readline", ()), ("readinto", (bytearray(2),))):
        t("%s with readinto that returns %s" % (name, label), lambda: getattr(BufferedReader(OddRead(b"abc"), 4), name)(*args))


def cause():
    class OddRead(Raw):
        def readinto(self, view): return "a"
    try:
        BufferedReader(OddRead(b"")).read(1)
    except OSError as e:
        return type(e.__cause__).__name__, str(e.__cause__), e.__suppress_context__, e.__context__ is e.__cause__, e.args


t("what it is from", cause)


class Raises(Raw):
    def readinto(self, view): raise KeyError("from readinto")


t("readinto that raises", lambda: BufferedReader(Raises(b"")).read(1))
views = []


class Keeps(Raw):
    def readinto(self, view):
        views.append((type(view).__name__, view.readonly, view.format, view.ndim, len(view), view.obj is None))
        return super().readinto(view)

    def write(self, view):
        views.append((type(view).__name__, view.readonly, view.format, view.ndim, len(view)))
        return super().write(view)


t("what it is given to read into", lambda: (BufferedReader(Keeps(b"abcdef"), 4).read(1), [v[:5] for v in views]))
for label, kind in (("no readall", 0), ("readall that returns None", None), ("a str", "a"), ("a bytearray", bytearray(b"z"))):
    class All(Raw):
        if kind == 0:
            readall = property()
            def read(self, n=-1):
                self.log.append(("read", n))
                piece = bytes(self.data[self.at:self.at + 3])
                self.at += len(piece)
                return piece
        else:
            def readall(self): return kind
    t("read() with " + label, lambda: [(x.read(1), x.read(), raw.took()[-3:]) for raw in [All(b"abcdefgh")] for x in [BufferedReader(raw, 2)]])
    t("from the first, with " + label, lambda: BufferedReader(All(b"abcdefgh"), 2).read())
for label, value in (("None", None), ("a str", "a"), ("nothing", b"")):
    class ReadReturns(Raw):
        readall = property()
        def read(self, n=-1): return value
    t("read() with read that returns " + label, lambda: [(x.read(), y.read(1), y.read()) for x in [BufferedReader(ReadReturns(b"ab"), 2)] for y in [BufferedReader(ReadReturns(b"ab"), 2)]])

print("---- writing")
r = Raw()
w = BufferedWriter(r, 8)
t("made", lambda: (r.took(), w.raw is r, w.closed, w.writable(), w.seekable(), repr(w)))
t("what fits is kept", lambda: (w.write(b"abc"), w.write(b"de"), r.took(), w.tell(), r.took(), bytes(r.data)))
t("what does not sends it on", lambda: (w.write(b"fghi"), r.took(), w.tell(), r.took(), bytes(r.data)))
t("flush", lambda: (w.flush(), r.took(), w.flush(), r.took(), bytes(r.data)))
t("more than the buffer holds", lambda: (w.write(b"0123456789abcdefghij"), r.took(), w.flush(), r.took()))
t("just as much", lambda: (w.write(b"12345678"), r.took(), w.write(b"1234567"), r.took(), w.write(b"8"), r.took(), w.write(b""), w.flush(), r.took()))
for size, sizes in ((4, (1, 3, 4, 5, 8, 9)), (1, (1, 2))):
    for n in sizes:
        def go():
            raw = Raw()
            x = BufferedWriter(raw, size)
            raw.took()
            return x.write(b"ab"[:min(2, size - 1)]), x.write(bytes(range(65, 65 + n))), raw.took(), x.flush(), raw.took()
        t("write of %d with a buffer of %d" % (n, size), go)
t("a raw stream that takes a little at a time", lambda: [(x.write(b"abcdefghij"), raw.took()[1:], x.flush(), raw.took(), bytes(raw.data)) for raw in [Raw(chunk=3)] for x in [BufferedWriter(raw, 4)]])
for arg in ("a", 1, None, [1], memoryview(b"abcd")[::2]):
    t("write(%s)" % type(arg).__name__, lambda: BufferedWriter(Raw()).write(arg))
t("of other things that have bytes", lambda: [(x.write(bytearray(b"ab")), x.write(memoryview(b"cd")), x.flush(), bytes(raw.data)) for raw in [Raw()] for x in [BufferedWriter(raw)]])
t("what it is given to write from", lambda: (views.clear(), [(x.write(b"abcdef"), x.flush()) for x in [BufferedWriter(Keeps(), 4)]], views))
t("what is written is what it was", lambda: [(x.write(source), source.__setitem__(0, 90), x.flush(), bytes(raw.data)) for raw in [Raw()] for x in [BufferedWriter(raw, 8)] for source in [bytearray(b"abc")]])
t("seek", lambda: [(x.write(b"abc"), raw.took() and None, x.seek(1), raw.took(), x.write(b"Z"), x.tell(), x.flush(), raw.took(), bytes(raw.data), x.seek(0, 2), x.seek(-1, 1)) for raw in [Raw()] for x in [BufferedWriter(raw, 8)]])
t("truncate", lambda: [(x.write(b"abcdef"), raw.took() and None, x.truncate(3), raw.took(), bytes(raw.data), x.tell(), x.truncate(), x.truncate(None), raw.took()) for raw in [Raw()] for x in [BufferedWriter(raw, 8)]])
t("writelines", lambda: [(x.writelines([b"ab", b"cd", b"efgh"]), x.flush(), bytes(raw.data)) for raw in [Raw()] for x in [BufferedWriter(raw, 4)]])
t("cannot be read", lambda: (attempt(lambda: BufferedWriter(Raw()).read()), attempt(lambda: BufferedWriter(Raw()).readline()), attempt(lambda: BufferedWriter(Raw()).readable()), attempt(lambda: BufferedWriter(Raw()).read1()), attempt(lambda: BufferedWriter(Raw()).readinto(bytearray(1))), hasattr(BufferedWriter, "peek")))

print("---- a raw stream that would block")


def blocked(script, size, *writes):
    raw = Raw(script=script)
    x = BufferedWriter(raw, size)
    raw.took()
    out = []
    for data in writes:
        try:
            out.append(x.write(data) if data is not None else x.flush())
        except BlockingIOError as e:
            out.append(("blocked", e.characters_written, e.args[1:], e.errno))
    return out, raw.took(), bytes(raw.data)


t("on flush", lambda: blocked([None], 8, b"abc", None, None))
t("when the buffer is full, and there is room once it has moved up", lambda: blocked([2, None], 8, b"abcdef", b"ghi", None))
t("and there is not", lambda: blocked([2, None], 8, b"abcdef", b"ghijklmn", None))
t("with more than the buffer holds", lambda: blocked([None], 4, b"abcdefghij", None))
t("after some of it", lambda: blocked([3, None], 4, b"abcdefghijkl", None))
t("with just what the buffer holds", lambda: blocked([None], 4, b"abcd", None))
t("again and again", lambda: blocked([None, None, 1, None, None], 4, b"ab", b"cdef", b"g", None, None, None))
for label, value in (("too many", 100), ("negative", -1), ("a str", "a"), ("huge", 2 ** 70), ("a float", 1.5), ("True", True), ("0", 0)):
    if value == 0:
        continue
    class OddWrite(Raw):
        def write(self, view): return value
    t("write that returns " + label, lambda: [(x.write(b"abc"), x.flush()) for x in [BufferedWriter(OddWrite(), 8)]])
    t("at once, " + label, lambda: BufferedWriter(OddWrite(), 2).write(b"abcdef"))


class WriteRaises(Raw):
    def write(self, view): raise KeyError("from write")


t("write that raises", lambda: [(x.write(b"ab"), attempt(x.flush), attempt(x.flush), attempt(lambda: x.write(b"cdefghij")), x.tell()) for x in [BufferedWriter(WriteRaises(), 4)]])

print("---- closing")
t("sends on what there is", lambda: [(x.write(b"abc"), raw.took() and None, x.close(), raw.took(), x.closed, raw.closed, bytes(raw.data), x.close(), repr(x)) for raw in [Raw()] for x in [BufferedWriter(raw, 8)]])
for make in (lambda: BufferedReader(Raw(b"abc"), 4), lambda: BufferedWriter(Raw(), 4), lambda: BufferedRandom(Raw(b"abc"), 4)):
    x = make()
    x.close()
    for name, args in (("read", ()), ("read", (1,)), ("read1", ()), ("peek", ()), ("readinto", (bytearray(1),)), ("readinto1", (bytearray(1),)), ("readline", ()), ("write", (b"a",)), ("write", (b"",)), ("flush", ()), ("seek", (0,)), ("tell", ()), ("truncate", ()),
                       ("__next__", ()), ("__enter__", ()), ("readable", ()), ("writable", ()), ("seekable", ()), ("fileno", ()), ("isatty", ()), ("detach", ()), ("readlines", ()), ("writelines", ([],)), ("__sizeof__", ())):
        if hasattr(x, name):
            t("%s.%s%r when closed" % (type(x).__name__, name, args), lambda: getattr(x, name)(*args) if name != "__sizeof__" else x.__sizeof__() < 1000)
t("what had been read can still be", lambda: [(x.read(1), raw.close(), x.read(2), x.readline(), attempt(x.read), attempt(lambda: x.read(1))) for raw in [Raw(b"abcd\nef")] for x in [BufferedReader(raw, 5)]])


class FlushRaises(Raw):
    def flush(self):
        self.log.append(("flush",))
        raise OSError("from flush")


class CloseRaises(Raw):
    def close(self):
        super().close()
        raise NameError("from close")


class BothRaise(WriteRaises):
    def close(self):
        super().close()
        raise NameError("from close")


t("when sending on fails", lambda: [(x.write(b"a"), attempt(x.close), x.closed, raw.closed) for raw in [WriteRaises()] for x in [BufferedWriter(raw, 4)]])
t("when closing the raw stream fails", lambda: [(attempt(x.close), x.closed) for x in [BufferedWriter(CloseRaises(), 4)]])


def both():
    x = BufferedWriter(BothRaise(), 4)
    x.write(b"a")
    try:
        x.close()
    except BaseException as e:
        return type(e).__name__, str(e), type(e.__context__).__name__, str(e.__context__), x.closed


t("when both do", both)
t("with", lambda: [(x.closed, x.__exit__(None, None, None), x.closed) for x in [BufferedWriter(Raw()).__enter__()]])
t("__del__", lambda: [(x.write(b"ab"), x.__del__(), x.closed, bytes(raw.data), x._finalizing) for raw in [Raw()] for x in [BufferedWriter(raw)]])

print("---- detached, and not yet made")
t("detach", lambda: [(x.write(b"ab"), x.detach() is raw, bytes(raw.data), x.raw, repr(x), raw.closed) for raw in [Raw()] for x in [BufferedWriter(raw)]])
d = BufferedRandom(Raw(b"abc"))
d.detach()
u = BufferedRandom.__new__(BufferedRandom)
for label, x in (("detached", d), ("not made", u)):
    for name, args in (("read", ()), ("read1", ()), ("peek", ()), ("readinto", (bytearray(1),)), ("readline", ()), ("write", (b"a",)), ("flush", ()), ("seek", (0,)), ("tell", ()), ("truncate", ()), ("close", ()), ("detach", ()), ("__next__", ()), ("readable", ()),
                       ("writable", ()), ("seekable", ()), ("fileno", ()), ("isatty", ()), ("_dealloc_warn", (1,))):
        t("%s%r of one that is %s" % (name, args, label), lambda: getattr(x, name)(*args))
    for name in ("closed", "name", "mode", "raw", "_finalizing"):
        t("%s of one that is %s" % (name, label), lambda: getattr(x, name))
    t("repr of one that is " + label, lambda: repr(x))
t("object.__new__", lambda: type(object.__new__(BufferedReader)).__name__)
t("made again", lambda: [(x.read(1), x.__init__(Raw(b"xyz"), 2), x.read()) for x in [BufferedReader(Raw(b"abc"))]])
t("made again, and it fails", lambda: [(attempt(lambda: x.__init__(Raw(readable=False))), attempt(x.read), attempt(lambda: x.__init__(Raw(b"q"), 0)), attempt(x.read), x.raw is not None) for x in [BufferedReader(Raw(b"abc"))]])

print("---- what they are made from")
for c in (BufferedReader, BufferedWriter, BufferedRandom):
    for label, args, kwargs in (("()", (), {}), ("(raw, 0)", (Raw(),  0), {}), ("(raw, -1)", (Raw(), -1), {}), ("(raw, 'a')", (Raw(), "a"), {}), ("(raw, None)", (Raw(), None), {}), ("(raw, 1.5)", (Raw(), 1.5), {}), ("(raw, 2 ** 70)", (Raw(), 2 ** 70), {}), ("(raw, 1, 2)", (Raw(), 1, 2), {}),
                                ("(raw=raw, buffer_size=3)", (), {"raw": Raw(), "buffer_size": 3}), ("(raw, other=1)", (Raw(),), {"other": 1}), ("(1)", (1,), {}), ("(None)", (None,), {}), ("(not readable)", (Raw(readable=False),), {}), ("(not writable)", (Raw(writable=False),), {}),
                                ("(not seekable)", (Raw(seekable=False),), {}), ("(BytesIO)", (BytesIO(b"ab"),), {})):
        t(c.__name__ + label, lambda: type(c(*args, **kwargs)).__name__)
t("how large", lambda: [(x.__sizeof__() - y.__sizeof__(), z.__sizeof__() - y.__sizeof__()) for x in [BufferedReader(Raw(), 100)] for y in [BufferedReader(Raw(), 1)] for z in [BufferedReader(Raw())]])


class Named(Raw):
    name = "the name"
    mode = "the mode"
    def fileno(self): return 42
    def isatty(self): return "as the raw stream says"


t("what is the raw stream's", lambda: [(x.name, x.mode, x.fileno(), x.isatty(), repr(x)) for x in [BufferedReader(Named())]])
t("that it has not", lambda: [(attempt(lambda: x.name), attempt(lambda: x.mode), attempt(x.fileno)) for x in [BufferedReader(Raw())]])
t("cannot be set", lambda: [(attempt(lambda: setattr(x, "name", 1)), attempt(lambda: setattr(x, "raw", 1)), attempt(lambda: setattr(x, "closed", 1)), setattr(x, "_finalizing", True), x._finalizing, attempt(lambda: setattr(x, "_finalizing", 1))) for x in [BufferedReader(Raw())]])
t("attributes", lambda: [(setattr(x, "a", 1), x.a, x.__dict__) for x in [BufferedReader(Raw())]])
t("pickle", lambda: BufferedReader(Raw()).__getstate__())
t("weakly referred to", lambda: [__import__("_weakref").ref(x)() is x for x in [BufferedReader(Raw())]])


class SelfNamed(Raw):
    @property
    def name(self): return holder[0]


holder = []
holder.append(BufferedReader(SelfNamed()))
t("a name that is itself", lambda: repr(holder[0]))

print("---- what is begun in the middle of something else")


class Reenters(Raw):
    def readinto(self, view):
        self.result = attempt(lambda: self.owner.read(1))
        return super().readinto(view)

    def write(self, view):
        self.result = attempt(lambda: self.owner.flush())
        return super().write(view)


def reenter(c, act):
    raw = Reenters(b"abcdef")
    raw.owner = c(raw, 4)
    return act(raw.owner), raw.result


t("reading", lambda: reenter(BufferedReader, lambda x: x.read(1)))
t("writing", lambda: reenter(BufferedWriter, lambda x: (x.write(b"ab"), x.flush())))

print("---- derived from")


class Sub(BufferedReader):
    def __init__(self, raw, tag):
        super().__init__(raw, 4)
        self.tag = tag

    def readline(self, size=-1):
        return b"<" + super().readline(size) + b">" if self.tag else "not bytes"


t("it", lambda: [(x.tag, x.read(1), repr(x), isinstance(x, _io._BufferedIOBase)) for x in [Sub(Raw(b"abc"), "t")]])
t("next goes by way of its readline", lambda: next(Sub(Raw(b"a\nb"), "t")))
t("which is to return bytes", lambda: next(Sub(Raw(b"a\nb"), "")))

print("---- BufferedRandom")
r = Raw(b"0123456789abcdef")
x = BufferedRandom(r, 8)
t("made", lambda: (r.took(), x.readable(), x.writable(), x.seekable()))
t("read and then write", lambda: (x.read(2), r.took(), x.write(b"XY"), r.took(), x.tell(), r.took(), x.read(2), r.took(), x.flush(), r.took(), bytes(r.data)))
t("write and then read", lambda: (x.seek(0), x.write(b"AB"), r.took() and None, x.read(3), r.took(), bytes(r.data)))
t("peek, readline and the rest after writing", lambda: [(y.write(b"Z"), y.peek()[:2], y.write(b"Y"), y.readline(), y.write(b"X"), y.read1(1), y.write(b"W"), y.readinto(v), bytes(v), y.flush(), bytes(raw.data)) for raw in [Raw(b"abc\ndefghijklm")] for y in [BufferedRandom(raw, 4)] for v in [bytearray(2)]])
t("seek after writing", lambda: [(y.write(b"ZZ"), y.seek(1), y.read(2), y.seek(-1, 1), y.write(b"Q"), y.seek(0), y.read()) for y in [BufferedRandom(Raw(b"abcdef"), 4)]])
t("truncate", lambda: [(y.read(2), y.truncate(), y.tell(), y.seek(0), y.read(), y.write(b"zz"), y.truncate(1), y.seek(0), y.read()) for y in [BufferedRandom(Raw(b"abcdef"), 4)]])
t("over BytesIO", lambda: [(y.read(1), y.write(b"XYZ"), y.read(), y.seek(0), y.read(), raw.getvalue()) for raw in [BytesIO(b"abcdefg")] for y in [BufferedRandom(raw, 3)]])

print("---- at random, against a bytearray")
state = [123456789]


def random(n):
    v = state[0]
    v ^= (v << 13) & 0xFFFFFFFF
    v ^= v >> 17
    v ^= (v << 5) & 0xFFFFFFFF
    state[0] = v
    return v % n


for size, chunk in ((1, None), (2, 1), (3, None), (4, 3), (8, None), (8, 5), (16, 7), (64, None)):
    raw = Raw(bytes(random(256) for i in range(40)), chunk=chunk)
    x, model, at, wrong = BufferedRandom(raw, size), bytearray(raw.data), 0, []
    digest = 0
    for step in range(3000):
        what = random(11)
        if what == 0:
            data = bytes(random(256) for i in range(random(size * 2 + 3)))
            x.write(data)
            if data:
                if at > len(model):
                    model.extend(bytes(at - len(model)))
                model[at:at + len(data)] = data
                at += len(data)
        elif what == 1:
            n = random(size * 2 + 3)
            got, want = x.read(n), bytes(model[at:at + n])
            at += len(want)
            if got != want:
                wrong.append((step, "read", got, want))
        elif what == 2:
            at = random(len(model) + 3)
            x.seek(at)
        elif what == 3:
            delta = random(9) - 4
            if at + delta >= 0:
                at += delta
                x.seek(delta, 1)
        elif what == 4:
            got = x.readline()
            end = model.find(b"\n", at)
            want = bytes(model[at:end + 1] if end >= 0 else model[at:])
            at += len(want)
            if got != want:
                wrong.append((step, "readline", got, want))
        elif what == 5:
            got = x.peek()
            if got != bytes(model[at:at + len(got)]) or (not got and at < len(model)):
                wrong.append((step, "peek"))
        elif what == 6:
            got = x.read1(random(size + 3))
            if got != bytes(model[at:at + len(got)]):
                wrong.append((step, "read1"))
            at += len(got)
        elif what == 7:
            target = bytearray(random(size * 2 + 2))
            n = x.readinto(target)
            want = bytes(model[at:at + len(target)])
            at += len(want)
            if n != len(want) or bytes(target[:n]) != want:
                wrong.append((step, "readinto"))
        elif what == 8:
            x.flush()
            if bytes(raw.data) != bytes(model):
                wrong.append((step, "flush"))
        elif what == 9 and random(6) == 0:
            n = random(len(model) + 2)
            x.truncate(n)
            del model[n:]
        elif what == 10 and len(model) > 200:
            x.truncate(20)
            del model[20:]
        if x.tell() != at:
            wrong.append((step, "tell", x.tell(), at))
            break
    for entry in raw.log:
        for ch in repr(entry):
            digest = (digest * 31 + ord(ch)) % 1000000007
    t("a buffer of %d, %r at a time" % (size, chunk), lambda: (wrong[:2], len(model), len(raw.log), digest))

print("---- BufferedRWPair")
rr, ww = Raw(b"abcdefgh"), Raw()
p = BufferedRWPair(rr, ww, 4)
t("made", lambda: (p.readable(), p.writable(), p.closed, p.isatty(), attempt(p.seekable), hasattr(p, "raw")))
t("reads from the one", lambda: (p.read(2), p.peek(1), p.read1(1), [(p.readinto(v), bytes(v), p.readinto1(v), bytes(v)) for v in [bytearray(2)]], p.read()))
t("writes to the other", lambda: (p.write(b"xy"), bytes(ww.data), p.flush(), bytes(ww.data)))
t("close", lambda: (p.write(b"z"), p.close(), p.closed, rr.closed, ww.closed, bytes(ww.data), p.close()))
for name, args in (("read", ()), ("peek", ()), ("read1", ()), ("readinto", (bytearray(1),)), ("write", (b"a",)), ("flush", ()), ("readable", ()), ("writable", ()), ("isatty", ())):
    t("%s%r when closed" % (name, args), lambda: getattr(p, name)(*args))
for label, args, kwargs in (("()", (), {}), ("(r)", (Raw(),), {}), ("(r, w, 0)", (Raw(), Raw(), 0), {}), ("(r, w, 'a')", (Raw(), Raw(), "a"), {}), ("(r, w, 1, 2)", (Raw(), Raw(), 1, 2), {}), ("(reader=r, writer=w)", (), {"reader": Raw(), "writer": Raw()}),
                            ("(not readable, w)", (Raw(readable=False), Raw()), {}), ("(r, not writable)", (Raw(), Raw(writable=False)), {}), ("(1, 2)", (1, 2), {})):
    t("BufferedRWPair" + label, lambda: type(BufferedRWPair(*args, **kwargs)).__name__)
n = BufferedRWPair.__new__(BufferedRWPair)
for name, args in (("read", ()), ("write", (b"",)), ("flush", ()), ("close", ()), ("readable", ()), ("isatty", ())):
    t("%s%r of one that is not made" % (name, args), lambda: getattr(n, name)(*args))
t("closed of one that is not made", lambda: n.closed)
t("by name", lambda: BufferedRWPair(Raw(b"ab"), Raw()).read(size=1))
t("what is wrong is for the reader to say", lambda: BufferedRWPair(Raw(b"ab"), Raw()).read("a"))
t("too many", lambda: BufferedRWPair(Raw(b"ab"), Raw()).read(1, 2))
t("flush(1)", lambda: BufferedRWPair(Raw(b"ab"), Raw()).flush(1))
t("isatty", lambda: [(BufferedRWPair(a, b).isatty()) for a, b in ((Named(), Raw()), (Raw(), Named()), (Raw(), Raw()))])


def pair_close(reader, writer):
    x = BufferedRWPair(reader, writer)
    try:
        x.close()
    except BaseException as e:
        return type(e).__name__, str(e), type(e.__context__).__name__, reader.closed, writer.closed
    return "closed"


t("when closing the writer fails", lambda: pair_close(Raw(), CloseRaises()))
t("when closing the reader fails", lambda: pair_close(CloseRaises(), Raw()))
t("when both do", lambda: pair_close(CloseRaises(), CloseRaises()))
