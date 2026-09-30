# The codecs for Chinese, Japanese and Korean: _multibytecodec, which runs them, and _codecs_cn, _codecs_hk, _codecs_iso2022, _codecs_jp, _codecs_kr and _codecs_tw, which have them.
import _multibytecodec
import codecs
import hashlib
import io
import os
import re
import sys
import tempfile
import warnings

warnings.simplefilter("ignore", DeprecationWarning)  # codecs.open()

ENCODINGS = ("big5", "big5hkscs", "cp932", "cp949", "cp950", "euc_jis_2004", "euc_jisx0213", "euc_jp", "euc_kr", "gb18030", "gb2312", "gbk", "hz", "iso2022_jp", "iso2022_jp_1", "iso2022_jp_2", "iso2022_jp_2004",
             "iso2022_jp_3", "iso2022_jp_ext", "iso2022_kr", "johab", "shift_jis", "shift_jis_2004", "shift_jisx0213")
MODULES = ("_codecs_cn", "_codecs_hk", "_codecs_iso2022", "_codecs_jp", "_codecs_kr", "_codecs_tw")


def attempt(label, f):
    try:
        print("   ", label, "=>", ascii(f()))
    except BaseException as e:
        print("   ", label, "=>", type(e).__name__, ascii(str(e)))


def brief(data):
    if isinstance(data, str):
        data = data.encode("utf-8", "surrogatepass")
    return len(data), hashlib.sha256(data).hexdigest()[:12]


def plain(text):
    "With no addresses"
    return re.sub(r" at 0x[0-9a-f]+", " at 0x", text)


print("---- which modules come with which")
before = set(sys.modules)
for encoding in ENCODINGS:
    codecs.lookup(encoding)
    print("   ", encoding, sorted(n for n in set(sys.modules) - before if n.startswith("_codecs_") or n == "_multibytecodec"))
    before = set(sys.modules)

print("---- what there is")
print(_multibytecodec.__spec__.origin, _multibytecodec.__doc__, sorted(n for n in vars(_multibytecodec) if not (n.startswith("__") and n.endswith("__"))))
print(type(_multibytecodec.__create_codec).__name__, _multibytecodec.__create_codec.__text_signature__, _multibytecodec.__create_codec.__doc__)
a_codec = __import__("_codecs_tw").getcodec("big5")
for c in (type(a_codec), _multibytecodec.MultibyteIncrementalEncoder, _multibytecodec.MultibyteIncrementalDecoder, _multibytecodec.MultibyteStreamReader, _multibytecodec.MultibyteStreamWriter):
    print(c.__name__, c.__module__, c.__qualname__, [b.__name__ for b in c.__bases__], hex(c.__flags__ & 0x7F80), c.__doc__, getattr(c, "__text_signature__", None))
    for n, v in sorted(vars(c).items()):
        print("   ", n, type(v).__name__, getattr(v, "__text_signature__", None), ascii((getattr(v, "__doc__", None) or "")[:44]) if not isinstance(v, str) else v)
for name in MODULES:
    module = __import__(name)
    print(name, module.__spec__.origin, module.__doc__, sorted(n for n in vars(module) if not (n.startswith("__") and n.endswith("__"))), type(module.getcodec).__name__, module.getcodec.__text_signature__, ascii(module.getcodec.__doc__))
print(plain(repr(__import__("_codecs_tw").__map_big5)), type(__import__("_codecs_tw").__map_big5).__name__, plain(repr(a_codec)))
for label, f in (("getcodec()", lambda: __import__("_codecs_tw").getcodec()), ("of a name that there is not", lambda: __import__("_codecs_tw").getcodec("nothing")), ("of another module's", lambda: __import__("_codecs_tw").getcodec("gbk")), ("of bytes", lambda: __import__("_codecs_tw").getcodec(b"big5")),
                 ("of half a character", lambda: __import__("_codecs_tw").getcodec("\ud800")), ("of BIG5", lambda: __import__("_codecs_tw").getcodec("BIG5")), ("of what has a null in it", lambda: type(__import__("_codecs_tw").getcodec("big5\0x")).__name__), ("of what begins with one", lambda: __import__("_codecs_tw").getcodec("\0big5")), ("of a str of another class", lambda: type(__import__("_codecs_tw").getcodec(type("S", (str,), {})("big5"))).__name__),
                 ("two are not the same", lambda: __import__("_codecs_tw").getcodec("big5") is __import__("_codecs_tw").getcodec("big5")), ("__create_codec()", lambda: _multibytecodec.__create_codec()), ("of what is not a capsule", lambda: _multibytecodec.__create_codec(5)),
                 ("of a capsule of another kind", lambda: _multibytecodec.__create_codec(__import__("_codecs_tw").__map_big5)), ("MultibyteCodec()", lambda: type(a_codec)()), ("derived from it", lambda: type("D", (type(a_codec),), {})),
                 ("an attribute of it", lambda: setattr(a_codec, "x", 1)), ("an attribute of the class", lambda: setattr(type(a_codec), "x", 1)), ("an encoder with no codec", lambda: _multibytecodec.MultibyteIncrementalEncoder()),
                 ("with one that is not one", lambda: type("E", (_multibytecodec.MultibyteIncrementalEncoder,), {"codec": 5})()), ("a reader with no codec", lambda: _multibytecodec.MultibyteStreamReader(io.BytesIO()))):
    attempt(label, f)

