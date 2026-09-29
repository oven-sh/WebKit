# The module unicodedata: every character there is, by everything that there is to ask about it.
import unicodedata as u


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


def digest(text):
    "Of a great deal of text, without going through it a character at a time"
    b = text.encode("utf-8", "surrogatepass")
    return len(b), int.from_bytes(b, "big") % (2 ** 89 - 1)


old = u.ucd_3_2_0
print("---- what there is")
t("the module", lambda: (u.__name__, u.__package__, u.__loader__.__name__, u.__doc__, sorted(n for n in vars(u) if not n.startswith("__")), u.unidata_version))
for name in sorted(n for n in vars(u) if not n.startswith("__")):
    x = getattr(u, name)
    if callable(x) and not isinstance(x, type):
        t(name, lambda: (type(x).__name__, x.__text_signature__, x.__doc__, x.__module__, x.__self__ is u))
C = u.UCD
t("UCD", lambda: (C.__name__, C.__module__, C.__qualname__, [b.__name__ for b in C.__mro__], sorted(vars(C)), C.__doc__, C.__text_signature__, C.__basicsize__, C.__flags__ & 0x7FFF, repr(C), attempt(C), attempt(setattr, C, "x", 1), attempt(lambda: type("D", (C,), {}))))
for name in sorted(vars(C)):
    x = vars(C)[name]
    t("UCD." + name, lambda: (type(x).__name__, getattr(x, "__text_signature__", None), x.__doc__ if not isinstance(x, str) else None))
t("ucd_3_2_0", lambda: (type(old) is C, old.unidata_version, repr(old).split(" at ")[0], attempt(setattr, old, "unidata_version", "x"), attempt(delattr, old, "unidata_version"), attempt(setattr, old, "x", 1), attempt(C.category, u, "a"), attempt(C.category, "a"), C.category(old, "a"), type(old.category).__name__, old.category.__self__ is old))
K = type(u._ucnhash_CAPI)
t("_ucnhash_CAPI", lambda: (K.__name__, K.__module__, K.__qualname__, [b.__name__ for b in K.__mro__], sorted(vars(K)), K.__doc__, K.__basicsize__, K.__flags__ & 0x7FFF, repr(K), repr(u._ucnhash_CAPI).split(" at ")[0], attempt(K), attempt(setattr, K, "x", 1), attempt(setattr, u._ucnhash_CAPI, "x", 1), attempt(lambda: type("D", (K,), {}))))

print("---- every character")
ALL = [chr(c) for c in range(0x110000)]
for label, db in (("now", u), ("3.2.0", old)):
    for name in ("category", "bidirectional", "combining", "mirrored", "east_asian_width", "decomposition"):
        f = getattr(db, name)
        t("%s %s" % (label, name), lambda: digest(",".join(map(str, map(f, ALL)))))
    for name in ("decimal", "digit", "numeric", "name"):
        f = getattr(db, name)
        t("%s %s" % (label, name), lambda: digest(",".join([str(f(c, "")) for c in ALL])))
    for form in ("NFC", "NFD", "NFKC", "NFKD"):
        t("%s %s" % (label, form), lambda: digest(",".join([db.normalize(form, c) for c in ALL])))
        t("%s is %s" % (label, form), lambda: digest("".join(["01"[db.is_normalized(form, c)] for c in ALL])))
NAMED = [c for c in ALL if u.name(c, None)]
t("how many have names", lambda: (len(NAMED), len([c for c in ALL if old.name(c, None)])))
t("by name and back", lambda: (all(u.lookup(u.name(c)) == c for c in NAMED), all(u.lookup(u.name(c).lower()) == c for c in NAMED[::7]), all(old.lookup(u.name(c)) == c for c in NAMED[::7])))
t("the longest names", lambda: sorted(((len(u.name(c)), u.name(c)) for c in NAMED), reverse=True)[:3])

