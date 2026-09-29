# The module _sre: what a regular expression is compiled to, what it finds, and what is said when they are given the wrong thing.
import _sre
import re
import sys
from re import _constants as C


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


P = re.compile
Pattern, Match = type(P("")), type(P("").match(""))
Scanner, Template = type(P("").scanner("")), type(re._compile_template(P("(a)"), r"\1"))
print("---- what there is")
t("the module", lambda: (_sre.__name__, _sre.__package__, _sre.__loader__.__name__, _sre.__doc__, sorted(n for n in vars(_sre) if not n.startswith("__"))))
t("constants", lambda: (_sre.MAGIC, _sre.CODESIZE, _sre.MAXREPEAT, _sre.MAXGROUPS, _sre.copyright, _sre.getcodesize(), C.MAGIC == _sre.MAGIC))
for c in (Pattern, Match, Scanner, Template):
    t(c.__name__, lambda: (c.__module__, c.__qualname__, [b.__name__ for b in c.__mro__], sorted(vars(c)), c.__text_signature__, c.__basicsize__, c.__itemsize__, c.__dictoffset__, c.__weakrefoffset__ > 0, c.__flags__ & 0x7FFF, repr(c), c.__doc__))
    t("made or derived from", lambda: (attempt(c), attempt(c, "a"), attempt(c.__new__, c), attempt(object.__new__, c), attempt(type, "X", (c,), {}), attempt(setattr, c, "x", 1)))
    for name in sorted(vars(c)):
        if name not in ("__doc__", "__module__"):
            t("%s.%s" % (c.__name__, name), lambda: (type(vars(c)[name]).__name__, getattr(vars(c)[name], "__text_signature__", None), getattr(vars(c)[name], "__doc__", None)))
for name in ("compile", "template", "getcodesize", "ascii_iscased", "unicode_iscased", "ascii_tolower", "unicode_tolower"):
    t(name, lambda: (type(getattr(_sre, name)).__name__, getattr(_sre, name).__text_signature__, getattr(_sre, name).__doc__, getattr(_sre, name).__module__))

print("---- characters")
for f in (_sre.ascii_iscased, _sre.unicode_iscased, _sre.ascii_tolower, _sre.unicode_tolower):
    t(f.__name__, lambda: [attempt(f, c) for c in (0, 65, 97, 90, 91, 127, 128, 0xB5, 0xC0, 0xDF, 0xFF, 0x130, 0x131, 0x17F, 0x1C5, 0x3A3, 0x3C2, 0x1E9E, 0x212A, 0xFB05, 0x10400, 0x10FFFF, 0x110000, 2 ** 31 - 1, -1, -65, True)])
    t(f.__name__ + " of the wrong thing", lambda: [attempt(f, *a) for a in ((), ("a",), (1.5,), (None,), (2 ** 31,), (-2 ** 31 - 1,), (2 ** 100,), (1, 2))] + [attempt(lambda: f(character=1))])

print("---- a pattern")
p = P(r"(?P<first>a+)(b)?(?P<last>c)?", re.I)
t("what it has", lambda: (p.pattern, p.flags, p.groups, p.groupindex, type(p.groupindex).__name__, dict(p.groupindex), attempt(p.groupindex.__setitem__, "x", 1) if hasattr(p.groupindex, "__setitem__") else "no __setitem__"))
t("with no names", lambda: (P("(a)").groupindex, type(P("(a)").groupindex).__name__, P("(a)").groupindex is P("(a)").groupindex, P("").groups))
t("nothing can be set", lambda: [attempt(setattr, p, n, 1) for n in ("pattern", "flags", "groups", "groupindex", "x")] + [attempt(delattr, p, "pattern"), attempt(lambda: p.__dict__)])
for source, flags in (("a", 0), ("a", re.I), ("a", re.I | re.M | re.S | re.X), ("a", re.A), ("a", re.U), (b"a", 0), (b"a", re.L), (b"a", re.L | re.I), ("a" * 300, 0), ("'\"\n\\", 0), ("\xe9а\U0001F600" * 100, 0), ("a", re.DEBUG & 0), (b"\xff" * 100, re.S)):
    t("repr", lambda: repr(P(source, flags)))
