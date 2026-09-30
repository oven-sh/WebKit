# The modules _md5, _sha1, _sha2, _sha3 and _blake2, which are what hashlib goes by where there is nothing else for it to go by.
import _blake2
import _md5
import _sha1
import _sha2
import _sha3
import array
import binascii
import inspect


def t(label, f):
    try:
        print(label, "=>", f())
    except Exception as e:
        print(label, "=>", type(e).__name__, e)


MODULES = (_md5, _sha1, _sha2, _sha3, _blake2)
MAKERS = {"md5": _md5.md5, "sha1": _sha1.sha1, "sha224": _sha2.sha224, "sha256": _sha2.sha256, "sha384": _sha2.sha384, "sha512": _sha2.sha512,
          "sha3_224": _sha3.sha3_224, "sha3_256": _sha3.sha3_256, "sha3_384": _sha3.sha3_384, "sha3_512": _sha3.sha3_512, "blake2b": _blake2.blake2b, "blake2s": _blake2.blake2s}
SHAKES = {"shake_128": _sha3.shake_128, "shake_256": _sha3.shake_256}
EVERY = {**MAKERS, **SHAKES}

print("---- what there is")
for module in MODULES:
    print(module.__name__, [(name, value if isinstance(value, (int, str)) else type(value).__name__) for name, value in sorted(vars(module).items()) if not name.startswith("__")])
    print("   ", module.__doc__, module.__spec__.origin)
for name, make in EVERY.items():
    h = make()
    kind = type(h)
    print(name, kind.__module__, kind.__qualname__, kind.__mro__[1:], hex(kind.__flags__), sorted(n for n in vars(kind) if not n.startswith("__") or n == "__new__"))
    print("   ", h.name, h.digest_size, h.block_size, [getattr(h, n, None) for n in ("_capacity_bits", "_rate_bits", "_suffix")], repr(h).split(" at ")[0])
    print("   ", [str(inspect.signature(getattr(kind, n))) for n in ("copy", "digest", "hexdigest", "update")], (kind.__doc__ or "")[:60])
    print("   ", [(getattr(kind, n).__doc__ or "")[:50] for n in ("copy", "digest", "hexdigest", "update", "name", "block_size", "digest_size")])
for f in (_md5.md5, _sha1.sha1, _sha2.sha224, _sha2.sha256, _sha2.sha384, _sha2.sha512):
    print(f.__name__, f.__module__, f.__text_signature__, f.__doc__)
for kind in (_sha3.sha3_224, _sha3.shake_128, _blake2.blake2b, _blake2.blake2s):
    t("signature of " + kind.__name__, lambda: str(inspect.signature(kind)))
print([(n, getattr(k, n)) for k in (_blake2.blake2b, _blake2.blake2s) for n in ("SALT_SIZE", "PERSON_SIZE", "MAX_KEY_SIZE", "MAX_DIGEST_SIZE")])

print("---- of nothing, and of a little")
for name, make in MAKERS.items():
    print(name, make().hexdigest(), make(b"abc").hexdigest())
for name, make in SHAKES.items():
    print(name, make().hexdigest(32), make(b"abc").hexdigest(32))

print("---- of every length there is anything to go wrong at")
data = bytes((i * 131 + (i >> 8) * 7 + 5) & 0xFF for i in range(70000))
LENGTHS = list(range(0, 300)) + [511, 512, 513, 1023, 1024, 1025, 4095, 4096, 4097, 65535, 65536, 65537, 70000]
for name, make in EVERY.items():
    finish = (lambda h: h.digest(41)) if name in SHAKES else (lambda h: h.digest())
    print(name, binascii.crc32(b"".join(finish(make(data[:n])) for n in LENGTHS)))

