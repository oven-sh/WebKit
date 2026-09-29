# An array that is changed by what is run in the middle of doing something to it: by the __index__() or __float__() of what is being put into it, by the __eq__() of what is being looked for in it, by the file that it is being
# written to or read from.
import array
import io

A = array.array


def attempt(f, *a):
    try:
        return f(*a)
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)


class Acts:
    "A number that does something when it is asked what number it is"
    def __init__(self, act, value=7): self.act, self.value = act, value
    def __index__(self):
        self.act()
        return self.value
    def __float__(self):
        self.act()
        return float(self.value)


class Equal:
    "What does something when it is compared, and says what it was told to say"
    def __init__(self, act, answer=False): self.act, self.answer, self.times = act, answer, 0
    def __eq__(self, other):
        self.times += 1
        self.act()
        return self.answer


ACTS = (("clear", lambda x: x.clear()), ("pop", lambda x: x and x.pop()), ("cut to one", lambda x: x.__delitem__(slice(1, None))), ("append", lambda x: x.append(1)), ("grow", lambda x: x.extend([1] * 1000)), ("nothing", lambda x: None))
for c in "bBhHiIlLqQfd":
    for name, act in ACTS:
        out = []
        for label, f in (
            ("x[0] =", lambda x, n: x.__setitem__(0, n)),
            ("x[-1] =", lambda x, n: x.__setitem__(-1, n)),
            ("x[2] =", lambda x, n: x.__setitem__(2, n)),
            ("append", lambda x, n: x.append(n)),
            ("insert 0", lambda x, n: x.insert(0, n)),
            ("insert 2", lambda x, n: x.insert(2, n)),
            ("insert -1", lambda x, n: x.insert(-1, n)),
            ("extend", lambda x, n: x.extend([1, n, 3])),
            ("extend by an iterator", lambda x, n: x.extend(iter([1, n, 3]))),
            ("fromlist", lambda x, n: x.fromlist([1, n, 3])),
        ):
            x = A(c, [10, 20, 30, 40])
            r = attempt(f, x, Acts(lambda: act(x)))
            # An array that is made shorter and then longer again has in CPython whatever was left where it used to be, and zeros here. So what is in it is looked at only if it was let alone.
            out.append((label, r, len(x), x.tolist() if name == "nothing" else None))
        print(c, name, "=>", out)

print("---- where it is, is asked first")
for name, act in ACTS:
    x = A("i", [10, 20, 30, 40])
    print(name, "=>", [(attempt(f, y, Acts(lambda: act(y), v)), len(y)) for f, v in ((lambda y, n: y[n], 3), (lambda y, n: y[n], -1), (lambda y, n: y.__setitem__(n, 5), 3), (lambda y, n: y.__delitem__(n), 3), (lambda y, n: y.pop(n), 3), (lambda y, n: y.insert(n, 5), 3), (lambda y, n: y[n:].tolist(), 2), (lambda y, n: y[:n].tolist(), 3), (lambda y, n: y[::n].tolist(), 2), (lambda y, n: y.__delitem__(slice(n, None)), 2), (lambda y, n: y.__setitem__(slice(n, None), A("i", [1])), 2), (lambda y, n: len(y * n), 2), (lambda y, n: len(y.__imul__(n)), 2), (lambda y, n: y.index(40, n), 1)) for y in [A("i", [10, 20, 30, 40])]])

print("---- while it is looked through")
for name, act in ACTS:
    for answer in (False, True):
        out = []
        for label, f in (("count", lambda x, e: x.count(e)), ("index", lambda x, e: x.index(e)), ("in", lambda x, e: e in x), ("remove", lambda x, e: x.remove(e))):
            # There is no coming to the end of what has something added to it every time that it is looked at.
            if name in ("grow", "append") and (not answer or label == "count"):
                continue
            x = A("i", [10, 20, 30, 40])
            e = Equal(lambda: act(x), answer)
            r = attempt(f, x, e)
            out.append((label, r, e.times, len(x)))
        print(name, answer, "=>", out)

print("---- while it is made")
for name, act in (("the list is cleared", lambda s: s.clear()), ("the list is cut", lambda s: s.__delitem__(slice(2, None))), ("the list grows", lambda s: s.append(9))):
    s = [1, None, 3, 4]
    s[1] = Acts(lambda: act(s))
    print(name, "=>", attempt(A, "i", s), len(s))
    s = [1, None, 3, 4]
    s[1] = Acts(lambda: act(s))
    x = A("i", [0])
    print(name, "fromlist =>", attempt(x.fromlist, s), x.tolist(), len(s))
    s = [1, None, 3, 4]
    s[1] = Acts(lambda: act(s))
    x = A("i", [0])
    print(name, "extend =>", attempt(x.extend, s), x.tolist(), len(s))

print("---- files")


class Writes:
    def __init__(self, act): self.act, self.sizes = act, []
    def write(self, b):
        self.sizes.append(len(b))
        self.act()


class Reads:
    def __init__(self, act, data): self.act, self.data = act, data
    def read(self, n):
        self.act()
        return self.data


for name, act in (("append", lambda x: x.append(1)), ("nothing", lambda x: None)):
    x = A("b", bytes(200000))
    w = Writes(lambda: act(x))
    print("tofile", name, "=>", attempt(x.tofile, w), w.sizes, len(x))
for name, act in ACTS:
    x = A("i", [10, 20])
    print("fromfile", name, "=>", attempt(x.fromfile, Reads(lambda: act(x), b"\1\0\0\0\2\0\0\0"), 2), len(x), x.tolist()[-2:])

print("---- what goes through it")
for name, act in ACTS[:4]:
    x = A("i", [1, 2, 3, 4, 5, 6])  # It is changed no more than three times.
    out = []
    for v in x:
        out.append(v)
        if len(out) < 4:
            act(x)
    print(name, "=>", out, len(x))