t("repr of what is made by hand", lambda: [repr(_sre.compile(s, f, [C.SUCCESS], 0, {}, ())) for s, f in ((None, 0), ("a", 512), ("a", 1), ("a", 1 | 2 | 1024), ("a", -1), (b"a", 32), ("a", 32 | 256), ("a", 32 | 4))])
t("equal", lambda: (P("a") == P("a"), P("a") is P("a"), P("a") == P("b"), P("a") != P("b"), P("a") == P("a", re.I), P("a") == P(b"a"), P("a") == "a", P("a") != "a", P("a").__eq__("a"), P("a").__lt__(P("a")), attempt(lambda: P("a") < P("b")), P("a", re.U) == P("a")))
t("equal, of two that are not the same one", lambda: [(a == b, a != b, a is b, hash(a) == hash(b)) for a in [_sre.compile("a", 32, [C.LITERAL, 97, C.SUCCESS], 0, {}, ())] for b in [_sre.compile("a", 32, [C.LITERAL, 97, C.SUCCESS], 0, {}, ())]])
t("not equal, by the code alone", lambda: [(a == b, a != b) for a in [_sre.compile("a", 32, [C.LITERAL, 97, C.SUCCESS], 0, {}, ())] for b in [_sre.compile("a", 32, [C.LITERAL, 98, C.SUCCESS], 0, {}, ())]])
t("hash", lambda: (hash(P("a")) == hash(P("a")), isinstance(hash(P("a")), int), {P("a"): 1}[P("a")], attempt(hash, _sre.compile(bytearray(b"a"), 0, [C.SUCCESS], 0, {}, ()))))
t("copies", lambda: (p.__copy__() is p, p.__deepcopy__({}) is p, p.__deepcopy__(None) is p, attempt(p.__deepcopy__), attempt(p.__copy__, 1)))
t("generic", lambda: (repr(Pattern[str]), repr(Match[bytes]), Pattern[str].__origin__ is Pattern))
t("weakly referred to", lambda: (__import__("_weakref").ref(p)() is p, attempt(__import__("_weakref").ref, p.match("a"))))

print("---- how each is called")
METHODS = ("match", "fullmatch", "search", "findall", "finditer", "scanner")
for name in METHODS:
    f = getattr(P("b"), name)
    done = lambda r: list(r) if name == "finditer" else r
    t(name, lambda: [ascii(done(f(*a, **k))).replace(hex(id(0))[:4], "") if name != "scanner" else type(f(*a, **k)).__name__ for a, k in ((("abc",), {}), (("abc", 1), {}), (("abc", 2), {}), (("abc", 0, 1), {}), (("abc", 1, 2), {}), (("abc",), {"pos": 1}), (("abc",), {"endpos": 1}), ((), {"string": "abc", "pos": 1, "endpos": 3}),
                                                                                                                                                   (("abc", -5), {}), (("abc", 100), {}), (("abc", 0, -1), {}), (("abc", 0, 100), {}), (("abc", 2, 1), {}), (("abc", True, 2 ** 63 - 1), {}), (("abc", -2 ** 63), {}))])
    for a, k in (((), {}), ((5,), {}), ((None,), {}), (([],), {}), ((b"abc",), {}), (("abc", "x"), {}), (("abc", 1.5), {}), (("abc", None), {}), (("abc", 0, None), {}), (("abc", 2 ** 63), {}), (("abc", 0, -2 ** 63 - 1), {}), (("abc", 0, 1, 2), {}), (("abc",), {"other": 1}), (("abc",), {"string": "x"}), ((), {"pos": 1})):
        t("%s(*%r, **%r)" % (name, a, k), lambda: done(f(*a, **k)))
for name in ("sub", "subn"):
    f = getattr(P("b"), name)
    t(name, lambda: [f(*a, **k) for a, k in ((("X", "abcb"), {}), (("X", "abcb", 1), {}), (("X", "abcb", 0), {}), (("X", "abcb", 5), {}), (("X", "abcb"), {"count": 1}), ((), {"repl": "X", "string": "abcb", "count": 1}), (("X", "abcb", -1), {}), (("", "abcb"), {}), (("X", ""), {}), (("X", "aaa"), {}), (("X", "abcb", True), {}))])
    for a, k in (((), {}), (("X",), {}), (("X", 5), {}), (("X", None), {}), ((5, "abc"), {}), ((None, "abc"), {}), ((b"X", "abc"), {}), (("X", b"abc"), {}), (("X", "abc", "x"), {}), (("X", "abc", 1.5), {}), (("X", "abc", None), {}), (("X", "abc", 2 ** 63), {}), (("X", "abc", 1, 2), {}), (("X", "abc"), {"other": 1}), ((5, "aaa"), {}), ((b"X", "aaa"), {})):
        t("%s(*%r, **%r)" % (name, a, k), lambda: f(*a, **k))
