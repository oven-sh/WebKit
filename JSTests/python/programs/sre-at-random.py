# Regular expressions made at random, tried on strings made at random. What compiles them is written in Python and is the same everywhere, so this is a test of what matches them.
import re
import warnings

warnings.simplefilter("ignore")
state = [987654321]


def random(n):
    v = state[0]
    v ^= (v << 13) & 0xFFFFFFFF
    v ^= v >> 17
    v ^= (v << 5) & 0xFFFFFFFF
    state[0] = v
    return v % n


def choose(things):
    return things[random(len(things))]


# Each is a few letters that there is a good chance of finding again, in characters of one width or another.
ALPHABETS = ("abc", "aAbB_ 1\n", "a\xe9\xc9\xdf b", "aаАσςΣK k", "a\U0001F600\U00010400\U00010428b", "abıİiIﬅﬆ s")


def atom(alphabet, depth, groups):
    what = random(26 if depth else 14)
    if what < 5:
        return re.escape(choose(alphabet))
    if what == 5:
        return "."
    if what == 6:
        return choose((r"\w", r"\W", r"\d", r"\D", r"\s", r"\S"))
    if what == 7:
        return choose((r"\b", r"\B", "^", "$", r"\A", r"\Z"))
    if what < 11:
        members = "".join(choose((re.escape(choose(alphabet)), re.escape(choose(alphabet)), r"\w", r"\s", r"\d", "a-c", "A-Z", "\xe0-\xff", "Ѐ-я", "\U00010400-\U0001044f")) for i in range(1 + random(3)))
        return "[" + choose(("", "", "^")) + members + "]"
    if what < 14:
        return re.escape(choose(alphabet)) + re.escape(choose(alphabet))
    inner = sequence(alphabet, depth - 1, groups)
    if what < 18:
        groups[0] += 1
        return "(" + inner + ")"
    if what == 18:
        return "(?:" + inner + ")"
    if what == 19:
        return "(?=" + inner + ")"
    if what == 20:
        return "(?!" + inner + ")"
    if what == 21:
        return choose(("(?<=", "(?<!")) + "".join(re.escape(choose(alphabet)) for i in range(1 + random(2))) + ")"
    if what == 22:
        return "(?>" + inner + ")"
    if what == 23 and groups[0]:
        return "\\%d" % (1 + random(groups[0]))
    if what == 24 and groups[0]:
        return "(?(%d)%s|%s)" % (1 + random(groups[0]), inner, sequence(alphabet, depth - 1, groups))
    return "(?:" + inner + "|" + sequence(alphabet, depth - 1, groups) + ")"


def repeated(alphabet, depth, groups):
    text = atom(alphabet, depth, groups)
    what = random(16)
    if what < 8:
        return text
    return text + ("*", "+", "?", "*?", "+?", "??", "{2}", "{1,2}", "{0,3}?", "*+", "++", "?+", "{2,}", "{,2}+")[random(14)]


def sequence(alphabet, depth, groups):
    text = "".join(repeated(alphabet, depth, groups) for i in range(1 + random(3)))
    return text + "|" + sequence(alphabet, depth, groups) if random(6) == 0 else text


def digest(text, h=0):
    for c in text:
        h = (h * 1000003 + ord(c)) % 2305843009213693951
    return h


def results(compiled, subject, empty):
    m = compiled.search(subject)
    yield m and (m.span(), m.groups(), m.lastindex, m.regs)
    m = compiled.match(subject)
    yield m and (m.span(), m.groups())
    m = compiled.fullmatch(subject)
    yield m and (m.span(), m.groups())
    m = compiled.search(subject, 1, len(subject) - 1)
    yield m and (m.span(), m.groups(), m.pos, m.endpos)
    yield compiled.findall(subject)
    yield compiled.split(subject)
    yield compiled.split(subject, 2)
    yield compiled.subn(empty + type(empty)(b"-" if isinstance(empty, bytes) else "-"), subject)
    yield compiled.sub(lambda m: m.group()[::-1] * 2, subject, 3)
    yield [(m.span(), m.lastindex) for m in compiled.finditer(subject)]
    scanner = compiled.scanner(subject)
    yield [m.span() for m in iter(scanner.match, None)]


tried = failed = matched = 0
for batch in range(60):
    total = 0
    for i in range(50):
        alphabet = choose(ALPHABETS)
        pattern = sequence(alphabet, 3, [0])
        flags = choose((0, 0, 0, re.I, re.I, re.M, re.S, re.A, re.I | re.A, re.I | re.M | re.S))
        as_bytes = alphabet.isascii() and pattern.isascii() and random(3) == 0
        if as_bytes:
            pattern = pattern.encode()
            flags = (flags & ~re.A) | choose((0, 0, re.L))
        tried += 1
        try:
            compiled = re.compile(pattern, flags)
        except (re.error, OverflowError, RecursionError) as e:
            failed += 1
            total = digest(type(e).__name__ + str(e), total)
            continue
        for j in range(6):
            subject = "".join(choose(alphabet) for k in range(random(14)))
            if as_bytes:
                subject = choose((bytes, bytes, bytearray, memoryview))(subject.encode())
            shown = ascii(list(results(compiled, subject, pattern[:0])))
            matched += "None, None, None, None" not in shown
            total = digest(shown, total)
    print(batch, total)
print(tried, failed, matched)
