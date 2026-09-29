# What is done with a bytearray or a deque, given an index, a value, a fill or a separator that changes it while it is being made a number or bytes of: makes it shorter, empties it, or makes it much longer, which moves it.
# What CPython does not allow to be changed meanwhile, and this does, is in interop/bytearray-while-it-is-changed.py.
import collections


def attempt(f, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)


def short(v):
    r = ascii(v)
    return r if len(r) < 120 else r[:120] + "... %d" % len(r)


ACTS = {
    "empties it": lambda b: b.clear(),
    "leaves one": lambda b: b.__delitem__(slice(1, None)),
    "takes one off": lambda b: b and b.pop(),
    "makes it much longer": lambda b: len(b) < 100000 and b.extend(bytes(100000) if isinstance(b, bytearray) else [0] * 100000),
    "empties it and fills it again": lambda b: (b.clear(), b.extend(b"zzzz")),
}
act = None


class I:
    "A number, that changes something when it is asked what number it is"
    def __init__(self, victim, v):
        self.victim, self.v = victim, v

    def __index__(self):
        act(self.victim)
        return self.v


class B:
    "Something with bytes in it, that changes something when it is asked for them"
    def __init__(self, victim, v):
        self.victim, self.v = victim, v

    def __buffer__(self, flags):
        act(self.victim)
        return memoryview(self.v)

    def __iter__(self):
        act(self.victim)
        return iter(self.v)


class It:
    "What gives numbers one at a time, and changes something at each"
    def __init__(self, victim, values):
        self.victim, self.values = victim, list(values)

    def __iter__(self):
        return self

    def __next__(self):
        if not self.values:
            raise StopIteration
        act(self.victim)
        return self.values.pop(0)


