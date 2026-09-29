# The module binascii, and base64, quopri and uu-encoding by way of it.
import binascii as b
import base64


def show(e):
    return type(e).__name__ + ": " + str(e)


def attempt(f, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return show(e)


def t(label, f):
    r = attempt(f)
    print(label, "=>", r if isinstance(r, str) else ascii(r))


def digest(values):
    data = b"|".join(v if isinstance(v, bytes) else str(v).encode() for v in values)
    return len(data), int.from_bytes(data, "big") % (2 ** 89 - 1)


state = 3


def some(most, alphabet=None):
    global state
    state = (state * 1103515245 + 12345) % 2 ** 31
    n = state // 65536 % (most + 1)
    out = bytearray()
    for _ in range(n):
        state = (state * 1103515245 + 12345) % 2 ** 31
        v = state // 65536
        out.append(alphabet[v % len(alphabet)] if alphabet else v % 256)
    return bytes(out)


print("---- what there is")
t("the module", lambda: (b.__name__, b.__package__, b.__loader__.__name__, b.__doc__, sorted(n for n in vars(b) if not n.startswith("__"))))
for name in sorted(n for n in vars(b) if not n.startswith("__")):
    x = getattr(b, name)
    if isinstance(x, type):
        t(name, lambda: (x.__name__, x.__module__, x.__qualname__, [c.__name__ for c in x.__mro__], sorted(vars(x)), x.__doc__, repr(x), repr(x("m"))))
    else:
        t(name, lambda: (type(x).__name__, x.__text_signature__, x.__doc__, x.__module__))


class Bool:
    def __init__(self, v): self.v = v
    def __bool__(self): return self.v


class Raises:
    def __bool__(self): raise ValueError("no bool")
    def __index__(self): raise ValueError("no index")


class Index:
    def __init__(self, v): self.v = v
    def __index__(self): return self.v


DECODERS = (b.a2b_uu, b.a2b_base64, b.a2b_hex, b.unhexlify, b.a2b_qp)
ENCODERS = (b.b2a_uu, b.b2a_base64, b.b2a_hex, b.hexlify, b.b2a_qp, b.crc32)
print("---- what they take")
for f in DECODERS:
    t(f.__name__, lambda: [attempt(f, *a) for a in ((), ("",), (b"",), (bytearray(b""),), (memoryview(b""),), ("\xe9",), ("一",), ("\0",), (5,), (None,), ([],), (1.5,), (memoryview(b"abcd")[::2],), (type("S", (str,), {})(""),), (type("B", (bytes,), {})(b""),), (b"", b"", b"", b""))])
for f in ENCODERS:
    t(f.__name__, lambda: [attempt(f, *a) for a in ((), ("",), ("a",), (b"",), (bytearray(b"a"),), (memoryview(b"a"),), (5,), (None,), ([],), (memoryview(b"abcd")[::2],), (type("B", (bytes,), {})(b"a"),), (b"", b"", b"", b"", b""))])

print("---- base64")
t("b2a_base64", lambda: [b.b2a_base64(x) for x in (b"", b"a", b"ab", b"abc", b"abcd", b"\0", b"\xff\xff\xff", b"\xfb\xef\xbe", bytes(range(256)))])
t("newline", lambda: [attempt(b.b2a_base64, b"ab", newline=v) for v in (True, False, 0, 1, "", "x", None, Bool(False), Raises())] + [attempt(b.b2a_base64, b"ab", False), attempt(lambda: b.b2a_base64(data=b"ab")), attempt(lambda: b.b2a_base64(b"ab", other=1))])
CASES = (b"", b"YQ==", b"YWI=", b"YWJj", b"YWJjZA==", b"YQ", b"YQ=", b"YWI", b"Y", b"YWJjZ", b"=", b"==", b"===", b"====", b"=YQ==", b"Y=Q=", b"YQ=a", b"YQ==a", b"YQ==YQ==", b"YQ===", b"YQ====", b"YWI==", b"YWJj=", b"YWJj==", b"YW Jj", b"YW\nJj\n", b"YW-Jj", b"YW_Jj", b"Y!W@J#j", b"\xffYWJj", b"YWJj\0", b" ", b"\n", b"!!!!", b"Y===", b"Y=", b"YW=Jj", b"YW==Jj", b"YWJ=j", b"////", b"++++", b"AAAA", b"A", b"AA", b"AAA", b"AAAAA", b"AAAAAA", b"AAAAAAA", b"YQ=\n=", b"YQ\n==", b"YQ= =", b"Y\nQ==")
t("a2b_base64", lambda: [(c, attempt(b.a2b_base64, c)) for c in CASES])
t("a2b_base64, strictly", lambda: [(c, attempt(b.a2b_base64, c, strict_mode=True)) for c in CASES])
t("strict_mode", lambda: [attempt(b.a2b_base64, b"YQ==a", strict_mode=v) for v in (True, False, 0, 1, "", "x", None, Bool(True), Raises())] + [attempt(b.a2b_base64, b"YQ==", True), attempt(b.a2b_base64, "YQ=="), attempt(b.a2b_base64, "YQ==\xe9")])
t("there and back", lambda: all(b.a2b_base64(b.b2a_base64(x)) == x and b.a2b_base64(b.b2a_base64(x, newline=False), strict_mode=True) == x for x in (some(70) for _ in range(3000))))
t("at random", lambda: (digest(b.b2a_base64(some(100)) for _ in range(2000)), digest(attempt(b.a2b_base64, some(24, b"ABCDwxyz0189+/==\n -")) for _ in range(6000)), digest(attempt(b.a2b_base64, some(16, b"ABCz09+/===\n"), strict_mode=True) for _ in range(6000))))
t("long", lambda: (digest([b.b2a_base64(bytes(range(256)) * 4000)]), len(b.a2b_base64(b"QUJD" * 250000))))

print("---- hex")
t("b2a_hex", lambda: [b.b2a_hex(x) for x in (b"", b"\0", b"\xff", b"abc", bytes(range(256)))] + [b.hexlify(b"\xb9\x01\xef"), type(b.hexlify(b"a")).__name__])
t("with something between", lambda: [attempt(b.hexlify, b"\x01\x02\x03\x04\x05", *a) for a in ((":",), (b":",), ("-", 2), ("-", -2), ("-", 0), ("-", 5), ("-", 6), ("-", -5), ("-", 100), ("-", 3), ("-", -3), ("",), ("ab",), (b"ab",), ("\xe9",), ("一",), (b"\xff",), (5,), (None,), (bytearray(b":"),), (":", "a"), (":", 1.5), (":", None), (":", 2 ** 31), (":", -2 ** 31 - 1), (":", 2 ** 31 - 1), (":", Index(2)), (":", Raises()), (":", True), (":", 1, 1))])
t("by name", lambda: [attempt(b.hexlify, b"\x01\x02\x03", **k) for k in ({"sep": ":"}, {"sep": ":", "bytes_per_sep": 2}, {"bytes_per_sep": 2}, {"other": 1})] + [attempt(lambda: b.hexlify(data=b"\x01\x02", sep="-")), attempt(lambda: b.b2a_hex(data=b"\x01"))])
t("a2b_hex", lambda: [attempt(b.a2b_hex, x) for x in (b"", b"00", b"ff", b"FF", b"fF", b"616263", "616263", b"0", b"000", b"0g", b"g0", b" 00", b"00 ", b"0x00", b"\xff\xff", b"--", b"+1", "٠٠", b"0\0", bytearray(b"4142"), memoryview(b"4142"))] + [b.unhexlify(b.hexlify(bytes(range(256)))) == bytes(range(256))])

print("---- checksums")
t("crc32", lambda: [b.crc32(x) for x in (b"", b"a", b"abc", b"123456789", b"\0", b"\xff" * 32, bytes(range(256)), b"x" * 6000)] + [type(b.crc32(b"")).__name__])
t("crc32 going on from", lambda: [attempt(b.crc32, b"abc", c) for c in (0, 1, 0xFFFFFFFF, 0x100000000, 0x100000001, -1, -2 ** 40, 2 ** 100 + 5, True, Index(7), 1.5, "a", None, Raises())] + [b.crc32(b"def", b.crc32(b"abc")) == b.crc32(b"abcdef"), attempt(lambda: b.crc32(b"", crc=0))])
t("crc_hqx", lambda: [b.crc_hqx(x, 0) for x in (b"", b"a", b"abc", b"123456789", b"\0", b"\xff" * 32, bytes(range(256)))] + [attempt(b.crc_hqx, b"abc", c) for c in (1, 0xFFFF, 0x10000, 0x10001, -1, 2 ** 100 + 5, True, Index(7), 1.5, "a", None, Raises())] + [attempt(b.crc_hqx, b"abc"), b.crc_hqx(b"def", b.crc_hqx(b"abc", 0)) == b.crc_hqx(b"abcdef", 0)])
t("at random", lambda: (digest(b.crc32(some(300)) for _ in range(2000)), digest(b.crc_hqx(some(300), 0x1D0F) for _ in range(2000))))

print("---- uu")
t("b2a_uu", lambda: [attempt(b.b2a_uu, x) for x in (b"", b"a", b"ab", b"abc", b"abcd", b"\0", b"\0\0\0", b"\xff\xff\xff", b"x" * 45, b"x" * 46, bytes(range(45)))])
t("backtick", lambda: [attempt(b.b2a_uu, x, backtick=v) for x in (b"", b"\0\0\0", b"a\0", b"abc") for v in (True, False)] + [attempt(b.b2a_uu, b"", backtick=Raises()), attempt(b.b2a_uu, b"", True)])
t("a2b_uu", lambda: [(x, attempt(b.a2b_uu, x)) for x in (b"", b" ", b"`", b"\n", b"!", b"!80", b"!80  ", b"!80``", b"#86)C", b"#86)C\n", b"#86)C\r\n", b"#86)C  \n", b"#86)Cx", b"#86)C`", b"#86)", b"#86", b"#", b"#\n", b"#86)\x1f", b"#86)a", b"#86)\x60", b"#86)\x61", b"#\xff\xff\xff\xff", b"M" + b"!" * 60, b"M" + b"!" * 59, b"M", b"\x7f", b"\x00", b"\x1f", b"_", b"$86)C", b"\"86)C", "#86)C", "#86)\xe9")])
t("there and back", lambda: all(b.a2b_uu(b.b2a_uu(x)) == x and b.a2b_uu(b.b2a_uu(x, backtick=True)) == x for x in (some(45) for _ in range(3000))))
t("at random", lambda: (digest(b.b2a_uu(some(45)) for _ in range(2000)), digest(attempt(b.a2b_uu, some(20, b" !#$0AZ_`a\n\r")) for _ in range(6000))))

print("---- quoted-printable")
QP = (b"", b"a", b"=", b"==", b"=3D", b"=3d", b"=3", b"=3G", b"=G3", b"=\n", b"=\r\n", b"=\r", b"=\rabc\ndef", b"a=\nb", b"a=\r\nb", b"a=", b"a=b", b"=41=42", b"=4", b"_", b"a_b", b"=5F", b"=00", b"=FF", b"=ff", b"= ", b"=\t", b"a =\n b", b"===", b"=3D=", b"\xff", b"=\n=\n", b"=\r\r\n", b"=A", b"=AB=", b"x=y=z")
t("a2b_qp", lambda: [(x, b.a2b_qp(x), b.a2b_qp(x, True), b.a2b_qp(x, header=True)) for x in QP] + [attempt(b.a2b_qp, "=41"), attempt(b.a2b_qp, "\xe9"), attempt(lambda: b.a2b_qp(data=b"=41")), attempt(b.a2b_qp, b"", Raises()), attempt(lambda: b.a2b_qp(b"", other=1))])
TEXTS = (b"", b"a", b"=", b"_", b" ", b"\t", b"a b", b"a ", b"a\t", b" a", b"a \n", b"a\t\n", b"a \r\n", b"a\n", b"a\r\n", b"a\r", b"\n", b"\r\n", b"\r", b"\n\n", b"a\nb\r\nc", b"a\r\nb\nc", b".", b".\n", b".a", b"a.", b"\n.", b"\n.\n", b"\n.a", b".\r", b".\0", b"\0", b"\x7f", b"\x7e", b"\x80", b"\xff", b"\x1f", b"!", b"a" * 75, b"a" * 76, b"a" * 77, b"a" * 200, b"=" * 30, b"a" * 73 + b"=", b"a" * 74 + b"=", b"a" * 72 + b"=", b"a" * 75 + b"\n", b"a" * 76 + b"\n", b"a" * 74 + b" \n", b"a" * 75 + b" ", b" " * 80, b"\t" * 80, b"a" * 75 + b"\r\nb", b"x\r\n" + b"a" * 80, b"x\n" + b"a" * 80, b"\xe9" * 30, b"a b_c=d.e", b"a\rb", b"a \rb", b"a\r \n")
for k in ({}, {"quotetabs": True}, {"istext": False}, {"header": True}, {"quotetabs": True, "header": True}, {"istext": False, "header": True, "quotetabs": True}):
    t("b2a_qp %r" % (k,), lambda: [b.b2a_qp(x, **k) for x in TEXTS])
t("b2a_qp called otherwise", lambda: [attempt(b.b2a_qp, b"a b\n", *a) for a in ((True,), (True, False), (True, False, True), (0, 0, 0), (Raises(),), (0, Raises()), (0, 0, Raises()), (0, 0, 0, 0))] + [attempt(lambda: b.b2a_qp(data=b"a b")), attempt(lambda: b.b2a_qp(b"", other=1))])
t("there and back", lambda: all(b.a2b_qp(b.b2a_qp(x, istext=False)) == x and b.a2b_qp(b.b2a_qp(x, quotetabs=True, istext=False, header=True), header=True) == x for x in (some(200) for _ in range(1500))))
t("at random", lambda: [digest(b.b2a_qp(some(200, b"ab =_.\t\r\n\xe9\0~"), **k) for _ in range(2500)) for k in ({}, {"quotetabs": True}, {"istext": False}, {"header": True})] + [digest(b.a2b_qp(some(30, b"a=3DdG\r\n_ "), h) for _ in range(5000)) for h in (False, True)])

print("---- what is written in Python over it")
DATA = bytes(range(256)) * 3
t("base64", lambda: (base64.b64encode(b"hello"), base64.b64decode(b"aGVsbG8="), base64.urlsafe_b64encode(b"\xfb\xef\xbe"), base64.urlsafe_b64decode(b"--_-"), base64.standard_b64encode(b"a"), digest([base64.encodebytes(DATA)]), base64.decodebytes(base64.encodebytes(DATA)) == DATA, attempt(base64.b64decode, b"a"), attempt(base64.b64decode, b"YQ==!", validate=True), base64.b64decode(b"YQ==!"), base64.b64encode(b"\xfb\xef", altchars=b"-_")))
t("the rest of base64", lambda: (base64.b32encode(b"hello"), base64.b32decode(b"NBSWY3DP"), base64.b16encode(b"hello"), base64.b16decode(b"68656C6C6F"), attempt(base64.b16decode, b"68656c6c6f"), base64.b16decode(b"68656c6c6f", casefold=True), base64.b85encode(b"hello world"), base64.b85decode(base64.b85encode(DATA)) == DATA, base64.a85encode(b"hello world"), base64.a85decode(base64.a85encode(DATA)) == DATA, base64.b32hexencode(b"hello"), base64.z85encode(b"hello world!")))
t("codecs", lambda: (__import__("codecs").encode(b"hello", "base64"), __import__("codecs").decode(b"aGVsbG8=\n", "base64"), __import__("codecs").encode(b"hello", "hex"), __import__("codecs").decode(b"68656c6c6f", "hex"), __import__("codecs").encode(b"h\xe9llo =", "quopri"), __import__("codecs").decode(b"h=E9llo =3D", "quopri"), __import__("codecs").encode(b"hello", "uu"), __import__("codecs").decode(__import__("codecs").encode(DATA, "uu"), "uu") == DATA))
