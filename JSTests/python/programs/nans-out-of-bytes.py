# A float that is read out of bytes can be any NaN at all, and there are NaNs that the engine has other uses for. Whatever the bytes are, what comes of them is a float, and stays one whatever is done to it.
import marshal
import math


def bytes_of(bits):
    return bits.to_bytes(8, "little")


def bits_of(x):
    return int.from_bytes(marshal.dumps(x)[1:], "little")


def by_marshal(bits):
    return marshal.loads(b"g" + bytes_of(bits))


def by_memoryview(bits):
    return memoryview(bytes_of(bits)).cast("d")[0]


def by_tolist(bits):
    return memoryview(bytes_of(bits)).cast("d").tolist()[0]


def by_complex(bits):
    return marshal.loads(b"y" + bytes_of(bits) + bytes_of(bits)).imag


def sign(x):
    return math.copysign(1.0, x)


def look(x):
    "What is so of any NaN, whatever else it has in it"
    minus = -x
    return (type(x).__name__, x != x, math.isnan(x), repr(x), sign(x), type(minus).__name__, math.isnan(minus), sign(minus), sign(-minus), sign(abs(x)), type(x + 1.0).__name__, math.isnan(x * 2.0), math.isnan(x - x), type(+x).__name__, isinstance([x][0], float), type((x, minus)[1]).__name__, str(x), "%r" % (minus,), bool(x), x == x, x < 1.0, math.isnan(math.copysign(x, -1.0)), sign(math.copysign(x, -1.0)), sign(math.copysign(x, 1.0)))


# Every way that the top sixteen bits of a NaN can be, with several ways for the rest
TOPS = [t for s in (0x7FF0, 0xFFF0) for t in range(s, s + 16)]
RESTS = (0x000000000001, 0x000000000000, 0xFFFFFFFFFFFF, 0x800000000000, 0x000012345678, 0x7FFFFFFFFFFE, 0x0000DEADBEE8, 0x100000000010)
PATTERNS = [t << 48 | r for t in TOPS for r in RESTS if (t & 0xF) or r]
for name, make in (("marshal", by_marshal), ("memoryview", by_memoryview), ("tolist", by_tolist), ("complex", by_complex)):
    seen = {}
    for bits in PATTERNS:
        seen.setdefault(look(make(bits))[:4] + look(make(bits))[5:7] + look(make(bits))[9:22], []).append(bits)
    print(name, len(PATTERNS), "patterns come to", len(seen), "kind:", sorted(seen)[0] if len(seen) == 1 else sorted(seen))
    print(name, "the sign is kept:", all(sign(make(bits)) == (-1.0 if bits >> 63 else 1.0) for bits in PATTERNS), "and turned:", all(sign(-make(bits)) == (1.0 if bits >> 63 else -1.0) for bits in PATTERNS))

# Over and over, so that it is compiled
def churn(make, bits, times):
    x = make(bits)
    total = 0
    for _ in range(times):
        x = -x
        y = abs(x)
        z = -y
        total += (type(x) is float) + (type(y) is float) + (type(z) is float) + (x != x) + (z != z)
    return total


print("over and over:", [churn(by_marshal, bits, 3000) for bits in (0x7FFE000000000000, 0x7FFC000000000000, 0xFFFFFFFFFFFFFFFF, 0x7FFFFFFFFFFFFFFF, 0xFFFE000012345678, 0x7FFE0000DEADBEE8, 0x7FF0000000000001)])

# In containers, and as what is looked up by
for bits in (0x7FFE000000000010, 0xFFFE000000000010, 0xFFFC000000000000, 0xFFFFFFFFFFFFFFFF):
    x = by_marshal(bits)
    print(hex(bits), [type(v).__name__ for v in ([x] * 2 + [-x])], type({"k": x}["k"]).__name__, type(sorted([x])[0]).__name__, type(max(x, x)).__name__, repr(complex(x, -x)), repr((x, -x)), "{:f} {:e} {!r}".format(x, -x, x), math.isnan(float(x)), math.isnan(sum([x, 1.0])), type(marshal.loads(marshal.dumps(x))).__name__, type(marshal.loads(marshal.dumps(-x))).__name__)

# Four bytes as well as eight
FOUR = [t << 16 | r for s in (0x7F80, 0xFF80) for t in range(s, s + 128, 3) for r in (0x0001, 0xFFFF, 0x0000, 0x8000) if (t & 0x7F) or r]
print("of four bytes:", len(FOUR), {look(memoryview(b.to_bytes(4, "little")).cast("f")[0])[:4] for b in FOUR}, all(sign(memoryview(b.to_bytes(4, "little")).cast("f")[0]) == (-1.0 if b >> 31 else 1.0) for b in FOUR))

# What tells one NaN from another is kept, all but one bit of it, which is left out of this
KEPT = [bits for bits in PATTERNS if not bits >> 50 & 1]
print("kept as it is:", len(KEPT), all(bits_of(by_marshal(bits)) == bits for bits in KEPT), all(bits_of(by_memoryview(bits)) == bits for bits in KEPT), all(bits_of(-by_marshal(bits)) == bits ^ 1 << 63 for bits in KEPT), all(bits_of(abs(by_marshal(bits))) == bits & ~(1 << 63) for bits in KEPT))
print("what is not a NaN is as it was:", all(bits_of(by_marshal(bits)) == bits and bits_of(by_memoryview(bits)) == bits for bits in (0, 1 << 63, 0x7FF0000000000000, 0xFFF0000000000000, 0x7FEFFFFFFFFFFFFF, 0xFFEFFFFFFFFFFFFF, 1, 0x8000000000000001, 0x3FF8000000000000, 0x4340000000000000, 0xC1E0000000000000, 0x41DFFFFFFFC00000)))