print("---- every character, and every two bytes")
EVERYTHING = "".join(map(chr, range(0xD800))) + "".join(map(chr, range(0xE000, 0x30000))) + "".join(map(chr, range(0xE0000, 0xE0200)))
PAIRS = bytes(b for first in range(256) for second in range(256) for b in (first, second))
NOISE = hashlib.shake_128(b"noise").digest(60000)
HIGH = bytes(b | 0x80 for b in hashlib.shake_128(b"high").digest(60000))
for encoding in ENCODINGS:
    encoded = EVERYTHING.encode(encoding, "ignore")
    print("   ", encoding, brief(encoded), brief(encoded.decode(encoding, "replace")), brief(EVERYTHING[:0x3000].encode(encoding, "replace")), brief(PAIRS.decode(encoding, "replace")), brief(PAIRS.decode(encoding, "ignore")), brief(bytes(range(256)).decode(encoding, "replace")),
          brief(NOISE.decode(encoding, "replace")), brief(HIGH.decode(encoding, "replace")), brief(HIGH.decode(encoding, "backslashreplace")))
one_at_a_time = {}
for encoding in ("big5hkscs", "euc_jis_2004", "gb18030", "iso2022_jp_2004", "shift_jisx0213", "johab", "hz", "iso2022_kr"):
    h = hashlib.sha256()
    for c in range(0x2E00, 0xA000, 3):
        try:
            h.update(chr(c).encode(encoding))
        except UnicodeEncodeError as e:
            h.update(b"!%d-%d" % (e.start, e.end))
    one_at_a_time[encoding] = h.hexdigest()[:12]
print(one_at_a_time)
print(brief("".join(map(chr, range(0x10000, 0x110000, 7))).encode("gb18030")), brief(bytes(b for a in range(0x81, 0xFF, 5) for b2 in range(0x30, 0x3A) for c in range(0x81, 0xFF, 7) for d in range(0x30, 0x3A, 3) for b in (a, b2, c, d)).decode("gb18030", "replace")))
print([("\u304b\u309a" + tail).encode(e, "replace") for e in ("euc_jis_2004", "shift_jis_2004", "iso2022_jp_2004") for tail in ("", "x", "\u309a")], ["\u00ca\u0304\u00ca\u030c\u00ca".encode("big5hkscs"), ascii(b"\x88\x62\x88\x64\x88\x66".decode("big5hkscs"))])

SAMPLES = {
    "big5": "\u4e2d\u6587 abc \u6e2c\u8a66", "big5hkscs": "\u4e2d\u6587 \u00ca\u0304 \U00020021", "cp932": "\u65e5\u672c\u8a9e \uff76\uff85 \u2460", "cp949": "\ud55c\uae00 \ub620\ubc29", "cp950": "\u4e2d\u6587 \u20ac", "euc_jis_2004": "\u65e5\u672c \u304b\u309a \U00020089",
    "euc_jisx0213": "\u65e5\u672c \u304b\u309a", "euc_jp": "\u65e5\u672c\u8a9e \uff76 \u4e02", "euc_kr": "\ud55c\uae00 abc", "gb18030": "\u4e2d\u6587 \u20ac \U0001f600 \u00e9", "gb2312": "\u4e2d\u6587 abc", "gbk": "\u4e2d\u6587 \u9ed1", "hz": "ab \u4e2d\u6587 ~ cd \u6c49",
    "iso2022_jp": "abc \u65e5\u672c def \u8a9e", "iso2022_jp_1": "\u65e5\u672c \u4e02 x", "iso2022_jp_2": "\u65e5\u672c \ud55c \u4e2d \u00e9 \u03b1", "iso2022_jp_2004": "\u65e5 \u304b\u309a \U00020089", "iso2022_jp_3": "\u65e5\u672c \u304b\u309a", "iso2022_jp_ext": "\u65e5 \uff76 \u4e02",
    "iso2022_kr": "ab \ud55c\uae00 cd", "johab": "\ud55c\uae00 \u3131", "shift_jis": "\u65e5\u672c\u8a9e \uff76\uff85 \\~", "shift_jis_2004": "\u65e5 \u304b\u309a \U00020089", "shift_jisx0213": "\u65e5\u672c \u304b\u309a",
}