t("split", lambda: [P("b").split(*a, **k) for a, k in ((("abcbd",), {}), (("abcbd", 1), {}), (("abcbd", 0), {}), (("abcbd", 5), {}), (("abcbd",), {"maxsplit": 1}), ((), {"string": "abcbd", "maxsplit": 1}), (("abcbd", -1), {}), (("",), {}), (("aaa",), {}))])
for a, k in (((), {}), ((5,), {}), ((b"a",), {}), (("a", "x"), {}), (("a", None), {}), (("a", 2 ** 63), {}), (("a", 1, 2), {}), (("a",), {"other": 1})):
    t("split(*%r, **%r)" % (a, k), lambda: P("b").split(*a, **k))

print("---- what can be looked through")


class S(str):
    pass


class B(bytes):
    pass


class Shows:
    def __init__(self, data): self.data = data
    def __buffer__(self, flags):
        log.append("__buffer__")
        return memoryview(self.data)

    def __release_buffer__(self, view):
        log.append("__release_buffer__")


log = []
t("a class derived from str", lambda: [(m.group(), type(m.group()).__name__, m.string is s, type(m.string).__name__, type(P("").match(s).group()).__name__, type(P(".*").match(s).group()).__name__, [type(x).__name__ for x in P("b").split(s)], type(P("z").sub("", s)).__name__, type(P("z").sub("", s)) is str, P("z").sub("", s) is s, [type(x).__name__ for x in P(".*").findall(s)]) for s in [S("abc")] for m in [P("b").search(s)]])
t("str itself", lambda: [(P(".*").match(s).group() is s, P("z").sub("", s) is s, P("z").split(s)[0] is s, P("abc").findall(s)[0] is s, P("b").search(s).string is s) for s in ["abc"]])
t("bytes itself", lambda: [(P(b".*").match(s).group() is s, P(b"z").sub(b"", s) is s, P(b"z").split(s)[0] is s, P(b"abc").findall(s)[0] is s) for s in [b"abc"]])
t("a class derived from bytes", lambda: [(m.group(), type(m.group()).__name__, m.string is s, type(P(b".*").match(s).group()).__name__, type(P(b"z").sub(b"", s)).__name__) for s in [B(b"abc")] for m in [P(b"b").search(s)]])
for make in (bytearray, memoryview, lambda b: memoryview(bytearray(b)), lambda b: memoryview(b)[1:], lambda b: memoryview(b"x" + b).cast("B")[1:]):
    t("bytes of another kind", lambda: [(m.group(), type(m.group()).__name__, m.span(), m.string is s, P(b"b").findall(s), P(b"b").split(s), P(b"b").sub(b"X", s), type(P(b"z").sub(b"X", s)).__name__, P(b"").sub(b"-", s), [x.span() for x in P(b".").finditer(s)]) for s in [make(b"abcb")] for m in [P(b"b").search(s)]])
t("what is not one after another", lambda: attempt(P(b"a").match, memoryview(b"abcd")[::2]))
t("what has been released", lambda: [(v.release(), attempt(P(b"a").match, v)) for v in [memoryview(b"abc")]])
t("of more than a byte each", lambda: [(m.group(), m.span()) for m in [P(b"\x01").search(memoryview(b"\0\0\x01\0").cast("H"))]])
t("a class that shows bytes", lambda: [(log.clear(), m := P(b"b").search(s), log[:], m.span(), log.clear(), m.group(), log[:], m.string is s, log.clear(), P(b"b").findall(s), log[:], log.clear(), list(P(b"b").finditer(s)) and None, log[:]) for s in [Shows(b"abcb")]])
t("the wrong one for the pattern", lambda: (attempt(P("a").match, b"a"), attempt(P(b"a").match, "a"), attempt(P("a").match, bytearray(b"a")), attempt(P(b"a").sub, b"", "a"), attempt(P("a").split, b"a"), attempt(P("a").scanner, b"a"), attempt(P("a").finditer, b"a")))
t("a pattern compiled from None does for either", lambda: [(c.match("a").span(), c.match(b"a").span(), c.pattern, c.match("a").re is c) for c in [_sre.compile(None, 0, [C.LITERAL, 97, C.SUCCESS], 0, {}, ())]])
t("changed afterwards", lambda: [(m.group(), s.__setitem__(1, 66), m.group(), s.clear(), m.group(), m.span(), m.groups(), repr(m)) for s in [bytearray(b"abc")] for m in [P(b"(b)c").search(s)]])

