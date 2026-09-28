# A str is counted in characters. In this engine it is kept in code units of 16 bits, of which a character past U+FFFF takes two. None of that is to show, and it is to take no longer to get at the last
# character of a long string than at the first.


def t(label, f):
    try:
        r = f()
    except BaseException as e:
        r = type(e).__name__ + ": " + str(e)
    print(label, "=>", ascii(r))


def _raises(f):
    try:
        f()
    except IndexError:
        return True
    return False


def _error(f):
    try:
        f()
    except UnicodeError as e:
        return e


def checksum(values):
    total = 0
    for value in values:
        total = (total * 31 + (value if isinstance(value, int) else sum(map(ord, value)))) % 1000000007
    return total


A = "\U0001f600"  # Two code units.
B = "中"  # One, of 16 bits.
C = "\U0001f601"  # Two, of which the first is the same as the first of that.
D = "\U0001f400"  # And the second the same as the second.
E = "\uff21"  # One, that as a code unit is more than either half of a pair, and as a character is less than any pair.
HIGH = "\ud83d"  # Half of a pair, by itself, which is a character.
LOW = "\ude00"
# The first half followed by the second is a pair, however it came about, so the two kinds are kept apart here: that is a difference from CPython that there is no getting away from.

STRINGS = {
    "empty": "", "ascii": "abcdef", "latin-1": "ab\xe9\xdfef", "16 bits": B * 3 + "ab" + B, "one pair": A, "pairs": A * 4, "a pair first": A + "abc", "a pair last": "abc" + A, "a pair in the middle": "ab" + A + "cd",
    "pairs and 16 bits": B + A + B + A + "x", "every other": ("a" + A) * 4, "half first": HIGH + "ab", "half last": "ab" + HIGH, "the other half": LOW + "ab" + LOW, "first halves and pairs": HIGH + A + HIGH + HIGH + C, "second halves and pairs": LOW + A + LOW + LOW + D + LOW,
    "pairs alike": A + C + D + A + C, "pairs and what is more than a half": E + A + E + C + "\ue000" + D + "\uffff",
}

