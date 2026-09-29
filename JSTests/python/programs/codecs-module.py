# _codecs: the codecs that are written in C, one by one, and what they do about what they cannot encode or decode.
import _codecs
import _warnings
import sys

_warnings._acquire_lock()
_warnings.filters.insert(0, ("error", None, Warning, None, 0))
_warnings._filters_mutated_lock_held()
_warnings._release_lock()


def show(e):
    if isinstance(e, UnicodeError) and len(e.args) >= 4:
        return "%s: %s | %r" % (type(e).__name__, e, tuple(e.args[:1]) + tuple(e.args[-3:]) if len(e.args) == 5 else e.args)
    return type(e).__name__ + ": " + str(e)


def attempt(f, *a):
    try:
        return f(*a)
    except BaseException as e:
        return show(e)


def t(label, f):
    r = attempt(f)
    print(label, "=>", r if isinstance(r, str) else ascii(r))


HANDLERS = (None, "strict", "ignore", "replace", "backslashreplace", "xmlcharrefreplace", "namereplace", "surrogateescape", "surrogatepass", "no such handler")
STATEFUL = ("utf_7", "utf_8", "utf_16", "utf_16_le", "utf_16_be", "utf_32", "utf_32_le", "utf_32_be", "unicode_escape", "raw_unicode_escape")
STATELESS = ("latin_1", "ascii", "charmap")
TEXTS = ("", "a", "abc", "\x7f\x80\xff", "\u0100\u07ff\u0800\uffff", "\U00010000\U0010ffff", "a\U0001F600b", "\ud800", "\udfff", "a\ud800b", "\ud800\ud801x\udc00", "\udc80\udcff", "\udc80a\ud900", "x\udcfe\udc7f", "+-", "a+b", "~\\", "\x00\x01\t\n\r", "hi\u20ac!", "\ufeff", "\ufffe\uffff",
         "'\"\\", "A\u2262\u0391.", "\u65e5\u672c\u8a9e", "a-\u20ac-", "\u20acA", "\u20ac-", "\xe9" * 3, "\u20ac" * 3 + "\ud800" * 2 + "z")

print("---- encoding")
for name in STATEFUL + STATELESS:
    f = getattr(_codecs, name + "_encode")
    for errors in HANDLERS:
        t("%s_encode, %s" % (name, errors), lambda: [attempt(f, s, errors) for s in TEXTS])
for name in ("utf_16", "utf_32"):
    for order in (-1, 0, 1, 5, -5):
        t("%s_encode with byteorder %d" % (name, order), lambda: [attempt(getattr(_codecs, name + "_encode"), s, "strict", order) for s in ("", "a\u20ac\U0001F600", "\ud800")])

print("---- decoding")
UTF8 = (b"", b"a", b"abc", b"\xc3\xa9", b"\xe2\x82\xac", b"\xf0\x9f\x98\x80", b"\x80", b"\xbf", b"\xc0", b"\xc1\x80", b"\xc2", b"\xc2a", b"\xe0", b"\xe0\x80", b"\xe0\x9f\x80", b"\xe0\xa0", b"\xe0\xa0a", b"\xe2\x82", b"\xe2\x82a", b"\xe2a", b"\xed\x9f\xbf", b"\xed\xa0", b"\xed\xa0\x80", b"\xed\xbf\xbf",
        b"\xf0", b"\xf0\x80", b"\xf0\x8f\xbf\xbf", b"\xf0\x90", b"\xf0\x90\x80", b"\xf0\x90\x80a", b"\xf0\x9f\x98", b"\xf4\x8f\xbf\xbf", b"\xf4\x90\x80\x80", b"\xf5", b"\xf8\x88\x80\x80\x80", b"\xff", b"\xfe\xff", b"a\xffb\xfec", b"\xef\xbb\xbfa", b"ab\xc3", b"ab\xe2\x82", b"\xc3\xa9\xff\xc3",
        b"\xf1\x80\x80", b"\xf1\x80\x80\xe2\x82\xac", b"\xe1\x80\xe1\x80\x80", b"\xff" * 5, b"a" * 20 + b"\xe9" + b"b" * 20)
