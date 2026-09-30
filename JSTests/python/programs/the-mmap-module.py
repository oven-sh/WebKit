# The module mmap.
import gc
import mmap
import os
import sys
import tempfile

LINUX = sys.platform == "linux"


def t(label, f):
    try:
        print(label, "=>", f())
    except Exception as e:
        print(label, "=>", type(e).__name__, e)


def kind(f):
    try:
        return f()
    except Exception as e:
        return type(e).__name__, str(e)


directory = tempfile.TemporaryDirectory()
counter = [0]


def file_of(content, mode="r+b"):
    counter[0] += 1
    path = os.path.join(directory.name, "f%d" % counter[0])
    with open(path, "wb") as f:
        f.write(content)
    return open(path, mode)


def mapped(content=b"0123456789abcdef", **settings):
    with file_of(content) as f:
        return mmap.mmap(f.fileno(), 0, **settings)


print("---- what there is")
everywhere = ("ACCESS_COPY", "ACCESS_DEFAULT", "ACCESS_READ", "ACCESS_WRITE", "ALLOCATIONGRANULARITY", "MAP_ANON", "MAP_ANONYMOUS", "MAP_PRIVATE", "MAP_SHARED", "PAGESIZE", "PROT_EXEC", "PROT_READ", "PROT_WRITE", "MADV_NORMAL", "MADV_RANDOM", "MADV_SEQUENTIAL", "MADV_WILLNEED", "MADV_DONTNEED", "error", "mmap")
print([n for n in everywhere if not hasattr(mmap, n)], mmap.error is OSError, mmap.__doc__, mmap.__spec__.origin, mmap.PAGESIZE == os.sysconf("SC_PAGESIZE") == mmap.ALLOCATIONGRANULARITY, mmap.MAP_ANON == mmap.MAP_ANONYMOUS)
print([(n, getattr(mmap, n)) for n in ("ACCESS_DEFAULT", "ACCESS_READ", "ACCESS_WRITE", "ACCESS_COPY", "PROT_READ", "PROT_WRITE", "PROT_EXEC", "MAP_SHARED", "MAP_PRIVATE", "MADV_NORMAL", "MADV_RANDOM", "MADV_SEQUENTIAL", "MADV_WILLNEED", "MADV_DONTNEED")])
c = mmap.mmap
print(c.__module__, c.__qualname__, [b.__name__ for b in c.__mro__[1:]], hex(c.__flags__), c.__doc__[:40], c.__text_signature__)
for n in sorted(vars(c)):
    v = vars(c)[n]
    print("   ", n, type(v).__name__, getattr(v, "__text_signature__", None), repr(v.__doc__)[:50])

print("---- reading")
m = mapped()
print(len(m), m.tell(), m.closed, m.seekable(), m.size(), m.read(4), m.tell(), m.read_byte(), m.read(0), m.read(None), m.tell(), m.read(), m.read(5), kind(m.read_byte), m.seek(0), m.read(-1), m.seek(2), m.read(100))
print(m[0], m[-1], m[3:6], m[:], m[::2], m[::-1], m[10:2], m[100:], m[-3:], m[1:12:5], m[True], repr(m), m.seek(0), repr(m))
lines = mapped(b"one\ntwo\n\nlast")
print(lines.readline(), lines.readline(), lines.readline(), lines.readline(), lines.readline(), lines.tell())
print([m.find(x) for x in (b"0", b"9a", b"f", b"", b"x", b"0123456789abcdefg")], m.seek(5), [m.find(x) for x in (b"0", b"5", b"9a", b"")], [m.rfind(x) for x in (b"0", b"5", b"9a", b"")], m.find(b"0", 0), m.find(b"a", 0, 10), m.find(b"a", 0, 11), m.find(b"a", -6), m.find(b"a", -100, 100), m.find(b"a", 11), m.find(b"", 16), m.find(b"", 17),
      m.find(b"a", 5, 2), m.find(b"a", None), m.find(b"a", None, 3), m.find(b"a", 0, None), m.find(bytearray(b"ab")), m.find(memoryview(b"ab")))