print("---- one at a time")
SOME = "\0", "a", "A", "0", " ", "\xa0", "\xbd", "\xe9", "\u0301", "\u0660", "\u2460", "\u2155", "\u3007", "\u4e00", "\u9fff", "\uac00", "\ud7a3", "\ud7a4", "\ud800", "\udfff", "\ue000", "\ufb01", "\ufdfa", "\uff21", "\uffff", "\U00010000", "\U0001d7ce", "\U0001f600", "\U00020000", "\U0002a6df", "\U0002a6e0", "\U00017000", "\U000187f7", "\U00018d00", "\U000323af", "\U000e0100", "\U000f0000", "\U000f01dc", "\U000f01dd", "\U000f0200", "\U000f03cc", "\U000f03cd", "\U0010ffff", "(", "\u2208", "\u0f77", "\u0344", "\u212b", "\u1e9b", "\u3392"
for c in SOME:
    t("U+%04X" % ord(c), lambda: [(db.category(c), db.bidirectional(c), db.combining(c), db.mirrored(c), db.east_asian_width(c), db.decomposition(c), db.decimal(c, None), db.digit(c, None), db.numeric(c, None), db.name(c, None), [db.normalize(f, c) for f in ("NFC", "NFD", "NFKC", "NFKD")]) for db in (u, old)])
t("what they are", lambda: [type(x).__name__ for x in (u.category("a"), u.combining("a"), u.mirrored("a"), u.decimal("1"), u.digit("1"), u.numeric("1"), u.numeric("\xbd"), u.name("a"), u.lookup("SPACE"), u.normalize("NFC", "a"), u.is_normalized("NFC", "a"), u.decomposition("a"))])
t("without a default", lambda: [attempt(f, c) for f in (u.decimal, u.digit, u.numeric, u.name, old.decimal, old.digit, old.numeric, old.name) for c in ("a" if f.__name__ != "name" else "\0", "\U0010ffff")])
t("whatever the default is", lambda: [f("\0", d) for f in (u.decimal, u.digit, u.numeric, u.name) for d in (None, 0, "x", [])])

print("---- how they are called")
for db in (u, old):
    for name in ("category", "bidirectional", "combining", "mirrored", "east_asian_width", "decomposition", "decimal", "digit", "numeric", "name"):
        f = getattr(db, name)
        t("%s of what is no character" % name, lambda: [attempt(f, *a) for a in ((), ("",), ("ab",), ("\U0001f600\U0001f600",), ("a" * 1000,), (5,), (None,), (b"a",), (["a"],), ("a", 1, 2), (type("S", (str,), {})("a"),), ("\U0001f600",), ("\ud83d",))] + [attempt(lambda: f(chr="a"))])
    for name in ("normalize", "is_normalized"):
        f = getattr(db, name)
        t("%s called otherwise" % name, lambda: [attempt(f, *a) for a in ((), ("NFC",), ("NFC", "a", "b"), ("nfc", "a"), ("", "a"), ("NFX", "a"), ("NFC ", "a"), ("NFKCC", "a"), ("NFX", ""), (5, ""), (5, "a"), ("NFC", 5), ("NFC", None), ("NFC", b"a"), (b"NFC", "a"), (None, None), ("NF\0C", "a"), (type("S", (str,), {})("NFC"), type("S", (str,), {})("e\u0301")))] + [attempt(lambda: f(form="NFC", unistr="a"))])
t("a str of a derived class comes back as a str", lambda: [type(u.normalize(f, type("S", (str,), {})(x))).__name__ for f in ("NFC", "NFD") for x in ("", "a", "\xe9", "e\u0301")])