UTF16 = (b"", b"a", b"a\x00", b"\x00a", b"\xff\xfe", b"\xfe\xff", b"\xff\xfea\x00", b"\xfe\xff\x00a", b"a\x00b", b"\x00\xd8", b"\xd8\x00", b"\x00\xd8\x00\xdc", b"\xd8\x00\xdc\x00", b"\x00\xdc", b"\xdc\x00", b"\x00\xd8a\x00", b"\xd8\x00\x00a", b"\x00\xd8\x00", b"\xd8\x00\xdc", b"\x00\xd8\x00\xd8\x00\xdc",
         b"\xac\x20", b"\x20\xac", b"=\xd8\x00\xde", b"\xd8=\xde\x00", b"\xff\xfe\xff\xfe", b"\xfe\xff\xfe\xff", b"a\x00\x00\xdcb\x00", b"\x00a\xdc\x00\x00b", b"\x00\xd8\x00\xd8", b"\xff")
UTF32 = (b"", b"a", b"a\x00", b"a\x00\x00", b"a\x00\x00\x00", b"\x00\x00\x00a", b"\xff\xfe\x00\x00", b"\x00\x00\xfe\xff", b"\xff\xfe\x00\x00a\x00\x00\x00", b"\x00\x00\xfe\xff\x00\x00\x00a", b"a\x00\x00\x00b", b"\x00\xd8\x00\x00", b"\x00\x00\xd8\x00", b"\x00\x00\x11\x00", b"\x00\x11\x00\x00",
         b"\xff\xff\xff\xff", b"\x00\xf6\x01\x00", b"\x00\x01\xf6\x00", b"\xff\xff\x10\x00", b"\x00\x10\xff\xff", b"a\x00\x00\x00\x00\xdc\x00\x00b\x00\x00\x00", b"\x00\x00\x00a\x00\x00\xdc\x00\x00\x00\x00b", b"\xff\xfe\x00\x00\xff\xfe\x00\x00", b"\x00\x00\x00\x80", b"\x80\x00\x00\x00")
UTF7 = (b"", b"a", b"+", b"+-", b"a+-b", b"+AGE-", b"+AGE", b"+AGEA", b"+AGEAYg-", b"+AGEAYg", b"+IKw-", b"+IKw", b"+IKw.", b"+IKwa", b"+2D3eAA-", b"+2D3eAA", b"+2D0-", b"+2D0", b"+2D0a", b"+2D0AYQ-", b"+3gA-", b"+A", b"+A-", b"+AA", b"+AA-", b"+AAA", b"+AAA-", b"+AGF-", b"+AGH-", b"+!", b"+ ", b"a+!b", b"\x80", b"a\xffb",
        b"~", b"\\", b"+AGE+AGI-", b"+AGE-+AGI-", b"+//8-", b"+/v8-", b"+AGE\x80", b"++", b"+/", b"+AGE-x+", b"+2D3", b"+2D3e", b"+2D3eA", b"+AGEA-", b"+AGEAY-", b"a+", b"+AAAAAA-", b"+2D3YPQ-", b"+2D3YPd4A-")
ESCAPES = (b"", b"a", b"\\", b"a\\", b"\\\\", b"\\n\\t\\r\\a\\b\\f\\v", b"\\'\\\"", b"\\\n", b"\\0", b"\\7", b"\\8", b"\\12", b"\\123", b"\\1234", b"\\377", b"\\400", b"\\777", b"\\x", b"\\x4", b"\\x41", b"\\x4g", b"\\xg", b"\\x414", b"\\u", b"\\u2", b"\\u20a", b"\\u20ac", b"\\u20acx", b"\\u20ag", b"\\U", b"\\U0001f60",
           b"\\U0001f600", b"\\U00110000", b"\\Uffffffff", b"\\U0010ffff", b"\\ud800", b"\\udc00", b"\\N", b"\\N{", b"\\N{}", b"\\N{DIGIT ONE}", b"\\N{digit one}", b"\\N{DIGIT ONE", b"\\N{no such name}", b"\\Nx", b"\\N{LATIN SMALL LETTER A}b", b"\\N{\xe9}", b"\\z", b"\\z\\y", b"\\ ", b"\xe9", b"\xff\\xff",
           b"a\\x4\\x41", b"\\u12\\u1234", b"\\x4\\", b"abc\\", b"\\\\\\", b"\\400\\z", b"\\z\\400")