twice = mapped(b"abcabcabc")
print(twice.find(b"abc"), twice.rfind(b"abc"), twice.rfind(b"abc", 0, 8), twice.rfind(b"abc", 0, 2), twice.rfind(b"", 0, 4), twice.find(b"c", 3), twice.rfind(b"a", 1, 3), twice.find(twice), twice.find(b"bca"), twice.rfind(b"bca"))
for label, f in (("m[16]", lambda: m[16]), ("m[-17]", lambda: m[-17]), ("m['a']", lambda: m["a"]), ("m[1.0]", lambda: m[1.0]), ("m[None]", lambda: m[None]), ("m[2 ** 70]", lambda: m[2 ** 70]), ("m[::0]", lambda: m[::0]), ("read('1')", lambda: m.read("1")), ("read(1.0)", lambda: m.read(1.0)), ("read(1, 2)", lambda: m.read(1, 2)), ("read(2 ** 70)", lambda: m.read(2 ** 70)),
                 ("read_byte(1)", lambda: m.read_byte(1)), ("readline(1)", lambda: m.readline(1)), ("find()", lambda: m.find()), ("find('a')", lambda: m.find("a")), ("find(5)", lambda: m.find(5)), ("find(b'a', 'x')", lambda: m.find(b"a", "x")), ("find(b'a', 0, 'x')", lambda: m.find(b"a", 0, "x")), ("find(b'a', None, 'x')", lambda: m.find(b"a", None, "x")),
                 ("find(b'a', 1.0)", lambda: m.find(b"a", 1.0)), ("find(b'a', 2 ** 70)", lambda: m.find(b"a", 2 ** 70)), ("find(b'a', 0, 0, 0)", lambda: m.find(b"a", 0, 0, 0)), ("find(view=b'a')", lambda: m.find(view=b"a")), ("tell(1)", lambda: m.tell(1)), ("size(1)", lambda: m.size(1)), ("seekable(1)", lambda: m.seekable(1)), ("iter", lambda: list(iter(mapped(b"ab")))),
                 ("in", lambda: (b"a" in m, b"x" in m)), ("an int in", lambda: 97 in m), ("bytes(m)", lambda: bytes(m)), ("hash", lambda: type(hash(m)).__name__), ("==", lambda: (m == m, m == mapped(), m == bytes(m))), ("+", lambda: m + b"x"), ("* 2", lambda: m * 2), ("an attribute", lambda: setattr(m, "x", 1)), ("closed = 1", lambda: setattr(m, "closed", 1)), ("bool", lambda: bool(m))):
    t("    " + label, f)

print("---- where it is")
for label, f in (("seek(3)", lambda: (m.seek(3), m.tell())), ("seek(2, 1)", lambda: (m.seek(2, 1), m.tell())), ("seek(-1, 1)", lambda: (m.seek(-1, 1), m.tell())), ("seek(-2, 2)", lambda: (m.seek(-2, 2), m.tell())), ("seek(0, 2)", lambda: (m.seek(0, 2), m.tell())), ("seek(16)", lambda: m.seek(16)), ("seek(17)", lambda: m.seek(17)), ("seek(-1)", lambda: m.seek(-1)),
                 ("seek(1, 2)", lambda: m.seek(1, 2)), ("seek(-17, 2)", lambda: m.seek(-17, 2)), ("seek(0, 3)", lambda: m.seek(0, 3)), ("seek(0, -1)", lambda: m.seek(0, -1)), ("seek(2 ** 63 - 1, 1)", lambda: m.seek(2 ** 63 - 1, 1)), ("seek(2 ** 63 - 1, 2)", lambda: m.seek(2 ** 63 - 1, 2)), ("seek(2 ** 63)", lambda: m.seek(2 ** 63)), ("seek('1')", lambda: m.seek("1")),
                 ("seek(0, '1')", lambda: m.seek(0, "1")), ("seek()", lambda: m.seek()), ("seek(0, 0, 0)", lambda: m.seek(0, 0, 0)), ("seek(pos=0)", lambda: m.seek(pos=0)), ("and it has not moved", lambda: m.tell())):
    t("    " + label, f)