print("---- characters of every width")
WIDE = "a\xe9а\U0001F600b\U00010400\ud800c"
t("one at a time", lambda: (P(".").findall(WIDE), [m.span() for m in P(".").finditer(WIDE)], len(WIDE), P("(?s).*").match(WIDE).span(), P("(?s).*").match(WIDE).group() is WIDE))
t("where things are", lambda: [(m.span(), m.group(), m.start(1), m.end(1), m.pos, m.endpos, repr(m)) for m in (P("(b)").search(WIDE), P("(\U00010400)").search(WIDE), P("(.)c").search(WIDE), P("(.)").search(WIDE, 3), P("(.)").search(WIDE, 4, 5), P("(.)$").search(WIDE, 0, 4), P("[\U0001F600-\U0001F64F](.)").search(WIDE))])
t("half of one", lambda: (P("\ud83d").search("\U0001F600"), P("\ude00").search("\U0001F600"), P("\ud83d").search("\ud83dx").span(), P("[\ud800-\udfff]").findall("a\U0001F600\ud800b\udc00"), P(".").findall("\udc00\ud800")))
t("split and replaced", lambda: (P("\U0001F600").split(WIDE), P("[^a-c]").sub("-", WIDE), P("").sub("|", WIDE), P("(.)").sub(r"\1\1", WIDE), P(".").sub(lambda m: str(m.start()), WIDE), P("(?<=\U0001F600)b").search(WIDE).span(), P("(?<!\U0001F600)b").search(WIDE), P(r"\b").sub("|", WIDE), P(r"(.)\1").search("x\U0001F600\U0001F600y").span()))
t("in either case", lambda: (P("\U00010428", re.I).search(WIDE).span(), P("[\U00010428-\U0001044f]", re.I).findall(WIDE), P("А", re.I).findall(WIDE), P("\xc9", re.I).findall(WIDE), P("\xc9", re.I | re.A).findall(WIDE), P("k", re.I).findall("KKk"), P("ſ", re.I).findall("sSſ"), P("[ﬅ]", re.I).findall("ﬅﬆ"), P("σ", re.I).findall("Σσς"), P("\xdf", re.I).findall("\xdfẞSS"), P("i", re.I).findall("iIİı")))
t("a long one, over and over", lambda: [sum(P(r"\w+|\W").match(text, i).end() - i for i in range(0, len(text), 7)) for text in [("word \U0001F600 " * 400)]])
t("the same, and a great many of them", lambda: [len(P(".").findall("\U0001F600" * 5000)), P("\U0001F600+$").search("\U0001F600" * 5000).span(), P("(?:\U0001F600\U0001F601)*").match("\U0001F600\U0001F601" * 3000).span()])

print("---- a match")
m = P(r"(?P<first>a+)(b)?(?P<last>c)?").search("xxaacyy", 1, 6)
t("what it has", lambda: (m.string, m.re.pattern, m.pos, m.endpos, m.lastindex, m.lastgroup, m.regs, m.regs is m.regs, repr(m), bool(m), m.__copy__() is m, m.__deepcopy__({}) is m))
t("nothing can be set", lambda: [attempt(setattr, m, n, 1) for n in ("string", "re", "pos", "endpos", "lastindex", "lastgroup", "regs", "x")] + [attempt(lambda: m.__dict__), attempt(hash, m) == hash(m), attempt(len, m), attempt(iter, m), attempt(reversed, m), attempt(lambda: "a" in m), attempt(list, m), attempt(lambda: [*m]), attempt(m.__setitem__, 0, 1) if hasattr(m, "__setitem__") else "no __setitem__", m == m, m == P("a").match("a")])
t("group", lambda: (m.group(), m.group(0), m.group(1), m.group(2), m.group(3), m.group("first"), m.group("last"), m.group(1, 2, 3), m.group(0, "first", 1), m.group(True), m[0], m[1], m[2], m["first"], m["last"]))


class Index:
    def __init__(self, v): self.v = v
    def __index__(self):
        if isinstance(self.v, BaseException): raise self.v
        return self.v


for g in (4, -1, "other", "", None, 1.5, b"first", (1,), [], 2 ** 63, -2 ** 63, 2 ** 100, Index(1), Index(4), Index(2 ** 100), Index(ValueError("no")), S("first")):
    label = "Index" if isinstance(g, Index) else repr(g)
    t("group %s" % label, lambda: [attempt(f, g) for f in (m.group, m.__getitem__, m.start, m.end, m.span)] + [attempt(m.group, 1, g), attempt(lambda: m[g])])
