# How long things take as what they are done to gets longer.
#
#   complexity.py [what]
#
# Each thing here is done about n times to something of about n elements, so it should take about four times as long when n is four times as much. It is timed at both, and what is printed is the power of n that
# it goes up by, which is 1 if so and 2 if each of the n takes as long as there are elements, and how long each of the n took at the larger size, in nanoseconds. It is not a test: what it prints depends on the
# machine and on what else the machine is doing. compare-complexity.py puts what two engines printed side by side and picks out what goes up faster in one, or takes many times as long.

import sys
import time
from math import log


class Key:
    def __init__(self, n):
        self.n = n

    def __hash__(self):
        return self.n

    def __eq__(self, other):
        return self.n == other.n


A = "\U0001f600"
B = "中"

# What to call it: what makes something of n elements, and what does something to it n times or so.
THINGS = {}


def thing(label, make):
    def add(do):
        THINGS[label] = (make, do)
        return do
    return add


def simple(label, make, do):
    THINGS[label] = (make, do)


for kind, unit in (("ascii", "a"), ("latin-1", "\xe9"), ("16 bits", B), ("pairs", A), ("mixed", "ab" + B + A)):
    m = (lambda unit: lambda n: unit * (n // len(unit)))(unit)
    simple("str %s: s[i]" % kind, m, lambda s, n: [s[i] for i in range(len(s))])
    simple("str %s: s[-i]" % kind, m, lambda s, n: [s[-i] for i in range(1, len(s))])
    simple("str %s: len(s)" % kind, m, lambda s, n: [len(s) for i in range(n)])
    simple("str %s: s[i:i + 2]" % kind, m, lambda s, n: [s[i:i + 2] for i in range(len(s))])
    simple("str %s: for c in s" % kind, m, lambda s, n: [c for c in s])
    simple("str %s: s[::-1]" % kind, m, lambda s, n: s[::-1])
    simple("str %s: s[::2]" % kind, m, lambda s, n: s[::2])
    simple("str %s: reversed(s)" % kind, m, lambda s, n: list(reversed(s)))
    simple("str %s: s.find(c, i)" % kind, m, lambda s, n: [s.find(s[0], i) for i in range(len(s))])
    simple("str %s: s.rfind(c, 0, i)" % kind, m, lambda s, n: [s.rfind(s[0], 0, i) for i in range(len(s))])
    simple("str %s: s.startswith(c, i)" % kind, m, lambda s, n: [s.startswith(s[0], i) for i in range(len(s))])
    simple("str %s: s.count(c, i, i + 2)" % kind, m, lambda s, n: [s.count(s[0], i, i + 2) for i in range(len(s))])
    simple("str %s: s.index(c, i)" % kind, m, lambda s, n: [s.index(s[0], i) for i in range(len(s) - 4)])
    simple("str %s: t += c" % kind, m, lambda s, n: _append_each(s))
    simple("str %s: t = c + t" % kind, m, lambda s, n: _prepend_each(s))
    simple("str %s: t += c, and len(t)" % kind, m, lambda s, n: _append_each_and_ask(s))
    for what, read in (("t[-1]", lambda t: t[-1]), ("t[0]", lambda t: t[0]), ("t.endswith(x)", lambda t: t.endswith("zz")), ("t[-2:]", lambda t: t[-2:]), ("t == x", lambda t: t == "zz"), ("x in t[-3:]", lambda t: "z" in t[-3:]), ("hash(t[-3:])", lambda t: hash(t[-3:])),
                       ("t.isdigit()", lambda t: t[-1:].isdigit()), ("bool(t)", lambda t: not t), ("f'{len(t)}'", lambda t: f"{len(t)}")):
        simple("str %s: t += c, and %s" % (kind, what), m, (lambda read: lambda s, n: _append_each_and_read(s, read))(read))
    simple("str %s: ''.join(list)" % kind, m, lambda s, n: "".join(list(s)))
    simple("str %s: s.split(c)" % kind, m, lambda s, n: s.split(s[0]))
    simple("str %s: s.replace(c, 'xy')" % kind, m, lambda s, n: s.replace(s[0], "xy"))
    simple("str %s: s.upper()" % kind, m, lambda s, n: s.upper())
    simple("str %s: s.title()" % kind, m, lambda s, n: s.title())
    simple("str %s: s.encode()" % kind, m, lambda s, n: s.encode())
    simple("str %s: s.encode().decode()" % kind, m, lambda s, n: s.encode().decode())
    simple("str %s: s.encode('utf-16')" % kind, m, lambda s, n: s.encode("utf-16"))
    simple("str %s: repr(s)" % kind, m, lambda s, n: repr(s))
    simple("str %s: hash of parts" % kind, m, lambda s, n: [hash(s[i:i + 3]) for i in range(len(s))])
    simple("str %s: s == t" % kind, m, lambda s, n: s == s[:-1] + "x")
    simple("str %s: s < t" % kind, m, lambda s, n: s < s[:-1] + "x")
    simple("str %s: sorted(s)" % kind, m, lambda s, n: sorted(s))
    simple("str %s: s.translate" % kind, m, lambda s, n: s.translate({97: "b"}))
    simple("str %s: s.isalpha()" % kind, m, lambda s, n: (s.isalpha(), s.isprintable(), s.isidentifier(), s.islower()))
    simple("str %s: s.strip(c)" % kind, m, lambda s, n: s.strip(s[0]))
    simple("str %s: s.center" % kind, m, lambda s, n: s.center(2 * len(s)))
    simple("str %s: format(s, '>n')" % kind, m, lambda s, n: format(s, ">" + str(2 * len(s))))
    simple("str %s: '%%s' %% s" % kind, m, lambda s, n: "%s|%s" % (s, s))
    simple("str %s: f'{s}'" % kind, m, lambda s, n: f"{s}|{s!r}")
    simple("str %s: c in s" % kind, m, lambda s, n: "zz" in s)
    simple("str %s: s.partition" % kind, m, lambda s, n: s.partition("zz"))
    simple("str %s: s.splitlines()" % kind, m, lambda s, n: (s[:4] + "\n") * (n // 5) and ((s[:4] + "\n") * (n // 5)).splitlines())
    simple("str %s: s.expandtabs()" % kind, m, lambda s, n: ((s[:3] + "\t") * (n // 4)).expandtabs())
    simple("str %s: dict of parts" % kind, m, lambda s, n: {s[i:i + 4] + str(i): i for i in range(len(s))})
    simple("str %s: iterator's hint" % kind, m, lambda s, n: _hints(s))


def _append_each(s):
    t = ""
    for c in s:
        t += c
    return len(t)


def _prepend_each(s):
    t = ""
    for c in s:
        t = c + t
    return len(t)


def _append_each_and_ask(s):
    t = ""
    total = 0
    for c in s:
        t += c
        total += len(t)
    return total


def _append_each_and_read(s, read):
    t = ""
    for c in s:
        t += c
        read(t)
    return len(t)


def _bytes_append_and_read(n, read):
    t = b""
    for i in range(n):
        t += b"x"
        read(t)
    return len(t)


def _hints(s):
    i = iter(s)
    return [i.__length_hint__() for c in i]


def drain(pop):
    def do(x, n):
        try:
            while True:
                pop(x)
        except (IndexError, KeyError):
            pass
    return do


L = lambda n: list(range(n))
simple("list: l[i]", L, lambda l, n: [l[i] for i in range(n)])
simple("list: l.append", lambda n: [], lambda l, n: [l.append(i) for i in range(n)])
simple("list: l.pop()", L, drain(lambda l: l.pop()))
simple("list: l += [x]", lambda n: [], lambda l, n: _iadd(l, n))
simple("list: l.extend(pair)", lambda n: [], lambda l, n: [l.extend((i, i)) for i in range(n)])
simple("list: l[i] = x", L, lambda l, n: [l.__setitem__(i, 0) for i in range(n)])
simple("list: l[-1:] = [x, y]", lambda n: [0], lambda l, n: [l.__setitem__(slice(-1, None), [i, i]) for i in range(n)])
simple("list: del l[-1]", L, lambda l, n: [l.__delitem__(-1) for i in range(n)])
simple("list: l.pop(0)", lambda n: list(range(min(n, 30000))), drain(lambda l: l.pop(0)))
simple("list: del l[:2]", lambda n: list(range(min(n, 30000))), lambda l, n: [l.__delitem__(slice(0, 2)) for i in range(len(l) // 2)])
simple("list: l.insert(0, x)", lambda n: [], lambda l, n: [l.insert(0, i) for i in range(min(n, 30000))])
simple("list: l[:0] = [x, y]", lambda n: [], lambda l, n: [l.__setitem__(slice(0, 0), [i, i]) for i in range(min(n, 15000))])
simple("list: del l[middle]", lambda n: list(range(min(n, 30000))), lambda l, n: [l.__delitem__(len(l) // 2) for i in range(len(l))])
simple("list: l.remove(x)", lambda n: list(range(min(n, 30000))), lambda l, n: [l.remove(i) for i in range(len(l))])
simple("list: of floats, l.pop(0)", lambda n: [i + 0.5 for i in range(min(n, 30000))], drain(lambda l: l.pop(0)))
simple("list: of strings, l.pop(0)", lambda n: [str(i) for i in range(min(n, 30000))], drain(lambda l: l.pop(0)))
simple("list: l[i:i + 2]", L, lambda l, n: [l[i:i + 2] for i in range(n)])
simple("list: l[:]", L, lambda l, n: l[:])
simple("list: l[::-1]", L, lambda l, n: l[::-1])
simple("list: l * 2", L, lambda l, n: l * 2)
simple("list: l + l", L, lambda l, n: l + l)
simple("list: l.reverse()", L, lambda l, n: l.reverse())
simple("list: l.sort()", lambda n: [(i * 7919) % n for i in range(n)], lambda l, n: l.sort())
simple("list: l.sort() sorted", L, lambda l, n: l.sort())
simple("list: l.sort(key)", lambda n: [(i * 7919) % n for i in range(n)], lambda l, n: l.sort(key=lambda x: -x))
simple("list: sorted(strings)", lambda n: [str((i * 7919) % n) for i in range(n)], lambda l, n: sorted(l))
simple("list: l.copy()", L, lambda l, n: l.copy())
simple("list: list(l)", L, lambda l, n: list(l))
simple("list: tuple(l)", L, lambda l, n: tuple(l))
simple("list: [*l, *l]", L, lambda l, n: [*l, *l])
simple("list: f(*l)", L, lambda l, n: (lambda *a: len(a))(*l))
simple("list: l == l2", L, lambda l, n: l == l[:])
simple("list: l < l2", L, lambda l, n: l < l[:])
simple("list: sum, min, max", L, lambda l, n: (sum(l), min(l), max(l), any(l), all(l)))
simple("list: len(l)", L, lambda l, n: [len(l) for i in range(n)])
simple("list: repr(l)", L, lambda l, n: repr(l))
simple("list: for, enumerate, zip", L, lambda l, n: [a + b + c for a, (b, c) in enumerate(zip(l, l))])
simple("list: reversed(l)", L, lambda l, n: list(reversed(l)))
simple("list: iterator's hint", L, lambda l, n: (lambda i: [i.__length_hint__() for x in i])(iter(l)))
simple("list: l.clear() and fill", L, lambda l, n: (l.clear(), l.extend(range(n))))
simple("list: of lists", lambda n: [[i] for i in range(n)], lambda l, n: [x[0] for x in l])
simple("list: a, *b = l", L, lambda l, n: (lambda a, *b: len(b))(*l))
simple("list: map, filter", L, lambda l, n: list(map(abs, filter(None, l))))
simple("list: hash(tuple(l))", L, lambda l, n: hash(tuple(l)))

simple("tuple: t[i]", lambda n: tuple(range(n)), lambda t, n: [t[i] for i in range(n)])
simple("tuple: t[i:i + 2]", lambda n: tuple(range(n)), lambda t, n: [t[i:i + 2] for i in range(n)])
simple("tuple: t + t, t * 2", lambda n: tuple(range(n)), lambda t, n: (t + t, t * 2, t[::-1], t[:]))
simple("tuple: for", lambda n: tuple(range(n)), lambda t, n: [x for x in t])

D = lambda n: {i: i for i in range(n)}
simple("dict: d[i]", D, lambda d, n: [d[i] for i in range(n)])
simple("dict: d[i] = x", lambda n: {}, lambda d, n: [d.__setitem__(i, i) for i in range(n)])
simple("dict: d[str]", lambda n: {str(i): i for i in range(n)}, lambda d, n: [d[str(i)] for i in range(n)])
simple("dict: d[tuple]", lambda n: {(i, i): i for i in range(n)}, lambda d, n: [d[(i, i)] for i in range(n)])
simple("dict: d[float]", lambda n: {i + 0.5: i for i in range(n)}, lambda d, n: [d[i + 0.5] for i in range(n)])
simple("dict: d[large int]", lambda n: {i << 70: i for i in range(n)}, lambda d, n: [d[i << 70] for i in range(n)])
simple("dict: d[multiples of 1024]", lambda n: {i * 1024: i for i in range(n)}, lambda d, n: [d[i * 1024] for i in range(n)])
simple("dict: d[negative]", lambda n: {-i: i for i in range(n)}, lambda d, n: [d[-i] for i in range(n)])
simple("dict: d[Key]", lambda n: {Key(i): i for i in range(n)}, lambda d, n: [d[Key(i)] for i in range(n)])
simple("dict: d[object]", lambda n: [object() for i in range(n)], lambda l, n: (lambda d: [d[x] for x in l])({x: 1 for x in l}))
simple("dict: i in d", D, lambda d, n: [i in d for i in range(2 * n)])
simple("dict: d.get", D, lambda d, n: [d.get(i) for i in range(2 * n)])
simple("dict: del d[i]", D, lambda d, n: [d.__delitem__(i) for i in range(n)])
simple("dict: del d[i] from the end", D, lambda d, n: [d.__delitem__(i) for i in range(n - 1, -1, -1)])
simple("dict: d.pop(i)", D, lambda d, n: [d.pop(i) for i in range(n)])
simple("dict: d.popitem()", D, drain(lambda d: d.popitem()))
simple("dict: next(iter(d)) and delete", D, lambda d, n: [d.__delitem__(next(iter(d))) for i in range(n)])
simple("dict: next(reversed(d)) and delete", D, lambda d, n: [d.__delitem__(next(reversed(d))) for i in range(n)])
simple("dict: put and delete in turn", lambda n: {}, lambda d, n: [(d.__setitem__(i, i), d.__delitem__(i)) for i in range(n)])
simple("dict: a queue", lambda n: {i: i for i in range(8)}, lambda d, n: [(d.__setitem__(i + 8, i), d.__delitem__(i)) for i in range(n)])
simple("dict: a queue, by iter", lambda n: {i: i for i in range(8)}, lambda d, n: [(d.__setitem__(i + 8, i), d.__delitem__(next(iter(d)))) for i in range(n)])
simple("dict: len(d)", D, lambda d, n: [len(d) for i in range(n)])
simple("dict: for k in d", D, lambda d, n: [k for k in d])
simple("dict: d.items()", D, lambda d, n: [k + v for k, v in d.items()])
simple("dict: list(d.values())", D, lambda d, n: list(d.values()))
simple("dict: d.copy()", D, lambda d, n: d.copy())
simple("dict: dict(d)", D, lambda d, n: dict(d))
simple("dict: {**d}", D, lambda d, n: {**d, **d})
simple("dict: d | d", D, lambda d, n: d | d)
simple("dict: d.update(d2)", D, lambda d, n: d.update({i + n: i for i in range(n)}))
simple("dict: d == d2", D, lambda d, n: d == dict(d))
simple("dict: d.setdefault", lambda n: {}, lambda d, n: [d.setdefault(i % (n // 2 + 1), []).append(i) for i in range(n)])
simple("dict: f(**d)", lambda n: {"k%d" % i: i for i in range(n)}, lambda d, n: (lambda **k: len(k))(**d))
simple("dict: d.keys() & d.keys()", D, lambda d, n: d.keys() & d.keys())
simple("dict: i in d.keys()", D, lambda d, n: (lambda k: [i in k for i in range(n)])(d.keys()))
simple("dict: (i, i) in d.items()", D, lambda d, n: (lambda k: [(i, i) in k for i in range(n)])(d.items()))
simple("dict: repr(d)", D, lambda d, n: repr(d))
simple("dict: dict.fromkeys", L, lambda l, n: dict.fromkeys(l))
simple("dict: reversed(d)", D, lambda d, n: list(reversed(d)))
simple("dict: d.clear() and fill", D, lambda d, n: (d.clear(), d.update((i, i) for i in range(n))))
simple("dict: after most are deleted, for", lambda n: _mostly_deleted(n), lambda d, n: [[k for k in d] for i in range(n // 64 + 1)])

SET = lambda n: set(range(n))
simple("set: i in s", SET, lambda s, n: [i in s for i in range(2 * n)])
simple("set: s.add", lambda n: set(), lambda s, n: [s.add(i) for i in range(n)])
simple("set: s.discard", SET, lambda s, n: [s.discard(i) for i in range(n)])
simple("set: s.remove from the end", SET, lambda s, n: [s.remove(i) for i in range(n - 1, -1, -1)])
simple("set: s.pop()", SET, drain(lambda s: s.pop()))
simple("set: s.pop() and add", SET, lambda s, n: [s.add(s.pop() + n) for i in range(n)])
simple("set: next(iter(s)) and discard", SET, lambda s, n: [s.discard(next(iter(s))) for i in range(n)])
simple("set: add and discard in turn", lambda n: set(), lambda s, n: [(s.add(i), s.discard(i)) for i in range(n)])
simple("set: s | s, &, -, ^", SET, lambda s, n: (s | s, s & s, s - s, s ^ s))
simple("set: with another", SET, lambda s, n: (lambda o: (s | o, s & o, s - o, s ^ o, s <= o, s.isdisjoint(o)))(set(range(n // 2, n + n // 2))))
simple("set: a small one & a large one", SET, lambda s, n: [{i} & s for i in range(n)])
simple("set: a large one & a small one", SET, lambda s, n: [s & {i} for i in range(n)])
simple("set: s.isdisjoint(a small one)", SET, lambda s, n: [s.isdisjoint({i}) for i in range(n)])
simple("set: s.intersection([x])", SET, lambda s, n: [s.intersection([i]) for i in range(n)])
simple("set: s.issuperset([x]), s >= {x}", SET, lambda s, n: [(s.issuperset([i]), s >= {i}, {i} <= s) for i in range(n)])


def in_place(operation):
    def run(s, n):
        for i in range(n):
            operation(s, i, n)
    return run


def ior(s, i, n): s |= {n + i}
def isub(s, i, n): s -= {i}
def ixor(s, i, n): s ^= {i, n + i}
def iand(s, i, n): s &= s
simple("set: s |= {x}", SET, in_place(ior))
simple("set: s -= {x}", SET, in_place(isub))
simple("set: s ^= {x, y}", SET, in_place(ixor))
simple("set: s.update([x])", SET, in_place(lambda s, i, n: s.update([n + i])))
simple("set: s.update((x,), {y})", SET, in_place(lambda s, i, n: s.update((n + i,), {-i})))
simple("set: s.difference_update([x])", SET, in_place(lambda s, i, n: s.difference_update([i])))
simple("set: s.symmetric_difference_update([x])", SET, in_place(lambda s, i, n: s.symmetric_difference_update([i])))
simple("set: a small one, s.intersection_update", SET, lambda s, n: [{i, -1}.intersection_update(s) for i in range(n)])
simple("set: a small one - s, ^ is not", SET, lambda s, n: [{i, -1} - s for i in range(n)])
simple("set: set().union(*many)", lambda n: [[i] for i in range(n)], lambda l, n: set().union(*l))
simple("set: s.difference(*many)", lambda n: [[i] for i in range(n)], lambda l, n: set(range(n)).difference(*l))
simple("set: s.intersection(*many)", lambda n: [[1, 2]] * n, lambda l, n: {1, 2, 3}.intersection(*l))
simple("set: s.update(*many)", lambda n: [[i] for i in range(n)], lambda l, n: set().update(*l))
simple("set: after most are taken out, for", SET, lambda s, n: (s.difference_update(range(n - 4)), [[x for x in s] for i in range(n)]))
simple("dict keys: d.keys() & {x}, | is not", D, lambda d, n: [d.keys() & {i} for i in range(n)])
simple("dict keys: d.keys() >= {x}, isdisjoint", D, lambda d, n: [(d.keys() >= {i}, d.keys().isdisjoint({i})) for i in range(n)])
simple("set: s.update", SET, lambda s, n: s.update(range(n, 2 * n)))
simple("set: s == s2", SET, lambda s, n: s == set(s))
simple("set: for", SET, lambda s, n: [x for x in s])
simple("set: len(s)", SET, lambda s, n: [len(s) for i in range(n)])
simple("set: s.copy(), frozenset(s)", SET, lambda s, n: (s.copy(), frozenset(s), hash(frozenset(s))))
simple("set: of strings", lambda n: {str(i) for i in range(n)}, lambda s, n: [str(i) in s for i in range(n)])
simple("set: sorted(s)", SET, lambda s, n: sorted(s))
simple("set: set(list)", L, lambda l, n: set(l))

BY = lambda n: bytes(n)
simple("bytes: b[i]", BY, lambda b, n: [b[i] for i in range(n)])
simple("bytes: b[i:i + 2]", BY, lambda b, n: [b[i:i + 2] for i in range(n)])
simple("bytes: for", BY, lambda b, n: [x for x in b])
simple("bytes: len(b)", BY, lambda b, n: [len(b) for i in range(n)])
simple("bytes: b.find(x, i)", BY, lambda b, n: [b.find(b"\0", i) for i in range(n)])
simple("bytes: b + b, b * 2, b[::-1]", BY, lambda b, n: (b + b, b * 2, b[::-1], b[::2]))
simple("bytes: t += b", lambda n: b"", lambda b, n: _bytes_append(n))
simple("bytes: t += b, all the way", lambda n: None, lambda x, n: _bytes_append_and_read(n, lambda t: None))
simple("bytes: t += b, and t[-1]", lambda n: None, lambda x, n: _bytes_append_and_read(n, lambda t: t[-1]))
simple("bytes: t += b, and len(t)", lambda n: None, lambda x, n: _bytes_append_and_read(n, len))
simple("bytes: t += chunk", lambda n: b"x" * 1024, lambda c, n: _bytes_chunks(c, n // 64))
simple("bytes: t = t[k:] from the front", lambda n: bytes(n), lambda b, n: _bytes_consume(b))
simple("bytearray: a += chunk, del a[:k]", lambda n: b"x" * 64, lambda c, n: _buffer(c, n // 16))
simple("bytes: b''.join", lambda n: [b"ab"] * n, lambda l, n: b"".join(l))
simple("bytes: b.split", lambda n: b"a," * n, lambda b, n: b.split(b","))
simple("bytes: b.replace", lambda n: b"a," * n, lambda b, n: b.replace(b",", b";;"))
simple("bytes: b.hex(), fromhex", BY, lambda b, n: bytes.fromhex(b.hex()))
simple("bytes: b.decode()", BY, lambda b, n: b.decode())
simple("bytes: hash, ==, <", BY, lambda b, n: (hash(b), b == b[:-1] + b"x", b < b[:-1] + b"x"))
simple("bytes: bytes(list)", lambda n: [i & 255 for i in range(n)], lambda l, n: bytes(l))
simple("bytes: list(b)", BY, lambda b, n: list(b))
simple("bytes: b.upper(), translate", lambda n: b"a" * n, lambda b, n: (b.upper(), b.translate(bytes(range(256)))))
simple("bytes: repr(b)", BY, lambda b, n: repr(b))
simple("bytes: x in b", BY, lambda b, n: (1 in b, b"ab" in b))
simple("bytes: int.from_bytes small", BY, lambda b, n: [int.from_bytes(b[i:i + 4]) for i in range(n)])

BA = lambda n: bytearray(n)
simple("bytearray: a[i]", BA, lambda a, n: [a[i] for i in range(n)])
simple("bytearray: a[i] = x", BA, lambda a, n: [a.__setitem__(i, 1) for i in range(n)])
simple("bytearray: a.append", lambda n: bytearray(), lambda a, n: [a.append(1) for i in range(n)])
simple("bytearray: a += b'x'", lambda n: bytearray(), lambda a, n: _iadd_bytes(a, n))
simple("bytearray: a.extend(b'xy')", lambda n: bytearray(), lambda a, n: [a.extend(b"xy") for i in range(n)])
simple("bytearray: a.pop()", BA, drain(lambda a: a.pop()))
simple("bytearray: del a[-1]", BA, lambda a, n: [a.__delitem__(-1) for i in range(n)])
simple("bytearray: del a[:1]", BA, lambda a, n: [a.__delitem__(slice(0, 1)) for i in range(n)])
simple("bytearray: a[-1:] = b'xy'", lambda n: bytearray(1), lambda a, n: [a.__setitem__(slice(-1, None), b"xy") for i in range(n)])
simple("bytearray: memoryview m[i]", BA, lambda a, n: (lambda m: [m[i] for i in range(n)])(memoryview(a)))
simple("bytearray: memoryview m[i:i + 2]", BA, lambda a, n: (lambda m: [m[i:i + 2] for i in range(n)])(memoryview(a)))
simple("bytearray: memoryview tolist, tobytes", BA, lambda a, n: (memoryview(a).tolist(), memoryview(a).tobytes(), bytes(memoryview(a))))

R = lambda n: range(n)
simple("range: r[i]", R, lambda r, n: [r[i] for i in range(n)])
simple("range: i in r", R, lambda r, n: [i in r for i in range(n)])
simple("range: r.index(i)", R, lambda r, n: [r.index(i) for i in range(n)])
simple("range: r.count(i)", R, lambda r, n: [r.count(i) for i in range(n)])
simple("range: len(r), r[i:], hash", R, lambda r, n: [(len(r), r[i:], hash(r)) for i in range(n)])
simple("range: r == r2", R, lambda r, n: [r == range(n) for i in range(n)])
simple("range: large", lambda n: range(1 << 70, (1 << 70) + n), lambda r, n: [x for x in r])
simple("range: reversed", R, lambda r, n: list(reversed(r)))

simple("int: str(large)", lambda n: 7 ** min(n, 5000), lambda x, n: str(x))
simple("int: int(digits)", lambda n: "7" * min(n, 4000), lambda x, n: int(x))
simple("int: hex(large)", lambda n: 1 << (4 * n), lambda x, n: hex(x))
simple("int: int(hex, 16)", lambda n: "f" * n, lambda x, n: int(x, 16))
simple("int: large + large", lambda n: 1 << (8 * n), lambda x, n: [x + x for i in range(64)])
simple("int: large >> i", lambda n: 1 << (8 * n), lambda x, n: [x >> 3 for i in range(64)])
simple("int: large & mask", lambda n: 1 << (8 * n), lambda x, n: [x & 255 for i in range(n)])
simple("int: bit_length, bit_count", lambda n: (1 << (8 * n)) - 1, lambda x, n: [x.bit_length() for i in range(n)] and x.bit_count())
simple("int: hash(large)", lambda n: (1 << (8 * n)) - 1, lambda x, n: [hash(x) for i in range(64)])
simple("int: large == large", lambda n: (1 << (8 * n)) - 1, lambda x, n: [x == x + 0 for i in range(64)])
simple("int: to_bytes, from_bytes", lambda n: (1 << (8 * n)) - 1, lambda x, n: int.from_bytes(x.to_bytes(n + 1)))
simple("int: x += 1 small", lambda n: 0, lambda x, n: sum(1 for i in range(n)))
simple("int: sum of large", lambda n: [1 << 70] * n, lambda l, n: sum(l))
simple("int: product", lambda n: n, lambda x, n: _product(min(n, 3000)))
simple("float: sum, repr", lambda n: [i + 0.5 for i in range(n)], lambda l, n: (sum(l), [repr(x) for x in l]))

simple("class: instances", lambda n: type("C", (), {"__init__": lambda s, i: setattr(s, "i", i)}), lambda C, n: [C(i).i for i in range(n)])
simple("class: attributes of one", lambda n: type("C", (), {})(), lambda o, n: [setattr(o, "a%d" % i, i) for i in range(n)] and [getattr(o, "a%d" % i) for i in range(n)])
simple("class: delete attributes of one", lambda n: type("C", (), {})(), lambda o, n: [setattr(o, "a%d" % i, i) for i in range(n)] and [delattr(o, "a%d" % i) for i in range(n)])
simple("class: vars(o)", lambda n: _with_attributes(n), lambda o, n: (len(vars(o)), list(vars(o).items()), dir(o)))
simple("class: o.__dict__[k]", lambda n: _with_attributes(n), lambda o, n: [o.__dict__["a%d" % i] for i in range(n)])
simple("class: len(o.__dict__)", lambda n: _with_attributes(n), lambda o, n: [len(o.__dict__) for i in range(n)])
simple("class: classes", lambda n: None, lambda x, n: [type("C%d" % i, (), {"a": i}) for i in range(n // 8)])
simple("class: subclasses of one", lambda n: type("B", (), {}), lambda B, n: [type("C", (B,), {}) for i in range(n // 8)] and len(B.__subclasses__()))
simple("class: a long line of them", lambda n: None, lambda x, n: _chain(min(n // 8, 400)))
simple("class: attributes of a class", lambda n: type("C", (), {}), lambda C, n: [setattr(C, "a%d" % i, i) for i in range(n // 4)] and [getattr(C, "a%d" % i) for i in range(n // 4)])
simple("class: isinstance", lambda n: [1, "a", 2.0, None] * (n // 4), lambda l, n: [isinstance(x, (int, str)) for x in l])
simple("class: slots", lambda n: type("C", (), {"__slots__": ("a", "b")}), lambda C, n: [setattr(C(), "a", i) for i in range(n)])

simple("function: calls", lambda n: (lambda a, b=1, *c, d=2, **e: a), lambda f, n: [f(i, 2, 3, d=4, x=5) for i in range(n)])
simple("function: closures", lambda n: None, lambda x, n: [(lambda i: lambda: i)(i)() for i in range(n)])
simple("function: many arguments", L, lambda l, n: (lambda *a, **k: len(a))(*l, **{"k%d" % i: i for i in range(min(n, 5000))}))
simple("function: generators", lambda n: None, lambda x, n: sum(i for i in range(n)))
simple("function: many generators", lambda n: None, lambda x, n: [next(i for i in (1,)) for j in range(n)])
simple("function: yield from, nested", lambda n: None, lambda x, n: sum(_nested(40, n // 40)))
simple("function: recursion", lambda n: None, lambda x, n: [_depth(200) for i in range(n // 200)])
simple("function: try and raise", lambda n: None, lambda x, n: [_raise_and_catch(i) for i in range(n)])
simple("function: raise from deep", lambda n: None, lambda x, n: [_raise_deep(50) for i in range(n // 50)])
simple("function: with", lambda n: type("M", (), {"__enter__": lambda s: s, "__exit__": lambda s, *a: None})(), lambda m, n: [_with(m) for i in range(n)])
simple("function: globals", lambda n: None, lambda x, n: [len for i in range(n)])
simple("function: exec small", lambda n: None, lambda x, n: [eval("1 + 1") for i in range(n // 16)])
simple("function: compile long", lambda n: "\n".join("x%d = %d" % (i, i) for i in range(n // 4)), lambda s, n: compile(s, "f", "exec"))
simple("function: compile one long expression", lambda n: " + ".join(["1"] * min(n // 4, 3000)), lambda s, n: compile(s, "f", "eval"))
simple("function: compile a long list", lambda n: "[" + ", ".join(str(i) for i in range(n // 2)) + "]", lambda s, n: eval(s))
simple("function: compile a long string", lambda n: repr("a" * n), lambda s, n: eval(s))
simple("function: compile long, 16 bits", lambda n: "\n".join("x%d = '%s'" % (i, B) for i in range(n // 4)), lambda s, n: compile(s, "f", "exec"))
simple("function: compile long, pairs", lambda n: "\n".join("x%d = '%s'" % (i, A) for i in range(n // 4)), lambda s, n: compile(s, "f", "exec"))
simple("function: a syntax error at the end", lambda n: "\n".join("x%d = %d" % (i, i) for i in range(n // 4)) + "\n(", lambda s, n: _syntax_error(s))
simple("function: a traceback from a long file", lambda n: compile("\n" * n + "1 / 0", "f", "exec"), lambda c, n: _raise_and_catch_code(c))


def _iadd(l, n):
    for i in range(n):
        l += [i]


def _iadd_bytes(a, n):
    for i in range(n):
        a += b"x"


def _bytes_append(n):
    t = b""
    for i in range(min(n, 20000)):
        t += b"x"
    return t


def _bytes_chunks(c, n):
    t = b""
    for i in range(n):
        t += c
    return len(t)


def _bytes_consume(b):
    while b:
        b = b[64:]


def _buffer(c, n):
    a = bytearray(c * n)
    while a:
        del a[:16]


def _mostly_deleted(n):
    d = {i: i for i in range(n)}
    for i in range(n - 4):
        del d[i]
    return d


def _product(n):
    x = 1
    for i in range(1, n):
        x *= i
    return x


def _with_attributes(n):
    o = type("C", (), {})()
    for i in range(n):
        setattr(o, "a%d" % i, i)
    return o


def _chain(n):
    C = object
    for i in range(n):
        C = type("C", (C,), {"a%d" % i: i})
    return C().a0


def _nested(depth, n):
    if depth:
        yield from _nested(depth - 1, n)
    else:
        yield from range(n)


def _depth(n):
    return n and _depth(n - 1)


def _raise_and_catch(i):
    try:
        raise ValueError(i)
    except ValueError as e:
        return e


def _raise_deep(n):
    try:
        _raise_at(n)
    except ValueError as e:
        return e


def _raise_at(n):
    if n:
        _raise_at(n - 1)
    raise ValueError


def _with(m):
    with m:
        pass


def _syntax_error(s):
    try:
        compile(s, "f", "exec")
    except SyntaxError as e:
        return e


def _raise_and_catch_code(c):
    try:
        exec(c)
    except ZeroDivisionError as e:
        return e.__traceback__.tb_next.tb_lineno


def timed(make, do, n):
    best = None
    for attempt in range(3):
        state = make(n)
        start = time.perf_counter()
        do(state, n)
        taken = time.perf_counter() - start
        best = taken if best is None else min(best, taken)
        # What takes long enough is not worth doing again.
        if taken > 0.3:
            break
    return best


def measure(make, do):
    # Long enough to time, and not so long that the square of it cannot be waited for.
    n = 2000
    small = timed(make, do, n)
    while small < 0.004 and n < 500000:
        n *= 4
        small = timed(make, do, n)
    if small > 2:
        return n, None, small / n * 1e9
    large = timed(make, do, 4 * n)
    return 4 * n, log(max(large, 1e-9) / max(small, 1e-9)) / log(4), large / (4 * n) * 1e9


only = sys.argv[-1] if len(sys.argv) > 1 and sys.argv[-1] != "--" and not sys.argv[-1].endswith(".py") else None
for label, (make, do) in THINGS.items():
    if only and only not in label:
        continue
    try:
        n, power, each = measure(make, do)
        print("%s | %d | %s | %.0f" % (label, n, "too slow" if power is None else "%.2f" % power, each), flush=True)
    except BaseException as e:
        print("%s | 0 | %s | 0" % (label, type(e).__name__ + ": " + str(e)[:60]), flush=True)