print("---- writing")
with file_of(b"0123456789abcdef") as f:
    m = mmap.mmap(f.fileno(), 0)
    print(m.write(b"AB"), m.tell(), m.write(bytearray(b"C")), m.write(memoryview(b"D")), m.write(b""), m.write_byte(69), m.write_byte(255), m.tell(), m[:8])
    m[0] = 120
    m[-1] = 0
    m[1:3] = b"yz"
    m[8:14:2] = b"123"
    m[3:3] = b""
    m[4:6] = bytearray(b"--")
    print(m[:], m.flush(), m.flush(0), m.flush(0, None), m.flush(0, 16))
    f.seek(0)
    print(f.read())
    print(m.move(0, 8, 4), m[:], m.move(2, 0, 10), m[:], m.move(0, 0, 0), m.move(16, 16, 0), m.move(0, 0, 16))
    m.seek(0)
    print(m.write(m[4:8]), m[:], m.seek(2), m.write(memoryview(m)[0:6]), m[:])
    for label, g in (("past the end", lambda: [m.seek(14), m.write(b"abc")]), ("and it has not moved", lambda: m.tell()), ("write_byte() at the end", lambda: [m.seek(16), m.write_byte(1)]), ("write('a')", lambda: m.write("a")), ("write(5)", lambda: m.write(5)), ("write()", lambda: m.write()), ("write(bytes=b'')", lambda: m.write(bytes=b"")), ("write_byte(256)", lambda: m.write_byte(256)),
                     ("write_byte(-1)", lambda: m.write_byte(-1)), ("write_byte('a')", lambda: m.write_byte("a")), ("write_byte(b'a')", lambda: m.write_byte(b"a")), ("write_byte(1.0)", lambda: m.write_byte(1.0)), ("write_byte(2 ** 70)", lambda: m.write_byte(2 ** 70)), ("write_byte()", lambda: m.write_byte()), ("m[0] = 256", lambda: m.__setitem__(0, 256)), ("= -1", lambda: m.__setitem__(0, -1)),
                     ("= 2 ** 70", lambda: m.__setitem__(0, 2 ** 70)), ("= 'a'", lambda: m.__setitem__(0, "a")), ("= b'a'", lambda: m.__setitem__(0, b"a")), ("= 1.0", lambda: m.__setitem__(0, 1.0)), ("= None", lambda: m.__setitem__(0, None)), ("= True", lambda: [m.__setitem__(0, True), m[0]][1]), ("m[16] = 0", lambda: m.__setitem__(16, 0)), ("m[-17] = 0", lambda: m.__setitem__(-17, 0)),
                     ("m['a'] = 0", lambda: m.__setitem__("a", 0)), ("m[1.0] = 0", lambda: m.__setitem__(1.0, 0)), ("too long", lambda: m.__setitem__(slice(0, 2), b"abc")), ("too short", lambda: m.__setitem__(slice(0, 2), b"a")), ("a str", lambda: m.__setitem__(slice(0, 2), "ab")), ("an int", lambda: m.__setitem__(slice(0, 2), 5)), ("backwards", lambda: [m.__setitem__(slice(3, None, -1), b"WXYZ"), m[:4]][1]),
                     ("no step", lambda: m.__setitem__(slice(None, None, 0), b"")), ("del m[0]", lambda: m.__delitem__(0)), ("del m[0:1]", lambda: m.__delitem__(slice(0, 1))), ("del m['a']", lambda: m.__delitem__("a")), ("del m[100]", lambda: m.__delitem__(100)), ("move(-1, 0, 1)", lambda: m.move(-1, 0, 1)), ("move(0, -1, 1)", lambda: m.move(0, -1, 1)), ("move(0, 0, -1)", lambda: m.move(0, 0, -1)),
                     ("move(0, 0, 17)", lambda: m.move(0, 0, 17)), ("move(16, 0, 1)", lambda: m.move(16, 0, 1)), ("move(0, 16, 1)", lambda: m.move(0, 16, 1)), ("move(0, 0, 2 ** 63 - 1)", lambda: m.move(0, 0, 2 ** 63 - 1)), ("move(0, 0)", lambda: m.move(0, 0)), ("move('0', 0, 0)", lambda: m.move("0", 0, 0)), ("move(2 ** 70, 0, 0)", lambda: m.move(2 ** 70, 0, 0)), ("flush(17)", lambda: m.flush(17)),
                     ("flush(-1)", lambda: m.flush(-1)), ("flush(0, 17)", lambda: m.flush(0, 17)), ("flush(0, -1)", lambda: m.flush(0, -1)), ("flush(16, 1)", lambda: m.flush(16, 1)), ("flush('0')", lambda: m.flush("0")), ("flush(0, '0')", lambda: m.flush(0, "0")), ("flush(0, 0, 0)", lambda: m.flush(0, 0, 0)), ("flush(None)", lambda: m.flush(None))):
        t("    " + label, g)
    m.close()