INPUTS = {"utf_8": UTF8, "utf_16": UTF16, "utf_16_le": UTF16, "utf_16_be": UTF16, "utf_32": UTF32, "utf_32_le": UTF32, "utf_32_be": UTF32, "utf_7": UTF7, "unicode_escape": ESCAPES, "raw_unicode_escape": ESCAPES,
          "latin_1": (b"", b"a", b"\x80\xff", bytes(range(256))), "ascii": (b"", b"a", b"\x80", b"a\xffb", b"\x7f\x80\x81z", bytes(range(120, 136))), "charmap": (b"", b"a", b"\x80\xff", bytes(range(256)))}
for name in STATEFUL:
    f = getattr(_codecs, name + "_decode")
    for errors in HANDLERS:
        for final in (False, True):
            t("%s_decode, %s, final=%s" % (name, errors, final), lambda: [attempt(f, s, errors, final) for s in INPUTS[name]])
for name in STATELESS:
    f = getattr(_codecs, name + "_decode")
    for errors in HANDLERS:
        t("%s_decode, %s" % (name, errors), lambda: [attempt(f, s, errors) for s in INPUTS[name]])
for name, data in (("utf_16_ex", UTF16), ("utf_32_ex", UTF32)):
    for order in (-1, 0, 1, 7):
        for final in (False, True):
            t("%s_decode, byteorder %d, final=%s" % (name, order, final), lambda: [attempt(getattr(_codecs, name + "_decode"), s, "replace", order, final) for s in data])
t("what is left out", lambda: [attempt(f, b) for f, b in ((_codecs.utf_8_decode, b"\xe2\x82"), (_codecs.utf_16_decode, b"a"), (_codecs.utf_7_decode, b"+AG"), (_codecs.unicode_escape_decode, b"\\x4"), (_codecs.raw_unicode_escape_decode, b"\\u1"), (_codecs.utf_16_ex_decode, b"\xff\xfea"), (_codecs.utf_32_ex_decode, b"\x00\x00\xfe\xff"))])

print("---- in pieces")


def pieces(decode, data, errors):
    "Decoded a piece at a time, cut at each place in turn, with what is left over each time put before the next."
    whole = attempt(decode, data, errors, True)
    out = []
    for cut in range(len(data) + 1):
        try:
            first, used = decode(data[:cut], errors, False)
            second, used2 = decode(data[used:], errors, True)
            out.append((cut, used) if (first + second, len(data)) == whole else (cut, used, first, second))
        except UnicodeError as e:
            out.append((cut, type(e).__name__, e.start, e.end, e.reason))
    return whole, out


SAMPLE = "a\xe9\u20ac\U0001F600z+~\\"
for name in ("utf_7", "utf_8", "utf_16_le", "utf_16_be", "utf_32_le", "utf_32_be", "unicode_escape", "raw_unicode_escape"):
    data = getattr(_codecs, name + "_encode")(SAMPLE)[0]
    t(name, lambda: pieces(getattr(_codecs, name + "_decode"), data, "strict"))
t("utf_8 with something wrong in it", lambda: pieces(_codecs.utf_8_decode, b"a\xe2\x82\xffb\xf0\x9f\xed\xa0\x80", "replace"))
t("utf_8 with a surrogate", lambda: pieces(_codecs.utf_8_decode, b"a\xed\xa0\x80b", "surrogatepass"))

print("---- other things that have bytes")
for value in (bytearray(b"ab"), memoryview(b"ab"), memoryview(b"abcd")[1:3], memoryview(b"abcd")[::2], "ab", 5, None, [97], b"ab".__class__(b"ab")):
    label = type(value).__name__
    t("utf_8_decode(%s)" % label, lambda: _codecs.utf_8_decode(value))
    t("unicode_escape_decode(%s)" % label, lambda: _codecs.unicode_escape_decode(value))
    t("escape_decode(%s)" % label, lambda: _codecs.escape_decode(value))
    t("readbuffer_encode(%s)" % label, lambda: _codecs.readbuffer_encode(value))
    t("utf_8_encode(%s)" % label, lambda: _codecs.utf_8_encode(value))
    t("escape_encode(%s)" % label, lambda: _codecs.escape_encode(value))