print("---- a piece at a time, cut everywhere")
for encoding, text in SAMPLES.items():
    whole = text.encode(encoding)
    wrong = []
    states = set()
    for cut in range(len(whole) + 1):
        d = codecs.getincrementaldecoder(encoding)()
        first = d.decode(whole[:cut])
        state = d.getstate()
        states.add(state)
        other = codecs.getincrementaldecoder(encoding)()
        other.setstate(state)
        if first + d.decode(whole[cut:], True) != text or first + other.decode(whole[cut:], True) != text:
            wrong.append(cut)
    for cut in range(len(text) + 1):
        e = codecs.getincrementalencoder(encoding)()
        first = e.encode(text[:cut])
        state = e.getstate()
        states.add(state)
        other = codecs.getincrementalencoder(encoding)()
        other.setstate(state)
        if first + e.encode(text[cut:], True) != whole or first + other.encode(text[cut:], True) != whole:
            wrong.append(-cut)
    d = codecs.getincrementaldecoder(encoding)()
    e = codecs.getincrementalencoder(encoding)()
    print("   ", encoding, whole, wrong, brief(repr(sorted(states, key=repr))), "".join(d.decode(bytes([b])) for b in whole) + d.decode(b"", True) == text, b"".join(e.encode(c) for c in text) + e.encode("", True) == whole)

print("---- what cannot be encoded or decoded")


def about(f):
    try:
        return f()
    except UnicodeError as e:
        return type(e).__name__, e.encoding, e.object if len(e.object) < 20 else len(e.object), e.start, e.end, e.reason


for encoding in ENCODINGS:
    print("   ", encoding, ascii(about(lambda: "a\u0e01b".encode(encoding))), ascii(about(lambda: "ab\U0010ffff".encode(encoding))), ascii(about(lambda: "\ud800".encode(encoding))), about(lambda: b"a\xffb".decode(encoding)), about(lambda: b"ab\x81".decode(encoding)),
          about(lambda: b"\x81\x20".decode(encoding)), about(lambda: b"\x1b$".decode(encoding)), about(lambda: b"\x1b$Zab".decode(encoding)), about(lambda: b"~".decode(encoding)), about(lambda: b"~{!".decode(encoding)), about(lambda: b"\x0e\x21".decode(encoding)))
for handler in ("ignore", "replace", "backslashreplace", "xmlcharrefreplace", "namereplace", "surrogateescape", "surrogatepass", "strict", "nothing of the name", "", "a\0b"):
    print("   ", ascii(handler), [ascii(about(lambda: attempt_value(lambda: "a\u0e01\U0001f600b".encode(e, handler)))) for e in ("gbk", "iso2022_jp", "hz")] if False else "", end="")
    for e in ("gbk", "iso2022_jp", "hz"):
        for f in (lambda: "a\u0e01\U0001f600\u4e2db".encode(e, handler), lambda: b"a\xff\x81b\x80".decode(e, handler)):
            try:
                print("", ascii(f()), end="")
            except Exception as x:
                print("", type(x).__name__, end="")
    print()

calls = []


def handler_that(result):
    def handle(e):
        calls.append((type(e).__name__, e.start, e.end, e.reason))
        return result(e) if callable(result) else result
    return handle


for name, result in (("str", ("<\u4e2d>", 2)), ("bytes", (b"<!>", 2)), ("str that cannot be", ("\u0e01", 2)), ("empty", ("", 2)), ("from the end", ("?", -1)), ("at the end", ("?", 4)), ("past the end", ("?", 5)), ("before the beginning", ("?", -5)), ("back to the beginning", lambda e: ("?", 0) if len(calls) < 3 else ("!", 4)),
                     ("2 ** 70", ("?", 2 ** 70)), ("-2 ** 70", ("?", -2 ** 70)), ("True", lambda e: ("?", True) if len(calls) < 3 else ("!", 4)), ("a float", ("?", 2.0)), ("a list", ["?", 2]), ("three", ("?", 2, 3)), ("one", ("?",)), ("None", None), ("an int first", (5, 2)), ("a bytearray", (bytearray(b"?"), 2)),
                     ("a tuple of another class", type("T", (tuple,), {})(("?", 2))), ("a str of another class", (type("S", (str,), {})("?"), 2)), ("an int of another class", ("?", type("I", (int,), {})(2))), ("what has __index__", ("?", type("X", (), {"__index__": lambda s: 2})())),
                     ("it raises", lambda e: 1 / 0), ("it raises what it was given", lambda e: (_ for _ in ()).throw(e)), ("it changes where", lambda e: (setattr(e, "start", 0), setattr(e, "end", 1), ("?", 3))[-1]), ("it changes what", lambda e: (setattr(e, "object", e.object * 2), ("?", 2))[-1])):
    codecs.register_error("test." + name, handler_that(result))
    for label, f in (("encode", lambda: "a\u0e01bc".encode("gbk", "test." + name)), ("decode", lambda: b"a\xffbc".decode("gbk", "test." + name)), ("encode, iso2022", lambda: "\u65e5\u0e01\u672cc".encode("iso2022_jp", "test." + name))):
        calls.clear()
        attempt("%s: %s" % (name, label), f)
        print("       ", calls[:6])
