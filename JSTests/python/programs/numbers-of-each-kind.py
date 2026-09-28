# int and float stay what they are
print(1, 1.0, 2.0 * 3, 6 / 3, 7 / 2, 2 * 3, 0.5 + 0.5, 1.5 - 0.5, 3.0 // 2, 3 // 2.0, 7.5 % 2, 2 ** 0.5, 4 ** 0.5, 2.0 ** 3)
print(type(1), type(1.0), type(2 / 1), type(2 // 1), type(True), type(1 + True), type(0.5 + 0.5), type(10 ** 30))
print(1 == 1.0, True, hash(1) == hash(1.0), {1: "a", 1.0: "b", True: "c"}, {1.0, 1, True})
print(-0.0, 0.0 * -1, 0 * -1, -0.0 == 0, abs(-0.0), 1 / 3, 1e100, 1e-7, 123456789.123456789, 1e15, 1e16, 1.5e300 * 1e10)
# arbitrary precision, in and out of what a double holds exactly
big = 2 ** 53
print(big - 1, big, big + 1, big + 2, big * big, (big + 1) - big, (big * big) // big, -(big + 1), (big + 1) % 10)
print(9007199254740991 + 1, 9007199254740993 - 2, 3037000500 * 3037000500, 2 ** 62 + 2 ** 62, 10 ** 18 * 10, 99999999999 * 99999999999)
n = 1
for i in range(1, 26): n *= i
print(n, n // 10 ** 20, n % 1000007, len(str(n)), n > 10 ** 25, n == n + 0, float(10 ** 20), int(1e20), 10 ** 20 / 10 ** 18)
print(7 // 2, -7 // 2, 7 // -2, -7 // -2, 7 % 3, -7 % 3, 7 % -3, -7 % -3, divmod(-7, 2), divmod(7.5, 2), -7.5 // 2, -7.5 % 2)
print(1 << 10, 1 << 40, 1 << 100, 2 ** 100 >> 90, -1 >> 1, 5 & 3, 5 | 3, 5 ^ 3, ~5, ~-1, 2 ** 70 & (2 ** 70 - 1), True & False, True | False)
print(int("42"), int("-17"), int(3.9), int(-3.9), int("ff", 16), float("1.5"), float("2"), float("inf"), float("-inf"), float("nan"), int(True))
print(round(2.5), round(3.5), round(-2.5), round(2.675, 2), round(0.125, 2), round(1.15, 1), round(1234, -2), round(7.0), round(1e20), round(2.5, 0))
print(bool(0), bool(0.0), bool(1), bool(-1), bool(0.1), bool(10 ** 30), not 0, not 1.0)
print(1 < 2 < 3, 1 < 3 < 2, 1 == 1.0 == True, 2 ** 60 < 2 ** 60 + 1, 2 ** 60 < 1e30, 0.1 + 0.2 == 0.3, 1 != 1.0, 3 > 2.5 >= 2.5)
print(min(3, 1.5, 2), max(1, 2.0), sum([1, 2, 3]), sum([0.5, 0.5]), sum([1, 2.0]), abs(-3), abs(-3.5), pow(2, 10), pow(2, 10, 1000), sum(range(101)))
for bad in (lambda: 1 / 0, lambda: 1 // 0, lambda: 1 % 0, lambda: 1.0 / 0, lambda: 0 ** -1, lambda: int("x"), lambda: float("x"), lambda: 1 + "a", lambda: "a" + 1, lambda: -"a", lambda: 1 < "a", lambda: int(float("nan"))):
    try: bad()
    except Exception as e: print(type(e).__name__, e)
print(f"{3.14159:.2f} {42:5d}|{42:<5}|{42:^5}|{42:05d} {1234567:,} {0.5:%} {255:x} {255:#x} {5:b} {1e10:.3e} {1.0} {2:.1f} {1/3:.3} {100.0:g} {-1.5:+.1f} {12345.678:,.2f}")
print("%d %s %5.2f %x %05d %-5d| %r %%" % (42, "s", 3.14159, 255, 42, 42, "q"), "%s" % 1.0, "%.3f" % 2, "%d" % 3.9, "%5s|%-5s|" % ("a", "b"))
import math
print(math.sqrt(16), math.sqrt(2), math.floor(2.7), math.ceil(2.1), math.floor(-2.5), math.pi, math.gcd(12, 18), math.factorial(20), math.factorial(25), math.isnan(float("nan")), math.log(math.e), math.hypot(3, 4))