for name, s in STRINGS.items():
    n = len(s)
    print("----", name, n)
    t("characters", lambda: [hex(ord(c)) for c in s])
    t("by index", lambda: [s[i] for i in range(n)] == list(s))
    t("from the end", lambda: [s[i] for i in range(-1, -n - 1, -1)] == list(s)[::-1])
    t("past the ends", lambda: [_ for _ in (n, n + 1, -n - 1) if not _raises(lambda: s[_])])
    t("every slice", lambda: checksum(s[i:j:k] for i in range(-n - 1, n + 2) for j in range(-n - 1, n + 2) for k in (1, 2, 3, -1, -2, -3)))
    t("every slice is what a list has", lambda: all(list(s[i:j:k]) == list(s)[i:j:k] for i in range(-n - 1, n + 2) for j in range(-n - 1, n + 2) for k in (1, 2, 3, -1, -2, -3)))
    t("reversed", lambda: "".join(reversed(s)) == s[::-1])
    t("find", lambda: [[s.find(c, i) for i in range(n + 1)] for c in sorted(set(s))])
    t("rfind", lambda: [[s.rfind(c, 0, i) for i in range(n + 1)] for c in sorted(set(s))])
    t("index and count", lambda: [(s.index(c), s.rindex(c), s.count(c), s.count(c, 1), s.count(c, 0, -1)) for c in sorted(set(s))])
    t("count of nothing", lambda: (s.count(""), s.count("", 1), s.count("", n), s.count("", n + 1)))
    t("startswith and endswith", lambda: [(s.startswith(s[i:], i), s.endswith(s[:i], 0, i), s.startswith(s[i:i + 1], i, i + 1)) for i in range(n + 1)])
    t("partition", lambda: [(s.partition(c), s.rpartition(c)) for c in sorted(set(s))])
    t("split", lambda: [(s.split(c), s.rsplit(c, 1)) for c in sorted(set(s))])
    t("replace", lambda: [(s.replace(c, "<>"), s.replace(c, "", 1), s.replace("", "|")) for c in sorted(set(s))[:2]])
    t("to a width", lambda: (s.center(n + 3, "*"), s.ljust(n + 2, A), s.rjust(n + 2, B), s.zfill(n + 2), s.center(n), s.ljust(n - 1)))
    t("format", lambda: (format(s, "*^" + str(n + 2)), format(s, ".2"), format(s, A + ">" + str(n + 1)), "%*s|%-*s|%.1s" % (n + 1, s, n + 1, s, s), f"{s:>{n + 1}}", f"{s!r:.4}"))
    t("strip", lambda: (s.strip(s[:1]), s.lstrip(s[-1:]), s.rstrip(s[-1:]), s.removeprefix(s[:1]), s.removesuffix(s[-1:])))
    t("times and plus", lambda: (len(s * 3), len(s + s), (s * 2)[n:] == s, (s + A)[n], (A + s)[1:] == s))
    t("in", lambda: [c in s for c in (A, B, C, D, E, HIGH, LOW, "a", "", A + C, C + D, HIGH + A, A + LOW)])
    halves = [h for h in (HIGH, LOW) if (LOW if h == HIGH else HIGH) not in list(s)]
    t("a half is not part of a pair", lambda: [(s.find(h), s.rfind(h), s.count(h), s.startswith(h), s.endswith(h), s.split(h), s.rsplit(h, 1), s.partition(h), s.rpartition(h), s.replace(h, "-"), s.removeprefix(h), s.removesuffix(h), s.strip(h), s.lstrip(h), s.rstrip(h)) for h in halves])
    t("nor one pair another that is half the same", lambda: [(s.strip(x), s.lstrip(x), s.rstrip(x), s.strip(x + "a"), s.find(x), s.count(x), s.replace(x, "-"), s.split(x)) for x in (A, C, D)])
    t("compared", lambda: (s < s + "a", s == s[:], s < A, s > B, [(s < x, s <= x, s > x, s >= x, s == x) for x in (A, B, C, D, E, HIGH, LOW, "a", "", s + A, s + E, s[:-1], s[:-1] + E, s[:-1] + A, s[:-1] + HIGH)], sorted([s, A, B, C, D, E, HIGH, LOW, "a", "\ue000", "\uffff", "\U00010000", "\U0010ffff", "\ud7ff"]), max(s + E), min(A + s)))
    t("iterators", lambda: (lambda i: (i.__length_hint__(), next(i, None), i.__length_hint__(), list(i), i.__length_hint__()))(iter(s)))
    t("an iterator set going part way", lambda: [(lambda i: (i.__setstate__(k), list(i))[1])(iter(s)) == list(s)[k:] for k in range(n + 1)])
    t("enumerate, zip, map", lambda: (list(enumerate(s))[-1:], list(zip(s, s[1:]))[-1:], max(map(ord, s), default=None), min(s, default=None)))
    t("translate and maketrans", lambda: (s.translate({ord(c): "!" for c in s[:1]}), s.translate(str.maketrans(s[:1], "z"[:len(s[:1])])), s.translate({0x1f600: None}), s.translate({0x1f600: B, 0x4e2d: A})))
    t("expandtabs and splitlines", lambda: ((s + "\t" + s + "\t|").expandtabs(4), (s + "\n" + s).splitlines(True)))
    t("case", lambda: (s.upper() == s.upper().upper(), len(s.lower()), s.title()[:2], s.capitalize()[-1:], s.swapcase()[:1], s.casefold()[-1:]))
    t("questions", lambda: (s.isalpha(), s.isprintable(), s.isidentifier(), s.isascii(), s.isalnum(), s.isspace(), s.islower(), s.isnumeric()))
    t("join", lambda: (len(s.join("abc")), len(A.join(s)), A.join(s)[1:2], len("".join([s, A, s]))))
    t("ord and chr", lambda: [chr(ord(c)) == c for c in s])
    t("repr and ascii", lambda: (len(repr(s)), ascii(s)))
    t("hash and dict", lambda: ({s: 1}[s[:]], hash(s) == hash("".join(list(s))), s in {s + ""}))

