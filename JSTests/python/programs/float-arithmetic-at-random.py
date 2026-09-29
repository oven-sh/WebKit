# Arithmetic on floats and complex numbers, to the last digit.
#
# Where CPython has a * b + c in one expression, what it is built with makes one instruction of it if the processor has one, and that rounds once and not twice. So what is expected here is what CPython gives on a
# processor of the same kind as it was taken on, which is ARM64.
state = [2463534242]


def rnd():
    v = state[0]
    v ^= (v << 13) & 0xFFFFFFFF
    v ^= v >> 17
    v ^= (v << 5) & 0xFFFFFFFF
    state[0] = v
    return v


def f():
    return (rnd() - 2 ** 31) / (rnd() % 100000 + 1) * 10.0 ** (rnd() % 7 - 3)


def run(name, op, n=20000):
    digest = 0
    for i in range(n):
        args = (f(), f(), f(), f())
        try:
            r = repr(op(*args))
        except (ZeroDivisionError, OverflowError, ValueError) as e:
            r = type(e).__name__
        for ch in r:
            digest = (digest * 31 + ord(ch)) % 1000000007
    print(name, digest)


run("complex * complex", lambda a, b, c, e: complex(a, b) * complex(c, e))
run("complex / complex", lambda a, b, c, e: complex(a, b) / complex(c, e))
run("float / complex", lambda a, b, c, e: a / complex(c, e))
run("int / complex", lambda a, b, c, e: 7 / complex(c, e))
run("complex * float", lambda a, b, c, e: complex(a, b) * c)
run("complex / float", lambda a, b, c, e: complex(a, b) / c)
run("abs(complex)", lambda a, b, c, e: abs(complex(a, b)))
run("complex ** 3", lambda a, b, c, e: complex(a / 1e3, b / 1e3) ** 3)
run("complex ** -2", lambda a, b, c, e: complex(a / 1e3, b / 1e3) ** -2)
run("complex ** complex", lambda a, b, c, e: complex(a / 1e3, b / 1e3) ** complex(c / 1e5, e / 1e5))
run("complex ** float", lambda a, b, c, e: complex(a / 1e3, b / 1e3) ** (c / 1e5))
run("float ** complex", lambda a, b, c, e: (abs(a) / 1e3) ** complex(c / 1e5, e / 1e5))
run("negative float ** float", lambda a, b, c, e: (-abs(a) / 1e3) ** (c / 1e5))
run("float % float", lambda a, b, c, e: a % b)
run("divmod", lambda a, b, c, e: divmod(a, b))
run("float // float", lambda a, b, c, e: a // b)
run("float ** float", lambda a, b, c, e: abs(a) ** (b / 1e4))
run("round to 3", lambda a, b, c, e: round(a, 3))
run("round to -2", lambda a, b, c, e: round(a, -2))
run("sum of floats", lambda a, b, c, e: sum([a, b, c, e]))
run("sum of complex", lambda a, b, c, e: sum([complex(a, b), complex(c, e), a]))
run("int / int", lambda a, b, c, e: int(a * 1e6) / (int(b * 1e3) or 1))
run("as_integer_ratio", lambda a, b, c, e: a.as_integer_ratio())
run("hash", lambda a, b, c, e: (hash(a), hash(complex(a, b))))
run("formatted", lambda a, b, c, e: ("%.3f %.10e %g" % (a, b, c), format(e, ".7"), format(complex(a, b), ".5g")))
