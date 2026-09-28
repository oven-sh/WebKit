# A dict and a set keep what is in them in the order that it was put in, with gaps where things have been taken out. What is taken from either end is done with there and then, so that popitem(), pop() and
# next(iter(d)) do not go over what was taken before. Whatever is done, and in whatever order, what is left is to be right.


class Random:
    def __init__(self, seed):
        self.state = seed

    def below(self, n):
        self.state = (self.state * 6364136223846793005 + 1442695040888963407) % (1 << 64)
        return (self.state >> 33) % n


def checksum(values):
    total = 0
    for value in values:
        total = (total * 31 + hash(value)) % 1000000007
    return total


def work_dict(seed, steps, span, weights):
    r = Random(seed)
    d = {}
    log = 0
    kinds = [kind for kind, weight in weights.items() for i in range(weight)]
    for step in range(steps):
        kind = kinds[r.below(len(kinds))]
        k = r.below(span)
        if kind == "put":
            d[k] = step
        elif kind == "delete":
            log += d.pop(k, -1)
        elif kind == "popitem" and d:
            log += sum(d.popitem())
        elif kind == "first" and d:
            first = next(iter(d))
            log += first + d.pop(first)
        elif kind == "last" and d:
            last = next(reversed(d))
            log += last
            del d[last]
        elif kind == "look":
            log += (k in d) + d.get(k, 0) + len(d)
        elif kind == "through" and step % 64 == 0:
            log += checksum(d.items()) + checksum(reversed(d)) + checksum(d.copy()) + checksum(list(d.values())[:3])
        elif kind == "clear" and step % 512 == 0:
            d.clear()
        log %= 1000000007
    return log, len(d), checksum(d.items()), list(d)[:3], list(d)[-3:]


def work_set(seed, steps, span, weights):
    # Which one pop() gives is up to the set, and what comes after depends on it. So it is kept up with here, in a list of what is in it and what is not, and what is printed is whether the two agree.
    r = Random(seed)
    s = set()
    there = [False] * span
    count = 0
    kinds = [kind for kind, weight in weights.items() for i in range(weight)]
    for step in range(steps):
        kind = kinds[r.below(len(kinds))]
        k = r.below(span)
        if kind == "put":
            s.add(k)
            count += not there[k]
            there[k] = True
        elif kind == "delete":
            s.discard(k)
            count -= there[k]
            there[k] = False
        elif kind in ("pop", "first") and s:
            if kind == "pop":
                x = s.pop()
            else:
                x = next(iter(s))
                s.remove(x)
            if not there[x] or x in s:
                return "gave %r at step %d" % (x, step)
            there[x] = False
            count -= 1
        elif kind == "through" and step % 64 == 0:
            expected = [i for i in range(span) if there[i]] if span <= 1000 else None
            if expected is not None and (sorted(s) != expected or sorted(s.copy()) != expected or sorted(s | {k}) != sorted({*expected, k}) or sorted(s - {k}) != [i for i in expected if i != k]):
                return "wrong at step %d" % step
        elif kind == "clear" and step % 512 == 0:
            s.clear()
            there = [False] * span
            count = 0
        if len(s) != count or (k in s) != there[k]:
            return "wrong about %r at step %d" % (k, step)
    return "right"


MIXES = {
    "a stack": {"put": 5, "popitem": 5, "pop": 5, "look": 1, "through": 1},
    "a stack that grows": {"put": 6, "popitem": 3, "pop": 3, "look": 1, "through": 1},
    "a queue": {"put": 5, "first": 5, "look": 1, "through": 1},
    "from both ends": {"put": 6, "first": 2, "last": 2, "popitem": 2, "pop": 2, "through": 1},
    "from anywhere": {"put": 5, "delete": 5, "look": 2, "through": 1},
    "everything": {"put": 8, "delete": 3, "popitem": 2, "pop": 2, "first": 2, "last": 2, "look": 3, "through": 1, "clear": 1},
    "mostly taking": {"put": 3, "delete": 3, "popitem": 3, "pop": 3, "first": 3, "last": 3, "through": 1},
}
for name, weights in MIXES.items():
    for span in (4, 30, 1000, 100000):
        print(name, span, work_dict(span + 1, 40000, span, weights), work_set(span + 2, 40000, span, weights))

# One at a time, for long enough that every place in the index has been used and given up many times over. What is not there is still to be found not to be.
d = {}
s = set()
for i in range(300000):
    d[i] = i
    s.add(i)
    assert d.popitem() == (i, i) and s.pop() == i and i not in d and i not in s and i + 1 not in d and i + 1 not in s
print("put and taken in turn", len(d), len(s))
d = {-1: -1}
s = {-1}
for i in range(300000):
    d[i] = i
    s.add(i)
    del d[i]
    s.remove(i)
    assert i not in d and i not in s and -1 in d and -1 in s
print("with one that stays", d, s)
for n in (0, 1, 2, 5, 8, 9, 100):
    d = {i: i for i in range(n)}
    order = []
    while d:
        order.append(d.popitem()[0] if len(order) % 2 else d.pop(next(iter(d))))
    print("from each end in turn", n, order[:12])