print("---- what is raised says where in characters")
for label, f in (("encode", lambda: ("a" + A + "\xe9").encode("ascii")), ("further on", lambda: (A * 3 + B).encode("latin-1")), ("a half", lambda: (A + HIGH + "a").encode("utf-8")), ("replaced", lambda: (A + B + "a").encode("ascii", "replace")),
                 ("as numbers", lambda: (A + B + "a").encode("ascii", "xmlcharrefreplace")), ("with backslashes", lambda: (A + B + HIGH).encode("ascii", "backslashreplace")), ("index", lambda: (A * 3).index("x")), ("int", lambda: int(A + "1")), ("format", lambda: (A + "{").format())):
    t(label, f)
t("where the error says", lambda: [(e.start, e.end, e.object == A * 2 + "\xe9" + A) for e in [_error(lambda: (A * 2 + "\xe9" + A).encode("ascii"))]])

print("---- long ones")
# Getting at each in turn is to take as long as there are characters, and not that many times over. A hundredth of these would be enough to tell.
N = 100000
for name, unit in (("ascii", "ab"), ("16 bits", B + "b"), ("pairs", A + "b"), ("all pairs", A + A), ("one pair at the end", None), ("one pair at the start", None)):
    s = unit * (N // 2) if unit else (B * (N - 1) + A if "end" in name else A + B * (N - 1))
    t(name, lambda: (len(s), checksum(ord(s[i]) for i in range(len(s))), checksum(ord(s[-i]) for i in range(1, len(s), 3)), sum(len(s) for i in range(N)) // N, checksum(s[i:i + 2] for i in range(0, len(s), 5)), checksum(s.find("b", i) for i in range(0, len(s), 7)),
                     checksum(s[::-1][:9]), len(s[::3]), checksum(s[::-7][:9]), checksum(map(ord, reversed(s))), sum(s.startswith(s[i], i) for i in range(0, len(s), 9)), s.count("b", N // 2), s.rfind("b", 0, N // 3), len(s[1:-1]), s[N // 2:][:2] == s[N // 2:N // 2 + 2]))
t("two at once", lambda: (lambda a, b: sum(a[i] == b[i] for i in range(N)))(A * N, (A + B) * (N // 2)))
t("many of them", lambda: (lambda strings: sum(len(x) + ord(x[i % 5]) for i in range(20) for x in strings))([A * 2 + str(i) + A * 2 for i in range(5000)]))

print("---- added to and looked at")
# What is added to over and over is kept in one place, of which each string on the way is the beginning, and what is known of where the pairs are is added to as well. Each is still to be what it was.


class Random:
    def __init__(self, seed):
        self.state = seed

    def below(self, n):
        self.state = (self.state * 6364136223846793005 + 1442695040888963407) % (1 << 64)
        return (self.state >> 33) % n


def agrees(s, model, r):
    if len(s) != len(model):
        return False
    for attempt in range(6):
        i = r.below(len(model))
        j = i + r.below(len(model) - i + 1)
        if s[i] != model[i] or s[i - len(model)] != model[i] or list(s[i:j]) != model[i:j] or s.find(model[i], i) != i or s[-1] != model[-1]:
            return False
    return True


for name, alphabet in (("pairs", [A, C, D]), ("pairs and others", [A, "a", B, C, "b", E]), ("few pairs", ["a"] * 20 + [B] * 5 + [A]), ("first halves and pairs", [HIGH, A, "a", HIGH, C]), ("second halves and pairs", [LOW, A, "a", LOW, D]), ("none", ["a", B, E])):
    r = Random(len(name))
    live = [("", [])]
    wrong = 0
    for step in range(6000):
        s, model = live[r.below(len(live))]
        added = [alphabet[r.below(len(alphabet))] for i in range(1 + r.below(3))]
        kind = r.below(8)
        if kind == 0:
            # The same one added to twice, of which only one can go after it where it is kept.
            other = [alphabet[r.below(len(alphabet))] for i in range(1 + r.below(3))]
            second = (s + "".join(other), model + other)
            wrong += not agrees(*second, r)
            live.append(second)
        if kind == 1:
            for c in added:
                s += c
        else:
            s += "".join(added)
        model = model + added
        wrong += not agrees(s, model, r)
        live.append((s, model))
        if len(live) > 30:
            live = live[r.below(20):]
        if len(model) > 2500:
            live = [("", [])]
    for s, model in live:
        wrong += list(s) != model or len(s) != len(model) or [s[i] for i in range(len(s))] != model
    print(name, "wrong:", wrong, "of the last:", len(live[-1][1]), checksum(live[-1][0]))
