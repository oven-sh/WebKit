# The first half of a surrogate pair followed by the second half is the pair, here, where in CPython they are two characters. So a string that ends with the first half is one character shorter, once the second half
# has been added to it, than it would be. What it was before is not changed by that, though the two are kept in the same place, one as the beginning of the other.

HIGH, LOW, PAIR = "\ud83d", "\ude00", "\U0001f600"


def show(label, *values):
    print(label, "=>", *[ascii(v) for v in values])


# What has been found out about where the pairs are is forgotten whenever memory is collected, and is found out again from whichever string is asked about next. `between` is called between one thing and the next, and
# halves-that-meet-after-collecting.mjs has it collect.
def run(between):


    # It is what is long that is kept that way: more than a page of memory, of which the largest that there are have 64K bytes.
    for prefix in ("", "a", "中", PAIR, "ab" * 40, ("a" + PAIR) * 30, "ab" * 40000, "\u4e2d" * 70000, ("a" + PAIR) * 30000, PAIR * 70000):
        n = len(prefix)
        # Long enough, and added to often enough, to be kept with room after it.
        s = prefix
        for i in range(6):
            s += "x"
            len(s), s[-1]
        n += 6
        before = s + HIGH
        between()
        show("with the first half", len(before) - n, before[-1], before[n:], before.find(HIGH) - n, before.endswith(HIGH), PAIR in before[n:])
        after = before + LOW
        between()
        show("and then the second", len(after) - n, after[-1], after[n:], after.find(PAIR, n) - n, after.endswith(PAIR), after.endswith(LOW), HIGH in after[n:], after == s + PAIR)
        between()
        show("what it was is as it was", len(before) - n, before[-1], before[n:], before.find(HIGH) - n, before.endswith(HIGH), before == s + HIGH, before < after, list(before)[n:])
        other = before + "y"
        between()
        show("something else after it", len(other) - n, other[n:], other[-2], list(other)[n:], other.find(HIGH) - n)
        between()
        show("and still", len(before) - n, before[-1], len(after) - n, after[-1], len(after + LOW) - n, (after + LOW)[-1], len(before + HIGH + LOW) - n)
        more = after
        for i in range(5):
            more += HIGH
            a = len(more)
            more += LOW
            between()
            show("again", a - n, len(more) - n, more[-1], more[n:].count(PAIR), more[n:].count(HIGH), more[n:].count(LOW))


if __name__ == "__main__":
    run(lambda: None)