print("---- names")
t("lookup", lambda: [attempt(u.lookup, n) for n in ("SPACE", "space", "Space", "LATIN SMALL LETTER A", "latin small letter a", "LATIN SMALL LETTER  A", " SPACE", "SPACE ", "", "A", "LATIN", "LATIN SMALL LETTER", "LATIN SMALL LETTER AA", "LATIN SMALL LETTER A WITH ACUTE", "GRINNING FACE", "NULL", "LINE FEED", "LF", "BEL", "BELL", "ALERT", "NO-BREAK SPACE", "NBSP", "ZWJ", "BOM", "BYTE ORDER MARK", "VS1", "VS256", "VS257", "LATIN CAPITAL LETTER GHA", "LATIN CAPITAL LETTER OI", "<control>", "<control-0000>", "U+0041", "0041", "\xe9", "\u4e00", "SPACE\0", "SPACE\0X", "\0", "x\0y")])
t("what is worked out", lambda: [attempt(u.lookup, n) for n in ("CJK UNIFIED IDEOGRAPH-4E00", "cjk unified ideograph-4e00", "CJK UNIFIED IDEOGRAPH-9FFF", "CJK UNIFIED IDEOGRAPH-A000", "CJK UNIFIED IDEOGRAPH-3400", "CJK UNIFIED IDEOGRAPH-33FF", "CJK UNIFIED IDEOGRAPH-4DBF", "CJK UNIFIED IDEOGRAPH-4DC0", "CJK UNIFIED IDEOGRAPH-20000", "CJK UNIFIED IDEOGRAPH-020000", "CJK UNIFIED IDEOGRAPH-2A6DF", "CJK UNIFIED IDEOGRAPH-2A6E0", "CJK UNIFIED IDEOGRAPH-323AF", "CJK UNIFIED IDEOGRAPH-323B0", "CJK UNIFIED IDEOGRAPH-", "CJK UNIFIED IDEOGRAPH-4E0", "CJK UNIFIED IDEOGRAPH-4E000", "CJK UNIFIED IDEOGRAPH-4E00 ", "CJK UNIFIED IDEOGRAPH-4E0G", "CJK UNIFIED IDEOGRAPH-04E00", "CJK UNIFIED IDEOGRAPH-1000000", "CJK UNIFIED IDEOGRAPH-110000", "CJK UNIFIED IDEOGRAPH-AC00", "CJK UNIFIED IDEOGRAPH", "CJK UNIFIED IDEOGRAPH-+E00", "TANGUT IDEOGRAPH-17000", "tangut ideograph-187f7", "TANGUT IDEOGRAPH-187F8", "TANGUT IDEOGRAPH-18D00", "TANGUT IDEOGRAPH-18D09", "TANGUT IDEOGRAPH-4E00", "TANGUT IDEOGRAPH-", "CJK COMPATIBILITY IDEOGRAPH-F900", "CJK COMPATIBILITY IDEOGRAPH-F8FF", "NUSHU CHARACTER-1B170", "KHITAN SMALL SCRIPT CHARACTER-18B00")])
t("Hangul", lambda: [attempt(u.lookup, n) for n in ("HANGUL SYLLABLE GA", "hangul syllable ga", "HANGUL SYLLABLE GAG", "HANGUL SYLLABLE HIH", "HANGUL SYLLABLE A", "HANGUL SYLLABLE AE", "HANGUL SYLLABLE GGWAELB", "HANGUL SYLLABLE SSYEONG", "HANGUL SYLLABLE ", "HANGUL SYLLABLE", "HANGUL SYLLABLE G", "HANGUL SYLLABLE GX", "HANGUL SYLLABLE GAX", "HANGUL SYLLABLE GA ", "HANGUL SYLLABLE  GA", "HANGUL SYLLABLE GAGG", "HANGUL SYLLABLE GAGGG", "HANGUL SYLLABLE YI", "HANGUL SYLLABLE I", "HANGUL SYLLABLE NG", "HANGUL SYLLABLE ANG", "HANGUL SYLLABLE GA\0", "HANGUL SYLLABLE GAS", "HANGUL SYLLABLE GASS", "HANGUL SYLLABLE WEO", "HANGUL SYLLABLE EO", "HANGUL CHOSEONG KIYEOK")])
t("every Hangul syllable", lambda: all(u.lookup(u.name(chr(c))) == chr(c) and old.lookup(u.name(chr(c)).lower()) == chr(c) for c in range(0xAC00, 0xD7A4)))
t("sequences", lambda: [attempt(db.lookup, n) for db in (u, old) for n in ("KEYCAP NUMBER SIGN", "keycap digit zero", "LATIN CAPITAL LETTER A WITH MACRON AND GRAVE", "LATIN SMALL LETTER I WITH DOT ABOVE AND ACUTE", "KATAKANA LETTER AINU P", "MODIFIER LETTER EXTRA-LOW EXTRA-HIGH CONTOUR TONE BAR", "BENGALI LETTER KHINYA", "TAMIL SYLLABLE KAU", "HIRAGANA LETTER BIDAKUON NGA")])
t("aliases were not there in 3.2.0", lambda: [attempt(old.lookup, n) for n in ("LF", "NULL", "BOM", "LATIN CAPITAL LETTER GHA", "SPACE", "GRINNING FACE", "CJK UNIFIED IDEOGRAPH-9FFF")])
t("too long", lambda: [attempt(u.lookup, "A" * n) for n in (255, 256, 257, 1000)] + [attempt(u.lookup, "\xe9" * n) for n in (128, 129)])
t("lookup of what is not a str", lambda: [attempt(u.lookup, *a) for a in ((), (5,), (None,), (b"SPACE",), (b"space",), (bytearray(b"SPACE"),), (memoryview(b"SPACE"),), (memoryview(bytearray(b"SPACE")),), (b"",), (b"\xff",), (b"\xe9x",), (["SPACE"],), ("SPACE", 1), ("\ud800",), (type("S", (str,), {})("SPACE"),), (memoryview(b"S.P.A.C.E")[::2],))] + [attempt(lambda: u.lookup(name="SPACE"))])

