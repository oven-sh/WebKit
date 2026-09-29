# The module _tokenize, which gives a program the tokens that the compiler's own scanner makes out, and tokenize, which is written in Python over it.
import _tokenize
import io
import tokenize
import warnings

T = _tokenize.TokenizerIter


def show(e):
    return type(e).__name__ + ": " + ascii(e.args) + " " + ascii([getattr(e, n, None) for n in ("filename", "lineno", "offset", "text", "end_lineno", "end_offset")] if isinstance(e, SyntaxError) else "")


def attempt(f, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return show(e)


def t(label, f):
    r = attempt(f)
    print(label, "=>", r if isinstance(r, str) else ascii(r))


# As far as the first thing that goes wrong. What CPython's does after that means nothing.
def tokens(lines, **k):
    k.setdefault("extra_tokens", True)
    source = iter(lines)
    out = []
    try:
        for token in T(source.__next__, **k):
            out.append(token)
    except BaseException as e:
        out.append(show(e))
    return out


def both(label, lines):
    if isinstance(lines, str):
        lines = lines.splitlines(True)
    for extra in (True, False):
        print(label, "with more" if extra else "with no more", ascii(lines))
        for token in tokens(lines, extra_tokens=extra):
            print("   ", token if isinstance(token, str) else ascii(token))


print("---- what there is")
t("the module", lambda: (_tokenize.__name__, _tokenize.__package__, _tokenize.__loader__.__name__, _tokenize.__doc__, sorted(n for n in vars(_tokenize) if not n.startswith("__"))))
t("the class", lambda: (T.__name__, T.__module__, T.__qualname__, [b.__name__ for b in T.__mro__], sorted(vars(T)), T.__text_signature__, T.__doc__, T.__basicsize__, T.__flags__ & 0x7FFF, repr(T), attempt(type, "X", (T,), {}), attempt(setattr, T, "x", 1)))
for a, k in (((), {}), ((None,), {}), ((None,), {"extra_tokens": True}), ((), {"readline": None, "extra_tokens": True}), ((None, True), {}), ((None,), {"extra_tokens": True, "encoding": None}), ((None,), {"extra_tokens": True, "encoding": 5}), ((None,), {"extra_tokens": True, "encoding": b"utf-8"}), ((None,), {"extra_tokens": True, "encoding": "a\0b"}),
             ((None,), {"extra_tokens": True, "other": 1}), ((None,), {"extra_tokens": 0}), ((None,), {"extra_tokens": "yes"}), ((None,), {"extra_tokens": []}), ((None,), {"encoding": "utf-8"}), ((None,), {"extra_tokens": True, "encoding": "no-such-codec"})):
    t("TokenizerIter(*%r, **%r)" % (a, k), lambda: type(T(*a, **k)).__name__)
t("what it is", lambda: [(iter(x) is x, attempt(setattr, x, "y", 1), attempt(lambda: x.__dict__), next(x), next(x), attempt(next, x), attempt(next, x), attempt(x.__next__, 1)) for x in [T(iter(["a"]).__next__, extra_tokens=True)]])

print("---- what it is given for lines")
for lines, k in (([5], {}), ([None], {}), ([b"x\n"], {}), ([bytearray(b"x\n")], {"encoding": "utf-8"}), (["x\n"], {"encoding": "utf-8"}), ([memoryview(b"x\n")], {"encoding": "utf-8"}), ([b"x\n"], {"encoding": "utf-8"}), ([b"\xe9 = 1\n"], {"encoding": "latin-1"}), ([b"\xe9 = 1\n"], {"encoding": "utf-8"}), ([b"x\n"], {"encoding": "no-such-codec"}),
                 (["\ud800 = 1\n"], {}), (["x = '\udcff'\n"], {}), ([b"\xff\xfex\0\n\0"], {"encoding": "utf-16"}), (["x\n", 5], {}), (["x\n", ""], {}), (["x\n", "", "y\n"], {}), ([""], {}), ([], {}), ([type("S", (str,), {})("x\n")], {}), ([type("B", (bytes,), {})(b"x\n")], {"encoding": "utf-8"})):
    t("%s %r" % (ascii([type(x).__name__ if isinstance(x, memoryview) else x for x in lines]), k), lambda: tokens(lines, **k))
t("what is not callable", lambda: attempt(next, T(5, extra_tokens=True)))
t("what raises", lambda: attempt(list, T(lambda: 1 / 0, extra_tokens=True)))
t("what takes something", lambda: attempt(list, T(lambda x: "", extra_tokens=True)))
t("what raises in the middle of a string", lambda: tokens(x if x else 1 / 0 for x in ['x = """\n', ""]))
t("KeyboardInterrupt", lambda: [attempt(list, T(lambda: (_ for _ in ()).throw(KeyboardInterrupt("stop")), extra_tokens=True))])

print("---- no more is read than has to be")
for extra in (True, False):
    for lines in (["a = 1\n", "b = 2\n"], ["if a:\n", "    b\n", "c\n"], ["x = (1,\n", "     2)\n", "y\n"], ['s = """a\n', 'b"""\n', "z\n"], ["a = 1 \\\n", "    + 2\n", "b\n"], ["# c\n", "\n", "a\n"], ['f"""{a\n', '}b\n', '"""\n', "c\n"], ["if a:\n", "  if b:\n", '    """x\n', '    y"""\n', '"""p\n', 'q"""\n'], ["a"], ["a\n", "b"]):
        log = []
        source = iter(lines)

        def readline():
            line = next(source, "")
            log.append("read " + ascii(line))
            return line

        for token in T(readline, extra_tokens=extra):
            log.append((token[0], token[1]))
        print(extra, ascii(log))

print("---- lines and what ends them")
both("nothing", [])
both("a line that nothing ends", ["x"])
both("two of them", ["x", "y"])
both("more than one line at a time", ["a\nb\n", "c\n"])
both("carriage returns", ["a\r\n", "b\r\n", "\r\n", "# c\r\n", "(\r\n", ")\r\n"])
both("a carriage return that nothing follows", ["a\r"])
both("one by itself", ["a\rb\n", "\r1\n", "x = \r'a'\n", "\r\r\n", "y\r+\n", "\r\\\n", "z\n", "\r.5\n", "\r# c\n"])
both("and then what is not ASCII", ["a \r\xe9\n"])
both("or this", ["\r€b\n"])
both("or this", ["x = \r\U0001F600\n"])
both("blank lines and comments", "\n  \n# a\n  # b\nx # c\n\n  # d\n")
both("a comment that nothing ends", ["x\n", "# c"])
both("white space that nothing ends", ["x\n", "   "])
both("form feeds and tabs", "\x0cx\n\x0c\n if y:\n\t\x0c z\n\tw\n")
both("continuation", "x = 1 + \\\n    2\nif a:\n    \\\n  b\n  \\\n    c\n")
both("continuation and then nothing", ["x \\\n"])
both("continuation and then a blank line", ["x \\\n", "\n", "y\n"])
both("something after the backslash", ["x \\ \n"])
both("a backslash at the very end", ["x \\"])
both("a backslash by itself", ["\\\n", "x\n"])

print("---- indentation")
both("in and out", "if a:\n  b\n  if c:\n      d\n  e\nf\n")
both("out at the end", "if a:\n  if b:\n    c")
both("out to nowhere", "if a:\n    b\n  c\n")
both("indented from the first", "  a\n  b\n")
both("tabs and spaces", "if a:\n        b\n\tc\n")
both("tabs and spaces that disagree", "if a:\n  \tb\n\t  c\n")
both("between brackets it means nothing", "x = [\n        1,\n  2,\n]\n")
both("comments have no say", "if a:\n    b\n# c\n        # d\n    e\n")
t("too deep", lambda: [x for x in tokens([" " * i + "if x:\n" for i in range(101)])][-2:])
t("as deep as may be", lambda: len(tokens([" " * i + "if x:\n" for i in range(100)])))

print("---- names, numbers and operators")
both("names", "a _b c1 \xe9 \u4e2d\u6587 \U0001D400 a\u0301 \u212b if match None __x__\n")
both("what no name can have in it", ["a\u20acb\n"])
both("nor begin with", ["\u20ac\n"])
both("what cannot be shown", ["a\u200bb\n"])
both("numbers", "0 1 12 1_000 0x1F 0o17 0b101 1.5 .5 5. 1e5 1E-5 1.5e+5 1j 1.5J 0_0 00 0e0 0j 1_0.0_1e1_0 0XaB 0O7 0B1 1__0\n")
for text in ("1_", "1__0", "0x", "0xg", "0x_", "0b2", "0b12", "0o8", "0o18", "1e", "1e+", "1.e", "1_.5", "1._5", "012", "0_7", "09.5", "09e1", "09j", "1a", "1if x else y", "1and 2", "1or 2", "1in x", "1is x", "1else", "1not in x", "0x1for x in y", "1.5if x else y", "1jif x else y", "1_a", "0b1a", "0o1a", "1e5a", "1.a", "1..a", "1...", "0xfor", "1\xe9"):
    both("a number", [text + "\n"])
both("operators", "+ - * / // % @ ** << >> & | ^ ~ := < > <= >= == != <> ( ) [ ] { } , : . ; = -> += -= *= /= //= %= @= &= |= ^= >>= <<= **= ... ! .. ....\n")
both("run together", "a+=-b**-c//=d<<=e>>=f!=g<>h->i:=j...k\n")
both("what begins nothing", "$ ? ` a$b\n")
both("what cannot be shown", ["a \x01 b\n"])
both("nor this", ["\x7f\n"])
both("a null", ["a = 1\n", "b \0 c\n"])
both("a null in a comment", ["# \0\n"])

print("---- brackets")
both("over lines", "x = (1,\n [2,\n  {3}])\n")
both("never closed", ["x = (1,\n", "2\n"])
both("never opened", [")\n"])
both("the wrong one", ["(]\n"])
both("the wrong one on another line", ["(\n", "]\n"])
both("more closed than opened", ["())(\n", ")\n"])
t("too many", lambda: tokens(["(" * 201 + "\n"])[-1:] + tokens(["(" * 201 + "\n"], extra_tokens=False)[-1:])
t("as many as may be", lambda: (len(tokens(["(" * 200 + ")" * 200 + "\n"])), len(tokens(["[" * 200 + "]" * 200 + "\n"], extra_tokens=False))))

print("---- strings")
both("of each kind", "'a' \"b\" '''c''' \"\"\"d\"\"\" r'e' b'f' rb'g' Rb'h' bR'i' u'j' U'k' B'l' '' \"\" '''''' 'a\\'b' \"a\\\"b\" '\\\\' r'\\'' 'a\\\nb'\n")
both("over lines", 'x = """a\nb\n  c""" + \'\'\'d\n\'\'\'\ny\n')
both("what comes after one on its last line", '\xe9 = """\xe9\n\xe9"""; \xe9 = 1\n')
both("what is not a prefix", "ur'a' bu'b' ub'c' rr'd' bb'e' fb'f' bf'g' tf'h' ft'i' ut'j' uf'k' x'l'\n")
for text in ("'abc", '"abc', "'abc\\", "'abc\\'", "'''abc", '"""abc\n', "'''abc''", "'a\nb'", "b'abc", "r'abc\\'", "'", '"""', "''''", "x = 'a' 'b", "'\\\n", "'a\\\n"):
    both("never ended", text.splitlines(True))
both("carriage returns in them", ["'a\rb' '\r' '\\\rx' \"\"\"a\rb\"\"\" f'a\rb{x}\r' f'\\\r{x}' f'\\\r}}' rf'\\\r{x}' 'a\\\r\n", "b' f'{x:\r}' # a\rb\n"])
both("what is wrong with what is in one is not looked into", "'\\x' b'\xe9' '\\N{no such}' '\\777' '\\q' b'\\u1234'\n")

print("---- strings with expressions in them")
for text in ("f'a'", "f''", "f'{x}'", "f'a{x}b'", "f'{x}{y}'", "f'{{'", "f'}}'", "f'a{{b}}c'", "f'{{{x}}}'", "f'{x!r}'", "f'{x:>10}'", "f'{x!r:>{w}}'", "f'{x=}'", "f'{x = }'", "f'{x:{y}{z}}'", "f'{x:{{}}}'", "f'{a[\"b\"]}'", "f'{f\"{x}\"}'", "f'{f'{x}'}'", "rf'\\{x}'", "fr'\\n{x}'", "Rf'a'", "F'a'", "f'\\N{DIGIT ONE}{x}'", "f'\\N{x}'", "rf'\\N{x}'", "f'\\{x}'", "f'\\}}'",
             "f'{x:\\n}'", "f'{lambda x: 1}'", "f'{(lambda x: 1)}'", "f'{x:=5}'", "f'{(x:=5)}'", "f'{x!=y}'", "f'{x:a{{b}'", "f'{x #c\n}'", "f'''{x #c\n}'''", "f'''a\n{x}\nb'''", "f'''{\nx\n}'''", "f'''{x:\n}'''", "f'''{x:a\nb}'''", "f'{x:\n}'", "t'a{x}b'", "t'{x!r:>5}'", "rt'\\{x}'", "tr'a'", "T'{x=}'", "t'''a\n{x}'''", "f'\xe9{\xe9}\xe9'", "f'\U0001F600{x}\U0001F600'", "f'{x}' f'{y}' 'z'",
             "f'{'", "f'{x'", "f'}'", "f'{x}}'", "f'{x", "f'a", "f'''a", "f'''{x", "f'{x!'", "f'{x:'", "f'{x:{'", "f'{)}'", "f'{(}'", "f'{x:{y:{z:{w}}}}'", "f'{[}'", "f'{x]}'", "f'{'a", "f'{\"a}'", "t'{'", "t'}'", "t'a", "f'{a b}'", "f'{}'", "f'{ }'", "f'{!r}'", "f'{:x}'", "f'{x!r!s}'", "f'{**x}'", "f'{x;y}'", "f'{\\}'", "f'{x\\\n}'", "f'''{x\\\n}'''"):
    both("one", text.splitlines(True))
t("nested too deeply", lambda: (tokens(["f'" + "{f'" * 150 + "\n"])[-1:], tokens(["f'{x:" + "{y:" * 3 + "\n"])[-1:]))

print("---- where things are, among characters of every width")
both("columns", "\xe9\xe9 = '\xe9' + \u4e2d + \U0001F600x # \xe9\U0001F600\n  \U0001D400 = (\xe9,\n    '\U0001F600', \u4e2d)\n")
both("errors", ["\xe9\U0001F600 = 1_\n"])
both("in bytes", ["\xe9\U0001F600 = 0123\n"])
both("and this", ["\xe9\U0001F600 = ub''\n"])
both("a string never ended", ["\xe9\U0001F600 = 'a\n"])
both("indentation", ["if \xe9:\n", "    \xe9\n", "  \xe9\U0001F600\n"])
both("continuation", ["\xe9\U0001F600 = 1 \\ \xe9\n"])

print("---- warnings")
for action in ("always", "error"):
    for lines in (["f'\\{x}'\n"], ["x\n", "f'\\}}'\n"], ["1if x else y\n"], ["'\\q'\n"], ["f'\\q{x}'\n"], ["\xe9 = f'\\{x}'\n"], ["0x1for x in y\n"]):
        for extra in (True, False):
            with warnings.catch_warnings(record=True) as caught:
                warnings.simplefilter(action)
                result = tokens(lines, extra_tokens=extra)
            print(action, ascii(lines), extra, len(result), result[-1] if isinstance(result[-1], str) else "no error", [(w.category.__name__, str(w.message), w.filename, w.lineno) for w in caught])

print("---- tokenize")
SOURCE = 'def f(a, b=1):\n    """doc"""\n    x = [a,  # c\n         b]\n\n    return f"{x!r:>{a}}" + \'\xe9\'\n'
t("generate_tokens", lambda: [tuple(x) for x in tokenize.generate_tokens(io.StringIO(SOURCE).readline)])
t("tokenize", lambda: [(tokenize.tok_name[x.type], x.string, x.start, x.end, x.exact_type) for x in tokenize.tokenize(io.BytesIO(SOURCE.encode()).readline)])
t("with a coding line", lambda: [(x.type, x.string) for x in tokenize.tokenize(io.BytesIO("# coding: latin-1\nx = '\xe9'\n".encode("latin-1")).readline)])
t("with a mark at the front", lambda: [(x.type, x.string, x.start) for x in tokenize.tokenize(io.BytesIO(b"\xef\xbb\xbfx = 1\n").readline)])
t("and back again", lambda: (tokenize.untokenize(tokenize.generate_tokens(io.StringIO(SOURCE).readline)) == SOURCE, tokenize.untokenize(tokenize.tokenize(io.BytesIO(SOURCE.encode()).readline)) == SOURCE.encode()))
for text in ("x = (", "'abc", '"""abc', "x = 1_", "if a:\n    b\n  c\n", "x \\", "f'{", "$", "0777", "1a", ")"):
    t("what tokenize makes of %r" % text, lambda: [x.string for x in tokenize.generate_tokens(io.StringIO(text).readline)])
t("without the extra ones", lambda: [tuple(x) for x in tokenize._generate_tokens_from_c_tokenizer(io.StringIO("if a: # c\n\n  b\n").readline)])

print("---- at random")
state = [135792468]


def random(n):
    v = state[0]
    v ^= (v << 13) & 0xFFFFFFFF
    v ^= v >> 17
    v ^= (v << 5) & 0xFFFFFFFF
    state[0] = v
    return v % n


def digest(text, h=0):
    for c in text:
        h = (h * 1000003 + ord(c)) % 2305843009213693951
    return h


BASE = SOURCE + 'class C(B, metaclass=M):\n\tx: int = 0x1F + 1_000.5e-3j\n\tdef g(self, *a, **k):\n\t\treturn t"""a{self.x!s:{k}}\n  b""" if a else rb\'\\x00\' \\\n\t\t\t+ \'\'\'q\n\'\'\'\n@d\nasync def h(): await x; y = {1: 2, **z} ; w = a <> b\n'
PIECES = ["'", '"', "'''", '"""', "{", "}", "(", ")", "[", "]", "\\", "\n", "\r\n", "\r", "\t", " ", "    ", "#", "f'", 't"', "rb'", ":", "!", "=", "0", "1_", "0x", "e", "j", ".", "\xe9", "\U0001F600", "\u20ac", "\x0c", "$", "\\N{", "{{", "}}", ";", "if", "_"]
for batch in range(40):
    total = 0
    for i in range(40):
        text = BASE
        for j in range(1 + random(4)):
            at = random(len(text))
            what = random(3)
            if what == 0:
                text = text[:at] + PIECES[random(len(PIECES))] + text[at:]
            elif what == 1:
                text = text[:at] + text[at + 1 + random(3):]
            else:
                text = text[:at] + PIECES[random(len(PIECES))] + text[at + 1:]
        lines = text.splitlines(True) if random(4) else [l + "\n" for l in text.split("\n")]
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            for extra in (True, False):
                total = digest(ascii(tokens(lines, extra_tokens=extra)), total)
    print(batch, total)