t("text that is not ASCII, where text will do", lambda: (_codecs.unicode_escape_decode("\xe9\u20ac"), _codecs.raw_unicode_escape_decode("\xe9"), _codecs.escape_decode("\xe9"), _codecs.readbuffer_encode("\u20ac"), attempt(_codecs.readbuffer_encode, "\ud800"), attempt(_codecs.unicode_escape_decode, "\ud800")))


class S(str):
    pass


t("of a class derived from str", lambda: (_codecs.utf_8_encode(S("a\xe9")), _codecs.latin_1_encode(S("a")), _codecs.charmap_build(S("ab")).__class__.__name__, attempt(_codecs.ascii_encode, S("\xe9"))))
for name, args in (("utf_8_decode", ()), ("utf_8_decode", (b"", 5)), ("utf_8_decode", (b"", b"strict")), ("utf_8_decode", (b"", "a\0b")), ("utf_8_decode", (b"", None, None)), ("utf_8_decode", (b"", None, 1, 2)), ("utf_8_encode", ()), ("utf_8_encode", ("", 5)), ("utf_8_encode", ("", None, 1)), ("utf_16_encode", ("", None, "a")),
                   ("utf_16_encode", ("", None, 2 ** 40)), ("utf_16_ex_decode", (b"", None, "a")), ("utf_16_ex_decode", (b"", None, 0, 0, 0)), ("latin_1_decode", (b"", None, 1)), ("charmap_decode", (b"", None, None, 1)), ("charmap_encode", ("", None, None, 1)), ("charmap_build", ()), ("charmap_build", (5,)), ("charmap_build", (b"a",)),
                   ("charmap_build", ("",)), ("escape_decode", (b"", 5)), ("escape_encode", (b"", 5)), ("readbuffer_encode", (b"", 5)), ("utf_7_encode", ("", 5)), ("unicode_escape_encode", ("", 5))):
    t("%s%r" % (name, args), lambda: getattr(_codecs, name)(*args))
t("by name", lambda: attempt(lambda: _codecs.utf_8_decode(data=b"")))

print("---- the escapes of bytes")
for errors in (None, "strict", "ignore", "replace", "other"):
    t("escape_decode, %s" % errors, lambda: [attempt(_codecs.escape_decode, s, errors) for s in ESCAPES])
t("escape_encode", lambda: (_codecs.escape_encode(bytes(range(256))), _codecs.escape_encode(b""), _codecs.escape_encode(b"'\"\\", "anything")))

print("---- charmap")
TABLE = "".join(chr(i) for i in range(128)) + "\u20ac\ufffe\u201a\u0192" + "\ufffe" * 60 + "\xe9" * 64
for errors in HANDLERS:
    t("decode by a str, %s" % errors, lambda: [attempt(_codecs.charmap_decode, s, errors, TABLE) for s in (b"", b"abc", b"\x80\x82\x83", b"\x81", b"a\x81b", b"\xff", b"\x84\x85")])
    t("by one that is short, %s" % errors, lambda: [attempt(_codecs.charmap_decode, s, errors, "xyz") for s in (b"\x00\x01\x02", b"\x03", b"\x00\xff")])
t("by a str with wide characters", lambda: (_codecs.charmap_decode(b"\x00\x01\x02", "strict", "a\U0001F600b"), _codecs.charmap_decode(b"\x00", "strict", "\ud800"), _codecs.charmap_decode(b"\x01", "replace", "a\ufffe")))
t("by a class derived from str", lambda: attempt(_codecs.charmap_decode, b"\x00\x01", "strict", S("ab")))
MAPS = ({97: 98}, {97: "b"}, {97: "xyz"}, {97: ""}, {97: None}, {97: 0xFFFE}, {97: "\ufffe"}, {97: 0x10FFFF}, {97: 0x110000}, {97: -1}, {97: 2 ** 70}, {97: 1.5}, {97: b"b"}, {97: True}, {97: "\U0001F600"}, {}, [1, 2], "abc" * 40, (None,) * 98, 5, None)
for errors in ("strict", "replace", "ignore"):
    t("decode by a mapping, %s" % errors, lambda: [attempt(_codecs.charmap_decode, b"a", errors, m) for m in MAPS])


