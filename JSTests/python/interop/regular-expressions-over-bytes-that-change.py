# For regular-expressions-over-bytes-that-change.mjs, and part of it.
#
# CPython has whatever a regular expression is going through keep its bytes where they are until it is done, and raises BufferError at whoever would resize it. Here nothing is kept where it is, so what goes through it
# looks again for where the bytes are after anything of a program's has been run. What comes of it matters less than that nothing is read that is not there.
import re

B = re.compile(b"b")


def show(label, f):
    try:
        print(label, "=>", ascii(f()))
    except BaseException as e:
        print(label, "=>", type(e).__name__ + ": " + str(e))


def sub(subject, act):
    return B.sub(lambda m: act(m) or b"X", subject)


def spans(subject, act):
    return [(m.span(), act(m)) and m.span() for m in B.finditer(subject)]


def search(pattern, subject):
    m = re.compile(pattern).search(subject)
    return m and (m.span(), m.group(), m.string is subject)


def findall(pattern, subject):
    return re.compile(pattern).findall(subject)


def in_python():
    for size in (1, 3, 40, 4000):
        show("emptied while it is replaced, %d" % size, lambda: [(sub(s, lambda m: s.clear()), s) for s in [bytearray(b"abcbd" * size)]][0][0][:12])
        show("made shorter, %d" % size, lambda: [(len(sub(s, lambda m: s.__delitem__(slice(len(s) // 2, None)))), len(s)) for s in [bytearray(b"abcbd" * size)]])
        show("made longer, %d" % size, lambda: [(len(sub(s, lambda m: len(s) < 100000 and s.extend(b"b" * 5000))), len(s)) for s in [bytearray(b"abcbd" * size)]])
        show("emptied while it is gone through, %d" % size, lambda: [(spans(s, lambda m: s.clear()), s) for s in [bytearray(b"abcbd" * size)]])
        show("made longer while it is gone through, %d" % size, lambda: [(len(spans(s, lambda m: len(s) < 100000 and s.extend(b"b" * 5000))), len(s)) for s in [bytearray(b"abcbd" * size)]])
        show("emptied and filled again, %d" % size, lambda: [(sub(s, lambda m: (s.clear(), s.extend(b"zbz" * size)) and None)[:12], len(s)) for s in [bytearray(b"abcbd" * size)]])
    show("by way of a view", lambda: [(sub(memoryview(s), lambda m: s.clear()), s) for s in [bytearray(b"abcbd" * 3)]])
    show("a match of what has gone", lambda: [(m.group(), s.clear(), m.group(), m.groups(), m.span(), repr(m), m.expand(rb"[\1]")) for s in [bytearray(b"abc")] for m in [re.search(b"(b)c", s)]])
    show("a scanner of what has gone", lambda: [(c.search().span(), s.clear(), c.search(), c.search()) for s in [bytearray(b"abcb")] for c in [B.scanner(s)]])
    show("split", lambda: [B.split(s) for s in [bytearray(b"abcbd")]])
