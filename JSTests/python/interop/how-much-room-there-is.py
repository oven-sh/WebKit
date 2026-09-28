# A list is a JavaScript array, a str a JavaScript string, a bytes a typed array and an int a BigInt, and each of those can be only so large. What would be larger is a MemoryError, and is found to be before
# anything is made. CPython would make these, if the machine had the memory.


def t(label, f):
    try:
        r = "made, " + type(f()).__name__
    except Exception as e:
        r = type(e).__name__ + ": " + str(e)
    print(label, "=>", r)


class Hinted:
    def __init__(self, hint):
        self.hint = hint

    def __iter__(self):
        return iter([1, 2])

    def __length_hint__(self):
        return self.hint


print("---- a list: 2 ** 28")
n = 2 ** 28 + 1
for label, f in (("[0] * n", lambda: [0] * n), ("[0, 1] * (n // 2 + 1)", lambda: [0, 1] * (n // 2 + 1)), ("l *= n", lambda: [0].__imul__(n)), ("list(range(n))", lambda: list(range(n))), ("[*range(n)]", lambda: [*range(n)]), ("sorted(range(n))", lambda: sorted(range(n))), ("[].extend(range(n))", lambda: [].extend(range(n))),
                 ("l[:] = range(n)", lambda: [].__setitem__(slice(None), range(n))), ("''.join(range(n))", lambda: "".join(range(n))), ("what says there will be n", lambda: list(Hinted(n))), ("what says there will be one fewer", lambda: list(Hinted(n - 1))), ("added to a list of one", lambda: [0].__iadd__(Hinted(n - 1)))):
    t(label, f)

print("---- a str: 2 ** 31 - 1")
n = 2 ** 31
for label, f in (("'a' * n", lambda: "a" * n), ("'ab' * (n // 2)", lambda: "ab" * (n // 2)), ("'a'.center(n)", lambda: "a".center(n)), ("'a'.ljust(n)", lambda: "a".ljust(n)), ("'a'.rjust(n)", lambda: "a".rjust(n)), ("'a'.zfill(n)", lambda: "a".zfill(n)), ("format(1, str(n))", lambda: format(1, str(n))),
                 ("format('a', str(n))", lambda: format("a", str(n))), ("format(1.5, str(n))", lambda: format(1.5, str(n))), ("format(1, '0' + str(n) + ',')", lambda: format(1, "0" + str(n) + ",")), ("format(1j, str(n))", lambda: format(1j, str(n))), ("'%*d' % (n, 1)", lambda: "%*d" % (n, 1)), ("'%*s' % (n, 'a')", lambda: "%*s" % (n, "a")),
                 ("'%-*s' % (n, 'a')", lambda: "%-*s" % (n, "a")), ("'%' + str(n) + 'd'", lambda: ("%" + str(n) + "d") % 1), ("'{:{}}'.format(1, n)", lambda: "{:{}}".format(1, n)), ("f'{1:{n}}'", lambda: f"{1:{n}}")):
    t(label, f)

print("---- a bytes: 2 ** 32")
n = 2 ** 32 + 1
for label, f in (("bytes(n)", lambda: bytes(n)), ("bytearray(n)", lambda: bytearray(n)), ("b'a' * n", lambda: b"a" * n), ("bytearray(b'a') * n", lambda: bytearray(b"a") * n), ("b'a'.center(n)", lambda: b"a".center(n)), ("b'a'.zfill(n)", lambda: b"a".zfill(n)), ("(5).to_bytes(n)", lambda: (5).to_bytes(n)),
                 ("bytearray().resize(n)", lambda: bytearray().resize(n)), ("bytes(what says there will be n)", lambda: bytes(Hinted(n))), ("bytearray().extend(what says there will be n)", lambda: bytearray().extend(Hinted(n)))):
    t(label, f)

print("---- a tuple: 2 ** 32 - 1")
n = 2 ** 32
for label, f in (("(0,) * n", lambda: (0,) * n), ("(0, 1) * (n // 2)", lambda: (0, 1) * (n // 2))):
    t(label, f)

print("---- an int: 2 ** 30 bits")
for label, f in (("1 << 2 ** 31", lambda: 1 << 2 ** 31), ("1 << 2 ** 40", lambda: 1 << 2 ** 40), ("-1 << 2 ** 40", lambda: -1 << 2 ** 40), ("0 << 2 ** 40", lambda: 0 << 2 ** 40), ("1 << 2 ** 63", lambda: 1 << 2 ** 63), ("1 << 2 ** 30", lambda: 1 << 2 ** 30), ("1 << 2 ** 20", lambda: 1 << 2 ** 20)):
    t(label, f)
