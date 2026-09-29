# What kind of thing each character is, and what it is in another case, of every character that there is. It goes by the version of Unicode that CPython 3.14 has, whatever the system has.


def digest(values, h=0):
    for v in values:
        h = (h * 1000003 + v) % 2305843009213693951
    return h


def of_text(texts):
    h = 0
    for text in texts:
        h = digest(map(ord, text), h * 31 + len(text))
    return h


QUESTIONS = (str.isalpha, str.isdecimal, str.isdigit, str.isnumeric, str.isalnum, str.isspace, str.islower, str.isupper, str.istitle, str.isprintable, str.isidentifier)
MAPPINGS = (str.lower, str.upper, str.title, str.capitalize, str.swapcase, str.casefold)
for plane in range(17):
    characters = list(map(chr, range(plane << 16, (plane + 1) << 16)))
    print("plane", plane, [sum(map(q, characters)) for q in QUESTIONS])
    # There is nothing in these yet, which the line above has seen to.
    if 4 <= plane <= 13:
        continue
    print("  which", [digest(map(q, characters)) for q in QUESTIONS])
    print("  in another case", [of_text(map(m, characters)) for m in MAPPINGS])
    print("  after a letter", digest(map(str.isidentifier, ["a" + c for c in characters])), of_text(map(str.title, ["a" + c for c in characters])), of_text(map(str.lower, ["A" + c + "Σ" for c in characters])), of_text(map(str.lower, ["Σ" + c + "a" for c in characters])))
    print("  shown", of_text(map(repr, characters)), of_text(map(ascii, characters)))
    print("  taken off, and split at", digest(map(len, map(str.strip, characters))), digest(len(("a" + c + "b").split()) for c in characters), digest(len(("a" + c + "b").splitlines()) for c in characters))


def number(f, c):
    try:
        return f(c)
    except ValueError:
        return -1


everything = [c for c in map(chr, range(0x110000)) if c.isnumeric() or c.isspace() or (not c.isprintable() and ord(c) < 0x30000) or ord(c) % 7 == 0]
print("int", digest(number(int, c) for c in everything), "float", digest(int(number(float, c)) for c in everything), "in the middle", digest(number(int, "1" + c + "2") for c in everything))
print("numbers in other scripts", int("١٢٣"), int("-१२"), int("１_２"), int("١f", 16), int("　١\xa0"), float("١.٥"), float("١e٢"), float(" -٣ "), complex("١+٢j"), complex("\xa0٣j\xa0"), int("\U0001D7CE\U0001D7CF"))
for text in ("١x", "١\xb2", "\xb2", "①", "1\x7f١", "١ ٢", "١€٢", "١" * 3 + "\ud800", "\xe9"):
    for f in (int, float, complex):
        try:
            print(ascii(text), f.__name__, f(text))
        except ValueError as e:
            print(ascii(text), f.__name__, ascii(str(e)))
print("spaces about a number", digest(number(int, c + "7" + c) for c in everything), digest(int(number(float, c + "7" + c)) for c in everything))
print("format", ["{:{}}".format(5, w) for w in ("٣", "\U0001D7D3", "４", "1٠", "0٣", "<٣")], "{0:٣}".format(5), "{:.٣}".format(1 / 3), "{:٩.١f}".format(2.5), format("ab", "٤"), format("abcdef", ".٢"), f"{5:٣}")
print("longer in another case", ["\xdf".upper(), "ŉ".upper(), "İ".lower(), "ﬃ".upper(), "ﬃ".title(), "ﬃ".capitalize(), "ǆ".title(), "Ǆ".title(), "ǅ".swapcase(), "ᾀ".upper(), "ᾈ".lower(), "ẞ".lower(), "ẞ".casefold(), "\xdf".casefold(), "ς".casefold(), "ͅ".upper(), "ΐ".upper()])
SIGMA = "Σ"
print("the last sigma", [ascii(s.lower()) for s in (SIGMA, "A" + SIGMA, SIGMA + "A", "A" + SIGMA + "A", "A" + SIGMA + " ", "A" + SIGMA + ".", "A." + SIGMA, "A'" + SIGMA + "'", "A" + SIGMA + "'A", "Á" + SIGMA, "1" + SIGMA, "A" + SIGMA + SIGMA, SIGMA + SIGMA, "A" + SIGMA + "\U0001D400", "\U0001D400" + SIGMA, "A\xad" + SIGMA + "\xad")])
print("and in a title", [ascii(f("a" + SIGMA + " " + SIGMA + "a" + SIGMA)) for f in (str.title, str.capitalize, str.swapcase, str.casefold)])
print("whole strings", ["Stra\xdfe İstanbul ﬁn \U00010400\U00010428".upper(), "Stra\xdfe İstanbul ﬁn \U00010400\U00010428".lower(), "Stra\xdfe İstanbul ﬁn \U00010400\U00010428".title(), "Stra\xdfe İstanbul ﬁn \U00010400\U00010428".swapcase(), "Stra\xdfe İstanbul ﬁn \U00010400\U00010428".casefold()])
print("half of a pair", [ascii(f("a\ud800b\udc00")) for f in MAPPINGS], [q("\ud800") for q in QUESTIONS])
print("nothing", [f("") for f in MAPPINGS], [q("") for q in QUESTIONS])
print("the same one", [f(s) == s for f in MAPPINGS for s in ("abc", "ABC", "123")])
for source in ("Å = 1", "á = 1", "℘ = 1", "゛ = 1", "a\xb7 = 1", "\xb7 = 1", "\U0001D400 = 1", "€ = 1", "a‍ = 1", "ำ = 1", "aำ = 1", "\xa0x = 1", "x\x0c = 1", "﻿x = 1", "x = 1 　", "\U000E0100 = 1", "ᢅ = 1", " ", "\U0010FFFF"):
    space = {}
    try:
        exec(source, space)
        print(ascii(source), "=>", ascii(sorted(n for n in space if n != "__builtins__")))
    except SyntaxError as e:
        print(ascii(source), "=>", ascii(e.msg), e.offset)
