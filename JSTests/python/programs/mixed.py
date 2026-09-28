# ints and floats side by side, wherever a value can be put. Each must come out what it went in.
l = [1]; l.append(0.5); l.append(2.0); l.append(3); print(l)
l = [0.5]; l.append(1); l.insert(0, 2); l.extend([3.0, 4]); print(l, l[1:], l[::-1], l[::2], l + [5, 6.0], l * 2, l.copy(), list(l), sorted(l), list(reversed(l)))
l = []; l.append(1); l.append(1.0); l.append(1); print(l, l.index(1.0), l.count(1), l.pop(), l.pop(0), l)
l = [1, 2, 3]; l[1] = 2.5; print(l); l[0:1] = [0.0, 0]; print(l); l += [7.0]; l += (8,); print(l); del l[0]; print(l); l.sort(); print(l); l.reverse(); print(l); l.remove(2.5); print(l)
z = [0] * 3; z[1] = 1.5; print(z); z = [0.0] * 3; z[1] = 1; print(z); z = [1, 2.0] * 2; print(z)
t = (1, 2.0, 3); print(t, t[1:], t + (4.0, 5), t * 2, tuple([1.0, 1]), t[0], t[1], max(t), min(t), sum(t), sorted(t, reverse=True))
a, b, c = 1, 2.0, 3; print(a, b, c); a, b = b, a; print(a, b); first, *rest = [1.0, 2, 3.0]; print(first, rest); *init, last = (1, 2.0, 3); print(init, last)
d = {"i": 1, "f": 1.0, "h": 0.5}; print(d, list(d.values()), list(d.items()), d["i"], d["f"], d.get("f"), d.pop("f"), d.setdefault("g", 2.0), d.copy(), {**d, "z": 3})
k = {1: "int", 2.0: "float", 3.5: "half"}; print(k, list(k), k[1.0], k[2], 2 in k, 2.0 in k, list(k.items())); k[1.0] = "still the int's"; k[2] = "still the float's"; print(k)
s = {1, 2.0, 3}; print(sorted(s), 1.0 in s, 2 in s, sorted(s | {4.0}), sorted({1.0, 1, 2}), len({1, 1.0, True}))
print([x for x in [1, 2.0, 3]], [x * 2 for x in [1, 2.0]], [x / 2 for x in [2, 4.0]], [x // 2 for x in [5, 5.0]], [x % 2 for x in [5, 5.0]], [x ** 2 for x in [3, 3.0]], [-x for x in [1, 1.0, 0, 0.0]], [abs(x) for x in [-1, -1.0]])
print(list(zip([1, 2.0], [3.0, 4])), list(enumerate([1.0, 2])), list(map(lambda x: x, [1, 2.0])), list(filter(None, [0, 0.0, 1, 1.0])), {x: x for x in [1, 2.5]}, list(x for x in (1, 2.0)))
def args(*a, **k): return a, k
def defaults(a=1, b=2.0, c=3): return [a, b, c]
print(args(1, 2.0, 3), args(*[1.0, 2]), args(x=1, y=1.0), defaults(), defaults(1.0), defaults(c=3.0), defaults(*[4, 5.0]), (lambda x, y=1.0: (x, y))(2))
def gen():
    yield 1
    yield 2.0
    got = yield 3
    yield got
g = gen(); print(next(g), next(g), next(g), g.send(4.0), list(gen())[:3])
class P:
    n = 1
    f = 1.0
    def __init__(self): self.i = 2; self.g = 2.0; self.l = [3, 3.0]
p = P(); print(p.n, p.f, p.i, p.g, p.l, P.n, P.f, sorted(p.__dict__.items())); p.i, p.g = p.g, p.i; print(p.i, p.g)
def closure():
    i, f = 1, 1.0
    def inner(): return i, f
    return inner
print(closure()(), [f() for f in [lambda v=v: v for v in (1, 2.0)]])
try: raise ValueError(1, 2.0)
except ValueError as e: print(e.args, e)
# a variable that is one and then the other, in a loop that gets hot
def hot(n):
    x = 0; seen = []
    for i in range(n):
        x = x + 1 if i % 3 else x + 0.5
        y = i if i % 2 else i * 1.0
        if i > n - 5: seen.append((y, type(y).__name__))
    return x, seen
print(hot(5000))
def acc(n, step):
    total = 0
    for _ in range(n): total += step
    return total
print(acc(0, 0.5), acc(4, 0.5), acc(4, 1), acc(4, 1.0), acc(5000, 1), acc(5000, 1.0), acc(5000, 0.5), acc(0, 1.0))
def pick(n):
    out = []
    for i in range(n):
        v = 1 if i & 1 else 1.0
        w = v * 2
        if i >= n - 4: out.append([v, w, v + w, v == w, v is v])
    return out
print(pick(5000))
m = [[0] * 2 for _ in range(2)]; m[0][0] = 1.0; m[1][1] = 1; print(m, [sum(r) for r in m], [[c * 2 for c in r] for r in m])
print(round(2.0), round(2.5), round(2, 1), round(2.0, 1), int(2.0), float(2), 2 == 2.0, 2 is 2.0 if False else "skip", divmod(5, 2), divmod(5.0, 2), max(1, 1.0), max(1.0, 1), min(2, 2.0), sum([1, 1.0]), sum([1.0, 1]), sum([]), sum([], 0.0))
print(str(1), str(1.0), repr(1.0), f"{1} {1.0} {1:.1f} {1.0:g} {2 / 1} {2 // 1} {2.0 // 1}", "%s %s" % (1, 1.0), "{} {}".format(1, 1.0), [str(x) for x in (1, 1.0)], hash(1) == hash(1.0), bool(0.0), bool(0))
print(2 ** 31, 2 ** 31 - 1, -2 ** 31, -2 ** 31 - 1, 2147483647 + 1, -2147483648 - 1, 65536 * 65536, 2 ** 31 * 1.0, float(2 ** 31), int(2.0 ** 31), 2 ** 31 // 2, (2 ** 31) - (2 ** 31), 2 ** 31 == 2.0 ** 31, {2 ** 40: "i"}[2.0 ** 40], type(2 ** 31 - 2 ** 31), [2 ** 31, 1, 0.5])