seen = []
codecs.register_error("test.same", lambda e: (seen.append(id(e)), ("?", e.end))[-1])
print("\u0e01a\u0e02b\u0e03".encode("big5", "test.same"), len(set(seen)), b"\xffa\xffb\xff".decode("big5", "test.same"), len(set(seen)))
changed = bytearray(b"a\xffbc")
codecs.register_error("test.changes", lambda e: (changed.__setitem__(slice(0, 3), b"xyz"), ("?", e.end))[-1])
attempt("what is being decoded is changed meanwhile", lambda: (codecs.decode(changed, "gbk", "test.changes"), changed))
changed = bytearray(b"a\xffbc")
attempt("a piece at a time", lambda: (codecs.getincrementaldecoder("gbk")("test.changes").decode(changed), changed))
changed = bytearray(b"\xffbc")
attempt("with something kept", lambda: (lambda d: (d.decode(b"\x81"), d.decode(changed), changed))(codecs.getincrementaldecoder("gbk")("test.changes")))
try:
    import _javascript
except ImportError:
    pass
else:
    # CPython will not have it made longer or shorter while it is looked at. Nobody is counted here.
    for change in (lambda: changed.extend(b"more" * 1000), lambda: changed.clear(), lambda: changed.__delitem__(slice(1, None))):
        changed = bytearray(b"a\xffbc")
        codecs.register_error("test.resizes", lambda e: (change(), ("?", e.end))[-1])
        for f in (lambda: codecs.decode(changed, "gbk", "test.resizes"), lambda: codecs.getincrementaldecoder("gbk")("test.resizes").decode(changed, True)):
            changed[:] = b"a\xffbc"
            try:
                assert isinstance(f(), str)
            except IndexError:
                pass

print("---- MultibyteCodec.encode() and decode()")
gbk = __import__("_codecs_cn").getcodec("gbk")
for label, f in (("encode", lambda: gbk.encode("\u4e2da")), ("decode", lambda: gbk.decode(b"\xd6\xd0a")), ("nothing", lambda: (gbk.encode(""), gbk.decode(b""))), ("by name", lambda: (gbk.encode(input="a", errors="strict"), gbk.decode(input=b"a", errors=None))), ("encode()", lambda: gbk.encode()), ("decode()", lambda: gbk.decode()),
                 ("encode(5)", lambda: gbk.encode(5)), ("encode(None)", lambda: gbk.encode(None)), ("encode(bytes)", lambda: gbk.encode(b"ab")), ("of what has __str__", lambda: gbk.encode(type("O", (), {"__str__": lambda s: "\u4e2d"})())), ("whose __str__ raises", lambda: gbk.encode(type("O", (), {"__str__": lambda s: 1 / 0})())),
                 ("whose __str__ gives no str", lambda: gbk.encode(type("O", (), {"__str__": lambda s: 5})())), ("a str of another class", lambda: gbk.encode(type("S", (str,), {"__str__": lambda s: "no"})("\u4e2d"))), ("decode('a')", lambda: gbk.decode("a")), ("decode(5)", lambda: gbk.decode(5)),
                 ("decode(bytearray)", lambda: gbk.decode(bytearray(b"\xd6\xd0"))), ("decode(memoryview)", lambda: gbk.decode(memoryview(b"x\xd6\xd0")[1:])), ("decode(every other byte)", lambda: gbk.decode(memoryview(b"\xd6x\xd0x")[::2])), ("errors=5", lambda: gbk.encode("a", 5)), ("errors=b'strict'", lambda: gbk.decode(b"a", b"strict")),
                 ("errors with a null", lambda: gbk.encode("a", "a\0b")), ("errors of half a character", lambda: gbk.encode("a", "\ud800")), ("errors that there are not, and no need", lambda: gbk.encode("a", "nothing")), ("and need", lambda: gbk.encode("\u0e01", "nothing")), ("three", lambda: gbk.encode("a", "strict", 1)),
                 ("a name that there is not", lambda: gbk.encode("a", nothing=1)), ("how many characters, not units", lambda: __import__("_codecs_cn").getcodec("gb18030").encode("\U0001f600a")), ("half a character in it", lambda: gbk.encode("a\udc80", "surrogateescape"))):
    attempt(label, f)