print("---- how it may be used")
for access in (mmap.ACCESS_DEFAULT, mmap.ACCESS_READ, mmap.ACCESS_WRITE, mmap.ACCESS_COPY):
    with file_of(b"0123456789") as f:
        m = mmap.mmap(f.fileno(), 0, access=access)
        print(repr(m), kind(lambda: m.write(b"AB")), kind(lambda: m.write_byte(67)), kind(lambda: m.__setitem__(3, 68)), kind(lambda: m.__setitem__(slice(4, 6), b"EF")), kind(lambda: m.move(6, 0, 2)), m[:], m.flush(), memoryview(m).readonly, kind(lambda: memoryview(m).__setitem__(9, 90)), kind(lambda: m.__delitem__(0)))
        f.seek(0)
        print("    in the file:", f.read())
for label, settings in (("PROT_READ", {"prot": mmap.PROT_READ}), ("PROT_WRITE", {"prot": mmap.PROT_WRITE}), ("both", {"prot": mmap.PROT_READ | mmap.PROT_WRITE}), ("private", {"flags": mmap.MAP_PRIVATE}), ("private, to read", {"flags": mmap.MAP_PRIVATE, "prot": mmap.PROT_READ})):
    m = mapped(**settings)
    print("   ", label, repr(m), kind(lambda: m.write(b"A")))
with file_of(b"0123456789", "rb") as f:
    print(mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)[:], mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_COPY)[:3], kind(lambda: mmap.mmap(f.fileno(), 0))[0], kind(lambda: mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_WRITE))[0])