class Map:
    def __init__(self, f): self.f, self.log = f, []
    def __getitem__(self, key):
        self.log.append(key)
        return self.f(key)


def raiser(e):
    def f(key): raise e
    return f


t("by what has __getitem__", lambda: [(attempt(_codecs.charmap_decode, b"ab", "replace", m), m.log) for m in (Map(lambda k: k + 1), Map(raiser(KeyError(1))), Map(raiser(IndexError(1))), Map(raiser(LookupError(1))), Map(raiser(ValueError("v"))), Map(lambda k: None))])
t("the bytes are as they were, whatever the mapping does", lambda: [(_codecs.charmap_decode(data, "strict", Map(lambda k: (data.__setitem__(slice(None), b"xyz"), k)[1])), data) for data in [bytearray(b"abc")]])
m = _codecs.charmap_build(TABLE)
t("charmap_build", lambda: (type(m).__name__, type(m).__module__, m.size(), sorted(k for k in vars(type(m)) if not k.startswith("__")), attempt(type(m)), attempt(m.size, 1), attempt(lambda: m[97]), attempt(setattr, m, "a", 1)))
t("of other tables", lambda: [(type(x).__name__, x.size() if hasattr(x, "size") else x) for x in (_codecs.charmap_build("\0a"), _codecs.charmap_build("ab"), _codecs.charmap_build("\0\0"), _codecs.charmap_build("\0\U0001F600"), _codecs.charmap_build("\0\ufffe\u20ac"), _codecs.charmap_build("\0"),
                                                                                                  _codecs.charmap_build("\0" + "".join(chr(1 + i * 200) for i in range(255))), _codecs.charmap_build("".join(chr(i) for i in range(300))), _codecs.charmap_build("\0" + "".join(chr(0x100 + i * 128) for i in range(255))))])
for errors in HANDLERS:
    t("encode by what it built, %s" % errors, lambda: [attempt(_codecs.charmap_encode, s, errors, m) for s in ("", "abc", "\u20ac\u201a\u0192", "\xe9", "\u20ad", "a\u20ad\u20aeb", "\U0001F600", "\ufffe", "\0", "\ud800", "\udc81")])
EMAPS = ({97: 98}, {97: b"b"}, {97: b"xyz"}, {97: b""}, {97: None}, {97: 255}, {97: 256}, {97: -1}, {97: 2 ** 70}, {97: "b"}, {97: 1.5}, {97: True}, {97: bytearray(b"b")}, {}, [1] * 98, 5, None, {97: 98, 63: 33}, {63: 33}, {63: None})
for errors in ("strict", "replace", "ignore", "xmlcharrefreplace", "backslashreplace"):
    t("encode by a mapping, %s" % errors, lambda: [attempt(_codecs.charmap_encode, "a", errors, x) for x in EMAPS])
DIGITS = {ord(c): ord(c) for c in "&#;0123456789\\xu"}
t("what stands in has to be encoded too", lambda: (_codecs.charmap_encode("\u20ac", "xmlcharrefreplace", DIGITS), attempt(_codecs.charmap_encode, "\u20ac", "xmlcharrefreplace", {38: 38}), attempt(_codecs.charmap_encode, "\u20ac", "backslashreplace", DIGITS), attempt(_codecs.charmap_encode, "\u2000", "backslashreplace", DIGITS),
                                                      attempt(_codecs.charmap_encode, "\u20ac", "replace", {}), _codecs.charmap_encode("\udc81", "surrogateescape", {})))
t("what is asked of the mapping", lambda: [(attempt(_codecs.charmap_encode, "abcd", "ignore", x), x.log) for x in (Map(lambda k: k if k != 98 and k != 99 else None), Map(raiser(KeyError(1))), Map(raiser(ValueError("v"))))])

print("---- handlers of a program's")
seen = []


def register(name, f):
    _codecs.register_error(name, f)
    return name


def noting(result):
    def handler(e):
        seen.append((type(e).__name__, e.encoding, e.object if len(e.object) < 30 else len(e.object), e.start, e.end, e.reason))
        return result(e) if callable(result) else result
    return handler


