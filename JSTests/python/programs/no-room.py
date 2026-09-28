# What there is no room for is a MemoryError or an OverflowError, and nothing worse.


def t(label, f):
    try:
        r = f()
        r = "made, " + type(r).__name__
    except Exception as e:
        r = type(e).__name__ + ": " + str(e)
    print(label, "=>", r)


class Hinted:
    def __init__(self, hint):
        self.hint = hint

    def __iter__(self):
        return iter(())

    def __length_hint__(self):
        return self.hint


for x in (2 ** 62, 2 ** 63 - 1, 2 ** 63, 2 ** 70):
    print("----", x)
    for label, f in (("'a' * x", lambda: "a" * x), ("'ab' * x", lambda: "ab" * x), ("'abcd' * x", lambda: "abcd" * x), ("x * 'a'", lambda: x * "a"), ("b'a' * x", lambda: b"a" * x), ("b'abcd' * x", lambda: b"abcd" * x), ("bytearray(b'a') * x", lambda: bytearray(b"a") * x), ("[0] * x", lambda: [0] * x),
                     ("[0, 1, 2, 3] * x", lambda: [0, 1, 2, 3] * x), ("(0,) * x", lambda: (0,) * x), ("(0, 1, 2, 3) * x", lambda: (0, 1, 2, 3) * x), ("l *= x", lambda: [0].__imul__(x)), ("b *= x", lambda: bytearray(b"a").__imul__(x)), ("bytes(x)", lambda: bytes(x)), ("bytearray(x)", lambda: bytearray(x)),
                     ("(5).to_bytes(x)", lambda: (5).to_bytes(x)), ("'a'.center(x)", lambda: "a".center(x)), ("'a'.ljust(x)", lambda: "a".ljust(x)), ("'a'.rjust(x)", lambda: "a".rjust(x)), ("'a'.zfill(x)", lambda: "a".zfill(x)), ("b'a'.center(x)", lambda: b"a".center(x)), ("b'a'.ljust(x)", lambda: b"a".ljust(x)),
                     ("b'a'.rjust(x)", lambda: b"a".rjust(x)), ("b'a'.zfill(x)", lambda: b"a".zfill(x)), ("bytearray(b'a').center(x)", lambda: bytearray(b"a").center(x)), ("bytearray(b'a').zfill(x)", lambda: bytearray(b"a").zfill(x)), ("bytearray().resize(x)", lambda: bytearray().resize(x)), ("list(range(x))", lambda: list(range(x))),
                     ("[*range(x)]", lambda: [*range(x)]), ("sorted(range(x))", lambda: sorted(range(x))), ("[].extend(range(x))", lambda: [].extend(range(x))), ("bytes(range(x))", lambda: bytes(range(x))), ("list(what says there are x)", lambda: list(Hinted(x))), ("''.join(range(x))", lambda: "".join(range(x))),
                     ("l[:] = range(x)", lambda: [].__setitem__(slice(None), range(x))), ("a, *b = range(x)", lambda: (lambda a, *b: b)(*[0]) if 0 else exec("a, *b = r", {"r": range(x)})), ("1 << x", lambda: 1 << x), ("'%*d' % (x, 1)", lambda: "%*d" % (x, 1)), ("'%.*f' % (x, 1.0)", lambda: "%.*f" % (x, 1.0)),
                     ("'{:{}}'.format(1, x)", lambda: "{:{}}".format(1, x)), ("format(1, str(x))", lambda: format(1, str(x))), ("format('a', str(x))", lambda: format("a", str(x))), ("format(1.5, '.' + str(x))", lambda: format(1.5, "." + str(x) + "f")), ("memoryview.cast", lambda: memoryview(bytes(8)).cast("B", (x, 1))),
                     ("range(x)[-1]", lambda: range(x)[-1]), ("len(range(x))", lambda: len(range(x))), ("reversed(range(x))", lambda: reversed(range(x))), ("enumerate([], x)", lambda: enumerate([], x)), ("'a'.expandtabs(x)", lambda: "a".expandtabs(x)), ("'a'.replace('a', 'b', x)", lambda: "a".replace("a", "b", x)),
                     ("'a'.split('a', x)", lambda: "a".split("a", x)), ("[].insert(x, 0)", lambda: [].insert(x, 0)), ("[0].pop(x)", lambda: [0].pop(x)), ("'abc'[x:]", lambda: "abc"[x:]), ("'abc'[:x]", lambda: "abc"[:x]), ("[0][x:x] = [1]", lambda: [0].__setitem__(slice(x, x), [1]))):
        # Where CPython 3.14.7 finds that it cannot have the memory for a bytearray, it raises MemoryError as it should, and then complains of the bytearray that it did not finish making.
        if x == 2 ** 62 and label in ("bytearray(b'a') * x", "bytearray(b'a').center(x)", "bytearray(b'a').zfill(x)"):
            continue
        t(label, f)
