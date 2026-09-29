# What is done with a bytearray by something that has hold of its bytes, when what it is given changes the bytearray. CPython does not allow that: while anything has hold of the bytes of a bytearray, to change how long it is
# raises BufferError. Here nothing has hold of them, since nothing keeps a pointer to them across anything of the program's, and it is what is in the bytearray by then that is gone by. See "Where the bytes are is not kept" in the README.


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
    "extend by what has bytes": lambda b: b.extend(B(b, b"XY")),
    "+": lambda b: b + B(b, b"XY"),
    "in": lambda b: I(b, 101) in b,
    "in, of what has bytes": lambda b: B(b, b"ef") in b,
    "count": lambda b: b.count(I(b, 101)),
    "count of what has bytes": lambda b: b.count(B(b, b"e")),
    "find": lambda b: b.find(I(b, 101)),
    "find what has bytes": lambda b: b.find(B(b, b"ef")),
    "index": lambda b: b.index(B(b, b"ef")),
    "startswith": lambda b: b.startswith(B(b, b"ab")),
    "endswith": lambda b: b.endswith(B(b, b"gh")),
    "endswith a tuple": lambda b: b.endswith((B(b, b"xx"), B(b, b"gh"))),
    "split": lambda b: b.split(B(b, b"d")),
    "rsplit": lambda b: b.rsplit(B(b, b"d")),
    "join": lambda b: short(b.join([B(b, b"1"), B(b, b"2"), B(b, b"3")])),
    "join of an iterator": lambda b: short(b.join(It(b, [b"1", b"2", b"3"]))),
    "join of itself": lambda b: short(b.join([b, B(b, b"2"), b])),
    "==": lambda b: b == B(b, b"abcdefgh"),
    "<": lambda b: b < B(b, b"abcdefgz"),
    "itself % what has bytes": lambda b: (b.__setitem__(slice(None), b"ab%scd%s"), short(b % (B(b, b"X"), B(b, b"Y"))))[1],
    "a view, set to what changes it": lambda b: memoryview(b).__setitem__(5, I(b, 65)),
    "a view, at what changes it": lambda b: memoryview(b)[I(b, 5)],
    "a view, sliced by what changes it": lambda b: bytes(memoryview(b)[I(b, 2):I(b, 6)]),
    "a view, set to what has bytes": lambda b: memoryview(b).__setitem__(slice(2, 4), B(b, b"XY")),
    "decode, with a handler that changes it": lambda b: (b.__setitem__(slice(None), b"ab\xffcd\xfeef"), b.decode("ascii", "changes"))[1],
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