BYTEARRAY = {
    "b[i]": lambda b: b[I(b, 5)],
    "b[-i]": lambda b: b[I(b, -2)],
    "b[i] = v": lambda b: b.__setitem__(I(b, 5), 65),
    "b[5] = i": lambda b: b.__setitem__(5, I(b, 65)),
    "b[-1] = i": lambda b: b.__setitem__(-1, I(b, 65)),
    "b[i] = i": lambda b: b.__setitem__(I(b, 5), I(b, 65)),
    "del b[i]": lambda b: b.__delitem__(I(b, 5)),
    "b[i:j]": lambda b: b[I(b, 2):I(b, 6)],
    "b[::i]": lambda b: b[::I(b, 2)],
    "b[i:j] = bytes": lambda b: b.__setitem__(slice(I(b, 2), I(b, 6)), b"XY"),
    "b[2:6] = what has bytes": lambda b: b.__setitem__(slice(2, 6), B(b, b"XY")),
    "b[2:6] = an iterator": lambda b: b.__setitem__(slice(2, 6), It(b, b"XY")),
    "b[::2] = what has bytes": lambda b: b.__setitem__(slice(None, None, 2), B(b, b"WXYZ")),
    "b[::2] = an iterator": lambda b: b.__setitem__(slice(None, None, 2), It(b, b"WXYZ")),
    "b[::i] = bytes": lambda b: b.__setitem__(slice(None, None, I(b, 2)), b"WXYZ"),
    "b[:] = itself by way of an iterator": lambda b: b.__setitem__(slice(None), It(b, b"WXYZ")),
    "del b[i:j]": lambda b: b.__delitem__(slice(I(b, 2), I(b, 6))),
    "del b[::i]": lambda b: b.__delitem__(slice(None, None, I(b, 2))),
    "append": lambda b: b.append(I(b, 65)),
    "insert at i": lambda b: b.insert(I(b, 5), 65),
    "insert i": lambda b: b.insert(5, I(b, 65)),
    "insert i at i": lambda b: b.insert(I(b, 5), I(b, 65)),
    "pop": lambda b: b.pop(I(b, 5)),
    "pop from the end": lambda b: b.pop(I(b, -1)),
    "remove": lambda b: b.remove(I(b, 101)),
    "extend by an iterator": lambda b: b.extend(It(b, b"XY")),
    "extend by numbers that change it": lambda b: b.extend([I(b, 65), I(b, 66)]),
    "+=": lambda b: b.__iadd__(B(b, b"XY")),
    "*": lambda b: short(b * I(b, 2)),
    "*=": lambda b: short(b.__imul__(I(b, 2))),
    "count from i to j": lambda b: b.count(b"e", I(b, 1), I(b, 7)),
    "find from i": lambda b: b.find(b"g", I(b, 1)),
    "rfind to j": lambda b: b.rfind(b"g", 0, I(b, 8)),
    "startswith from i": lambda b: b.startswith(b"cd", I(b, 2)),
    "replace": lambda b: b.replace(B(b, b"c"), B(b, b"XYZ")),
    "replace so many": lambda b: b.replace(b"c", b"XYZ", I(b, 1)),
    "split so many": lambda b: b.split(b"d", I(b, 1)),
    "partition": lambda b: b.partition(B(b, b"d")),
    "rpartition": lambda b: b.rpartition(B(b, b"d")),
    "strip": lambda b: b.strip(B(b, b"ah")),
    "lstrip": lambda b: b.lstrip(B(b, b"ah")),
    "removeprefix": lambda b: b.removeprefix(B(b, b"ab")),
    "removesuffix": lambda b: b.removesuffix(B(b, b"gh")),
    "center": lambda b: b.center(I(b, 12), b"*"),
    "ljust": lambda b: b.ljust(I(b, 12)),
    "rjust": lambda b: b.rjust(I(b, 12), b"-"),
    "zfill": lambda b: b.zfill(I(b, 12)),
    "expandtabs": lambda b: b.expandtabs(I(b, 4)),
    "translate": lambda b: b.translate(B(b, bytes(range(255, -1, -1)))),
    "translate, deleting": lambda b: b.translate(None, B(b, b"ce")),
    "hex": lambda b: short(b.hex(":", I(b, 2))),
    "% a number": lambda b: bytearray(b"%d|%c|") % (5, I(b, 65)) + b,
    "resize": lambda b: b.resize(I(b, 3)),
    "bytearray(b, ...)": lambda b: short(bytearray(It(b, b))),
    "bytes of an iterator over it": lambda b: short(bytes(It(b, b))),
    "int.from_bytes": lambda b: int.from_bytes(It(b, b"ab")),
}
DEQUE = {
    "d[i]": lambda d: d[I(d, 5)],
    "d[i] = v": lambda d: d.__setitem__(I(d, 5), "set"),
    "del d[i]": lambda d: d.__delitem__(I(d, 5)),
    "insert": lambda d: d.insert(I(d, 5), "in"),
    "rotate": lambda d: d.rotate(I(d, 3)),
    "*": lambda d: len(d * I(d, 2)),
    "*=": lambda d: len(d.__imul__(I(d, 2))),
    "index from i": lambda d: d.index(103, I(d, 1)),
    "index from i to j": lambda d: d.index(103, I(d, 1), I(d, 7)),
    "extend by an iterator": lambda d: d.extend(It(d, b"XY")),
    "extendleft by an iterator": lambda d: d.extendleft(It(d, b"XY")),
    "+=": lambda d: d.__iadd__(It(d, b"XY")),
}

import codecs
current = []
codecs.register_error("changes", lambda e: (act(current[0]), ("?", e.end))[1])

for act_name, act in ACTS.items():
    print("----", act_name)
    for name, operation in BYTEARRAY.items():
        b = bytearray(b"abcdefgh")
        current[:] = [b]
        result = attempt(operation, b)
        print(name, "=>", short(result), "|", short(b))
    for name, operation in DEQUE.items():
        d = collections.deque(b"abcdefgh")
        result = attempt(operation, d)
        print(name, "=>", short(result), "|", short(d))