print("---- a piece at a time")
for name, make in EVERY.items():
    finish = (lambda h: h.digest(33)) if name in SHAKES else (lambda h: h.digest())
    whole = finish(make(data[:5000]))
    same = []
    for step in (1, 3, 7, 63, 64, 65, 71, 72, 73, 104, 127, 128, 129, 135, 136, 137, 143, 144, 145, 167, 168, 169, 200, 1000, 4999):
        h = make()
        for i in range(0, 5000, step):
            h.update(data[i:min(i + step, 5000)])
        same.append(finish(h) == whole)
    growing = make()
    at = 0
    for step in range(100):
        growing.update(data[at:at + step])
        at += step
    print(name, all(same), finish(growing) == finish(make(data[:at])))

print("---- asking does not end it")
for name, make in EVERY.items():
    finish = (lambda h: h.hexdigest(10)) if name in SHAKES else (lambda h: h.hexdigest()[:20])
    h = make(b"one")
    first = finish(h)
    again = finish(h)
    h.update(b"two")
    print(name, first == again, finish(h) == finish(make(b"onetwo")), h.digest(10).hex() == finish(h) if name in SHAKES else h.digest().hex()[:20] == finish(h))

print("---- a copy goes its own way")
for name, make in EVERY.items():
    finish = (lambda h: h.hexdigest(10)) if name in SHAKES else (lambda h: h.hexdigest()[:20])
    h = make(data[:100])
    c = h.copy()
    h.update(b"left")
    c.update(b"right")
    print(name, type(c) is type(h), c is not h, finish(h) == finish(make(data[:100] + b"left")), finish(c) == finish(make(data[:100] + b"right")), c.name, c.digest_size)

print("---- as much of a SHAKE as is asked for")
for name, make in SHAKES.items():
    h = make(b"how much")
    print(name, [h.hexdigest(n) for n in (0, 1, 2, 5)], [binascii.crc32(h.digest(n)) for n in (135, 136, 137, 167, 168, 169, 336, 337, 1000, 100000)], h.digest(500)[:100] == h.digest(100))
    for label, f in (("none", lambda: h.digest()), ("negative", lambda: h.digest(-1)), ("too much", lambda: h.digest(1 << 29)), ("far too much", lambda: h.digest(1 << 70)), ("a float", lambda: h.digest(1.0)), ("a str", lambda: h.hexdigest("1")),
                     ("by name", lambda: h.hexdigest(length=3)), ("True", lambda: h.hexdigest(True)), ("just under", lambda: len(h.digest((1 << 20)))), ("hex negative", lambda: h.hexdigest(-1)), ("hex too much", lambda: h.hexdigest(1 << 29)), ("two", lambda: h.digest(1, 2))):
        t("    " + label, f)

print("---- what can be hashed")


class Index:
    def __index__(self):
        return 3


class Exporter:
    def __buffer__(self, flags):
        return memoryview(b"exported")


for name, make in (("md5", _md5.md5), ("sha256", _sha2.sha256), ("sha3_256", _sha3.sha3_256), ("blake2s", _blake2.blake2s)):
    print(name)
    for label, value in (("bytes", b"abc"), ("bytearray", bytearray(b"abc")), ("memoryview", memoryview(b"abc")), ("part of one", memoryview(b"xabcx")[1:4]), ("every other", memoryview(b"a.b.c.")[::2]), ("array", array.array("H", [1, 2, 3])),
                         ("what exports", Exporter()), ("str", "abc"), ("int", 3), ("None", None), ("list", [1, 2]), ("tuple", ()), ("float", 1.5), ("Index", Index())):
        t("    made with " + label, lambda: make(value).hexdigest()[:16])
        t("    given " + label, lambda: [h := make(), h.update(value), h.hexdigest()[:16]][-1])