RESULTS = (("a str", lambda e: ("<?>", e.end)), ("nothing", lambda e: ("", e.end)), ("bytes", lambda e: (b"<?>", e.end)), ("not ASCII", lambda e: ("\xe9", e.end)), ("wide", lambda e: ("\u20ac", e.end)), ("wider", lambda e: ("\U0001F600", e.end)), ("a surrogate", lambda e: ("\ud800", e.end)),
           ("from the start", lambda e: ("!", e.start + 1)), ("a negative position", lambda e: ("!", -1)), ("the end", lambda e: ("!", len(e.object))), ("past the end", lambda e: ("!", len(e.object) + 1)), ("before the beginning", lambda e: ("!", -len(e.object) - 1)), ("None", None), ("a list", ["!", 1]), ("one thing", ("!",)),
           ("three", ("!", 1, 2)), ("an int for the text", (5, 1)), ("None for the text", (None, 1)), ("a str for the position", ("!", "a")), ("a float", ("!", 1.5)), ("huge", ("!", 2 ** 70)), ("True", lambda e: ("!", True) if len(e.object) < 3 else ("!", e.end)), ("a bytearray", lambda e: (bytearray(b"!"), e.end)),
           ("odd bytes", lambda e: (b"!", e.end)), ("two bytes", lambda e: (b"!?", e.end)), ("four bytes", lambda e: (b"!?!?", e.end)), ("derived from str", lambda e: (S("s"), e.end)), ("derived from tuple", lambda e: type("T", (tuple,), {})(("t", e.end))))
ENCODERS = (("utf_8", "a\ud800b"), ("utf_16", "a\ud800b"), ("utf_16_be", "a\ud800b"), ("utf_32", "a\ud800b"), ("ascii", "a\xe9\u20acb"), ("latin_1", "a\u20ac\u20adb"), ("charmap", "a\u20acb"))
DECODERS = (("utf_8", b"a\xffb"), ("utf_16_le", b"a\x00\x00\xdcb\x00"), ("utf_32_le", b"a\x00\x00\x00\x00\xdc\x00\x00b\x00\x00\x00"), ("ascii", b"a\xffb"), ("utf_7", b"a\xffb"), ("unicode_escape", b"a\\x4b"), ("raw_unicode_escape", b"a\\u4b"), ("charmap", b"a\x80b"))
for label, result in RESULTS:
    name = register("test." + label, noting(result))
    t("encoding, and it returns " + label, lambda: [attempt(getattr(_codecs, c + "_encode"), s, name, *((m,) if c == "charmap" else ())) for c, s in ENCODERS])
    t("decoding, and it returns " + label, lambda: [attempt(getattr(_codecs, c + "_decode"), s, name, *((TABLE,) if c == "charmap" else ())) for c, s in DECODERS])
t("what they were told", lambda: (len(seen), seen[:16]))
t("that raises", lambda: [attempt(f, s, register("test.raises", raiser(KeyError("from the handler")))) for f, s in ((_codecs.utf_8_encode, "\ud800"), (_codecs.utf_8_decode, b"\xff"))])
t("that raises what it was given", lambda: [attempt(f, s, register("test.again", lambda e: (_ for _ in ()).throw(e))) for f, s in ((_codecs.ascii_encode, "\xe9"), (_codecs.ascii_decode, b"\xff"))])
count = [0]


def once(e):
    count[0] += 1
    return ("", e.end)


t("it is looked up once, and there is one exception", lambda: [(_codecs.utf_8_decode(b"\xff\xfe\xfd", register("test.once", once)), count[0])])
same = []
t("the same one each time", lambda: (_codecs.ascii_decode(b"\xffa\xfe", register("test.same", lambda e: (same.append(e), ("", e.end))[1])), same[0] is same[1], same[0].start, same[0].end))
t("it is not looked up until it is wanted", lambda: (_codecs.utf_8_decode(b"abc", "no such handler"), _codecs.utf_8_encode("abc", "no such handler"), _codecs.ascii_decode(b"", "no such handler"), _codecs.latin_1_decode(b"\xff", "no such handler")))


def replacing(new, position):
    def handler(e):
        e.object = new
        return ("<>", position)
    return handler


