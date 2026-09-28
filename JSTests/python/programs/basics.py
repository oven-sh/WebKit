def fib(n):
    return n if n < 2 else fib(n - 1) + fib(n - 2)

class Vec:
    def __init__(self, x, y):
        self.x = x
        self.y = y
    def __add__(self, o):
        return Vec(self.x + o.x, self.y + o.y)
    def __repr__(self):
        return f"Vec({self.x}, {self.y})"

print(fib(20), Vec(1, 2) + Vec(3, 4.0))
print(4 / 2, 7 // 2, -7 // 2, -7 % 3, 2 ** 100, 2 ** -1, 1e16, 0.1 + 0.2, 10 ** 20 // 3)
print([x * x for x in range(5) if x % 2 == 0], {k: v for k, v in zip("ab", (1, 2))}, {1, 2} | {3})
def gen(n):
    for i in range(n):
        got = yield i
        if got: print("got", got)
g = gen(3); print(next(g), g.send("hi"), list(g))
try:
    {}["missing"]
except KeyError as e:
    print("KeyError", e, repr(e))
finally:
    print("finally")
def f(a, b=2, *args, c=3, **kw): return (a, b, args, c, kw)
print(f(1), f(1, 5, 6, 7, c=8, d=9), f(b=1, a=0))