t("how they are called", lambda: (attempt(lambda: m.group(group=1)), attempt(lambda: m.start(group=1)), attempt(m.start, 1, 2), attempt(m.span, 1, 2), attempt(m.__getitem__), attempt(m.__getitem__, 1, 2), attempt(m.groups, 1, 2), attempt(lambda: m.groups(other=1)), attempt(m.groupdict, 1, 2), attempt(m.expand), attempt(m.expand, 1, 2), attempt(lambda: m.expand(other=1))))
t("start, end and span", lambda: ([(m.start(g), m.end(g), m.span(g)) for g in (0, 1, 2, 3, "first", "last")], m.start(), m.end(), m.span()))
t("groups", lambda: (m.groups(), m.groups(None), m.groups("-"), m.groups(default=5), P("a").match("a").groups(), P("(a)|(b)").match("b").groups(), P("(a)|(b)").match("b").groups("")))
t("groupdict", lambda: (m.groupdict(), m.groupdict("-"), m.groupdict(default=5), P("(a)").match("a").groupdict(), type(m.groupdict()).__name__, m.groupdict() is m.groupdict(), list(P("(?P<b>.)(?P<a>.)").match("xy").groupdict())))
t("lastindex and lastgroup", lambda: [(x.lastindex, x.lastgroup) for x in (P("a").match("a"), P("(a)").match("a"), P("(a)(b)?").match("a"), P("((a)(b))").match("ab"), P("(?P<x>a)(?P<y>b)?").match("ab"), P("(?P<x>a)(b)").match("ab"), P("(a)|(?P<y>b)").match("b"), P("(?P<x>(?P<y>a))").match("a"), P("(a)*").match("b"), P("(?:(a)|b)*").match("ab"))])
t("repr", lambda: [repr(x) for x in (P("").match(""), P(".*").match("a" * 100), P(b".*").match(b"a" * 100), P("(?s).*").match("'\"\n"), P(".*").match("\xe9\U0001F600" * 40), P("b").search(S("abc")), P(b"b").search(bytearray(b"abc")))])
t("expand", lambda: (m.expand(r"\1"), m.expand(r"[\1|\g<first>|\g<1>|\g<0>]"), m.expand(""), m.expand("plain"), m.expand(r"\n\t\\"), m.expand(template=r"\3"), attempt(m.expand, r"\2"), attempt(m.expand, r"\4"), attempt(m.expand, r"\g<other>"), attempt(m.expand, r"\g<"), attempt(m.expand, "\\"), attempt(m.expand, b"a"), attempt(m.expand, 5), attempt(m.expand, None), m.expand(S(r"\1")), type(m.expand(S("plain"))).__name__))
t("expand, of bytes", lambda: [(x.expand(rb"[\1]"), x.expand(b"plain"), x.expand(bytearray(rb"[\1]")), x.expand(memoryview(rb"[\1]")), type(x.expand(bytearray(b"plain"))).__name__, attempt(x.expand, "a")) for x in [P(b"(b)").search(b"abc")]])

print("---- replacing")
t("with a template", lambda: (P("(a)(b)?").sub(r"[\1\2]", "ab a"), P("(?P<x>a)").sub(r"\g<x>\g<x>", "aa"), P("a").sub(r"\\", "aa"), P("a").sub(r"\n", "a"), P("(a)").sub(r"\g<1>0", "a"), P("(a)").sub(r"\10" if False else r"\g<1>", "a"), P("a").sub("\\\\1", "a"), attempt(P("a").sub, r"\1", "a"), attempt(P("a").sub, r"\g<x>", "a"), attempt(P("a").sub, r"\q", "a"), attempt(P("a").sub, "\\", "a"), P("z").sub(r"\1" if False else "x", "a")))
t("a template is looked at even if nothing is found", lambda: attempt(P("z").sub, r"\1", "a"))
t("with a great many parts", lambda: P("(a)(b)(c)").sub(r"\1-\2-\3-\1-\2-\3-\1-\2-\3-\1-\2-\3", "abc"))
t("with a function", lambda: (P("a").sub(lambda m: "X", "aba"), P("a").sub(lambda m: None, "aba"), P("a").sub(lambda m: "", "aba"), P("(a)").sub(lambda m: m.group(1) * 2, "aba"), attempt(P("a").sub, lambda m: 5, "aba"), attempt(P("a").sub, lambda m: b"X", "aba"), attempt(P("a").sub, lambda m: 1 / 0, "aba"), attempt(P("a").sub, lambda: "X", "aba"), P("z").sub(lambda m: 5, "aba"), P("a").sub(str.upper if False else (lambda m: m.group().upper()), "aba"), P("a").sub(S, "a")[:10]))
t("with a class derived from str", lambda: (P("a").sub(S("X"), "aba"), type(P("a").sub(S("X"), "aba")).__name__, P("(a)").sub(S(r"[\1]"), "aba"), type(P("a").sub(S("X"), "a")).__name__))
t("bytes", lambda: (P(b"a").sub(b"X", b"aba"), P(b"(a)").sub(rb"[\1]", b"aba"), P(b"a").sub(bytearray(b"X"), b"aba"), P(b"(a)").sub(bytearray(rb"[\1]"), b"aba"), P(b"(a)").sub(memoryview(rb"[\1]"), b"aba"), P(b"a").sub(lambda m: b"X", bytearray(b"aba")), type(P(b"a").sub(bytearray(b"X"), b"a")).__name__, attempt(P(b"a").sub, lambda m: "X", b"aba"), attempt(P(b"a").sub, "X", b"aba"), attempt(P(b"a").sub, "\\1", b"aba")))
t("nothing at all", lambda: (P("").sub("-", "abc"), P("").sub("-", ""), P("x*").sub("-", "abxd"), P("").subn("-", "abc"), P("a|").sub("-", "baac"), P(r"\b").sub("|", "ab cd"), P("(?=b)").sub("-", "abab"), P("").sub("-", "abc", 2), P("^").sub("-", "a\nb"), P("(?m)^").sub("-", "a\nb"), P("$").sub("-", "a\n"), P("(?m)$").sub("-", "a\nb\n")))
t("what re keeps", lambda: (re._compile_template(P("(a)"), r"\1") is re._compile_template(P("(a)"), r"\1"), type(re._compile_template(P("a"), "x")).__name__))