t("that puts other bytes in place of what is being decoded", lambda: [attempt(f, s, register("test.other", replacing(b"XYZ", 1))) for f, s in ((_codecs.utf_8_decode, b"ab\xffcd"), (_codecs.ascii_decode, b"ab\xffcd"), (_codecs.utf_7_decode, b"ab\xffcd"), (_codecs.unicode_escape_decode, b"ab\\xcd"))])
t("longer ones", lambda: attempt(_codecs.utf_8_decode, b"\xff", register("test.longer", replacing(b"0123456789", 2))))
t("what is not bytes", lambda: [attempt(_codecs.utf_8_decode, b"\xff", register("test.notbytes", replacing(x, 0))) for x in ("abc", bytearray(b"abc"), None, 5)])


def deleting(e):
    del e.object
    return ("", 0)


t("that takes it away", lambda: attempt(_codecs.utf_8_decode, b"\xff", register("test.deleting", deleting)))
t("that goes back", lambda: [(attempt(_codecs.utf_8_encode, "ab\ud800c", register("test.back", lambda e: (n.append(1), ("<", 0 if len(n) < 3 else e.end))[1])), len(n)) for n in [[]]])
t("what the exception has is a copy", lambda: [(_codecs.utf_8_decode(data, register("test.copy", lambda e: (kept.append(e.object), ("", e.end))[1])), type(kept[0]).__name__, kept[0] == data, kept[0] is data) for data in [b"a\xff"] for kept in [[]]])
t("of a str it is the str", lambda: [(_codecs.ascii_encode(text, register("test.str", lambda e: (kept.append(e.object), ("", e.end))[1])), kept[0] is text) for text in ["a\xe9" * 2] for kept in [[]]])

print("---- the handlers that are built in, called by hand")
E = lambda obj, a, b, enc="utf-8": UnicodeEncodeError(enc, obj, a, b, "r")
D = lambda obj, a, b, enc="utf-8": UnicodeDecodeError(enc, obj, a, b, "r")
T = lambda obj, a, b: UnicodeTranslateError(obj, a, b, "r")
CASES = [("E", E("a\xe9\u20ac\U0001F600\ud800b", a, b)) for a, b in ((0, 1), (1, 2), (1, 5), (0, 6), (2, 2), (3, 1), (-1, 2), (0, 100), (100, 200), (5, 6), (-5, -1))]
CASES += [("D", D(b"a\xe9\xff\x00b", a, b)) for a, b in ((0, 1), (1, 3), (0, 5), (2, 2), (3, 1), (-1, 2), (0, 100), (100, 200))]
CASES += [("T", T("a\xe9\u20ac\U0001F600b", a, b)) for a, b in ((0, 1), (1, 4), (2, 2), (3, 1), (0, 100))]
CASES += [("E of nothing", E("", 0, 0)), ("D of nothing", D(b"", 0, 0)), ("ValueError", ValueError("v")), ("UnicodeError", UnicodeError("u")), ("an int", 5), ("None", None), ("a class", UnicodeEncodeError)]
for name in HANDLERS[1:-1]:
    f = _codecs.lookup_error(name)
    t(name, lambda: [(label, getattr(e, "start", None), getattr(e, "end", None), attempt(f, e)) for label, e in CASES])
    t(name + " with the wrong number", lambda: (attempt(f), attempt(f, 1, 2), attempt(lambda: f(exc=1))))
for enc in ("utf-8", "utf8", "UTF_8", "utf-16", "utf16", "utf-16-le", "utf_16_be", "UTF-16LE", "utf-32", "utf-32-be", "utf32le", "cp65001", "CP65001", "ascii", "latin-1", "utf-9", "utf", "utf-", "utf-8x", "utf-16-xe", "utf-16-l", "", "utf-16le "):
    t("surrogatepass for %r" % enc, lambda: (attempt(_codecs.lookup_error("surrogatepass"), E("a\ud800\udbffb", 1, 3, enc)), attempt(_codecs.lookup_error("surrogatepass"), D(b"\xed\xa0\x80\x00\xd8\x00\x00\xd8\x00", 0, 1, enc)), attempt(_codecs.lookup_error("surrogatepass"), D(b"\x00\x00\xd8\x00", 0, 1, enc))))
