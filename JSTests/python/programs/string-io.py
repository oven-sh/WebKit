# io.StringIO and io.IncrementalNewlineDecoder
import sys
import _io
from _io import StringIO, IncrementalNewlineDecoder


def t(label, f):
    try:
        r = f()
    except BaseException as e:
        r = type(e).__name__ + ": " + str(e)
    print(label, "=>", ascii(r) if not isinstance(r, str) else ascii(r)[1:-1])


def attempt(f):
    try:
        return f()
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)


print("---- the classes")
for c in (StringIO, IncrementalNewlineDecoder):
    t(c.__name__, lambda: (c.__module__, [b.__name__ for b in c.__mro__], sorted(k for k in vars(c) if k != "__doc__")))

print("---- IncrementalNewlineDecoder, of text")
for translate in (True, False):
    d = IncrementalNewlineDecoder(None, translate)
    t("translate=%r" % translate, lambda: [(d.decode(s), d.newlines, d.getstate()) for s in ("abc", "a\nb", "a\r", "\nb", "\r", "c", "x\r\ny", "\r\r", "\n\r", "", "\r")])
    t("at the end", lambda: (d.decode("", True), d.getstate(), d.decode("\r", True), d.decode("\r", final=True), d.newlines))
    t("reset", lambda: (d.decode("\r"), d.getstate(), d.reset(), d.getstate(), d.newlines, d.decode("z")))
for text, want in (("\n", "LF"), ("\r", "CR"), ("\r\n", "CRLF"), ("\r\n\n", "LF and CRLF"), ("\r\r\n", "CR and CRLF"), ("\n\r", "CR and LF"), ("\n\r\r\n", "all"), ("abc", "none")):
    t("newlines with " + want, lambda: [(x.decode(text, True), x.newlines) for tr in (True, False) for x in [IncrementalNewlineDecoder(None, tr)]])
t("wide characters", lambda: [(x.decode("€\r\n\U0001F600\rĀ\n\ud800\r", True), x.newlines) for tr in (True, False) for x in [IncrementalNewlineDecoder(None, tr)]])
t("characters that look like them in one byte or the other", lambda: [(x.decode("Ċਊ഍č਀", True), x.newlines) for tr in (True, False) for x in [IncrementalNewlineDecoder(None, tr)]])
t("setstate", lambda: [(x.setstate((b"", 1)), x.getstate(), x.decode("a"), x.setstate((b"", 0)), x.decode("a"), x.setstate((b"xyz", 7)), x.getstate()) for x in [IncrementalNewlineDecoder(None, True)]])
for state in ((), (b"",), (b"", 1, 2), [b"", 1], None, (b"", "a"), (b"", 1.5), (b"", None), (b"", -1), (b"", 2 ** 64 + 3), (b"", True), ("anything", 0)):
    t("setstate(%r)" % (state,), lambda: [(x.setstate(state), x.getstate()) for x in [IncrementalNewlineDecoder(None, True)]])
for arg in (b"abc", 1, None, ["a"]):
    t("decode(%r)" % (arg,), lambda: IncrementalNewlineDecoder(None, True).decode(arg))


class S(str):
    pass


t("of a class derived from str", lambda: [(type(x.decode(S("abc"))).__name__, type(x.decode(S("a\rb"))).__name__, type(x.decode(S("a\r"))).__name__, type(x.decode(S("b"))).__name__, type(x.decode(S(""))).__name__) for x in [IncrementalNewlineDecoder(None, True)]])
for label, args, kwargs in (("()", (), {}), ("(None)", (None,), {}), ("(None, 1, 'e', 2)", (None, 1, "e", 2), {}), ("by name", (), {"decoder": None, "translate": [], "errors": 5}), ("(None, other=1)", (None,), {"other": 1})):
    t("IncrementalNewlineDecoder" + label, lambda: type(IncrementalNewlineDecoder(*args, **kwargs)).__name__)
