# io.TextIOWrapper, over codecs that are registered here, so that it does not matter what else there is.
import _codecs
import _io
import sys
from _io import TextIOWrapper, BytesIO, BufferedReader, BufferedWriter, BufferedRandom, _BufferedIOBase, _RawIOBase, UnsupportedOperation

sys.unraisablehook = lambda unraisable: None


def show(e):
    context = e.__context__
    return type(e).__name__ + ": " + str(e) + (" | after %s: %s" % (type(context).__name__, context) if context is not None else "")


def attempt(f, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return show(e)


def t(label, f):
    r = attempt(f)
    print(label, "=>", r if isinstance(r, str) else ascii(r))


# ---- Codecs, as encodings/*.py has them


class Decoder:
    "codecs.BufferedIncrementalDecoder"
    function = None

    def __init__(self, errors="strict"): self.errors, self.buffer = errors, b""

    def decode(self, input, final=False):
        data = self.buffer + input
        result, consumed = type(self).function(data, self.errors, final)
        self.buffer = data[consumed:]
        return result

    def reset(self): self.buffer = b""
    def getstate(self): return (self.buffer, 0)
    def setstate(self, state): self.buffer = state[0]


class Encoder:
    "codecs.IncrementalEncoder"
    function = None

    def __init__(self, errors="strict"): self.errors = errors
    def encode(self, input, final=False): return type(self).function(input, self.errors)[0]
    def reset(self): pass
    def getstate(self): return 0
    def setstate(self, state): pass


class UTF16Decoder(Decoder):
    "encodings.utf_16.IncrementalDecoder"

    def __init__(self, errors="strict"):
        super().__init__(errors)
        self.decoder = None

    def decode(self, input, final=False):
        data = self.buffer + input
        if self.decoder is None:
            result, consumed, byteorder = _codecs.utf_16_ex_decode(data, self.errors, 0, final)
            if byteorder == -1:
                self.decoder = _codecs.utf_16_le_decode
            elif byteorder == 1:
                self.decoder = _codecs.utf_16_be_decode
            elif consumed >= 2:
                raise UnicodeDecodeError("utf-16", data, 0, 2, "Stream does not start with BOM")
        else:
            result, consumed = self.decoder(data, self.errors, final)
        self.buffer = data[consumed:]
        return result

    def reset(self):
        super().reset()
        self.decoder = None

    def getstate(self):
        state = super().getstate()[0]
        if self.decoder is None:
            return (state, 2)
        return (state, int(self.decoder is not _codecs.utf_16_le_decode))

    def setstate(self, state):
        super().setstate(state)
        self.decoder = None if state[1] == 2 else _codecs.utf_16_be_decode if state[1] else _codecs.utf_16_le_decode


class UTF16Encoder(Encoder):
    "encodings.utf_16.IncrementalEncoder"

    def __init__(self, errors="strict"):
        super().__init__(errors)
        self.encoder = None

    def encode(self, input, final=False):
        if self.encoder is None:
            self.encoder = _codecs.utf_16_le_encode
            return _codecs.utf_16_encode(input, self.errors)[0]
        return self.encoder(input, self.errors)[0]

    def reset(self): self.encoder = None
    def getstate(self): return 2 if self.encoder is None else 0
    def setstate(self, state): self.encoder = None if state else _codecs.utf_16_le_encode


class Info(tuple):
    pass


known = {}


def codec(key, name, encode, decode, encoder=None, decoder=None, **more):
    info = Info((encode, decode, None, None))
    info.name = name
    info.incrementalencoder = encoder or type("E", (Encoder,), {"function": staticmethod(encode)})
    info.incrementaldecoder = decoder or type("D", (Decoder,), {"function": staticmethod(decode)})
    for k, v in more.items():
        setattr(info, k, v)
    known[key] = info


final_only = lambda f: lambda data, errors, final: f(data, errors)
codec("test_utf_8", "utf-8", _codecs.utf_8_encode, _codecs.utf_8_decode)
codec("test_utf_8_slow", "test-utf-8", _codecs.utf_8_encode, _codecs.utf_8_decode)
codec("test_ascii", "ascii", _codecs.ascii_encode, final_only(_codecs.ascii_decode))
codec("test_latin_1", "iso8859-1", _codecs.latin_1_encode, final_only(_codecs.latin_1_decode))
codec("test_utf_16", "utf-16", _codecs.utf_16_encode, _codecs.utf_16_decode, UTF16Encoder, UTF16Decoder)
codec("test_utf_16_slow", "test-utf-16", _codecs.utf_16_encode, _codecs.utf_16_decode, UTF16Encoder, UTF16Decoder)
codec("test_utf_16_le", "utf-16-le", _codecs.utf_16_le_encode, _codecs.utf_16_le_decode)
codec("test_utf_16_be", "utf-16-be", _codecs.utf_16_be_encode, _codecs.utf_16_be_decode)
codec("test_utf_32_le", "utf-32-le", _codecs.utf_32_le_encode, _codecs.utf_32_le_decode)
codec("test_utf_32_be", "utf-32-be", _codecs.utf_32_be_encode, _codecs.utf_32_be_decode)
codec("test_utf_7", "utf-7", _codecs.utf_7_encode, _codecs.utf_7_decode)
codec("test_binary", "test-binary", _codecs.latin_1_encode, final_only(_codecs.latin_1_decode), _is_text_encoding=False)
_codecs.register(known.get)
U8 = "test_utf_8"


class Buffer(_BufferedIOBase):
    "Takes note of all that is asked of it."

    def __init__(self, data=b"", chunk=None, seekable=True, readable=True, writable=True, read1=True):
        self.data, self.at, self.chunk, self.log, self.can = bytearray(data), 0, chunk, [], (seekable, readable, writable)
        if read1:
            self.read1 = self._read1

    def seekable(self): return self.can[0]
    def readable(self): return self.can[1]
    def writable(self): return self.can[2]

    def take(self, n):
        if n is None or n < 0:
            n = len(self.data)
        if self.chunk is not None:
            n = min(n, self.chunk)
        piece = bytes(self.data[self.at:self.at + n])
        self.at += len(piece)
        return piece

    def read(self, n=-1):
        self.log.append(("read", n))
        if n is None or n < 0:
            piece = bytes(self.data[self.at:])
            self.at = len(self.data)
            return piece
        return self.take(n)

    def _read1(self, n=-1):
        self.log.append(("read1", n))
        return self.take(n)

    def write(self, b):
        self.log.append(("write", type(b).__name__, bytes(b)))
        self.data[self.at:self.at + len(b)] = b
        self.at += len(b)
        return len(b)

    def flush(self): self.log.append(("flush",))

    def seek(self, pos, whence=0):
        self.log.append(("seek", pos, whence))
        self.at = pos if whence == 0 else self.at + pos if whence == 1 else len(self.data) + pos
        return self.at

    def tell(self):
        self.log.append(("tell",))
        return self.at

    def truncate(self, pos=None):
        self.log.append(("truncate", pos))
        del self.data[self.at if pos is None else pos:]
        return len(self.data)

    def took(self):
        log, self.log[:] = self.log[:], []
        return log


print("---- the class")
t("TextIOWrapper", lambda: (TextIOWrapper.__module__, [b.__name__ for b in TextIOWrapper.__mro__], sorted(k for k in vars(TextIOWrapper) if k != "__doc__")))

print("---- what it is made from")
b = Buffer(b"abc")
x = TextIOWrapper(b, U8)
t("what is asked of the buffer", lambda: b.took())
t("what it has", lambda: (x.encoding, x.errors, x.buffer is b, x.line_buffering, x.write_through, x.newlines, x.closed, x._CHUNK_SIZE, x._finalizing, x.readable(), x.writable(), x.seekable(), x.isatty(), repr(x), attempt(lambda: x.name), attempt(lambda: x.mode), attempt(x.fileno)))
for label, args, kwargs in (("()", (), {}), ("(None)", (None, U8), {}), ("(5)", (5, U8), {}), ("an encoding that there is not", (Buffer(), "test_none_such"), {}), ("one that is not of text", (Buffer(), "test_binary"), {}), ("an int for the encoding", (Buffer(), 5), {}), ("bytes", (Buffer(), b"test_utf_8"), {}),
                            ("a zero in it", (Buffer(), "test\0utf"), {}), ("''", (Buffer(), ""), {}), ("errors=5", (Buffer(), U8, 5), {}), ("errors=b''", (Buffer(), U8, b"strict"), {}), ("errors with a zero", (Buffer(), U8, "a\0b"), {}), ("errors that there are not", (Buffer(), U8, "no such"), {}), ("errors=None", (Buffer(), U8, None), {}),
                            ("errors=''", (Buffer(), U8, ""), {}), ("newline=5", (Buffer(), U8, None, 5), {}), ("newline=b'\\n'", (Buffer(), U8, None, b"\n"), {}), ("newline='x'", (Buffer(), U8, None, "x"), {}), ("newline='\\n\\r'", (Buffer(), U8, None, "\n\r"), {}), ("newline='\\r\\n\\n'", (Buffer(), U8, None, "\r\n\n"), {}),
                            ("newline with a zero", (Buffer(), U8, None, "\0"), {}), ("newline='\\n\\0'", (Buffer(), U8, None, "\n\0"), {}), ("seven", (Buffer(), U8, None, None, 0, 0, 0), {}), ("by name", (), {"buffer": Buffer(), "encoding": U8, "errors": "ignore", "newline": "\r\n", "line_buffering": [1], "write_through": "yes"}),
                            ("another name", (Buffer(), U8), {"other": 1}), ("an encoding in other letters", (Buffer(), "TEST-Utf 8"), {})):
    t(label, lambda: [(y.encoding, y.errors, y.line_buffering, y.write_through) for y in [TextIOWrapper(*args, **kwargs)]])
t("what cannot be read", lambda: [(raw.took(), y.readable(), attempt(y.read), attempt(y.readline), attempt(next, y), y.write("a"), y.newlines) for raw in [Buffer(readable=False)] for y in [TextIOWrapper(raw, U8)]])
t("what cannot be written", lambda: [(raw.took(), y.writable(), attempt(y.write, "a"), y.read()) for raw in [Buffer(b"ab", writable=False)] for y in [TextIOWrapper(raw, U8)]])
t("what cannot be sought in", lambda: [(raw.took(), y.seekable(), attempt(y.tell), attempt(y.seek, 0), y.read(1), y.write("z")) for raw in [Buffer(b"ab", seekable=False)] for y in [TextIOWrapper(raw, U8)]])
t("what is not at its beginning", lambda: [(y.write("a"), raw.took(), y.flush(), raw.took()) for raw in [Buffer(b"abcd")] if raw.seek(2) for y in [TextIOWrapper(raw, "test_utf_16")]])


class Lacking:
    def __init__(self, *names):
        for n in names:
            setattr(self, n, lambda *a: False)


t("what has not what is asked for", lambda: [attempt(TextIOWrapper, Lacking(*names), U8) for names in ((), ("readable",), ("readable", "writable"), ("readable", "writable", "seekable"))])
for label, changes in (("no incrementaldecoder", {"incrementaldecoder": ...}), ("no incrementalencoder", {"incrementalencoder": ...}), ("no name", {"name": ...}), ("a name that is not a str", {"name": 5}), ("a decoder that cannot be made", {"incrementaldecoder": 5}), ("an encoder that raises", {"incrementalencoder": lambda errors: 1 / 0})):
    codec("test_changed", "x", _codecs.utf_8_encode, _codecs.utf_8_decode)
    for k, v in changes.items():
        if v is ...:
            delattr(known["test_changed"], k)
        else:
            setattr(known["test_changed"], k, v)
    _codecs.unregister(known.get)
    _codecs.register(known.get)
    t("a codec with " + label, lambda: [(y.write("a\xe9"), y.flush(), bytes(y.buffer.data)) for y in [TextIOWrapper(Buffer(), "test_changed")]])
known["test_plain"] = (_codecs.utf_8_encode, _codecs.utf_8_decode, None, None)
t("a codec that is a plain tuple", lambda: TextIOWrapper(Buffer(), "test_plain"))
made = []
codec("test_made", "x", _codecs.utf_8_encode, _codecs.utf_8_decode, lambda *a, **k: (made.append(("encoder", a, k)), Encoder())[1], lambda *a, **k: (made.append(("decoder", a, k)), Decoder())[1])
t("what the encoder and decoder are made with", lambda: (TextIOWrapper(Buffer(), "test_made") and None, TextIOWrapper(Buffer(), "test_made", "ignore") and None, made))

print("---- writing")
for newline in (None, "", "\n", "\r", "\r\n"):
    t("newline=%r" % (newline,), lambda: [(y.write("a\nb\rc\r\nd\n"), y.write(""), raw.took()[3:], y.flush(), raw.took(), bytes(raw.data)) for raw in [Buffer()] for y in [TextIOWrapper(raw, U8, newline=newline)]])
    t("a line at a time", lambda: [(raw.took() and None, y.write("ab"), raw.took(), y.write("c\n"), raw.took(), y.write("d\r"), raw.took(), y.write("e"), raw.took()) for raw in [Buffer()] for y in [TextIOWrapper(raw, U8, newline=newline, line_buffering=True)]])
t("at once", lambda: [(raw.took() and None, y.write("ab"), raw.took(), y.write(""), raw.took(), y.write("c\n"), raw.took()) for raw in [Buffer()] for y in [TextIOWrapper(raw, U8, write_through=True)]])
t("what waits", lambda: [(raw.took() and None, setattr(y, "_CHUNK_SIZE", 8), y.write("abc"), y.write("de"), raw.took(), y.write("fgh"), raw.took(), y.write("i"), y.write("0123456789"), raw.took(), y.write("j"), y.write("\xe9\xe9\xe9\xe9"), raw.took(), y.flush(), raw.took()) for raw in [Buffer()] for y in [TextIOWrapper(raw, U8)]])
for enc in ("test_utf_8", "test_utf_8_slow", "test_ascii", "test_latin_1", "test_utf_16", "test_utf_16_slow", "test_utf_16_le", "test_utf_16_be", "test_utf_32_le", "test_utf_32_be", "test_utf_7"):
    for errors in ("strict", "replace", "backslashreplace", "surrogateescape"):
        t("%s, %s" % (enc, errors), lambda: [([attempt(y.write, s) for s in ("a", "\xe9", "€", "\U0001F600", "\ud800", "\udc80", "b")], y.flush(), bytes(raw.data)) for raw in [Buffer()] for y in [TextIOWrapper(raw, enc, errors)]])
for arg in (b"a", 5, None, ["a"], bytearray(b"a")):
    t("write(%r)" % (arg,), lambda: TextIOWrapper(Buffer(), U8).write(arg))
t("write()", lambda: (attempt(TextIOWrapper(Buffer(), U8).write), attempt(TextIOWrapper(Buffer(), U8).write, "a", "b")))


class S(str):
    def replace(self, *a): return "replaced%r" % (a,)


t("of a class derived from str", lambda: [(y.write(S("a\nb")), y.flush(), bytes(raw.data)) for raw in [Buffer()] for y in [TextIOWrapper(raw, U8, newline="\r\n")]])
t("how many were written", lambda: [(y.write("\U0001F600"), y.write("a\U0001F600\n"), y.write("")) for y in [TextIOWrapper(Buffer(), U8, newline="\r\n")]])
for label, value in (("a str", "s"), ("None", None), ("a bytearray", bytearray(b"b")), ("a memoryview", memoryview(b"m")), ("derived from bytes", type("B", (bytes,), {})(b"db"))):
    codec("test_encoder", "x", _codecs.utf_8_encode, _codecs.utf_8_decode, lambda errors: type("E", (Encoder,), {"encode": lambda self, s: value})())
    _codecs.unregister(known.get)
    _codecs.register(known.get)
    t("an encoder that returns " + label, lambda: [(y.write("a"), y.flush(), bytes(y.buffer.data)) for y in [TextIOWrapper(Buffer(), "test_encoder")]])


class WriteFails(Buffer):
    def write(self, b):
        self.log.append(("write", bytes(b)))
        raise OSError("from write")


t("when sending it on fails", lambda: [(y.write("abc"), attempt(y.flush), raw.took()[3:], y.flush(), raw.took(), y.write("d"), attempt(y.close), raw.took(), y.closed) for raw in [WriteFails()] for y in [TextIOWrapper(raw, U8)]])
t("writelines", lambda: [(y.writelines(["a", "b\n", "c"]), y.flush(), bytes(raw.data), attempt(y.writelines, ["d", b"e"]), attempt(y.writelines, 5)) for raw in [Buffer()] for y in [TextIOWrapper(raw, U8)]])

print("---- reading")
TEXT = "ab\ncd\r\nef\rgh\n\n\r\r\nij"
for newline in (None, "", "\n", "\r", "\r\n"):
    for chunk in (1, 2, 3, 100):
        def go():
            out = []
            for act in (lambda y: y.read(), lambda y: [y.read(1) for i in range(25)], lambda y: [y.read(3) for i in range(9)], lambda y: y.readlines(), lambda y: list(y), lambda y: [y.readline(2) for i in range(14)], lambda y: [y.readline(0), y.readline(1), y.readline(100), y.readline(-1), y.readline(-5), y.read()]):
                y = TextIOWrapper(Buffer(TEXT.encode()), U8, newline=newline)
                y._CHUNK_SIZE = chunk
                out.append((act(y), y.newlines))
            return out
        t("newline=%r, %d at a time" % (newline, chunk), go)
t("what is asked of the buffer", lambda: [(raw.took() and None, y.read(1), raw.took(), y.read(2), raw.took(), y.read(10000), raw.took(), y.read(), raw.took(), y.read(1), raw.took(), y.readline(), raw.took()) for raw in [Buffer(b"abcdef" * 3000)] for y in [TextIOWrapper(raw, U8)]][0][2::2])
t("of one that has no read1", lambda: [(raw.took() and None, y.read(1), raw.took(), y.readline(), raw.took()) for raw in [Buffer(b"ab\ncd", read1=False)] for y in [TextIOWrapper(raw, U8)]])
t("how much, by how many bytes there are to a character", lambda: [(setattr(y, "_CHUNK_SIZE", 4), raw.took() and None, y.read(1), y.read(10), raw.took()) for raw in [Buffer("€".encode() * 50)] for y in [TextIOWrapper(raw, U8)]])
WIDE = "a\xe9€\U0001F600b\n\U0001F601\U0010FFFFc\r\nd"
for enc in ("test_utf_8", "test_utf_16", "test_utf_16_le", "test_utf_32_be", "test_utf_7"):
    for chunk in (1, 2, 3, 5, 100):
        def go():
            data = known[enc][0](WIDE)[0]
            out = []
            for act in (lambda y: y.read(), lambda y: [y.read(1) for i in range(len(WIDE) + 2)], lambda y: [y.read(2) for i in range(8)], lambda y: [y.readline(3) for i in range(6)], lambda y: list(y)):
                y = TextIOWrapper(Buffer(data), enc, newline="")
                y._CHUNK_SIZE = chunk
                out.append(act(y))
            return out
        t("%s, %d at a time" % (enc, chunk), go)
for arg in ("a", 1.5, [1], 2 ** 70, b"1"):
    t("read(%r)" % (arg,), lambda: TextIOWrapper(Buffer(b"abc"), U8).read(arg))
    t("readline(%r)" % (arg,), lambda: TextIOWrapper(Buffer(b"abc"), U8).readline(arg))
t("read(None) and readline(None)", lambda: (TextIOWrapper(Buffer(b"abc"), U8).read(None), attempt(TextIOWrapper(Buffer(b"abc"), U8).readline, None), attempt(TextIOWrapper(Buffer(b"abc"), U8).read, 1, 2)))
for errors in ("strict", "replace", "ignore", "surrogateescape", "backslashreplace"):
    t("what cannot be decoded, %s" % errors, lambda: [(attempt(y.read, 2), attempt(y.read), attempt(y.readline)) for y in [TextIOWrapper(Buffer(b"ab\xffcd\xe2\x82"), U8, errors)]])
for label, value in (("None", None), ("a str", "s"), ("an int", 5), ("a bytearray", bytearray(b"ba\n")), ("a memoryview", memoryview(b"mv\n")), ("a list", [97])):
    class Odd(Buffer):
        def _read1(self, n=-1): return value if not self.log.append(1) and len(self.log) < 3 else b""
        def read(self, n=-1): return value if not self.log.append(1) and len(self.log) < 3 else b""
    t("a buffer that returns " + label, lambda: (attempt(TextIOWrapper(Odd(), U8).read, 1), attempt(TextIOWrapper(Odd(), U8).read), attempt(TextIOWrapper(Odd(), U8).readline), attempt(next, TextIOWrapper(Odd(), U8))))
for label, value in (("bytes", b"b"), ("None", None), ("an int", 5), ("derived from str", S("ds")), ("a line of one", S("ds\n"))):
    for newline in (None, "\n"):
        codec("test_decoder", "x", _codecs.utf_8_encode, _codecs.utf_8_decode, None, lambda errors: type("D", (Decoder,), {"decode": lambda self, b, final=False: value if b else ""})())
        _codecs.unregister(known.get)
        _codecs.register(known.get)
        t("a decoder that returns %s, newline=%r" % (label, newline), lambda: [(r if isinstance(r, str) and ": " in r else (type(r).__name__, r)) for r in (attempt(TextIOWrapper(Buffer(b"a"), "test_decoder", newline=newline).read, 1), attempt(TextIOWrapper(Buffer(b"a"), "test_decoder", newline=newline).read), attempt(TextIOWrapper(Buffer(b"a"), "test_decoder", newline=newline).readline))])
for label, value in (("None", None), ("a list", [b"", 0]), ("one thing", (b"",)), ("three", (b"", 0, 0)), ("a str for the bytes", ("", 0)), ("a bytearray", (bytearray(), 0)), ("a str for the flags", (b"", "f")), ("None for the flags", (b"", None)), ("large flags", (b"", 2 ** 40))):
    codec("test_state", "x", _codecs.utf_8_encode, _codecs.utf_8_decode, None, lambda errors: type("D", (Decoder,), {"getstate": lambda self: value})())
    _codecs.unregister(known.get)
    _codecs.register(known.get)
    t("a decoder whose state is " + label, lambda: [(attempt(y.read, 1), attempt(y.tell)) for y in [TextIOWrapper(Buffer(b"abc"), "test_state", newline="\n")]])

print("---- where it is")
for enc in ("test_utf_8", "test_utf_8_slow", "test_latin_1", "test_utf_16", "test_utf_16_le", "test_utf_32_be", "test_utf_7"):
    for chunk in (1, 2, 3, 7, 100):
        for newline in (None, ""):
            def go():
                data = known[enc][0](WIDE)[0]
                expected = WIDE.replace("\r\n", "\n") if newline is None else WIDE
                cookies, wrong = [], []
                for n in range(len(expected) + 1):
                    y = TextIOWrapper(BytesIO(data), enc, newline=newline)
                    y._CHUNK_SIZE = chunk
                    y.read(n)
                    cookie = y.tell()
                    cookies.append(cookie)
                    rest = y.read()
                    if rest != expected[n:]:
                        wrong.append((n, "after tell", rest))
                    if y.seek(cookie) != cookie or y.read() != expected[n:]:
                        wrong.append((n, "after seek"))
                    z = TextIOWrapper(BytesIO(data), enc, newline=newline)
                    z._CHUNK_SIZE = chunk
                    z.seek(cookie)
                    if z.read() != expected[n:]:
                        wrong.append((n, "in another"))
                return cookies, wrong
            if enc != "test_latin_1":
                t("%s, %d at a time, newline=%r" % (enc, chunk, newline), go)
t("what is asked in telling", lambda: [(setattr(y, "_CHUNK_SIZE", 4), y.read(3), raw.took() and None, y.tell(), raw.took(), y.tell(), y.read(1), y.tell()) for raw in [Buffer("a\xe9€\U0001F600bcdefgh".encode())] for y in [TextIOWrapper(raw, U8)]])
t("what is asked in seeking", lambda: [(setattr(y, "_CHUNK_SIZE", 4), y.read(3), y.tell(), raw.took() and None, y.seek(0), raw.took(), y.seek(1), raw.took(), y.seek(0, 1), raw.took(), y.seek(0, 2), raw.took(), y.read(), y.seek(3), y.read(2)) for raw in [Buffer(b"abcdefgh")] for y in [TextIOWrapper(raw, U8)]])
t("after lines", lambda: [(y.readline(), y.tell(), y.readline(), y.tell(), y.seek(3), y.readline(), y.tell()) for y in [TextIOWrapper(BytesIO(b"ab\ncd\nef"), U8)]])
t("not while it is being gone through", lambda: [(next(y), attempt(y.tell), y.readline(), attempt(y.tell), y.flush(), y.tell(), next(y), attempt(y.tell), attempt(next, y), y.tell(), y.seek(0), next(y), attempt(y.tell), y.seek(0), attempt(y.tell)) for y in [TextIOWrapper(BytesIO(b"ab\ncd\nef\n"), U8)]])
for args in ((0, 3), (0, -1), (1, 1), (-1, 1), (1, 2), (-1, 2), (-1,), (-1, 0), ("a",), (None,), (1.5,), (0, "a"), (0, 1.5), (), (0, 0, 0), (2 ** 64,), (2 ** 168 - 1,), (2 ** 168,), (2 ** 200,), (True,), (0.0, 1), (0.0, 2), (0.0,), (2 ** 63,)):
    t("seek%r" % (args,), lambda: TextIOWrapper(BytesIO(b"abcdef"), U8).seek(*args))
t("a cookie that says more than there is", lambda: [attempt(TextIOWrapper(BytesIO(b"abcdef"), U8).seek, c) for c in (1 << 128, (5 << 128) | (2 << 96), (1 << 128) | (10 << 96), (1 << 160) | (1 << 128) | (6 << 96), 1 << 64, (7 << 64) | 2, (1 << 128) | (1 << 96) | 100)])
t("past the end", lambda: [(y.seek(100), y.read(), y.tell(), y.write("z"), y.flush(), y.buffer.getvalue()) for y in [TextIOWrapper(BytesIO(b"ab"), U8)]])
t("truncate", lambda: [(y.read(2), raw.took() and None, y.truncate(), raw.took(), y.truncate(1), raw.took(), y.truncate(None), attempt(y.truncate, "a"), attempt(y.truncate, 1, 2), bytes(raw.data)) for raw in [Buffer(b"abcdef")] for y in [TextIOWrapper(raw, U8)]])

print("---- reading and writing")
t("write after read", lambda: [(y.read(2), y.write("XY"), y.read(), y.seek(0), y.read(), y.buffer.getvalue()) for y in [TextIOWrapper(BytesIO(b"abcdefgh"), U8)]])
t("having said where", lambda: [(y.read(2), y.seek(y.tell()), y.write("XY"), y.seek(0), y.read()) for y in [TextIOWrapper(BytesIO(b"abcdefgh"), U8)]])
t("read after write", lambda: [(y.write("abc"), y.read(), y.seek(0), y.read(), y.write("d"), y.seek(0), y.read()) for y in [TextIOWrapper(BytesIO(), U8)]])
t("over a BufferedRandom", lambda: [(y.write("h\xe9llo\nw\xf6rld\n"), y.seek(0), y.readline(), y.tell(), y.write("W"), y.seek(0), y.read(), raw.getvalue()) for raw in [BytesIO()] for y in [TextIOWrapper(BufferedRandom(raw), U8)]])
for enc in ("test_utf_16", "test_utf_16_slow"):
    t("the mark at the beginning, " + enc, lambda: [(y.write("a"), y.write("b"), y.flush(), bytes(raw.data), y.seek(0), y.write("c"), y.flush(), bytes(raw.data), y.seek(0, 2), y.write("d"), y.flush(), bytes(raw.data), y.seek(0), y.read(), y.seek(4), y.write("e"), y.seek(0), y.read()) for raw in [Buffer()] for y in [TextIOWrapper(raw, enc)]])
    t("added to what has one, " + enc, lambda: [(raw.seek(0, 2), [(y.write("b"), y.flush()) for y in [TextIOWrapper(raw, enc)]], bytes(raw.data)) for raw in [Buffer(b"\xff\xfea\x00")]])
    t("at the end of nothing, " + enc, lambda: [(y.seek(0, 2), y.write("a"), y.flush(), bytes(raw.data)) for raw in [Buffer()] for y in [TextIOWrapper(raw, enc)]])

print("---- reconfigure")
t("nothing", lambda: [(raw.took() and None, y.write("a"), y.reconfigure(), raw.took(), y.encoding, y.errors, y.line_buffering, y.write_through) for raw in [Buffer()] for y in [TextIOWrapper(raw, U8, "ignore", line_buffering=True)]])
t("the encoding", lambda: [(y.write("\xe9"), y.reconfigure(encoding="test_latin_1"), y.encoding, y.errors, y.write("\xe9"), y.reconfigure(encoding="test_utf_16"), y.write("\xe9"), y.flush(), bytes(raw.data)) for raw in [Buffer()] for y in [TextIOWrapper(raw, U8, "ignore")]])
t("the errors", lambda: [(attempt(y.write, "\xe9"), y.reconfigure(errors="replace"), y.encoding, y.errors, y.write("\xe9"), y.reconfigure(errors="xmlcharrefreplace"), y.write("\xe9"), y.reconfigure(encoding="test_ascii"), y.errors, y.flush(), bytes(raw.data)) for raw in [Buffer()] for y in [TextIOWrapper(raw, "test_ascii")]])
t("the newline", lambda: [(y.write("a\n"), y.reconfigure(newline="\r\n"), y.write("b\n"), y.reconfigure(newline=""), y.write("c\n"), y.reconfigure(newline="\r"), y.write("d\n"), y.reconfigure(newline=None), y.write("e\n"), y.reconfigure(newline="\n"), y.write("f\n"), y.flush(), bytes(raw.data)) for raw in [Buffer()] for y in [TextIOWrapper(raw, U8)]])
t("for reading", lambda: [(y.reconfigure(newline="\r"), y.readline(), attempt(lambda: y.reconfigure(newline="\n")), attempt(lambda: y.reconfigure(encoding=U8)), attempt(lambda: y.reconfigure(errors="ignore")), attempt(lambda: y.reconfigure(newline=None)), y.reconfigure(line_buffering=True), y.line_buffering, y.readline()) for y in [TextIOWrapper(Buffer(b"a\rb\nc\r"), U8)]])
t("how it is sent on", lambda: [(y.reconfigure(line_buffering=True), y.line_buffering, y.reconfigure(write_through=True), y.write_through, y.reconfigure(line_buffering=False, write_through=0), y.line_buffering, y.write_through, y.reconfigure(line_buffering=5, write_through=-1), y.line_buffering, y.write_through, y.reconfigure(line_buffering=None), y.line_buffering) for y in [TextIOWrapper(Buffer(), U8)]])
for kwargs in ({"encoding": 5}, {"encoding": b"a"}, {"encoding": "test_none_such"}, {"encoding": "test_binary"}, {"encoding": "test\0x"}, {"errors": 5}, {"errors": "no such"}, {"errors": "a\0b"}, {"newline": 5}, {"newline": "x"}, {"newline": b"\n"}, {"newline": "\n\0"}, {"newline": "\0"}, {"line_buffering": "a"}, {"line_buffering": 1.5}, {"line_buffering": 2 ** 70},
               {"write_through": "a"}, {"write_through": []}, {"other": 1}):
    t("reconfigure(**%r)" % (kwargs,), lambda: [(y.reconfigure(**kwargs), y.encoding, y.errors) for y in [TextIOWrapper(Buffer(), U8)]])
t("reconfigure(1)", lambda: TextIOWrapper(Buffer(), U8).reconfigure(1))
t("it is as it was if it fails", lambda: [(attempt(lambda: y.reconfigure(encoding="test_none_such", newline="\r\n", line_buffering=True)), y.encoding, y.line_buffering, y.write("a\n"), y.flush(), bytes(raw.data)) for raw in [Buffer()] for y in [TextIOWrapper(raw, U8)]])

print("---- closed, detached, and not made")
t("close", lambda: [(y.write("abc"), raw.took() and None, y.close(), raw.took(), y.closed, raw.closed, y.close(), repr(y)) for raw in [Buffer()] for y in [TextIOWrapper(raw, U8)]])
closed = TextIOWrapper(Buffer(b"abc"), U8)
closed.close()
detached = TextIOWrapper(Buffer(b"abc"), U8)
t("detach", lambda: [(y.write("ab"), raw.took() and None, y.detach() is raw, raw.took(), y.buffer, repr(y), raw.closed) for raw in [Buffer()] for y in [TextIOWrapper(raw, U8)]])
detached.detach()
unmade = TextIOWrapper.__new__(TextIOWrapper)
for label, y in (("closed", closed), ("detached", detached), ("not made", unmade)):
    for name, args in (("read", ()), ("read", (0,)), ("readline", ()), ("readlines", ()), ("write", ("a",)), ("write", ("",)), ("write", (5,)), ("writelines", ([],)), ("flush", ()), ("seek", (0,)), ("tell", ()), ("truncate", ()), ("close", ()), ("detach", ()), ("__next__", ()), ("__iter__", ()), ("__enter__", ()), ("readable", ()),
                       ("writable", ()), ("seekable", ()), ("fileno", ()), ("isatty", ()), ("reconfigure", ()), ("__getstate__", ())):
        t("%s%r of one that is %s" % (name, args, label), lambda: (lambda r: type(r).__name__ if isinstance(r, (TextIOWrapper, Buffer)) else r)(getattr(y, name)(*args)))
    for name in ("closed", "name", "mode", "buffer", "encoding", "errors", "newlines", "line_buffering", "write_through", "_CHUNK_SIZE", "_finalizing"):
        t("%s of one that is %s" % (name, label), lambda: (lambda r: type(r).__name__ if isinstance(r, Buffer) else r)(getattr(y, name)))
    t("repr of one that is " + label, lambda: repr(y))
    t("_CHUNK_SIZE = 1 of one that is " + label, lambda: setattr(y, "_CHUNK_SIZE", 1))
t("object.__new__", lambda: type(object.__new__(TextIOWrapper)).__name__)
t("made again", lambda: [(y.read(1), y.__init__(Buffer(b"xyz"), "test_latin_1", "ignore"), y.read(), y.encoding, y.errors) for y in [TextIOWrapper(Buffer(b"abc"), U8)]])
t("made again, and it fails", lambda: [(attempt(y.__init__, Buffer(), "test_none_such"), attempt(y.read), y.encoding, y.buffer, attempt(y.__init__, Buffer(), U8, None, "x"), attempt(y.read), attempt(lambda: y.errors)) for y in [TextIOWrapper(Buffer(b"abc"), U8)]])


class FlushFails(Buffer):
    def flush(self): raise OSError("from flush")


class CloseFails(Buffer):
    def close(self):
        super().close()
        raise NameError("from close")


class BothFail(FlushFails):
    def close(self):
        _BufferedIOBase.close.__get__(self)() if False else None
        raise NameError("from close")


t("when flushing fails", lambda: [(attempt(y.close), attempt(lambda: y.closed)) for y in [TextIOWrapper(FlushFails(), U8)]])
t("when closing the buffer fails", lambda: [(attempt(y.close), y.closed) for y in [TextIOWrapper(CloseFails(), U8)]])
t("when both do", lambda: attempt(TextIOWrapper(BothFail(), U8).close))
t("with", lambda: [(y.closed, y.__exit__(None, None, None), y.closed) for y in [TextIOWrapper(Buffer(), U8).__enter__()]])
t("__del__", lambda: [(y.write("ab"), y.__del__(), y.closed, bytes(raw.data), y._finalizing) for raw in [Buffer()] for y in [TextIOWrapper(raw, U8)]])
warned = []


class Warns(Buffer):
    def _dealloc_warn(self, source): warned.append(type(source).__name__)


t("what is told when it is let go of", lambda: [(setattr(y, "_finalizing", True), y.close(), warned) for y in [TextIOWrapper(Warns(), U8)]])

print("---- the rest")


class Named(Buffer):
    name = "the name"
    mode = "the mode"
    def fileno(self): return 42
    def isatty(self): return "as the buffer says"


t("what is the buffer's", lambda: [(y.name, attempt(lambda: y.mode), y.fileno(), y.isatty(), repr(y)) for y in [TextIOWrapper(Named(), U8)]])
t("mode", lambda: [(setattr(y, "mode", "r+"), y.mode, repr(y), vars(y)) for y in [TextIOWrapper(Named(), U8)]])
for name in ("name", "closed", "newlines", "errors", "encoding", "buffer", "line_buffering", "write_through"):
    t(name + " cannot be set", lambda: setattr(TextIOWrapper(Buffer(), U8), name, 1))
for value in (1, 100, 0, -1, "a", 1.5, None, 2 ** 70, True):
    t("_CHUNK_SIZE = %r" % (value,), lambda: [(setattr(y, "_CHUNK_SIZE", value), y._CHUNK_SIZE) for y in [TextIOWrapper(Buffer(), U8)]])
t("del _CHUNK_SIZE", lambda: delattr(TextIOWrapper(Buffer(), U8), "_CHUNK_SIZE"))
t("_finalizing", lambda: [(setattr(y, "_finalizing", True), y._finalizing, attempt(setattr, y, "_finalizing", 1), attempt(delattr, y, "_finalizing")) for y in [TextIOWrapper(Buffer(), U8)]])
t("attributes", lambda: [(setattr(y, "a", 1), y.a, y.__dict__) for y in [TextIOWrapper(Buffer(), U8)]])
t("weakly referred to", lambda: [__import__("_weakref").ref(y)() is y for y in [TextIOWrapper(Buffer(), U8)]])


class SelfNamed(Buffer):
    @property
    def name(self): return holder[0]


holder = []
holder.append(TextIOWrapper(SelfNamed(), U8))
t("a name that is itself", lambda: repr(holder[0]))


class Sub(TextIOWrapper):
    def __init__(self, raw, tag):
        super().__init__(raw, U8)
        self.tag = tag

    def readline(self, size=-1):
        return "<" + super().readline(size) + ">" if self.tag else b"not a str"


t("derived from", lambda: [(y.tag, y.read(1), repr(y), isinstance(y, _io._TextIOBase)) for y in [Sub(Buffer(b"abc"), "t")]])
t("next goes by way of its readline", lambda: next(Sub(Buffer(b"a\nb"), "t")))
t("which is to return a str", lambda: next(Sub(Buffer(b"a\nb"), "")))


class ClosedSays(TextIOWrapper):
    closed = True


t("one that says that it is closed", lambda: [(attempt(y.read), attempt(y.write, "a"), attempt(y.flush), attempt(y.tell), attempt(y.readline)) for y in [ClosedSays(Buffer(b"abc"), U8)]])

print("---- at random, against a str")
state = [1234567]


def random(n):
    v = state[0]
    v ^= (v << 13) & 0xFFFFFFFF
    v ^= v >> 17
    v ^= (v << 5) & 0xFFFFFFFF
    state[0] = v
    return v % n


ALPHABET = ["a", "b", "c", "\n", "\n", "\r", "\r\n", "\xe9", "€", "\U0001F600", " "]
for enc in ("test_utf_8", "test_utf_8_slow", "test_utf_16", "test_utf_16_le", "test_utf_32_be", "test_utf_7"):
    for newline in (None, "", "\n", "\r", "\r\n"):
        for chunk in (1, 3, 8, 8192):
            source = "".join(ALPHABET[random(len(ALPHABET))] for i in range(120))
            model = source.replace("\r\n", "\n").replace("\r", "\n") if newline is None else source
            y = TextIOWrapper(BytesIO(known[enc][0](source)[0]), enc, newline=newline)
            y._CHUNK_SIZE = chunk
            at, marks, wrong, digest = 0, [(0, 0)], [], 0
            for step in range(250):
                what = random(6)
                if what == 0:
                    n = random(7)
                    got, want = y.read(n), model[at:at + n]
                    # What may be half of a line ending is kept back, so less may come, and not a "\r" without its "\n".
                    if got != want:
                        wrong.append((step, "read", got, want))
                    at += len(got)
                elif what == 1:
                    got = y.readline()
                    rest = model[at:]
                    if newline in (None, "\n"):
                        ends = [rest.find("\n") + 1]
                    elif newline == "":
                        ends = [i + (2 if rest[i:i + 2] == "\r\n" else 1) for i in (rest.find("\n"), rest.find("\r")) if i >= 0]
                    else:
                        ends = [rest.find(newline) + len(newline) if rest.find(newline) >= 0 else 0]
                    end = min([e for e in ends if e > 0] or [len(rest)])
                    if got != rest[:end]:
                        wrong.append((step, "readline", got, rest[:end]))
                    at += len(got)
                elif what == 2:
                    # Working out where it is can come to giving the decoder what begins in the middle of a character.
                    try:
                        cookie = y.tell()
                    except UnicodeDecodeError as e:
                        digest = (digest * 31 + e.start * 7 + e.end) % 1000000007
                        continue
                    marks.append((cookie, at))
                    digest = (digest * 31 + cookie) % 1000000007
                elif what == 3:
                    cookie, at = marks[random(len(marks))]
                    y.seek(cookie)
                elif what == 4 and random(4) == 0:
                    got = y.read()
                    if got != model[at:]:
                        wrong.append((step, "read()"))
                    at = len(model)
                elif what == 5:
                    n = random(4)
                    got = y.readline(n)
                    if not model[at:].startswith(got) or len(got) > n:
                        wrong.append((step, "readline(n)", got))
                    at += len(got)
            if wrong or chunk == 3:
                t("%s, newline=%r, %d at a time" % (enc, newline, chunk), lambda: (wrong[:2], len(marks), digest))
_codecs.unregister(known.get)
