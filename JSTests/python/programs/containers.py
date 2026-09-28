l = [3, 1, 2]
l.append(4); l.extend([5, 6]); l.insert(0, 0); print(l, len(l), l[0], l[-1], l[1:3], l[::-1], l[::2], 3 in l, 9 in l, l.index(2), l.count(1))
print(l.pop(), l.pop(0), l, sorted(l), sorted(l, reverse=True), sorted(["bb", "a", "ccc"], key=len), list(reversed(l)), l + [9], l * 2, [] == [], [1, 2] < [1, 3], [1] < [1, 0])
l.sort(); print(l); l.sort(reverse=True); print(l); l.reverse(); print(l); l.remove(3); print(l); del l[0]; print(l); l[0] = 99; print(l); l[1:2] = [7, 8, 9]; print(l); del l[1:3]; print(l); l.clear(); print(l)
a = [1, 2, 3]; b = a; c = a.copy(); b.append(4); print(a, c, a is b, a is c, a == b, [[0] * 2] * 2, [[]] * 2 == [[], []])
t = (1, "a", 2.0); print(t, t[0], t[-1], t[1:], len(t), t + (3,), t * 2, (), (1,), 1 in t, t.index("a"), t.count(1), tuple([1, 2]), (1, 2) < (1, 3), (1, 2) == (1, 2), hash((1, 2)) == hash((1, 2)))
x, y = 1, 2; x, y = y, x; print(x, y); (p, q), r = (1, 2), 3; print(p, q, r); first, *rest = [1, 2, 3]; print(first, rest); *init, last = "abc"; print(init, last); h, *m, z = range(5); print(h, m, z)
d = {"a": 1, "b": 2}
d["c"] = 3; print(d, len(d), d["a"], d.get("z"), d.get("z", 0), "a" in d, "z" in d, list(d), list(d.keys()), list(d.values()), list(d.items()))
print(d.pop("a"), d.pop("zz", None), d, d.setdefault("e", 5), d.setdefault("e", 6), d); d.update({"f": 7}, g=8); print(d); del d["b"]; print(d, d.copy() == d, dict(a=1), dict([("x", 1)]), {**d, "h": 9}, d | {"i": 1})
for k, v in d.items(): print(k, v, end=";")
print()
print({(1, 2): "t", 1: "i", "1": "s", None: "n", 1.5: "f", True: "b"}, {(1, 2): 1}[(1, 2)], {i: i * i for i in range(4)}, dict(zip("ab", [1, 2])), d.keys(), d.values(), d.items())
s = {3, 1, 2, 1}; s.add(5); s.discard(9); s.remove(1); print(sorted(s), len(s), 2 in s, sorted(s | {9}), sorted(s & {2, 3}), sorted(s - {2}), sorted(s ^ {2, 7}), {1} <= {1, 2}, {1, 2} < {1, 2}, set(), set("aab") == {"a", "b"}, frozenset([1]) == {1}, sorted({1, 2}.union([3])), {1, 2}.isdisjoint({3}))
print(list(range(5)), list(range(2, 8, 2)), list(range(5, 0, -1)), range(3), len(range(10)), range(10)[3], range(10)[-1], 5 in range(10), list(range(0)), range(1, 10, 3)[1:], sum(range(4)))
print(list(enumerate("ab")), list(enumerate("ab", 1)), list(zip([1, 2], "ab", (True, False))), list(map(str, [1, 2])), list(map(lambda a, b: a + b, [1, 2], [3, 4])), list(filter(None, [0, 1, "", "a"])), list(filter(lambda v: v > 1, [1, 2, 3])))
print(any([0, 1]), all([1, 0]), any([]), all([]), min([3, 1, 2]), max("a", "b"), min([1, 2], key=lambda v: -v), max([], default=0), sum([[1], [2]], []), len({}), bool([]), bool([0]), bool({}), bool(()), bool(""), bool(set()))
m = [[1, 2], [3, 4]]; print([v for row in m for v in row], [[row[i] for row in m] for i in range(2)], sorted({v % 2 for row in m for v in row}), sum(v for row in m for v in row), list(v * 2 for v in range(3)))
class K:
    def __init__(self, v): self.v = v
    def __hash__(self): return hash(self.v)
    def __eq__(self, o): return isinstance(o, K) and self.v == o.v
print({K(1): "a", K(1): "b"}[K(1)], len({K(1), K(1), K(2)}), K(1) in [K(1)], K(1) in {K(1)}, [K(1)].index(K(1)), K(1) == K(1), K(1) != K(2))
for bad in (lambda: [][0], lambda: {}["k"], lambda: [].pop(), lambda: [1].remove(2), lambda: (1,)[3], lambda: {[]: 1}, lambda: {1}.remove(2), lambda: [1, 2][1.0], lambda: len(1), lambda: iter(1), lambda: (lambda a, b: 0)(*[1]), lambda: [1].index(5), lambda: 1[0], lambda: {}.popitem()):
    try: bad()
    except Exception as e: print(type(e).__name__, e)
try:
    a, b = [1, 2, 3]
except ValueError as e: print(e)
try:
    a, b = [1]
except ValueError as e: print(e)