u = IncrementalNewlineDecoder.__new__(IncrementalNewlineDecoder)
for name, args in (("decode", ("a",)), ("getstate", ()), ("setstate", ((b"", 0),)), ("reset", ())):
    t("%s of one that is not made" % name, lambda: getattr(u, name)(*args))
t("newlines of one that is not made", lambda: u.newlines)
t("no attributes", lambda: setattr(IncrementalNewlineDecoder(None, True), "a", 1))
t("newlines cannot be set", lambda: setattr(IncrementalNewlineDecoder(None, True), "newlines", 1))

print("---- over another decoder")


class Decoder:
    def __init__(self, result=None, state=(b"kept", 5)): self.log, self.result, self.state = [], result, state
    def decode(self, data, final=False):
        self.log.append(("decode", data, final))
        return data.decode("latin-1") if self.result is None else self.result
    def getstate(self):
        self.log.append(("getstate",))
        return self.state
    def setstate(self, state):
        self.log.append(("setstate", state))
        return "from setstate"
    def reset(self):
        self.log.append(("reset",))
        return "from reset"


inner = Decoder()
d = IncrementalNewlineDecoder(inner, True)
t("decode", lambda: (d.decode(b"a\r"), d.decode(b"\nb", True), d.decode(b"c", final=0), inner.log))
t("getstate", lambda: (d.getstate(), d.decode(b"\r"), d.getstate()))
t("setstate and reset", lambda: (inner.log.clear(), d.setstate((b"x", 9)), d.setstate((b"y", 8)), d.reset(), inner.log))
for label, result in (("bytes", b"a"), ("None", None), ("an int", 5)):
    if result is not None:
        t("a decoder that returns " + label, lambda: IncrementalNewlineDecoder(Decoder(result), True).decode(b"a"))
for label, state in (("a list", [b"", 0]), ("None", None), ("one thing", (b"",)), ("three", (b"", 0, 0)), ("a str for the flags", (b"", "a")), ("large flags", (b"", 2 ** 63)), ("larger", (b"", 2 ** 64 + 1)), ("negative", (b"", -1)), ("anything for the buffer", (5, 1))):
    t("a decoder whose state is " + label, lambda: IncrementalNewlineDecoder(Decoder(state=state), True).getstate())
t("a decoder that has no such thing", lambda: (attempt(lambda: IncrementalNewlineDecoder(5, True).decode(b"")), attempt(lambda: IncrementalNewlineDecoder(5, True).getstate()), attempt(lambda: IncrementalNewlineDecoder(5, True).reset())))

print("---- StringIO, made from")
for label, args, kwargs in (("nothing", (), {}), ("a str", ("abc",), {}), ("None", (None,), {}), ("''", ("",), {}), ("by name", (), {"initial_value": "xy", "newline": None}), ("bytes", (b"abc",), {}), ("an int", (5,), {}), ("three", ("a", None, 1), {}), ("another name", (), {"other": ""}),
                            ("derived from str", (S("abc"),), {})):
    t(label, lambda: [(x.getvalue(), x.tell(), x.read(), type(x.getvalue()).__name__) for x in [StringIO(*args, **kwargs)]])
for newline in (None, "", "\n", "\r", "\r\n", "x", "\n\r", "\r\n\n", " ", b"\n", 5, "€", "\x00", S("\r")):
    t("newline=%r" % (newline,), lambda: [(x.getvalue(), x.readlines(), x.newlines, x.write("p\nq\rr\r\ns"), x.getvalue(), x.newlines) for x in [StringIO("a\nb\rc\r\nd", newline)]])