print("---- making one")
with file_of(b"x" * (mmap.PAGESIZE * 2 + 10)) as f:
    n = f.fileno()
    whole = mmap.PAGESIZE * 2 + 10
    print(len(mmap.mmap(n, 0)) == whole, len(mmap.mmap(n, 5)), len(mmap.mmap(n, whole)) == whole, len(mmap.mmap(n, 0, offset=mmap.PAGESIZE)) == mmap.PAGESIZE + 10, len(mmap.mmap(n, 7, offset=mmap.PAGESIZE * 2)), len(mmap.mmap(fileno=n, length=3, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ, access=mmap.ACCESS_DEFAULT, offset=0, trackfd=True)),
          len(mmap.mmap(n, 4, mmap.MAP_PRIVATE, mmap.PROT_READ, mmap.ACCESS_DEFAULT, 0)), repr(mmap.mmap(n, 1, offset=mmap.PAGESIZE)).replace(str(mmap.PAGESIZE), "PAGESIZE"))
    for label, g in (("mmap()", lambda: mmap.mmap()), ("one argument", lambda: mmap.mmap(n)), ("a str", lambda: mmap.mmap("1", 0)), ("the file itself", lambda: mmap.mmap(f, 0)), ("length a str", lambda: mmap.mmap(n, "1")), ("length -1", lambda: mmap.mmap(n, -1)), ("length 2 ** 70", lambda: mmap.mmap(n, 2 ** 70)), ("longer than the file", lambda: mmap.mmap(n, whole + 1)),
                     ("offset -1", lambda: mmap.mmap(n, 0, offset=-1)), ("offset past the end", lambda: mmap.mmap(n, 0, offset=mmap.PAGESIZE * 3)), ("offset at the end", lambda: mmap.mmap(n, 0, offset=whole)), ("offset and length past the end", lambda: mmap.mmap(n, 11, offset=mmap.PAGESIZE * 2)), ("offset not of a page", lambda: kind(lambda: mmap.mmap(n, 0, offset=1))[0]),
                     ("offset a str", lambda: mmap.mmap(n, 0, offset="1")), ("offset 2 ** 70", lambda: mmap.mmap(n, 0, offset=2 ** 70)), ("access 4", lambda: mmap.mmap(n, 0, access=4)), ("access -1", lambda: mmap.mmap(n, 0, access=-1)), ("access a str", lambda: mmap.mmap(n, 0, access="1")), ("access and flags", lambda: mmap.mmap(n, 0, flags=mmap.MAP_PRIVATE, access=mmap.ACCESS_READ)),
                     ("access and prot", lambda: mmap.mmap(n, 0, prot=mmap.PROT_READ, access=mmap.ACCESS_WRITE)), ("access, and what flags and prot are anyway", lambda: len(mmap.mmap(n, 3, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ | mmap.PROT_WRITE, access=mmap.ACCESS_READ))), ("trackfd not by name", lambda: mmap.mmap(n, 0, mmap.MAP_SHARED, mmap.PROT_READ, 0, 0, True)),
                     ("a name that there is not", lambda: mmap.mmap(n, 0, nothing=1)), ("no such descriptor", lambda: kind(lambda: mmap.mmap(10 ** 6, 10))[0]), ("-2", lambda: kind(lambda: mmap.mmap(-2, 10))[0]), ("a class derived from it", lambda: [x := type("M", (mmap.mmap,), {})(n, 3), setattr(x, "y", 1), x.y, repr(x)][2:])):
        t("    " + label, g)
with file_of(b"") as f:
    t("    a file with nothing in it", lambda: mmap.mmap(f.fileno(), 0))
    t("    and a length", lambda: mmap.mmap(f.fileno(), 1))
print("of no file")
a = mmap.mmap(-1, 100)
print(len(a), a[:5], a.write(b"hello"), a[:7], repr(a), kind(a.size)[0], a.flush(), a.find(b"llo", 0), a.closed)
for label, g in (("nothing", lambda: kind(lambda: mmap.mmap(-1, 0))[0]), ("to read", lambda: [x := mmap.mmap(-1, 10, access=mmap.ACCESS_READ), x[:2], kind(lambda: x.write(b"a"))][1:]), ("a copy", lambda: [x := mmap.mmap(-1, 10, access=mmap.ACCESS_COPY), x.write(b"a"), x[:2]][1:]), ("private", lambda: len(mmap.mmap(-1, 10, flags=mmap.MAP_PRIVATE))),
                 ("large", lambda: [x := mmap.mmap(-1, 1 << 28), len(x), x[-1], x.__setitem__(-1, 7), x[-1], x.close()][1:]), ("an offset", lambda: kind(lambda: len(mmap.mmap(-1, 10, offset=mmap.PAGESIZE))) in (10, ("OSError", "[Errno 22] Invalid argument")))):
    t("    " + label, g)
print("the descriptor")
with file_of(b"0123456789") as f:
    tracked = mmap.mmap(f.fileno(), 0)
    untracked = mmap.mmap(f.fileno(), 0, trackfd=False)
print(tracked[:3], tracked.size(), untracked[:3], kind(untracked.size)[0], kind(lambda: untracked.resize(5)), tracked.write(b"AB"), untracked[:3])

