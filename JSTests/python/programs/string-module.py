# The module _string, which goes through a format as str.format() does, and string.Formatter, which is written in Python over it.
import _string
import string


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


# As far as it goes, and then what went wrong
def all_of(iterator):
    out = []
    try:
        for x in iterator:
            out.append(x)
    except BaseException as e:
        out.append(show(e))
    return out


parse, split = _string.formatter_parser, _string.formatter_field_name_split
t("the module", lambda: (_string.__name__, _string.__package__, _string.__loader__.__name__, _string.__doc__, sorted(n for n in vars(_string) if not n.startswith("__"))))
for f in (parse, split):
    t(f.__name__, lambda: (type(f).__name__, f.__text_signature__, f.__doc__, f.__module__))
    t(f.__name__ + " of the wrong thing", lambda: [attempt(f, *a, **k) for a, k in (((), {}), ((5,), {}), ((None,), {}), ((b"a",), {}), (("a", "b"), {}), ((), {"string": "a"}), (([],), {}))])
for c in (type(parse("")), type(split("")[1])):
    t(c.__name__, lambda: (c.__module__, c.__qualname__, [b.__name__ for b in c.__mro__], sorted(vars(c)), c.__doc__, c.__basicsize__, c.__flags__ & 0x7FFF, repr(c), attempt(c), attempt(type, "X", (c,), {}), attempt(setattr, c, "x", 1)))
t("what they are", lambda: [(iter(x) is x, attempt(setattr, x, "y", 1), next(x), attempt(next, x), attempt(next, x)) for x in (parse("a"), split("a.b")[1])])
FORMATS = ("", "a", "abc", "{}", "{0}", "{a}", "a{}b", "{}{}", "{{", "}}", "{{}}", "a{{b}}c", "{{{}}}", "{{{{", "{a!r}", "{a!s}", "{a!x}", "{a:}", "{a:>10}", "{a!r:>10}", "{!r}", "{:x}", "{!r:}", "{a:{b}}", "{a:{b}{c}}", "{a:{b:{c}}}", "{a.b}", "{a[b]}", "{a[0]}", "{a.b[c].d}", "{a[}]}", "{a[:]}", "{a[!]}", "{a[{]}", "{a[]]}",
           "{", "}", "a{", "a}", "{a", "{a!", "{a!r", "{a!rx}", "{a!r:", "{a:", "{a:{", "{a:{}", "{a:}}", "{a{b}", "{a}}", "{}}", "}{", "{a!}", "{a!:}", "{a! }", "{a!\xe9}", "{a!\U0001F600}", "{\xe9}", "\xe9{\U0001F600!\u4e2d:\xe9}\U0001F600", "{ }", "{ a }", "{a }", "{\n}", "{a:\n}", "{0!r:{1}}{2}", "{a[b}", "{a[b]c}", "{:{}}", "{:{{}}}", "{a:{{}", "{a:}{", "{[}", "{[]}", "{.}", "{.a}", "{a.}", "{a..b}", "{0.}", "{a:b!r}", "{a:!r}")
for text in FORMATS:
    t("parse %s" % ascii(text), lambda: all_of(parse(text)))
NAMES = ("", "a", "0", "10", "007", "a.b", "a[b]", "a[0]", "a[10]", "a.b.c", "a[b][c]", "a.b[c].d[0]", "0.a", "0[a]", ".a", "[a]", ".", "[", "]", "a.", "a[", "a[]", "a[b", "a]", "a[b]c", "a[b].", "a..b", "a.[b]", "a[.]", "a[[]", "a[]]", "a[b]]", "a[ 0]", "a[0 ]", "a[-1]", "a[+1]", "a[1.5]", "a[\u0663]", "\u0663", "\u0663\u0664.a", "a[\U0001D7D9]", "-1", "1a", "a1", " ", " a", "\xe9.\xe9[\xe9]", "\U0001F600[\U0001F600].\U0001F600",
         "9223372036854775807", "9223372036854775808", "a[9223372036854775807]", "a[9223372036854775808]", "99999999999999999999999", "a[99999999999999999999999]", "a.0", "a.b c", "a[b c]", "a[b.c]", "a.b[", "a!r", "a:b", "a{b}")
for text in NAMES:
    t("split %s" % ascii(text), lambda: [(first, all_of(rest)) for first, rest in [split(text)]])


class S(str):
    pass


t("the whole of a str is that str", lambda: [(next(parse(s))[0] is s, split(s)[0] is s, next(parse("{" + s + "}"))[1] is s, next(parse(s + "{{"))[0] == s + "{") for s in ["abcdef"]])
t("a class derived from str", lambda: [(all_of(parse(s)), type(next(parse(s))[0]).__name__, split(S("a.b"))[0], type(split(S("ab"))[0]).__name__, next(parse(S("ab")))[0] is not None) for s in [S("a{b}")]])
t("what each is", lambda: [[type(x).__name__ for x in item] for item in parse("a{b!r:c}{}{{")] + [[type(x).__name__ for x in item] for item in split("a.b[c][0]")[1]] + [type(split("0")[0]).__name__, type(split("a")[0]).__name__, type(split("")[0]).__name__])

print("---- and it is what format() goes by")
VALUES = {"a": {"b": 1, "}": 2, ":": 3, "!": 4, "{": 5, "]": 6, "0": 7, 0: 8, "b c": 9, "b.c": 10}, "b": 5, "c": "<", "\xe9": 11}
for text in FORMATS:
    t("format %s" % ascii(text), lambda: (attempt(text.format, 1, 2, 3, **VALUES), attempt(text.format_map, VALUES), attempt(string.Formatter().format, text, 1, 2, 3, **VALUES)))

print("---- string.Formatter")
F = string.Formatter()
t("parse", lambda: [list(F.parse(x)) for x in ("a{b!r:c}d", "", "{}", "{{}}")])
t("get_field", lambda: (F.get_field("a[b]", (), VALUES), F.get_field("0", (5,), {}), F.get_field("0.real", (5,), {}), attempt(F.get_field, "a[z]", (), VALUES), attempt(F.get_field, "z", (), {}), attempt(F.get_field, "1", (5,), {}), attempt(F.get_field, "", (5,), {})))
t("vformat", lambda: (F.vformat("{0}{a}", (1,), {"a": 2}), attempt(F.vformat, "{}{0}", (1,), {}), attempt(F.vformat, "{0}{}", (1,), {}), F.vformat("{}{}", (1, 2), {}), attempt(F.vformat, "{:{:{:{}}}}", (1, 2, 3, 4), {})))


class Upper(string.Formatter):
    def get_value(self, key, args, kwargs): return "<%s>" % (key,)
    def convert_field(self, value, conversion): return value.upper() if conversion == "u" else super().convert_field(value, conversion)
    def format_field(self, value, spec): return "[%s|%s]" % (value, spec)


t("derived from", lambda: (Upper().format("{a}{0!u:x}{b.c!r}{}"), attempt(Upper().format, "{a!z}")))
t("Template", lambda: (string.Template("$a ${b} $$ $c").safe_substitute(a=1, b=2), attempt(string.Template("$a $b").substitute, a=1), string.Template("$a").get_identifiers(), string.Template("$").is_valid(), string.capwords("a b  c")))
