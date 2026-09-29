# CPython does not let an array be made longer or shorter while there is a memoryview of it: "BufferError: cannot resize an array that is exporting buffers". It can, because a view is released the moment that nothing refers to
# it. Here that would be at the next collection, and programs that are right would fail. A view has no pointer in it, only where it is looking, and looks each time. So an array can be resized under one, and the worst that comes of
# it is an error.
import array
import io
import struct
import js

A = array.array


def attempt(f, *a):
    try:
        return f(*a)
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)


CHANGES = (("cleared", lambda x: x.clear()), ("cut to one", lambda x: x.__delitem__(slice(1, None))), ("popped", lambda x: x.pop()), ("added to", lambda x: x.extend([5] * 100000)), ("cleared and filled again", lambda x: (x.clear(), x.extend([7] * 8))), ("doubled", lambda x: x.__imul__(2)), ("reversed", lambda x: x.reverse()))
for c in "bid":
    for size in (8, 5000):
        for name, change in CHANGES:
            x = A(c, range(8)) * (size // 8)
            m = memoryview(x)
            part = m[2:6]
            last = m[-1:]
            change(x)
            print(c, size, name, "=>", len(x), len(m), m.nbytes, attempt(m.__getitem__, 0), attempt(m.__getitem__, size - 1), attempt(lambda: len(m.tolist())), attempt(lambda: len(m.tobytes())), attempt(part.tolist), attempt(last.tolist), attempt(m.__setitem__, size - 1, 1), attempt(lambda: len(bytes(m))), attempt(lambda: m == m), attempt(lambda: len(m.hex())), attempt(lambda: len(list(m))), attempt(lambda: struct.unpack_from("b", m, size - 1)))

print("---- itself")
for c in "bB":
    x = A(c, [1, 2, 3])
    print(c, attempt(x.frombytes, x), x.tolist(), attempt(x.frombytes, memoryview(x)), x.tolist(), attempt(x.extend, x), len(x), attempt(x.fromfile, io.BytesIO(x), 3), len(x))
x = A("i", [1, 2, 3])
print(attempt(x.frombytes, memoryview(x).cast("B")), x.tolist(), attempt(x.__setitem__, slice(1, 2), x), x.tolist())

print("---- while something is being written into it")


class Acts:
    def __init__(self, act, value=7): self.act, self.value = act, value
    def __index__(self):
        self.act()
        return self.value


for name, change in CHANGES[:4]:
    x = A("i", range(8))
    print("pack_into", name, "=>", attempt(struct.pack_into, "<ii", x, 24, Acts(lambda: change(x)), 9), len(x), x.tolist()[-2:])
    x = A("i", range(8))
    m = memoryview(x)
    print("through a view", name, "=>", attempt(m.__setitem__, 7, Acts(lambda: change(x))), len(x), x.tolist()[-2:])
    x = A("b", bytes(8))

    class Reads(io.RawIOBase):
        def readable(self): return True
        def readinto(self, b):
            change(x)
            return attempt(lambda: (b.__setitem__(slice(0, 2), b"ab"), 2)[1]) if False else 0

    print("readinto", name, "=>", attempt(io.BytesIO(b"abcdefgh").readinto, x), len(x))

print("---- a great many, so that there is collecting")
views = []
for i in range(300):
    x = A("d", [float(i)] * 500)
    views.append(memoryview(x)[10:20])
    if i % 3 == 0:
        x.clear()
    elif i % 3 == 1:
        x.extend([1.0] * 5000)
print(sum(1 for v in views if isinstance(attempt(v.tolist), list) and len(v.tolist()) == 10), sum(1 for v in views if isinstance(attempt(v.tolist), str)), sorted({attempt(v.tolist) for v in views if isinstance(attempt(v.tolist), str)}))

print("---- what JavaScript sees")
x = A("i", [1, 2, 3])
print(js.Function("a", "return [typeof a, a.length, a[0], a[2], [...a].join(), Array.from(a).length, String(a), a.typecode, a.itemsize, a.tolist().length, ArrayBuffer.isView(a)].join(' | ')")(x))
print(js.Function("a", "a.append(4); a[0] = 9; return a.tolist().join()")(x), x)
print(js.Function("a, mv", "const m = mv(a); return [m.format, m.nbytes, m.tolist().join()].join(' | ')")(x, memoryview))
print("an Int32Array is what it was:", [(type(v).__name__, memoryview(v).format, memoryview(v).tolist(), A("i", v).tolist(), A("i", bytes(v)).tolist(), attempt(A("i").frombytes, v), attempt(A("b").frombytes, js.Uint8Array.of(1, 2))) for v in [js.Int32Array.of(5, 6)]])
