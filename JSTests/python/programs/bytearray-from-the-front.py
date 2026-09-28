# What is taken from the front of a bytearray is not made up for by moving the rest, if there is much of it: the bytearray begins further on in what it has. Whatever is done after that is to come out as it would.


class Random:
    def __init__(self, seed):
        self.state = seed

    def below(self, n):
        self.state = (self.state * 6364136223846793005 + 1442695040888963407) % (1 << 64)
        return (self.state >> 33) % n


def checksum(values):
    total = 0
    for value in values:
        total = (total * 31 + value) % 1000000007
    return total


def work(seed, steps, start, weights):
    r = Random(seed)
    a = bytearray(r.below(256) for i in range(start))
    model = list(a)
    kinds = [kind for kind, weight in weights.items() for i in range(weight)]
    for step in range(steps):
        kind = kinds[r.below(len(kinds))]
        n = len(model)
        k = r.below(min(n, 200) + 1)
        piece = [r.below(256) for i in range(r.below(120))]
        if kind == "front":
            del a[:k]
            del model[:k]
        elif kind == "front, some put back":
            a[:k] = bytes(piece[:k // 2])
            model[:k] = piece[:k // 2]
        elif kind == "front, more put back":
            a[:k] = bytes(piece)
            model[:k] = piece
        elif kind == "pop(0)" and n:
            if a.pop(0) != model.pop(0):
                return "pop(0) at step %d" % step
        elif kind == "back":
            del a[n - k:]
            del model[n - k:]
        elif kind == "middle":
            i = r.below(n + 1)
            del a[i:i + k]
            del model[i:i + k]
        elif kind == "add":
            a += bytes(piece)
            model += piece
        elif kind == "add much":
            a.extend(bytes(piece) * 20)
            model.extend(piece * 20)
        elif kind == "append":
            a.append(k & 255)
            model.append(k & 255)
        elif kind == "insert":
            i = r.below(n + 1)
            a.insert(i, k & 255)
            model.insert(i, k & 255)
        elif kind == "write" and n:
            i = r.below(n)
            a[i] = k & 255
            model[i] = k & 255
        elif kind == "clear" and step % 200 == 0:
            a.clear()
            model.clear()
        elif kind == "resize":
            size = r.below(2 * n + 2)
            a.resize(size)
            model = model[:size] + [0] * (size - len(model))
        elif kind == "every other" and n:
            del a[::2]
            del model[::2]
        elif kind == "times":
            a *= 2
            model *= 2
            if len(model) > 20000:
                del a[5000:]
                del model[5000:]
        if len(a) != len(model) or (model and (a[0] != model[0] or a[-1] != model[-1] or a[len(model) // 2] != model[len(model) // 2])):
            return "%s at step %d" % (kind, step)
        if step % 50 == 0 and (list(a) != model or bytes(a) != bytes(model) or a != bytearray(model) or list(memoryview(a)) != model or a.find(bytes(model[-3:])) != bytes(model).find(bytes(model[-3:]))):
            return "%s at step %d, all of it" % (kind, step)
    return "right", len(a), checksum(a)


MIXES = {
    "what comes in is dealt with": {"add": 4, "front": 5},
    "in large pieces": {"add much": 2, "front": 9},
    "from both ends": {"add": 3, "add much": 1, "front": 4, "back": 3, "pop(0)": 2, "append": 2},
    "put back": {"add": 3, "add much": 1, "front": 2, "front, some put back": 3, "front, more put back": 2},
    "everything": {"add": 3, "add much": 1, "front": 4, "front, some put back": 1, "front, more put back": 1, "pop(0)": 1, "back": 1, "middle": 1, "append": 2, "insert": 1, "write": 2, "clear": 1, "resize": 1, "every other": 1, "times": 1},
}
for name, weights in MIXES.items():
    for start in (0, 10, 999, 1000, 1001, 5000, 20000):
        print(name, start, work(start + 1, 1500, start, weights))

# All of it, a little at a time, from each size that matters
for n in (0, 1, 999, 1000, 1001, 1002, 2000, 4096, 20000):
    for k in (1, 7, 999, 1000, 1001):
        a = bytearray(i & 255 for i in range(n))
        taken = []
        while a:
            taken += a[:k]
            del a[:k]
        print("taken", k, "at a time from", n, taken == [i & 255 for i in range(n)], len(a), a == b"")
a = bytearray(b"x" * 5000)
del a[:4000]
b = a.copy()
c = a[:]
m = memoryview(a)
print("copies, and what looks at it", len(a), len(b), len(c), len(m), a == b == c == m, bytes(m[:3]), m[999], a.hex()[:6], hash(bytes(a)) == hash(bytes(b)))
m.release()
a += b"y" * 5000
del a[:1000]
print("and after", len(a), set(a), b == b"x" * 1000, c == b"x" * 1000)
