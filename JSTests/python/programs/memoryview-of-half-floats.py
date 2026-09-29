# A memoryview whose items are floats of two bytes: format 'e'.
import math


def attempt(f, *a):
    try:
        return f(*a)
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)


def digest(values):
    h = 0
    for v in values:
        for c in repr(v):
            h = (h * 1000003 + ord(c)) % (2 ** 61 - 1)
    return h


def unpack(bits):
    return memoryview(bits.to_bytes(2, "little")).cast("e")[0]


def pack(x):
    b = bytearray(2)
    memoryview(b).cast("e")[0] = x
    return int.from_bytes(b, "little")


inf, nan = float("inf"), float("nan")
print([unpack(b) for b in (0x0000, 0x8000, 0x3C00, 0xBC00, 0x4000, 0x3555, 0x7BFF, 0xFBFF, 0x0001, 0x8001, 0x03FF, 0x0400, 0x7C00, 0xFC00, 0x7E00, 0xFE00, 0x7C01, 0x7FFF)])
print({type(unpack(b)).__name__ for b in range(0, 65536, 7)})
everything = [unpack(b) for b in range(65536)]
print(len(everything), digest(x for x in everything if x == x), sum(x != x for x in everything), [math.copysign(1, unpack(b)) for b in (0x7E00, 0xFE00, 0x7C01, 0xFC01)])
print("there and back:", all(pack(unpack(b)) == b for b in range(65536) if unpack(b) == unpack(b)))
# All but 0x0100, which is the one bit of a NaN that is not kept here.
print("a NaN there and back:", [hex(pack(unpack(b))) for b in (0x7E00, 0xFE00, 0x7E01, 0x7EFF, 0xFEFF, 0x7C01, 0x7CFF, 0xFC01, 0x7C80)], all(pack(unpack(b)) == b for s in (0x7C00, 0xFC00) for b in range(s + 1, s + 1024) if not b & 0x100))
print("all at once:", memoryview(bytes(range(16))).cast("e").tolist(), len(memoryview(bytes(16)).cast("e")), memoryview(bytes(16)).cast("e").itemsize, memoryview(bytes(16)).cast("e").format)
print("rounded to the nearest, and to the even one of two:", [hex(pack(x)) for x in (1.0, 1.0 + 2 ** -11, 1.0 + 2 ** -11 + 2 ** -30, 1.0 + 2 ** -10, 1.0 + 3 * 2 ** -11, 1.0 + 3 * 2 ** -11 - 2 ** -30, 2.0 - 2 ** -11, 2.0 - 2 ** -12, 2.0 - 2 ** -11 - 2 ** -30, 0.1, -0.1, 1 / 3, 65504.0, 65519.0, 65519.999)])
print("too large:", [attempt(pack, x) for x in (65520.0, -65520.0, 65536.0, 1e5, 1e308, -1e308)])
print("on the way to nothing:", [hex(pack(x)) for x in (2.0 ** -14, 2.0 ** -14 - 2.0 ** -25, 2.0 ** -15, 2.0 ** -24, 2.0 ** -25, 2.0 ** -25 + 2.0 ** -40, 2.0 ** -25 - 2.0 ** -40, 3 * 2.0 ** -25, 2.0 ** -26, 5e-324, -5e-324, -(2.0 ** -24), 0.0, -0.0)])
print("what is not finite:", [hex(pack(x)) for x in (inf, -inf, nan, -nan)])
print("what is not a float:", [attempt(pack, x) for x in (1, True, -2, 10 ** 400, 70000, "a", None, b"a", 1j, [])])
print("and the same of four bytes and of eight:", [[attempt(memoryview(bytearray(8)).cast(f).__setitem__, 0, x) for x in (1, 10 ** 400, -10 ** 400, 1e300, "a", None)] for f in "fd"])
state = 1


def some():
    global state
    state = (state * 1103515245 + 12345) % 2 ** 31
    m = state
    state = (state * 1103515245 + 12345) % 2 ** 31
    return (1 - 2 * (state & 1)) * (1 + m / 2 ** 31) * 2.0 ** (state // 2 % 45 - 28)


print("at random:", digest(attempt(pack, some()) for _ in range(20000)))
a, b = memoryview(bytes([0, 60, 0, 192])).cast("e"), memoryview(bytes([0, 60, 0, 192])).cast("e")
print("compared:", a == b, a == memoryview(bytes([0, 60, 0, 64])).cast("e"), memoryview(bytes([0, 126])).cast("e") == memoryview(bytes([0, 126])).cast("e"), memoryview(bytes([0, 0])).cast("e") == memoryview(bytes([0, 128])).cast("e"), a == memoryview(bytes([0, 0, 128, 63, 0, 0, 0, 192])).cast("f"), a.tolist() == [1.0, -2.0], 1.0 in a, a.index(-2.0), a.count(1.0))
print("at the very end of what there is:", [memoryview(bytes(n) + b"\x00\x3c").cast("e")[-1] for n in (0, 2, 6, 14, 30, 62, 4094)])