print("---- how they are called")
for name, make in EVERY.items():
    finish = (lambda h: h.hexdigest(8)) if name in SHAKES else (lambda h: h.hexdigest()[:16])
    print(name)
    for label, f in (("data=", lambda: finish(make(data=b"x"))), ("string=", lambda: finish(make(string=b"x"))), ("both", lambda: make(data=b"x", string=b"y")), ("one of each", lambda: make(b"x", string=b"y")), ("string=None", lambda: finish(make(string=None))),
                     ("data=None", lambda: finish(make(data=None))), ("not for security", lambda: finish(make(b"x", usedforsecurity=False))), ("usedforsecurity by position", lambda: make(b"x", False)), ("usedforsecurity=[]", lambda: finish(make(b"x", usedforsecurity=[]))),
                     ("something else", lambda: make(b"x", other=1)), ("update()", lambda: make().update()), ("update(a, b)", lambda: make().update(b"a", b"b")), ("update(obj=)", lambda: make().update(obj=b"a")), ("update comes to", lambda: make().update(b"a")),
                     ("copy(1)", lambda: make().copy(1)), ("digest(x=1)", lambda: make().digest(x=1)), ("name = ", lambda: setattr(make(), "name", "x")), ("something new", lambda: setattr(make(), "other", 1)), ("hash()", lambda: isinstance(hash(make()), int)),
                     ("==", lambda: make() == make()), ("bool", lambda: bool(make())), ("the class", lambda: finish(type(make())(b"x"))), ("__new__", lambda: finish(type(make()).__new__(type(make())))), ("object.__new__", lambda: object.__new__(type(make()))),
                     ("deriving", lambda: type("D", (type(make()),), {})), ("of another", lambda: type(make()).update(_md5.md5() if name != "md5" else _sha1.sha1(), b"x")), ("a weak reference", lambda: __import__("weakref").ref(make()) and "there can be"),
                     ("pickled", lambda: __import__("pickle").dumps(make())), ("copy.copy", lambda: __import__("copy").copy(make()))):
        t("    " + label, f)