print("---- the four that keep something")
Encoder, Decoder = codecs.getincrementalencoder("iso2022_jp"), codecs.getincrementaldecoder("iso2022_jp")
Reader, Writer = codecs.getreader("iso2022_jp"), codecs.getwriter("iso2022_jp")
print([[b.__name__ for b in c.__mro__] for c in (Encoder, Decoder, Reader, Writer)])
for label, f in (("errors", lambda: [c().errors for c in (Encoder, Decoder)] + [c(io.BytesIO()).errors for c in (Reader, Writer)]), ("given", lambda: [Encoder("ignore").errors, Decoder(errors="replace").errors, Reader(io.BytesIO(), "x").errors, Writer(stream=io.BytesIO(), errors="y").errors]),
                 ("None", lambda: Encoder(None)), ("5", lambda: Decoder(5)), ("a null in it", lambda: Encoder("a\0b")), ("half a character", lambda: Encoder("\ud800")), ("two", lambda: Encoder("a", "b")), ("a name that there is not", lambda: Decoder(nothing=1)), ("Reader()", lambda: Reader()),
                 ("Reader(s, None)", lambda: Reader(io.BytesIO(), None)), ("Writer(s, 5)", lambda: Writer(io.BytesIO(), 5)), ("three", lambda: Writer(io.BytesIO(), "a", "b")), ("a stream that is anything", lambda: (Reader(5).stream, Writer(None).stream)),
                 ("__init__ takes anything", lambda: (Encoder().__init__(1, 2, x=3), _multibytecodec.MultibyteStreamReader.__init__(Reader(io.BytesIO()), 1, 2, 3)))):
    attempt(label, f)
e = Encoder()
for label, f in (("errors = 'ignore'", lambda: (setattr(e, "errors", "ignore"), e.errors, e.encode("\u0e01a"))), ("= 'mine'", lambda: (setattr(e, "errors", "mine"), e.errors, e.errors is e.errors)), ("= 5", lambda: setattr(e, "errors", 5)), ("= None", lambda: setattr(e, "errors", None)), ("del", lambda: delattr(e, "errors")),
                 ("= a null in it", lambda: (setattr(e, "errors", "ign\0ore"), e.errors)), ("= 'ignore' and a null", lambda: (setattr(e, "errors", "ignore\0x"), e.errors)), ("= half a character", lambda: setattr(e, "errors", "\ud800")), ("= a str of another class", lambda: (setattr(e, "errors", type("S", (str,), {})("strict")), type(e.errors).__name__)),
                 ("stream = 1", lambda: setattr(Reader(io.BytesIO()), "stream", 1)), ("an attribute", lambda: (setattr(e, "x", 1), e.x)), ("of one that is not derived", lambda: setattr(type("E", (), {}), "x", 1))):
    attempt(label, f)

print("---- MultibyteIncrementalEncoder")
e = Encoder()
print(e.getstate(), e.encode("a"), e.getstate(), e.encode("\u65e5"), e.getstate(), e.encode("b"), e.getstate(), e.encode("\u65e5"), e.encode("", True), e.getstate(), e.encode("\u65e5"), e.reset(), e.getstate(), e.encode("a"), e.encode(input="\u65e5", final=True))
e = codecs.getincrementalencoder("euc_jis_2004")()
print(e.encode("\u304b"), e.getstate(), e.encode("\u309a"), e.getstate(), e.encode("\u304b"), e.encode("x"), e.encode("\u304b"), e.encode("", True), e.encode("\u304b"), e.reset(), e.encode("x"), e.encode("\u304b"), e.getstate(), e.setstate(0), about(lambda: e.encode("\u309a")))
for label, f in (("encode()", lambda: e.encode()), ("encode(5)", lambda: e.encode(5)), ("encode(bytes)", lambda: e.encode(b"a")), ("final whatever is true", lambda: e.encode("\u304b", [1])), ("three", lambda: e.encode("a", True, 1)), ("strict, and then again", lambda: [e.encode("\u304b"), about(lambda: e.encode("\u0e01")), e.getstate(), e.encode("\u309a")]),
                 ("setstate()", lambda: e.setstate()), ("setstate('a')", lambda: e.setstate("a")), ("setstate(1.0)", lambda: e.setstate(1.0)), ("setstate(True)", lambda: (e.setstate(True), e.getstate())), ("setstate(-1)", lambda: e.setstate(-1)), ("setstate(2 ** 136)", lambda: e.setstate(2 ** 136)),
                 ("setstate(2 ** 136 - 1)", lambda: e.setstate(2 ** 136 - 1)), ("setstate(9)", lambda: e.setstate(9)), ("setstate(8, of nothing)", lambda: (e.setstate(8), e.getstate())), ("what is not UTF-8", lambda: e.setstate(1 | 0xFF << 8)), ("half of something", lambda: e.setstate(1 | 0xE3 << 8)),
                 ("three characters", lambda: (e.setstate(3 | int.from_bytes(b"abc", "little") << 8), e.getstate(), e.encode("d"))), ("eight of them", lambda: (e.setstate(8 | int.from_bytes(b"abcdefgh", "little") << 8), e.getstate(), e.encode("", True))), ("an int of another class", lambda: (e.setstate(type("I", (int,), {})(0)), e.getstate())),
                 ("getstate(1)", lambda: e.getstate(1)), ("reset(1)", lambda: e.reset(1))):
    attempt(label, f)