print("---- reading")
x = StringIO("hello\nworld\n\nlast")
t("read", lambda: (x.read(2), x.read(0), x.tell(), x.read(100), x.read(1), x.read(), x.tell()))
t("read(None) and negative", lambda: (x.seek(3), x.read(None), x.seek(3), x.read(-1), x.seek(3), x.read(-100)))
t("readline", lambda: (x.seek(0), x.readline(), x.readline(3), x.readline(0), x.readline(100), x.readline(), x.readline(None), x.readline(), x.readline(-1)))
t("readlines and iteration", lambda: (x.seek(0), x.readlines(), x.seek(0), x.readlines(1), x.readlines(7), x.seek(0), list(x), list(x), x.seek(6), next(x), iter(x) is x))
t("next at the end", lambda: (x.seek(0, 2), next(x)))
for name, args in (("read", ("a",)), ("read", (1.5,)), ("read", (1, 2)), ("read", (2 ** 70,)), ("readline", ("a",)), ("readline", (1.5,)), ("readlines", ("a",))):
    t("%s%r" % (name, args), lambda: (x.seek(0), getattr(x, name)(*args)))
WIDE = "a\U0001F600b€\ud800c\udc00\U0010FFFFd\n\U0001F601e"
w = StringIO(WIDE)
t("a character is a character", lambda: (len(WIDE), w.read(1), w.read(1), w.tell(), w.read(3), w.tell(), w.read(2), w.read(1), w.tell(), w.readline(), w.tell(), w.read(), w.tell(), w.getvalue() == WIDE))
t("seek among them", lambda: [(w.seek(i), w.read(1)) for i in range(len(WIDE) + 1)])
t("written among them", lambda: (w.seek(1), w.write("X"), w.seek(3), w.write("\U0001F602\U0001F603"), w.getvalue(), w.tell(), len(w.getvalue())))
t("how many were written", lambda: [(y.write("\U0001F600"), y.write("a\U0001F600\ud800"), y.tell(), y.write(""), len(y.getvalue())) for y in [StringIO()]])
t("truncated among them", lambda: [(y.truncate(2), y.getvalue(), y.truncate(1), y.getvalue()) for y in [StringIO("a\U0001F600b")]])

print("---- where it is")
x = StringIO("0123456789")
t("seek", lambda: (x.seek(3), x.tell(), x.seek(0, 1), x.seek(0, 2), x.seek(15), x.tell(), x.read(), x.seek(0, 0)))
for args in ((-1,), (1, 1), (-1, 1), (1, 2), (-1, 2), (0, 3), (0, -1), ("a",), (0, "a"), (1.5,), (), (0, 0, 0), (2 ** 63,), (2 ** 63 - 1,), (None,)):
    t("seek%r" % (args,), lambda: (x.seek(1), x.seek(*args)))
t("past the end", lambda: [(y.seek(5), y.read(), y.readline(), y.getvalue(), y.write("Z"), y.getvalue(), y.tell()) for y in [StringIO("ab")]])
t("as far as can be", lambda: [(y.seek(2 ** 63 - 1), y.read(), y.readline(), attempt(lambda: y.write("a"))) for y in [StringIO("ab")]])

print("---- writing")
x = StringIO()
t("write", lambda: (x.write("abc"), x.write("de"), x.write(""), x.tell(), x.getvalue(), x.getvalue() == x.getvalue()))
t("over what is there", lambda: (x.seek(1), x.write("XY"), x.getvalue(), x.tell(), x.seek(4), x.write("123"), x.getvalue()))
for arg in (b"a", 1, None, ["a"], 1.5):
    t("write(%r)" % (arg,), lambda: x.write(arg))
t("write()", lambda: x.write())
t("of a class derived from str", lambda: [(y.write(S("ab")), type(y.getvalue()).__name__, y.getvalue()) for y in [StringIO()]])
t("writelines", lambda: [(y.writelines(["a", "b\n", "c"]), y.writelines(()), y.getvalue()) for y in [StringIO()]])
t("with bytes in it", lambda: [(attempt(lambda: y.writelines(["a", b"b"])), y.getvalue()) for y in [StringIO()]])
t("truncate", lambda: [(y.seek(4), y.truncate(), y.getvalue(), y.tell(), y.truncate(2), y.getvalue(), y.tell(), y.truncate(10), y.getvalue(), y.truncate(0), y.getvalue(), y.truncate(None), y.write("z"), y.getvalue()) for y in [StringIO("0123456789")]])
for arg in (-1, "a", 1.5, 2 ** 70, True):
    t("truncate(%r)" % (arg,), lambda: StringIO("abc").truncate(arg))