t("surrogatepass of what is not one", lambda: (attempt(_codecs.lookup_error("surrogatepass"), E("ab", 0, 1)), attempt(_codecs.lookup_error("surrogatepass"), E("\ud800a", 0, 2)), attempt(_codecs.lookup_error("surrogatepass"), D(b"abc", 0, 1)), attempt(_codecs.lookup_error("surrogatepass"), D(b"\xed\xa0", 0, 1))))
t("surrogateescape", lambda: (attempt(_codecs.lookup_error("surrogateescape"), E("\udc80\udcff", 0, 2)), attempt(_codecs.lookup_error("surrogateescape"), E("\udc7f", 0, 1)), attempt(_codecs.lookup_error("surrogateescape"), E("\udc80a", 0, 2)), attempt(_codecs.lookup_error("surrogateescape"), D(b"\x80\x81\x82\x83\x84\x85", 0, 6)),
                                 attempt(_codecs.lookup_error("surrogateescape"), D(b"\x80a\x81", 0, 3)), attempt(_codecs.lookup_error("surrogateescape"), D(b"a\x80", 0, 2))))


def broken(e, **changes):
    for k, v in changes.items():
        if v is Ellipsis:
            delattr(e, k)
        else:
            setattr(e, k, v)
    return e


for name in ("ignore", "replace", "backslashreplace", "xmlcharrefreplace", "namereplace", "surrogatepass", "surrogateescape"):
    f = _codecs.lookup_error(name)
    t(name + " of an exception that has been changed", lambda: [attempt(f, x) for x in (broken(E("ab", 0, 1), object=...), broken(E("ab", 0, 1), object=b"ab"), broken(E("ab", 0, 1), object=5), broken(D(b"ab", 0, 1), object=...), broken(D(b"ab", 0, 1), object="ab"), broken(E("ab", 0, 1), encoding=...), broken(E("ab", 0, 1), encoding=5),
                                                                                          broken(E("\ud800b", 0, 1), object="x"), broken(E("ab", 0, 1), start=5), broken(E("ab", 0, 1), end=-5), UnicodeEncodeError.__new__(UnicodeEncodeError), UnicodeDecodeError.__new__(UnicodeDecodeError))])

print("---- the registry of handlers")
t("lookup_error", lambda: (attempt(_codecs.lookup_error, "no such"), attempt(_codecs.lookup_error, ""), attempt(_codecs.lookup_error, 5), attempt(_codecs.lookup_error, None), attempt(_codecs.lookup_error), attempt(_codecs.lookup_error, "a\0b"), attempt(_codecs.lookup_error, "\xe9"), attempt(_codecs.lookup_error, "x" * 500)[:60]))
t("register_error", lambda: (_codecs.register_error("test.f", len), _codecs.lookup_error("test.f") is len, _codecs.register_error("test.f", max), _codecs.lookup_error("test.f") is max, attempt(_codecs.register_error, "test.g", 5), attempt(_codecs.register_error, "test.g", None), attempt(_codecs.register_error, 5, len),
                                attempt(_codecs.register_error, "test.g"), attempt(_codecs.register_error, "a\0b", len), attempt(_codecs.lookup_error, "test.g")))
t("_unregister_error", lambda: (_codecs._unregister_error("test.f"), _codecs._unregister_error("test.f"), attempt(_codecs.lookup_error, "test.f"), attempt(_codecs._unregister_error, "strict"), attempt(_codecs._unregister_error, "surrogateescape"), attempt(_codecs._unregister_error, 5), attempt(_codecs._unregister_error), _codecs._unregister_error("")))
original = _codecs.lookup_error("replace")
t("one that is built in can be replaced", lambda: (_codecs.register_error("namereplace", lambda e: ("N", e.end)), _codecs.ascii_encode("\xe9", "namereplace"), _codecs.register_error("replace", lambda e: ("R", e.end)), _codecs.ascii_encode("\xe9", "replace"), _codecs.utf_16_encode("\ud800", "replace"), _codecs.utf_8_decode(b"\xff", "replace"),
                                                     _codecs.utf_16_decode(b"\x00\xdc", "replace"), _codecs.register_error("replace", original), _codecs.register_error("strict", lambda e: ("S", e.end)), attempt(_codecs.ascii_encode, "\xe9", "strict"), attempt(_codecs.ascii_encode, "\xe9"), attempt(_codecs.utf_8_encode, "\ud800", "strict"),
                                                     attempt(_codecs.utf_8_decode, b"\xff"), attempt(_codecs.ascii_decode, b"\xff", "strict")))