print("---- MultibyteIncrementalDecoder")
d = Decoder()
print(d.getstate(), d.decode(b"a"), d.decode(b"\x1b"), d.getstate(), d.decode(b"$"), d.getstate(), d.decode(b"B"), d.getstate(), d.decode(b"F"), d.getstate(), ascii(d.decode(b"|")), d.getstate(), d.reset(), d.getstate(), d.decode(b"F|"), d.decode(input=b"", final=True))
for label, f in (("cut short at the end", lambda: about(lambda: Decoder().decode(b"\x1b$BF", True))), ("and what was kept is kept", lambda: (lambda d: [d.decode(b"\x1b$BF"), about(lambda: d.decode(b"", True)), d.getstate(), ascii(d.decode(b"|"))])(Decoder())), ("ignored", lambda: Decoder("ignore").decode(b"\x1b$BF", True)),
                 ("replaced", lambda: Decoder("replace").decode(b"\x1b$BF", True)), ("decode()", lambda: d.decode()), ("decode('a')", lambda: d.decode("a")), ("decode(bytearray)", lambda: d.decode(bytearray(b"a"))), ("three", lambda: d.decode(b"a", True, 1)), ("setstate()", lambda: d.setstate()), ("setstate(5)", lambda: d.setstate(5)),
                 ("setstate([])", lambda: d.setstate([b"", 0])), ("setstate(())", lambda: d.setstate(())), ("one", lambda: d.setstate((b"",))), ("three", lambda: d.setstate((b"", 0, 0))), ("a str", lambda: d.setstate(("", 0))), ("a bytearray", lambda: d.setstate((bytearray(), 0))), ("a float", lambda: d.setstate((b"", 0.0))),
                 ("-1", lambda: d.setstate((b"", -1))), ("2 ** 64", lambda: d.setstate((b"", 2 ** 64))), ("2 ** 64 - 1", lambda: (d.setstate((b"", 2 ** 64 - 1)), d.getstate())), ("eight kept", lambda: (d.setstate((b"12345678", 0)), d.getstate())), ("nine", lambda: about(lambda: d.setstate((b"123456789", 0)))),
                 ("True", lambda: (d.setstate((b"", True)), d.getstate())), ("a tuple of another class", lambda: (d.setstate(type("T", (tuple,), {})((b"a", 0))), d.getstate())), ("bytes of another class", lambda: (d.setstate((type("B", (bytes,), {})(b"a"), 0)), d.getstate())),
                 ("more kept than there is room for", lambda: about(lambda: (lambda d: [d.setstate((b"\x1b\x1b\x1b\x1b\x1b\x1b\x1b", 0)), d.decode(b"\x1b\x1b\x1b")])(codecs.getincrementaldecoder("gb18030")()))), ("getstate(1)", lambda: d.getstate(1)), ("reset(1)", lambda: d.reset(1))):
    attempt(label, f)
d = codecs.getincrementaldecoder("gb18030")()
print([ascii(d.decode(bytes([b]))) for b in "\U0001f600\u00e9\u4e2da".encode("gb18030")], d.getstate())