print("---- one after another")
t("findall", lambda: (P("a").findall("aba"), P("(a)").findall("aba"), P("(a)(b)?").findall("aba"), P("(a)|(b)").findall("ab"), P("").findall("ab"), P("a*").findall("baac"), P("(a)*").findall("baac"), P("()").findall("ab"), P("()()").findall("a"), P("a").findall(""), P("(?:a)").findall("aa"), P("((a)b)").findall("abab")))
t("split", lambda: (P(",").split("a,b,,c"), P("(,)").split("a,b"), P("(,)|(;)").split("a,b;c"), P("").split("abc"), P("x*").split("axbc"), P(r"\b").split("ab cd"), P("(?=b)").split("abab"), P(",").split(""), P(",").split(","), P("()").split("ab"), P(",").split("a,b,c", 1), P("(a)(b)?").split("xay")))
t("finditer", lambda: [(type(it).__name__, iter(it) is it, [x.span() for x in it], list(it), attempt(next, it)) for it in [P("a").finditer("aba")]])
t("it goes on from where it was", lambda: [(next(it).span(), next(it).span(), attempt(next, it)) for it in [P("a|").finditer("ab")]])
t("a scanner", lambda: [(s.pattern.pattern, s.search().span(), s.match(), s.search(), s.match(), attempt(setattr, s, "pattern", 1), attempt(s.search, 1), attempt(lambda: s.match(x=1))) for s in [P("a").scanner("baa")]])
t("match and search by turns", lambda: [(s.match().span(), s.search().span(), s.match().span(), s.match(), s.search()) for s in [P("a").scanner("abaab")]])
t("of nothing", lambda: [([x.span() for x in iter(s.search, None)], s.search()) for s in (P("").scanner(""), P("").scanner("ab"), P("").scanner(b"") if False else P(b"").scanner(b""), P("").scanner("ab", 1), P("").scanner("ab", 2), P("").scanner("ab", 5), P("").scanner("ab", 1, 1), P("a").scanner(""))])

print("---- code made by hand")
OK = [C.LITERAL, 97, C.SUCCESS]
t("compile", lambda: [(c.match("a").span(), c.pattern, c.flags, c.groups, c.groupindex) for c in (_sre.compile("x", 0, OK, 0, {}, ()), _sre.compile(pattern="x", flags=0, code=OK, groups=0, groupindex={}, indexgroup=()), _sre.compile("x", True, OK, False, {}, ()), _sre.compile(S("x"), 0, OK, 0, {}, ()))])
for a in ((), ("x",), ("x", 0, OK, 0, {}), ("x", 0, OK, 0, {}, (), 1), ("x", "0", OK, 0, {}, ()), ("x", 1.5, OK, 0, {}, ()), ("x", 2 ** 31, OK, 0, {}, ()), ("x", 0, tuple(OK), 0, {}, ()), ("x", 0, None, 0, {}, ()), ("x", 0, OK, "0", {}, ()), ("x", 0, OK, 2 ** 63, {}, ()), ("x", 0, OK, 0, [], ()), ("x", 0, OK, 0, None, ()), ("x", 0, OK, 0, {}, []), ("x", 0, OK, 0, {}, None),
          (5, 0, OK, 0, {}, ()), ([], 0, OK, 0, {}, ()), ("x", 0, ["a"], 0, {}, ()), ("x", 0, [1.5], 0, {}, ()), ("x", 0, [None], 0, {}, ()), ("x", 0, [-1], 0, {}, ()), ("x", 0, [2 ** 32], 0, {}, ()), ("x", 0, [2 ** 64], 0, {}, ()), ("x", 0, [2 ** 32 - 1], 0, {}, ()), ("x", 0, [], 0, {}, ()), ("x", 0, OK, -1, {}, ()), ("x", 0, OK, _sre.MAXGROUPS + 1, {}, ()), (5, 0, ["a"], 0, {}, ()), (5, 0, [], 0, {}, ())):
    t("compile%s" % ascii(a), lambda: type(_sre.compile(*a)).__name__)