print("---- what BLAKE2 can be told")
for name, make, word in (("blake2b", _blake2.blake2b, 8), ("blake2s", _blake2.blake2s, 4)):
    big = make.MAX_DIGEST_SIZE
    print(name)
    print("    each size", binascii.crc32(b"".join(make(b"abc", digest_size=n).digest() for n in range(1, big + 1))), [make(digest_size=n).digest_size for n in (1, big // 2, big)])
    print("    each length of key", binascii.crc32(b"".join(make(data[:n * 5], key=data[:n]).digest() for n in range(0, make.MAX_KEY_SIZE + 1))))
    print("    a key and nothing else", make(key=b"k").hexdigest()[:32], make(b"", key=b"k").hexdigest()[:32])
    print("    each length of salt", binascii.crc32(b"".join(make(b"abc", salt=data[:n]).digest() for n in range(0, make.SALT_SIZE + 1))))
    print("    each length of person", binascii.crc32(b"".join(make(b"abc", person=data[:n]).digest() for n in range(0, make.PERSON_SIZE + 1))))
    print("    a tree", binascii.crc32(b"".join(make(b"abc", fanout=f, depth=d, leaf_size=l, node_offset=o, node_depth=nd, inner_size=i, last_node=last).digest()
                                              for f in (0, 1, 2, 255) for d in (1, 2, 255) for l in (0, 1, 4096, 2 ** 32 - 1) for o in (0, 1, 2 ** 32, 2 ** 48 - 1) for nd in (0, 1, 255) for i in (0, 1, big) for last in (False, True))))
    print("    all of it", make(data[:1000], digest_size=big - 3, key=b"key", salt=b"salt", person=b"me", fanout=2, depth=3, leaf_size=77, node_offset=5, node_depth=1, inner_size=9, last_node=True).hexdigest())
    print("    with a key, a piece at a time", all(make(data[:n], key=b"k").digest() == [h := make(key=b"k"), [h.update(data[i:i + 1]) for i in range(n)], h.digest()][-1] for n in (0, 1, 63, 64, 65, 127, 128, 129, 200)))
    copied = make(data[:200], key=b"k", digest_size=5, last_node=True).copy()
    print("    a copy has it too", copied.digest_size, copied.hexdigest() == make(data[:200], key=b"k", digest_size=5, last_node=True).hexdigest())
    for label, f in (("digest_size=0", lambda: make(digest_size=0)), ("digest_size too big", lambda: make(digest_size=big + 1)), ("digest_size=-1", lambda: make(digest_size=-1)), ("digest_size a float", lambda: make(digest_size=1.0)), ("digest_size huge", lambda: make(digest_size=2 ** 40)),
                     ("digest_size an Index", lambda: make(digest_size=Index()).digest_size), ("key too long", lambda: make(key=bytes(make.MAX_KEY_SIZE + 1))), ("key a str", lambda: make(key="k")), ("key None", lambda: make(key=None)), ("salt too long", lambda: make(salt=bytes(make.SALT_SIZE + 1))),
                     ("salt a str", lambda: make(salt="s")), ("person too long", lambda: make(person=bytes(make.PERSON_SIZE + 1))), ("person an int", lambda: make(person=1)), ("fanout=-1", lambda: make(fanout=-1)), ("fanout=256", lambda: make(fanout=256)),
                     ("depth=0", lambda: make(depth=0)), ("depth=256", lambda: make(depth=256)), ("leaf_size=-1", lambda: make(leaf_size=-1)), ("leaf_size too big", lambda: make(leaf_size=2 ** 32)), ("leaf_size huge", lambda: make(leaf_size=2 ** 64)), ("leaf_size a float", lambda: make(leaf_size=1.5)),
                     ("node_offset=-1", lambda: make(node_offset=-1)), ("node_offset 2**48", lambda: make(node_offset=2 ** 48).hexdigest()[:16]), ("node_offset 2**64-1", lambda: make(node_offset=2 ** 64 - 1).hexdigest()[:16]), ("node_offset 2**64", lambda: make(node_offset=2 ** 64)),
                     ("node_depth=-1", lambda: make(node_depth=-1)), ("node_depth=256", lambda: make(node_depth=256)), ("inner_size=-1", lambda: make(inner_size=-1)), ("inner_size too big", lambda: make(inner_size=big + 1)), ("last_node=[]", lambda: make(last_node=[]).hexdigest()[:16]),
                     ("last_node='x'", lambda: make(last_node="x").hexdigest()[:16]), ("key by position", lambda: make(b"", 5)), ("a bytearray for each", lambda: make(bytearray(b"d"), key=bytearray(b"k"), salt=bytearray(b"s"), person=memoryview(b"p")).hexdigest()[:16]),
                     ("several wrong, which is said first", lambda: make(digest_size=0, key=bytes(100), salt=bytes(100), fanout=-1)), ("and again", lambda: make(salt=bytes(100), person=bytes(100), key=bytes(100), depth=0)), ("and a str besides", lambda: make("s", digest_size=0))):
        t("    " + label, f)

print("---- what is being hashed is changed meanwhile")


class Changes:
    def __init__(self, target):
        self.target = target

    def __bool__(self):
        self.target.clear()
        return True


target = bytearray(b"abc" * 100)
t("by usedforsecurity", lambda: (_sha2.sha256(target, usedforsecurity=Changes(target)).hexdigest()[:16], len(target)))
target = bytearray(b"abc" * 100)
t("of a BLAKE2, by last_node", lambda: (_blake2.blake2b(b"", key=target[:10], last_node=Changes(target)).hexdigest()[:16], len(target)))
# CPython does not let this one be changed, having hold of it by then. What matters is that nothing is read from where it was.
target = bytearray(b"k" * 100000)
try:
    _blake2.blake2b(b"", key=memoryview(target)[:10], last_node=Changes(target)).hexdigest()
except BufferError:
    pass
try:
    _blake2.blake2b(b"", key=target, last_node=Changes(target)).hexdigest()
except (BufferError, ValueError):
    pass
print("its key: that is got through")