print("---- what else can look at it")
with file_of(b"0123456789") as f:
    m = mmap.mmap(f.fileno(), 0)
    v = memoryview(m)
    print(v.nbytes, v.format, v.itemsize, v.readonly, v.obj is m, bytes(v[2:5]), v[0], v.tolist()[:3], v.contiguous, v.shape, v.strides)
    v[0] = 65
    v[1:3] = b"BC"
    m[3] = 68
    print(m[:5], bytes(v[:5]), bytes(m) == bytes(v), bytearray(m)[:4], b"".join([m, m])[:12], int.from_bytes(m[:2]), m.find(v[1:3], 0), f.readinto(m), m[:])
    import re, struct
    print(re.search(rb"[3-5]+", m).span(), struct.unpack_from("<H", m, 0), struct.pack_into("<H", m, 0, 0x4142), m[:3], os.write(os.open(os.devnull, os.O_WRONLY), m), m.__buffer__(0).nbytes, kind(lambda: m.__buffer__(1).readonly))
    v.release()
    m.close()
r = mapped(access=mmap.ACCESS_READ)
print(memoryview(r).readonly, kind(lambda: r.__buffer__(1)), kind(lambda: struct.pack_into("<H", r, 0, 1)), kind(lambda: file_of(b"ab").readinto(r)), bytes(memoryview(r)[:3]))

print("---- advice")
m = mapped(b"x" * (mmap.PAGESIZE * 3))
print(m.madvise(mmap.MADV_NORMAL), m.madvise(mmap.MADV_SEQUENTIAL, 0), m.madvise(mmap.MADV_WILLNEED, mmap.PAGESIZE), m.madvise(mmap.MADV_RANDOM, mmap.PAGESIZE, mmap.PAGESIZE), m.madvise(mmap.MADV_NORMAL, 0, None), m.madvise(mmap.MADV_NORMAL, mmap.PAGESIZE, 2 ** 40), m.madvise(mmap.MADV_NORMAL, 0, 0))
for label, g in (("madvise()", lambda: m.madvise()), ("a str", lambda: m.madvise("1")), ("start -1", lambda: m.madvise(0, -1)), ("start at the end", lambda: m.madvise(0, len(m))), ("length -1", lambda: m.madvise(0, 0, -1)), ("too long", lambda: m.madvise(0, 1, 2 ** 63 - 1)), ("start a str", lambda: m.madvise(0, "1")), ("length a str", lambda: m.madvise(0, 0, "1")),
                 ("no such advice", lambda: kind(lambda: m.madvise(10 ** 6))[0]), ("not from where a page begins", lambda: kind(lambda: m.madvise(mmap.MADV_NORMAL, 1))[0] if LINUX else "OSError"), ("four", lambda: m.madvise(0, 0, 0, 0)), ("by name", lambda: m.madvise(option=0))):
    t("    " + label, g)

print("---- another size")
with file_of(b"0123456789") as f:
    m = mmap.mmap(f.fileno(), 0)
    if LINUX:
        assert (m.resize(20), len(m), m[:], m.size(), os.fstat(f.fileno()).st_size) == (None, 20, b"0123456789" + bytes(10), 20, 20)
        m[15] = 65
        assert (m.resize(4), len(m), m[:], m.size()) == (None, 4, b"0123", 4)
        m.seek(4)
        assert (m.resize(2), m.tell(), m.read(), kind(m.read_byte)) == (None, 4, b"", ("ValueError", "read byte out of range"))
        big = mmap.PAGESIZE * 64
        assert (m.resize(big), len(m), m[:3], m[-1]) == (None, big, b"01\0", 0)
        assert kind(lambda: mmap.mmap(-1, 10).resize(20)) == ("ValueError", "mmap: can't expand a shared anonymous mapping")
        assert [x := mmap.mmap(-1, 10, flags=mmap.MAP_PRIVATE), x.write(b"ab"), x.resize(mmap.PAGESIZE * 4), len(x) == mmap.PAGESIZE * 4, x[:3]][3:] == [True, b"ab\0"]
        assert [x := mmap.mmap(-1, 10), x.resize(5), len(x)][2] == 5
    else:
        assert kind(lambda: m.resize(20)) == ("SystemError", "mmap: resizing not available--no mremap()")
    for label, g in (("resize(-1)", lambda: m.resize(-1)), ("resize(2 ** 63 - 1)", lambda: kind(lambda: m.resize(2 ** 63 - 1))[0] in ("OSError", "SystemError")), ("resize('1')", lambda: m.resize("1")), ("resize()", lambda: m.resize()), ("resize(2 ** 70)", lambda: m.resize(2 ** 70)), ("resize(newsize=1)", lambda: m.resize(newsize=1)),
                     ("to read", lambda: mapped(access=mmap.ACCESS_READ).resize(5)), ("a copy", lambda: mapped(access=mmap.ACCESS_COPY).resize(5))):
        t("    " + label, g)