t("what the names are kept as", lambda: [(c.groupindex, c.match("a").lastgroup, c.match("a").groupdict(), c.match("a").group("n")) for c in [_sre.compile("x", 0, [C.MARK, 0, C.LITERAL, 97, C.MARK, 1, C.SUCCESS], 1, {"n": 1}, (None, "n"))]])
t("names and no tuple of them", lambda: [(c.match("a").lastgroup, c.match("a").lastindex, c.match("a").groupdict()) for c in [_sre.compile("x", 0, [C.MARK, 0, C.LITERAL, 97, C.MARK, 1, C.SUCCESS], 1, {"n": 1}, ())]])
t("a tuple and no names", lambda: [(c.match("a").lastgroup, c.groupindex) for c in [_sre.compile("x", 0, [C.MARK, 0, C.LITERAL, 97, C.MARK, 1, C.SUCCESS], 1, {}, (None, "n"))]])
t("names that are wrong", lambda: [(attempt(x.group, "n"), attempt(x.group, "s"), attempt(x.group, "big"), attempt(x.groupdict)) for c in [_sre.compile("x", 0, OK, 0, {"n": 5, "s": "a", "big": 2 ** 100}, ())] for x in [c.match("a")]])
BAD = {
    "no SUCCESS at the end": [C.LITERAL, 97],
    "an argument is missing": [C.LITERAL, C.SUCCESS] and [C.SUCCESS, C.LITERAL],
    "no such thing": [999, C.SUCCESS],
    "a mark too many": [C.MARK, 0, C.SUCCESS],
    "AT what": [C.AT, 99, C.SUCCESS],
    "a set with no end": [C.IN, 3, C.LITERAL, 97, C.SUCCESS],
    "a set that skips too far": [C.IN, 99, C.LITERAL, 97, C.FAILURE, C.SUCCESS],
    "a category that there is not": [C.IN, 4, C.CATEGORY, 99, C.FAILURE, C.SUCCESS],
    "what is not for a set": [C.IN, 3, C.ANY, C.FAILURE, C.SUCCESS],
    "a bitmap that is too short": [C.IN, 4, C.CHARSET, 0, C.FAILURE, C.SUCCESS],
    "a jump by itself": [C.JUMP, 1, C.SUCCESS],
    "a branch to two places": [C.BRANCH, 5, C.LITERAL, 97, C.JUMP, 7, 5, C.LITERAL, 98, C.JUMP, 3, 0, C.ANY, C.SUCCESS],
    "least is more than most": [C.REPEAT_ONE, 6, 2, 1, C.LITERAL, 97, C.SUCCESS, C.SUCCESS],
    "a repeat with no UNTIL": [C.REPEAT, 5, 0, 1, C.LITERAL, 97, C.SUCCESS, C.SUCCESS],
    "a group that there is not": [C.GROUPREF, 0, C.SUCCESS],
    "info with flags that there are not": [C.INFO, 4, 8, 0, 0, C.SUCCESS],
    "info with a prefix and a set": [C.INFO, 4, 5, 0, 0, C.SUCCESS],
    "info that is literal with no prefix": [C.INFO, 4, 2, 0, 0, C.SUCCESS],
    "info with a prefix that is too long": [C.INFO, 6, 1, 1, 1, 9, 0, C.SUCCESS],
    "info that ends in the wrong place": [C.INFO, 5, 0, 0, 0, 0, C.SUCCESS],
    "an assertion with no SUCCESS": [C.ASSERT, 4, 0, C.LITERAL, 97, C.SUCCESS],
    "SUBPATTERN": [C.SUBPATTERN, C.SUCCESS],
    "RANGE outside a set": [C.RANGE, 97, 98, C.SUCCESS],
}
for label, code in BAD.items():
    t(label, lambda: type(_sre.compile("x", 0, [int(c) for c in code], 0, {}, ())).__name__)