t("written to, read, and written to again", lambda: [(y.write("abc"), y.getvalue(), y.write("def"), y.seek(0), y.read(), y.write("ghi"), y.getvalue(), y.seek(2), y.read(2), y.write("!"), y.getvalue()) for y in [StringIO()]])
t("a great deal", lambda: [(sum(y.write("0123456789" * 100) for i in range(300)), len(y.getvalue()), y.tell(), y.seek(299990), y.read(20), y.truncate(5), y.getvalue()) for y in [StringIO()]])

print("---- closed, and not made")
x = StringIO("abc")
t("close", lambda: (x.closed, x.close(), x.closed, x.close()))
n = StringIO.__new__(StringIO)
for label, y in (("closed", x), ("not made", n)):
    for name, args in (("read", ()), ("readline", ()), ("readlines", ()), ("write", ("a",)), ("write", ("",)), ("write", (b"a",)), ("writelines", ([],)), ("seek", (0,)), ("tell", ()), ("truncate", ()), ("getvalue", ()), ("flush", ()), ("isatty", ()), ("readable", ()),
                       ("writable", ()), ("seekable", ()), ("__next__", ()), ("__enter__", ()), ("__iter__", ()), ("__getstate__", ()), ("fileno", ()), ("detach", ()), ("read", ("a",)), ("__setstate__", (("", "\n", 0, None),))):
        t("%s%r of one that is %s" % (name, args, label), lambda: getattr(y, name)(*args) if name not in ("__enter__", "__iter__") else type(getattr(y, name)(*args)).__name__)
    for name in ("closed", "newlines", "line_buffering", "encoding", "errors"):
        t("%s of one that is %s" % (name, label), lambda: getattr(y, name))
t("open", lambda: [(y.readable(), y.writable(), y.seekable(), y.isatty(), y.flush(), y.line_buffering, y.encoding, y.errors, y.newlines) for y in [StringIO()]])
t("with", lambda: [(y.closed, y.__exit__(None, None, None), y.closed) for y in [StringIO().__enter__()]])
t("made again", lambda: [(y.read(2), y.__init__("new"), y.tell(), y.read(), y.close(), y.__init__("again"), y.closed, y.read(), y.__init__(), y.getvalue()) for y in [StringIO("old")]])
t("made again, and it fails", lambda: [(attempt(lambda: y.__init__(5)), y.read(), attempt(lambda: y.__init__("a", "x")), y.getvalue()) for y in [StringIO("old")]])
for name in ("closed", "newlines", "line_buffering"):
    t(name + " cannot be set", lambda: setattr(StringIO(), name, 1))

print("---- state")
x = StringIO("ab\ncd", newline=None)
x.seek(2)
t("__getstate__", lambda: (x.__getstate__(), StringIO("a\r\nb", "\r\n").__getstate__(), StringIO("a", "").__getstate__(), StringIO().__getstate__()))
x.extra = [1]
t("with attributes", lambda: (x.__getstate__(), x.__dict__))
t("__setstate__", lambda: [(y.__setstate__(("x\r\ny", None, 1, None)), y.getvalue(), y.tell(), y.readlines(), y.__setstate__(("pq", "\r", 5, {"a": 1})), y.getvalue(), y.tell(), y.a, y.write("\n"), y.getvalue(), y.__setstate__((None, "\n", 0, {"b": 2}, "more")), y.getvalue(), sorted(vars(y))) for y in [StringIO("old")]])
for state in ((), ("", "\n", 0), ["", "\n", 0, None], None, (b"a", "\n", 0, None), ("", "x", 0, None), ("", 5, 0, None), ("", "\n", "a", None), ("", "\n", -1, None), ("", "\n", 2 ** 70, None), ("", "\n", 1.5, None), ("", "\n", 0, []), ("", "\n", 0, 5), ("", "\n", True, None)):
    t("__setstate__(%r)" % (state,), lambda: [(y.__setstate__(state), y.getvalue(), y.tell()) for y in [StringIO("old")]])