print("---- MultibyteStreamReader")
text = "abc \u65e5\u672c\u8a9e\ndef \u8a9e\n\nlast \u65e5"
data = text.encode("iso2022_jp")
for label, f in (("read()", lambda: Reader(io.BytesIO(data)).read()), ("read(None)", lambda: Reader(io.BytesIO(data)).read(None)), ("read(-1)", lambda: Reader(io.BytesIO(data)).read(-1)), ("read(-5)", lambda: Reader(io.BytesIO(data)).read(-5)), ("read(0)", lambda: Reader(io.BytesIO(data)).read(0)),
                 ("read(1) over and over", lambda: (lambda r: [r.read(1) for _ in range(len(data) + 2)])(Reader(io.BytesIO(data)))), ("read(5)", lambda: (lambda r: [r.read(5) for _ in range(9)])(Reader(io.BytesIO(data)))), ("readline()", lambda: (lambda r: [r.readline() for _ in range(6)])(Reader(io.BytesIO(data)))),
                 ("readline(3)", lambda: (lambda r: [r.readline(3) for _ in range(14)])(Reader(io.BytesIO(data)))), ("readlines()", lambda: Reader(io.BytesIO(data)).readlines()), ("readlines(6)", lambda: (lambda r: [r.readlines(6) for _ in range(8)])(Reader(io.BytesIO(data)))), ("readlines(0)", lambda: Reader(io.BytesIO(data)).readlines(0)),
                 ("gone through", lambda: list(Reader(io.BytesIO(data)))), ("read('a')", lambda: Reader(io.BytesIO(data)).read("a")), ("read(1.0)", lambda: Reader(io.BytesIO(data)).read(1.0)), ("read(True)", lambda: Reader(io.BytesIO(data)).read(True)), ("read(2 ** 70)", lambda: Reader(io.BytesIO(data)).read(2 ** 70)),
                 ("read(2 ** 40)", lambda: Reader(io.BytesIO(data)).read(2 ** 40)), ("read(1, 2)", lambda: Reader(io.BytesIO(data)).read(1, 2)), ("read(sizeobj=1)", lambda: Reader(io.BytesIO(data)).read(sizeobj=1)), ("readline('a')", lambda: Reader(io.BytesIO(data)).readline("a")), ("readlines('a')", lambda: Reader(io.BytesIO(data)).readlines("a")),
                 ("cut short", lambda: about(lambda: Reader(io.BytesIO(data[:-4])).read())), ("cut short, a little at a time", lambda: about(lambda: (lambda r: [r.read(4) for _ in range(12)])(Reader(io.BytesIO(data[:8]))))), ("cut short and replaced", lambda: Reader(io.BytesIO(data[:8]), "replace").read()),
                 ("reset()", lambda: (lambda r: [r.read(7), r.reset(), r.read(4)])(Reader(io.BytesIO(data)))), ("a stream with no read", lambda: Reader(5).read()), ("that gives a str", lambda: Reader(io.StringIO("a")).read()), ("that gives a bytearray", lambda: Reader(type("S", (), {"read": lambda s, *a: bytearray(b"a")})()).read()),
                 ("that gives None", lambda: Reader(type("S", (), {"read": lambda s, *a: None})()).read()), ("that raises", lambda: Reader(type("S", (), {"read": lambda s, *a: 1 / 0})()).read()), ("what it is asked for", lambda: (lambda asked: (Reader(type("S", (), {"read": lambda s, *a: asked.append(a) or b"", "readline": lambda s, *a: asked.append(("line",) + a) or b""})()), asked))([])[1]),
                 ("bytes of another class", lambda: Reader(type("S", (), {"read": lambda s, *a: type("B", (bytes,), {})(b"ab")})()).read())):
    attempt(label, f)
asked = []
stream = type("S", (), {"read": lambda s, *a: asked.append(("read",) + a) or b"", "readline": lambda s, *a: asked.append(("readline",) + a) or b""})()
r = Reader(stream)
r.read(), r.read(7), r.readline(), r.readline(3), r.readlines(), r.readlines(9), r.read(2 ** 31 + 5)
print(asked)

print("---- MultibyteStreamWriter")


def written(f, encoding="iso2022_jp", errors="strict"):
    out = io.BytesIO()
    w = codecs.getwriter(encoding)(out, errors)
    result = f(w)
    return out.getvalue(), result


for label, f in (("write()", lambda: written(lambda w: [w.write("a\u65e5"), w.write("\u672c"), w.write("b")])), ("and reset()", lambda: written(lambda w: [w.write("a\u65e5"), w.reset(), w.write("\u672c"), w.reset(), w.reset()])), ("writelines()", lambda: written(lambda w: w.writelines(["a", "\u65e5", "b"]))),
                 ("a tuple", lambda: written(lambda w: w.writelines(("\u65e5", "\u672c")))), ("a str", lambda: written(lambda w: w.writelines("a\u65e5"))), ("none", lambda: written(lambda w: w.writelines([]))), ("what is gone through and no more", lambda: written(lambda w: w.writelines(iter(["a"])))), ("a set", lambda: written(lambda w: w.writelines({"a"}))),
                 ("a dict", lambda: written(lambda w: w.writelines({0: "a"}))), ("5", lambda: written(lambda w: w.writelines(5))), ("with what is not a str in it", lambda: written(lambda w: w.writelines(["a", 5, None]))), ("write(5)", lambda: written(lambda w: w.write(5))), ("write(bytes)", lambda: written(lambda w: w.write(b"a"))),
                 ("write()", lambda: written(lambda w: w.write())), ("write('a', 'b')", lambda: written(lambda w: w.write("a", "b"))), ("write(strobj='a')", lambda: written(lambda w: w.write(strobj="a"))), ("reset(1)", lambda: written(lambda w: w.reset(1))), ("what cannot be", lambda: about(lambda: written(lambda w: w.write("a\u0e01")))),
                 ("ignored", lambda: written(lambda w: w.write("a\u0e01b"), errors="ignore")), ("something kept", lambda: written(lambda w: [w.write("\u304b"), w.write("\u309a"), w.write("\u304b"), w.reset(), w.write("\u304b"), w.write("x")], "euc_jis_2004")),
                 ("a list that grows", lambda: written(lambda w: (lambda lines: w.writelines(type("L", (list,), {"__getitem__": lambda s, i: (list.append(s, "z") if len(s) < 5 else None, list.__getitem__(s, i))[1]})(lines)))(["a", "b"]))),
                 ("whose length raises", lambda: written(lambda w: w.writelines(type("L", (), {"__len__": lambda s: 1 / 0, "__getitem__": lambda s, i: "a"})()))), ("a stream with no write", lambda: codecs.getwriter("gbk")(5).write("a")), ("that raises", lambda: codecs.getwriter("gbk")(type("S", (), {"write": lambda s, b: 1 / 0})()).write("a")),
                 ("what it is given", lambda: (lambda got: (codecs.getwriter("gbk")(type("S", (), {"write": lambda s, b: got.append(b)})()).writelines(["\u4e2d", "", "a"]), got)[1])([]))):
    attempt(label, f)

