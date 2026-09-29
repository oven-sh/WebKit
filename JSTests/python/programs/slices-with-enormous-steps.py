# Slices whose steps and bounds are as large as numbers can be, and larger, of every kind of sequence that is built in: read, deleted and assigned to. `del a[9::1 << 333]` takes one item, and one more step from there is
# further than can be counted.
import array


def attempt(f, *a):
    try:
        return f(*a)
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)


def digest(value):
    h = 0
    for c in repr(value):
        h = (h * 1000003 + ord(c)) % (2 ** 61 - 1)
    return h


STEPS = (1 << 333, 2 ** 63 - 1, 2 ** 63, 2 ** 62, 2 ** 63 - 2, -(1 << 333), -(2 ** 63), -(2 ** 63) + 1, -(2 ** 62), 2 ** 31, 2 ** 32, -(2 ** 31))
STARTS = (None, 0, 1, 5, 9, 10, -1, -10, 1 << 333, -(1 << 333), 2 ** 63 - 1, -(2 ** 63))
STOPS = (None, 0, 9, 1 << 333, -(1 << 333))
SLICES = [slice(a, b, s) for s in STEPS for a in STARTS for b in STOPS]
CHANGED = (
    ("list", lambda: list(range(10)), lambda n: [7] * n),
    ("bytearray", lambda: bytearray(range(10)), lambda n: bytes([7] * n)),
    ("array of one byte", lambda: array.array("b", range(10)), lambda n: array.array("b", [7] * n)),
    ("array of eight", lambda: array.array("q", range(10)), lambda n: array.array("q", [7] * n)),
    ("array of floats", lambda: array.array("d", range(10)), lambda n: array.array("d", [7] * n)),
    ("memoryview", lambda: memoryview(bytearray(range(10))), lambda n: bytes([7] * n)),
)
READ = CHANGED + (("tuple", lambda: tuple(range(10)), None), ("bytes", lambda: bytes(range(10)), None), ("str", lambda: "0123456789", None), ("range", lambda: range(10), None))
for name, make, fill in READ:
    got = [list(make()[s]) for s in SLICES]
    print(name, "read:", len(got), sum(map(len, got)), digest(got))
for name, make, fill in CHANGED:
    out = []
    for s in SLICES:
        n = len(make()[s])
        for f in (lambda x: x.__delitem__(s), lambda x: x.__setitem__(s, fill(n)), lambda x: x.__setitem__(s, fill(n + 1)), lambda x: x.__setitem__(s, fill(0))):
            x = make()
            out.append((attempt(f, x), list(x)))
    print(name, "deleted and assigned to:", len(out), digest(out))
a = array.array("b", range(10))
del a[9::1 << 333]
print(a)

# And where the sequence and the slice are both written out, so that it is worked out when the source is compiled
print("0123456789"[9::9223372036854775807], "0123456789"[1::9223372036854775807], "0123456789"[::9223372036854775806], "0123456789"[::-9223372036854775807], "0123456789"[5::-9223372036854775807], "0123456789"[9::1 << 333], "0123456789"[::-(1 << 333)], "0123456789"[1::4611686018427387904], "0123456789"[9223372036854775807::-9223372036854775807])
print((0, 1, 2, 3)[3::9223372036854775807], (0, 1, 2, 3)[1::9223372036854775807], (0, 1, 2, 3)[::-9223372036854775807], (0, 1, 2, 3)[1::9223372036854775806], (0, 1, 2, 3)[2::-9223372036854775808])
print(b"0123456789"[9::9223372036854775807], b"0123456789"[1::9223372036854775807], b"0123456789"[::-9223372036854775807], b"0123456789"[3::9223372036854775805])