print("---- closed")
m = mapped()
print(m.closed, m.close(), m.closed, m.close(), repr(m))
for name, arguments in (("read", ()), ("read_byte", ()), ("readline", ()), ("write", (b"",)), ("write_byte", (0,)), ("find", (b"",)), ("rfind", (b"",)), ("seek", (0,)), ("tell", ()), ("size", ()), ("flush", ()), ("move", (0, 0, 0)), ("resize", (1,)), ("madvise", (0,)), ("__len__", ()), ("__getitem__", (0,)), ("__getitem__", (slice(0, 1),)),
                        ("__setitem__", (0, 0)), ("__setitem__", (slice(0, 0), b"")), ("__delitem__", (0,)), ("__enter__", ()), ("__exit__", (None, None, None)), ("__buffer__", (0,)), ("seekable", ())):
    t("    " + name, lambda: getattr(m, name)(*arguments))
for label, g in (("memoryview", lambda: memoryview(m)), ("bytes", lambda: bytes(m)), ("bytearray", lambda: bytearray(m)), ("in", lambda: b"a" in m), ("iter", lambda: list(m)), ("join", lambda: b"".join([m])), ("find(m)", lambda: mapped().find(m)), ("write(m)", lambda: mapped().write(m))):
    t("    " + label, g)
with mapped() as inside:
    print(inside.closed, inside[:2])
print(inside.closed, kind(lambda: inside[:2]), kind(lambda: mapped().__exit__()), kind(lambda: mapped().__exit__(1, 2)), kind(lambda: mapped().close(1)))
class Closes:
    def __init__(self, what): self.what = what
    def __index__(self):
        self.what.close()
        return 0
for label, g in (("m[x]", lambda m: m[Closes(m)]), ("m[x:]", lambda m: m[Closes(m):]), ("m[x] = 0", lambda m: m.__setitem__(Closes(m), 0)), ("m[0] = x", lambda m: m.__setitem__(0, Closes(m))), ("m[x:0] = b''", lambda m: m.__setitem__(slice(Closes(m), 0), b"")), ("find(b'', x)", lambda m: m.find(b"", Closes(m))), ("find(b'', 0, x)", lambda m: m.find(b"", 0, Closes(m))),
                 ("madvise(0, 0, x)", lambda m: m.madvise(0, 0, Closes(m))), ("seek(x)", lambda m: m.seek(Closes(m))), ("read(x)", lambda m: m.read(Closes(m))), ("move(x, 0, 0)", lambda m: m.move(Closes(m), 0, 0))):
    t("    closed while it is asked: " + label, lambda: g(mapped()))

print("---- what is told to whoever is listening")
heard = []
listening = [True]
sys.addaudithook(lambda event, arguments: heard.append((event, arguments[1:])) if listening[0] and event.startswith("mmap") else None)
mapped(access=mmap.ACCESS_READ)
mmap.mmap(-1, 10)
kind(lambda: mmap.mmap(-1, -1))
print(heard)
listening[0] = False

print("---- two of the same file")
with file_of(b"0123456789") as f:
    a, b = mmap.mmap(f.fileno(), 0), mmap.mmap(f.fileno(), 0)
    a[0] = 65
    private = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_COPY)
    private[1] = 66
    print(b[:3], private[:3], a[:3])

print("---- a great many, none of them kept")
for i in range(2000):
    mmap.mmap(-1, 1 << 20)[0]
    mapped()
gc.collect()
print("done")
directory.cleanup()