print("---- what is written over them")
with tempfile.TemporaryDirectory() as directory:
    for encoding, sample in SAMPLES.items():
        path = os.path.join(directory, encoding)
        lines = [sample + "\n", "plain\n", sample * 3 + "\r\n", sample]
        with open(path, "w", encoding=encoding, newline="") as f:
            f.writelines(lines)
        with open(path, encoding=encoding, newline="") as f:
            first = f.readline()
            where = f.tell()
            rest = f.read()
            f.seek(where)
            again = f.read()
            f.seek(0)
            some = f.read(3)
            there = f.tell()
            more = f.read(4)
            f.seek(there)
            print("   ", encoding, os.path.getsize(path), first + rest == "".join(lines), again == rest, where, there, f.read(4) == more, sum(1 for _ in open(path, encoding=encoding)))
    path = os.path.join(directory, "appended")
    for piece in ("\u65e5\u672c", "abc", "\u8a9e"):
        with open(path, "a", encoding="iso2022_jp") as f:
            f.write(piece)
    print(open(path, "rb").read(), ascii(open(path, encoding="iso2022_jp").read()))
    with codecs.open(path, "w", "shift_jis") as f:
        f.write("\u65e5\u672c\n\u8a9e")
    with codecs.open(path, "r", "shift_jis") as f:
        attempt("codecs.open(): readlines()", f.readlines)
        attempt("readline()", f.readline)
        attempt("read()", f.read)
print(ascii(str(b"\x93\xfa\x96{", "shift_jis")), ascii(bytes("\u65e5\u672c", "euc-jp")), ascii(codecs.decode(b"\xc7\xd1", "EUC_KR")), ascii(b"\xa4\xa4".decode("Big5")), codecs.lookup("ms932").name, codecs.lookup("windows-31j").name, codecs.lookup("csISO2022JP").name, codecs.lookup("uhc").name, codecs.lookup("eucgb2312_cn").name)
print(ascii(io.TextIOWrapper(io.BytesIO("\u65e5\u672c\r\n\u8a9e".encode("cp932")), "cp932").read()), ascii(codecs.iterdecode([b"\x1b$BF", b"|K", b"\\\x1b(B"], "iso2022_jp").__next__()), list(codecs.iterencode(["\u65e5", "\u672c"], "iso2022_jp")))
from email.header import Header, decode_header
header = Header("\u65e5\u672c\u8a9e\u306e\u4ef6\u540d", "iso-2022-jp")
print(header.encode(), ascii(decode_header(header.encode())), ascii(str(Header("\ud55c\uae00", "euc-kr").encode())))
from email.message import EmailMessage
message = EmailMessage()
message.set_content("\u65e5\u672c\u8a9e\u306e\u672c\u6587\n", charset="iso-2022-jp")
print(message["Content-Type"], message["Content-Transfer-Encoding"], message.get_payload(decode=True), ascii(message.get_content()))
import json, csv
buffer = io.BytesIO()
wrapper = io.TextIOWrapper(buffer, "gb18030", newline="")
csv.writer(wrapper).writerows([["\u4e2d\u6587", "a,b"], ["\U0001f600", "\u00e9"]])
wrapper.flush()
print(buffer.getvalue(), ascii(list(csv.reader(io.TextIOWrapper(io.BytesIO(buffer.getvalue()), "gb18030", newline="")))), ascii(json.loads(json.dumps("\u4e2d", ensure_ascii=False).encode("gbk").decode("gbk"))))
source = "# -*- coding: euc-jp -*-\nvalue = '\u65e5\u672c'\n".encode("euc_jp")
namespace = {}
exec(compile(source, "<euc-jp>", "exec"), namespace)
print(ascii(namespace["value"]))