print("---- derived from")


class Sub(StringIO):
    def __init__(self, data, tag):
        super().__init__(data)
        self.tag = tag

    def readline(self, size=-1):
        return "<" + super().readline(size) + ">" if self.tag else b"not a str"


t("it", lambda: [(y.tag, y.read(1), isinstance(y, _io._TextIOBase), dict(vars(y))) for y in [Sub("abc", "t")]])
t("next goes by way of its readline", lambda: next(Sub("a\nb", "t")))
t("which is to return a str", lambda: next(Sub("a\nb", "")))
t("attributes", lambda: [(setattr(y, "a", 1), y.a, y.__dict__) for y in [StringIO()]])
t("weakly referred to", lambda: [__import__("_weakref").ref(y)() is y for y in [StringIO()]])

print("---- at random, against a list of characters")
state = [362436069]


def random(n):
    v = state[0]
    v ^= (v << 13) & 0xFFFFFFFF
    v ^= v >> 17
    v ^= (v << 5) & 0xFFFFFFFF
    state[0] = v
    return v % n


ALPHABET = ["a", "b", "\n", "\r", "€", "\U0001F600", "\xe9", "\U0010FFFF", "z"]
for newline in ("\n", "", None, "\r", "\r\n"):
    x, model, at, wrong = StringIO(newline=newline), [], 0, []
    for step in range(6000):
        what = random(8)
        if what < 2:
            text = "".join(ALPHABET[random(len(ALPHABET))] for i in range(random(7)))
            n = x.write(text)
            if n != len(text):
                wrong.append((step, "write"))
            if newline is None:
                text = text.replace("\r\n", "\n").replace("\r", "\n")
            elif newline in ("\r", "\r\n"):
                text = text.replace("\n", newline)
            if text:
                if at > len(model):
                    model.extend("\0" * (at - len(model)))
                model[at:at + len(text)] = list(text)
                at += len(text)
        elif what == 2:
            n = random(9) - 2
            got = x.read(n)
            want = "".join(model[at:] if n < 0 else model[at:at + n])
            at += len(want)
            if got != want:
                wrong.append((step, "read", got, want))
        elif what == 3:
            at = random(len(model) + 4)
            x.seek(at)
        elif what == 4 and random(3) == 0:
            n = random(len(model) + 3)
            x.truncate(n)
            del model[n:]
        elif what == 5:
            if x.getvalue() != "".join(model):
                wrong.append((step, "getvalue"))
        elif what == 6:
            got = x.readline()
            rest = "".join(model[at:])
            if newline in (None, "\n"):
                end = rest.find("\n")
                want = rest[:end + 1] if end >= 0 else rest
            elif newline == "":
                ends = [i for i in (rest.find("\n"), rest.find("\r")) if i >= 0]
                if not ends:
                    want = rest
                else:
                    end = min(ends)
                    want = rest[:end + 2] if rest[end:end + 2] == "\r\n" else rest[:end + 1]
            else:
                end = rest.find(newline)
                want = rest[:end + len(newline)] if end >= 0 else rest
            at += len(want)
            if got != want:
                wrong.append((step, "readline", got, want))
        elif what == 7 and len(model) > 300:
            x.truncate(10)
            del model[10:]
        if x.tell() != at:
            wrong.append((step, "tell", x.tell(), at))
            break
    t("newline=%r" % (newline,), lambda: (wrong[:2], len(model), sum(map(ord, model)) % 100003, x.newlines))
