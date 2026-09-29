# CPython has whatever struct is packing into, or unpacking from a piece at a time, stay as it is until that is done, and raises BufferError at whoever would resize it. Here nothing is kept as it is. What is to be written is
# worked out first and written afterwards, if there is still room where it was to go. What comes of it matters less than that nothing is read or written that is not there.
import struct


def attempt(f, *a):
    try:
        return f(*a)
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)


class Acts:
    def __init__(self, act, value=1): self.act, self.value = act, value
    def __index__(self):
        self.act()
        return self.value
    def __float__(self):
        self.act()
        return float(self.value)
    def __bool__(self):
        self.act()
        return True


for size in (8, 64, 4096, 1 << 20):
    for name, act in (("cleared", lambda b: b.clear()), ("cut to one", lambda b: b.__delitem__(slice(1, None))), ("cut to just too few", lambda b: b.__delitem__(slice(size - 1, None))), ("cut to just enough", lambda b: b.__delitem__(slice(size - 0, None))), ("added to", lambda b: b.extend(bytes(1 << 16))), ("cleared and filled again", lambda b: (b.clear(), b.extend(b"#" * size)))):
        b = bytearray(b"." * size)
        r = attempt(struct.pack_into, "<HH", b, size - 4, Acts(lambda: act(b), 0x4142), 0x4344)
        print("pack_into", size, name, "=>", r, len(b), bytes(b[-6:]))
        if size > 8:
            b = bytearray(b"." * size)
            r = attempt(struct.pack_into, "<d?", b, -9, Acts(lambda: act(b)), True)
            print("pack_into from the end", size, name, "=>", r, len(b))

for size in (8, 4096):
    for name, act in (("cleared", lambda b: b.clear()), ("cut short", lambda b: b.__delitem__(slice(3, None))), ("cut to a whole number", lambda b: b.__delitem__(slice(4, None))), ("added to", lambda b: b.extend(b"zz" * 100000)), ("changed", lambda b: b.__setitem__(slice(2, 4), b"XY"))):
        b = bytearray(b"ab" * (size // 2))
        it = struct.iter_unpack("<2s", b)
        first = next(it)
        act(b)
        rest = []
        while True:
            r = attempt(next, it)
            rest.append(r)
            if isinstance(r, str):
                break
        print("iter_unpack", size, name, "=>", first, len(rest), rest[:2], rest[-1], it.__length_hint__(), attempt(next, it))

# What raises after having done its worst
b = bytearray(b"........")
def worst():
    b.clear()
    raise ValueError("and then this")
print(attempt(struct.pack_into, "<HH", b, 2, 0x4142, Acts(worst)), b)
b = bytearray(b"........")
print(attempt(struct.pack_into, "<HH", b, 2, 0x4142, "not a number"), b)

# A memoryview of it that has been released, or is of something that has gone
b = bytearray(b"........")
m = memoryview(b)
print(attempt(struct.pack_into, "<HH", m, 2, Acts(m.release, 0x4142), 0x4344), b)
m = memoryview(bytearray(b"abcdefgh"))
it = struct.iter_unpack("<H", m)
print(next(it), m.release(), attempt(next, it), attempt(next, it))
