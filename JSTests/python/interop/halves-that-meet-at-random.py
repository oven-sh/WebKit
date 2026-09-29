# Strings that are made from one another by adding to them, of halves of surrogate pairs and whole ones and other things, and asked about in no particular order, with memory collected now and then. What has been
# found out about where the pairs are in one is a start on another that begins the same, and is forgotten when memory is collected. Each string has beside it the code units that it should have, from which what it
# should say is worked out afresh. The first half of a pair followed by the second is the pair, here, so this is not something that CPython agrees with.

HIGH, LOW = 0xD83D, 0xDE00


class Random:
    def __init__(self, seed):
        self.state = seed

    def below(self, n):
        self.state = (self.state * 6364136223846793005 + 1442695040888963407) % (1 << 64)
        return (self.state >> 33) % n


def characters(units):
    result = []
    i = 0
    while i < len(units):
        if 0xD800 <= units[i] < 0xDC00 and i + 1 < len(units) and 0xDC00 <= units[i + 1] < 0xE000:
            result.append(chr(0x10000 + ((units[i] - 0xD800) << 10) + (units[i + 1] - 0xDC00)))
            i += 2
        else:
            result.append(chr(units[i]))
            i += 1
    return result


def run(collect):
    for name, start in (("nothing", ""), ("short", "ab\U0001f600"), ("long", "ab" * 40000), ("long, 16 bits", "中" * 70000), ("long, with pairs", "a\U0001f600" * 30000), ("long, all pairs", "\U0001f600" * 70000)):
        r = Random(len(name))
        n = len(start)
        wrong = 0
        asked = 0
        # What each begins with does not end with half of anything, so what comes after it can be taken by itself.
        live = [(start + "x", [ord("x")])]
        for step in range(4000):
            what = r.below(10)
            s, units = live[r.below(len(live))]
            if what < 5:
                added = [(HIGH, LOW, HIGH, LOW, ord("y"), 0x4E2D)[r.below(6)] for i in range(1 + r.below(2))]
                s += "".join(map(chr, added))
                live.append((s, units + added))
                # As often as not it is not looked at yet.
                if r.below(2):
                    continue
                units = units + added
            elif what == 5:
                collect()
                continue
            expected = characters(units)
            asked += 1
            i = r.below(len(expected))
            kind = r.below(4)
            if kind == 0:
                ok = len(s) == n + len(expected)
            elif kind == 1:
                ok = s[-1] == expected[-1] and s[n + i] == expected[i]
            elif kind == 2:
                ok = list(s[n + i:]) == expected[i:]
            else:
                ok = len(s) == n + len(expected) and s[i - len(expected)] == expected[i]
            wrong += not ok
            if len(units) > 40 or len(live) > 24:
                live = live[:1] + live[1 + r.below(len(live)):]
        print(name, "asked", asked, "wrong", wrong)


if __name__ == "__main__":
    run(lambda: None)