print("---- more than one character")
POOL = "aeAEoO\u0300\u0301\u0302\u0303\u0304\u0306\u0307\u0308\u030a\u030c\u0323\u0327\u0328\u0345\u05b0\u05b1\u05bc\u0e38\u0f71\u0f72\u0f74\u0f80\u1100\u1101\u1112\u1113\u1161\u1175\u1176\u11a7\u11a8\u11c2\u11c3\uac00\uac01\uac1c\u00c5\u212b\u1e0c\u1e0d\u1e69\u01d5\u0344\u0f73\u0f75\u0f81\u09c7\u09be\u09d7\u0b47\u0b56\u0b3e\u1025\u102e\u3099\u309a\u304b\u30cf\ufb01\ufdfa\u2126\u00b5\u017f\u1e9b\u2460\u3392\uff21\U0001d15e\U0001d165\U0001d16e\U0002f800\U00011099\U000110ba\U00011131\U00011127\U000114b9\U000114ba\ud800\U0010ffff\u0378"
state = 5


def some(most):
    global state
    out = []
    state = (state * 1103515245 + 12345) % 2 ** 31
    for _ in range(1 + state // 65536 % most):
        state = (state * 1103515245 + 12345) % 2 ** 31
        out.append(POOL[state // 65536 % len(POOL)])
    return "".join(out)


for label, db in (("now", u), ("3.2.0", old)):
    for form in ("NFC", "NFD", "NFKC", "NFKD"):
        state = 5
        strings = [some(12) for _ in range(6000)]
        t("%s %s at random" % (label, form), lambda: (digest("|".join(db.normalize(form, s) for s in strings)), digest("".join("01"[db.is_normalized(form, s)] for s in strings)), all(db.is_normalized(form, db.normalize(form, s)) for s in strings), all(db.normalize(form, db.normalize(form, s)) == db.normalize(form, s) for s in strings)))
MARKS = "\u0300\u0323\u0327\u0345\u0301\u0328\u05b0\u0e38\u0f71\u3099\u031b\u0334\u0338\u20d2\u1dce"
t("a great many marks", lambda: [digest(u.normalize(f, "a" + (MARKS * n)[::step] + "b" + MARKS[::-1] * n)) for f in ("NFC", "NFD", "NFKC", "NFKD") for n, step in ((1, 1), (2, 1), (3, -1), (50, 1), (50, -1), (2000, 3))])
t("many that go together", lambda: [ascii(u.normalize("NFC", s)) for s in ("a\u0308\u0304", "u\u0308\u0304\u0301", "o\u0328\u0304", "s\u0323\u0307", "s\u0307\u0323", "a\u0323\u0302\u0301", "e\u0327\u0306", "\u1100\u1161\u11a8\u11a8", "\u1100\u1161\u1161", "\uac00\u11a8", "\uac01\u11a8", "\u1100\uac00", "a\u0301" * 30, ("a" + "\u0323" * 25 + "\u0301") * 2, "\u0f71\u0f72\u0f74", "\u09c7\u09be\u09c7\u09d7", "\u0344", "\u0301", "\u0301a", "\u1112\u1175\u11c2", "\u1113\u1161", "\u1100\u1176", "\u1100\u1161\u11a7", "\u1100\u1161\u11c3")])
t("long, and in a form already", lambda: [(u.normalize(f, s) == s, u.is_normalized(f, s)) for f in ("NFC", "NFD", "NFKC", "NFKD") for s in ("a" * 100000, "\u4e00" * 100000, "\xe9" * 1000, "\U0001f600" * 1000)])
t("what grows", lambda: [(len(u.normalize(f, "\ufdfa" * 1000)), len(u.normalize(f, "\uac01" * 1000)), len(u.normalize(f, "\u1e9b\u0323" * 1000)), len(u.normalize(f, "\U0001d15e" * 1000))) for f in ("NFC", "NFD", "NFKC", "NFKD")])

print("---- where else it shows")
t("\\N in source", lambda: [attempt(eval, "'\\N{%s}'" % n) for n in ("SPACE", "space", "LATIN SMALL LETTER A WITH ACUTE", "GRINNING FACE", "LF", "bom", "LATIN CAPITAL LETTER GHA", "CJK UNIFIED IDEOGRAPH-4E00", "cjk unified ideograph-2a6df", "TANGUT IDEOGRAPH-17000", "HANGUL SYLLABLE GAG", "hangul syllable hih", "KEYCAP NUMBER SIGN", "NO SUCH NAME", "", " ", "SPACE ", "A" * 300, "\xe9", "VS17", "EGYPTIAN HIEROGLYPH-13460", "GARAY CAPITAL LETTER A", "LEAFLESS TREE")])
t("\\N in bytes", lambda: [attempt(b.decode, "unicode_escape") for b in (b"\\N{SPACE}", b"\\N{space}x", b"\\N{LF}", b"\\N{KEYCAP NUMBER SIGN}", b"\\N{HANGUL SYLLABLE GA}", b"\\N{CJK UNIFIED IDEOGRAPH-4E00}", b"\\N{NOPE}", b"\\N{}", b"\\N{SPACE", b"\\N", b"\\N{\xe9}", b"\\N{LEAFLESS TREE}")])
t("namereplace", lambda: ["a\xe9\u4e00\uac00\U0001f600\ud800\ue000\U000f0000\U000f01dc\U000f01dd\U000f0200\U000f03cc\U000f03cd\U00017000\U0010ffff\x80\u0378\U0001fabe".encode("ascii", "namereplace"), "\xe9\u0100".encode("latin-1", "namereplace")])
namespace = {}
t("what NFKC would make a name of, and is not one", lambda: [attempt(exec, s, {}) for s in ("x\u00b2 = 4", "\u00b2 = 4", "x\u2460 = 1", "\u00bd = 1", "x\u00a0y = 1")])
t("names in source are as NFKC makes them", lambda: (exec("\ufb01 = 1; \u212b = 2; \u00b5 = 3; e\u0301 = 5; \u1e9b\u0323 = 6; \U0001d465 = 7; \uff21 = 8; \u2126 = 9; \u017f = 10; \u1100\u1161 = 11; \u0132 = 12", namespace), sorted((ascii(k), v) for k, v in namespace.items() if k != "__builtins__")))
t("what is no name", lambda: [str(attempt(compile, s, "<s>", "exec")).split(" at ")[0] for s in ("\u2460 = 1", "a\u2460 = 1", "\u0301 = 1", "\U0001f600 = 1", "a\u037a = 1", "\u037a = 1", "\u309b = 1", "a\u309b = 1", "\ufe7f = 1")])