GOOD = {
    "a set": ([C.IN, 4, C.LITERAL, 97, C.FAILURE, C.SUCCESS], 0),
    "a set that is turned about": ([C.IN, 5, C.NEGATE, C.LITERAL, 97, C.FAILURE, C.SUCCESS], 0),
    "a range": ([C.IN, 5, C.RANGE, 97, 99, C.FAILURE, C.SUCCESS], 0),
    "a bitmap": ([C.IN, 11, C.CHARSET, 0, 0, 0, 2, 0, 0, 0, 0, C.FAILURE, C.SUCCESS], 0),
    "a branch": ([C.BRANCH, 5, C.LITERAL, 97, C.JUMP, 7, 5, C.LITERAL, 98, C.JUMP, 2, 0, C.SUCCESS], 0),
    "one repeated": ([C.REPEAT_ONE, 6, 1, _sre.MAXREPEAT, C.LITERAL, 97, C.SUCCESS, C.SUCCESS], 0),
    "as few as will do": ([C.MIN_REPEAT_ONE, 6, 0, _sre.MAXREPEAT, C.LITERAL, 97, C.SUCCESS, C.LITERAL, 97, C.SUCCESS], 0),
    "a group repeated": ([C.REPEAT, 9, 0, _sre.MAXREPEAT, C.MARK, 0, C.LITERAL, 97, C.MARK, 1, C.MAX_UNTIL, C.SUCCESS], 1),
    "marks the wrong way about": ([C.MARK, 1, C.LITERAL, 97, C.MARK, 0, C.SUCCESS], 1),
    "half a group": ([C.MARK, 0, C.LITERAL, 97, C.SUCCESS], 1),
    "the second and not the first": ([C.MARK, 2, C.LITERAL, 97, C.MARK, 3, C.SUCCESS], 2),
    "FAILURE": ([C.FAILURE, C.SUCCESS], 0),
    "SUCCESS in the middle": ([C.LITERAL, 97, C.SUCCESS, C.LITERAL, 98, C.SUCCESS], 0),
    "UNTIL with no REPEAT": ([C.MAX_UNTIL, C.SUCCESS], 0),
    "a literal that no character is": ([C.LITERAL, 0x110000, C.SUCCESS], 0),
    "anything but that": ([C.NOT_LITERAL, 2 ** 32 - 1, C.SUCCESS], 0),
    "by the locale": ([C.LITERAL_LOC_IGNORE, 97, C.SUCCESS], 0),
    "a word by the locale": ([C.IN, 4, C.CATEGORY, C.CATEGORY_LOC_WORD, C.FAILURE, C.SUCCESS], 0),
}
for label, (code, groups) in GOOD.items():
    t(label, lambda: [attempt(lambda: (lambda x: x and (x.span(), x.groups(), x.regs))(_sre.compile(None, 0, [int(c) for c in code], groups, {}, ()).search(s))) for s in ("aab", "bA", "", "аa", "\U0001F600a", b"aab", b"\xe9A_")])
t("template", lambda: [type(_sre.template(P("(a)"), x)).__name__ for x in ([""], ["a"], ["a", 1, "b"], ["", 1, ""], [b"a", 1, b""], ["a", 0, "b", 1, "c"], [None], [5, 1, 6], ["a", True, "b"], ["a", 99, "b"])])
for a in ((), (P("a"),), (P("a"), [""], 1), (P("a"), ()), (P("a"), None), (P("a"), "a"), (P("a"), []), (P("a"), ["a", 1]), (P("a"), ["a", "x", "b"]), (P("a"), ["a", 1.5, "b"]), (P("a"), ["a", -1, "b"]), (P("a"), ["a", 2 ** 63, "b"]), (P("a"), ["a", None, "b"]), (None, ["a"]), (5, ["a", 1, "b"])):
    t("template%s" % ascii(tuple("P" if isinstance(x, Pattern) else x for x in a)), lambda: type(_sre.template(*a)).__name__)

print("---- what takes a long time, and what goes deep")
t("deep", lambda: (P("(?:a|b)*").match("ab" * 50000).span(), P("(a|b)*?c").match("ab" * 20000 + "c").span(), P("(?:(?:a)*)*b").match("a" * 20 + "b").span(), P("(a*)*").match("a" * 1000).span(), P("(?:a?){12}a{12}").match("a" * 12).span(), P("((a))+" ).match("a" * 30000).span(), P(".*?x").search("a" * 100000 + "x").span(), P("(?s)(.)*").match("ab\n" * 10000).span()))
t("many groups", lambda: [(c.groups, c.match("a" * 500).lastindex, len(c.match("a" * 500).groups()), c.match("a" * 500).span(500)) for c in [P("(a)" * 500)]])
t("by the locale", lambda: (P(rb"\w+", re.L).findall(b"ab_1 \xe9\xff"), P(b"A", re.L | re.I).findall(b"aA\xc0\xe0"), P(b"[a-c]", re.L | re.I).findall(b"aBd"), P(rb"\b", re.L).sub(b"|", b"ab \xe9"), P(b"\xe9", re.L | re.I).findall(b"\xc9\xe9"), P(rb"(a)\1", re.L | re.I).findall(b"aA")))
