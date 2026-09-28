def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)

# true division is rounded once
cases = [
    (10**400, 10**399), (10**400, 3), (3, 10**400), (10**308 * 17, 10), (2**1024, 2), (2**1024, 1), (2**1024 - 2**970, 1), (2**1024 - 2**970 - 1, 1),
    (2**53 + 1, 1), (2**53 + 1, 2), (2**54 + 3, 2), (2**54 + 2, 4), (2**60 + 2**7, 2**7 + 1), (10**23, 1), (10**23, 10), (123456789012345678901234567890, 987654321),
    (-10**30, 7), (10**30, -7), (-10**30, -7), (0, -10**30), (0, 10**30), (1, 2**1074), (1, 2**1075), (3, 2**1075), (1, 2**1076), (2**1000 + 1, 2**2074), (5, 2**1076), (7, 2**1076),
    (2**2000, 2**1000), (2**2000 + 1, 2**1000 - 1), (10**500, 10**200 + 1), (9007199254740993, 3), (18014398509481985, 5), (2**63, 3), (2**64 - 1, 2**11), (2**64 + 2**11, 2**11),
    (True, 10**20), (10**20, True), (2**31, 2**31 - 1), (-2**31, 3), (2**62 + 1, 2**62 - 1),
]
for a, b in cases:
    show(f"{a.bit_length()}b/{b.bit_length()}b", lambda: (a / b).hex())
show("zero", lambda: 10**30 / 0)
seed = 12345
def rnd(bits):
    global seed
    v = 0
    for _ in range(bits // 16 + 1):
        seed = (seed * 1103515245 + 12345) % 2**31
        v = (v << 16) | (seed >> 8 & 0xffff)
    return v >> (16 - bits % 16) or 1
acc = 0
for i in range(600):
    a, b = rnd(20 + i % 200), rnd(20 + (i * 7) % 190)
    acc = (acc * 31 + hash(a / b)) % (2**61 - 1)
print("many", acc)

# pow with a modulus and a negative power
for args in [(3, -1, 7), (3, -2, 7), (38, -1, 97), (2, -1, 4), (10, -3, 7**20), (-3, -1, 7), (3, -1, -7), (-3, -5, -7), (5, -1, 1), (5, -1, -1), (0, -1, 5), (10**30 + 7, -1, 10**40 + 121), (True, -1, 5), (7, -1, 2**64), (6, -1, 9), (3, -10**20, 1000003)]:
    show(f"pow{args}", lambda: pow(*args))
show("bit_length", lambda: [(x).bit_length() for x in (0, 1, -1, 255, 256, 2**31, -2**31, 2**64, 2**64 - 1, 10**100)])

# float.hex() and float.fromhex()
for x in [0.0, -0.0, 1.0, -1.0, 0.5, 3.14159, 1e300, 1e-300, 5e-324, 2.2250738585072014e-308, 1.7976931348623157e308, float("inf"), float("-inf"), float("nan"), 255.0, 1/3]:
    show(f"hex {x!r}", lambda: (x.hex(), float.fromhex(x.hex()) == x or x != x))
for s in ["0x1p0", "1", "  0x1.8p1  ", "-0x.8", "0X1P+3", "1.", ".1", "0x", "", "p1", "0x1p", "0x1p+", "0x1.p-1", "inf", "-Infinity", "nan", "+nan", "infin", "0x1p1024", "0x1.fffffffffffff8p1023", "0x1.fffffffffffff7p1023",
          "0x1p-1075", "0x1.0000000000001p-1075", "0x1p-1074", "0x0.8p-1074", "0x1.00000000000008p0", "0x1.00000000000018p0", "0x1.000000000000081p0", "0x10000000000000000000000000", "0x1p99999999999999999999", "0x1p-99999999999999999999",
          "0x0p99999999999999999999", "1 2", "0x1g", "0x1.1.1", "abc", "0xabc.defp-4", "\t0x1\n", "0x1\0", "١"]:
    show(f"fromhex {s!r}", lambda: float.fromhex(s).hex())
class F(float): pass
show("class methods", lambda: (type(F.fromhex("0x1p0")).__name__, type(F.from_number(2)).__name__, float.from_number(3), float.from_number(True), float.__getformat__("double"), float.__getformat__("float")))
show("from_number str", lambda: float.from_number("1"))
show("getformat", lambda: float.__getformat__("x"))
show("fromhex type", lambda: float.fromhex(1))
show("getnewargs", lambda: ((1.5).__getnewargs__(), (7).__getnewargs__(), F(2.5).__getnewargs__(), True.__getnewargs__(), (10**30).__getnewargs__()))
