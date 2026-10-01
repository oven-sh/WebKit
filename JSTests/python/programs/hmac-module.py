# _hmac: HMAC over the hashes that Python has of its own
import _hmac, _md5, _sha2, hmac, hashlib, re, copy, pickle, array
address = re.compile("0x[0-9a-f]+")
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
    print(label.encode("ascii", "backslashreplace").decode(), "->", address.sub("0x", ascii(r)))
names = ["md5", "sha1", "sha224", "sha256", "sha384", "sha512", "sha3_224", "sha3_256", "sha3_384", "sha3_512", "blake2s", "blake2b"]
other_names = {"sha2_224": "sha224", "sha2_256": "sha256", "sha2_384": "sha384", "sha2_512": "sha512", "blake2s_32": "blake2s", "blake2b_32": "blake2b"}
def by_hand(name, key, msg):
    "RFC 2104, written out"
    h = lambda d: hashlib.new(name, d).digest()
    block = hashlib.new(name).block_size
    if len(key) > block: key = h(key)
    key = key.ljust(block, b"\0")
    return h(bytes(k ^ 0x5c for k in key) + h(bytes(k ^ 0x36 for k in key) + msg))
print("===== what it comes to")
keys = [b"", b"k", b"key", b"\x0b" * 20, b"\xaa" * 63, b"\xaa" * 64, b"\xaa" * 65, b"\xaa" * 71, b"\xaa" * 72, b"\xaa" * 73, b"\xaa" * 104, b"\xaa" * 105, b"\xaa" * 127, b"\xaa" * 128, b"\xaa" * 129, b"\xaa" * 136, b"\xaa" * 137, b"\xaa" * 144, b"\xaa" * 145, bytes(range(256)) * 5]
messages = [b"", b"a", b"Hi There", b"what do ya want for nothing?", b"\xdd" * 50, b"x" * 63, b"x" * 64, b"x" * 65, b"x" * 127, b"x" * 128, b"x" * 129, b"x" * 143, b"x" * 144, b"x" * 2047, b"x" * 2048, b"x" * 2049, bytes(range(256)) * 50]
for n in names:
    rows = []
    for k in keys:
        for m in messages:
            d = _hmac.compute_digest(k, m, n)
            o = _hmac.new(k, m, n)
            p = _hmac.new(k, None, n)
            for i in range(0, len(m), 97): p.update(m[i:i + 97])
            rows.append((d.hex(), d == by_hand(n, k, m), o.digest() == d, o.hexdigest() == d.hex(), p.digest() == d, getattr(_hmac, "compute_" + {"blake2s": "blake2s_32", "blake2b": "blake2b_32"}.get(n, n))(k, m) == d, hmac.digest(k, m, n) == d, hmac.new(k, m, n).digest() == d))
    print(n, hashlib.md5(repr(rows).encode()).hexdigest(), all(all(r[1:]) for r in rows), rows[2 * len(messages) + 2][0])
print("===== one of them")
for n in names + list(other_names) + ["MD5", "Sha256", "SHA3_256", "BLAKE2B", "Sha2_256"]:
    attempt(n, lambda: (lambda h: (h.name, h.block_size, h.digest_size, repr(h), type(h.name).__name__, type(h.block_size).__name__, len(h.digest()), len(h.hexdigest())))(_hmac.new(b"k", b"m", n)))
h = _hmac.new(b"key", b"abc", "sha256")
attempt("asked again and again", lambda: (h.digest() == h.digest(), h.hexdigest(), h.update(b"def"), h.hexdigest(), _hmac.new(b"key", b"abcdef", "sha256").hexdigest()))
attempt("copy", lambda: (lambda c: (c is not h, type(c) is type(h), c.hexdigest() == h.hexdigest(), c.update(b"x"), c.hexdigest() == h.hexdigest(), c.name, repr(c)))(h.copy()))
attempt("copy.copy", lambda: copy.copy(h))
attempt("pickle", lambda: pickle.dumps(h))
attempt("attributes", lambda: setattr(h, "x", 1))
for a in ("name", "block_size", "digest_size"):
    attempt("set " + a, lambda: setattr(h, a, 1))
attempt("hash, ==, bool", lambda: (hash(h) == hash(h), h == h, h == h.copy(), bool(h)))
attempt("weak reference", lambda: __import__("weakref").ref(h))
print("===== what is no hash")
class S(str): pass
class L(str):
    def lower(self): return "sha256"
class B(str):
    def lower(self): return 5
class R(str):
    def lower(self): raise ZeroDivisionError
for v in ("", "x", "sha", "sha-256", "sha256 ", " sha256", "sha512_224", "sha512_256", "shake_128", "shake_256", "ripemd160", "sm3", "md5-sha1", "blake2", "blake2b_64", "sha2_", "sha256\0", "sha\u0132", "\u017fha256", "\ud800", None, 5, b"sha256", _sha2.sha256, _md5.md5, _sha2.sha256(), [], ("sha256",), S("sha256"), S("SHA256"), L("nothing"), B("nothing"), R("nothing"), L("sha1"), True):
    shown = v.__name__ if callable(v) else type(v).__name__ + ":" + ascii(v) if not isinstance(v, (str, bytes, int, type(None))) or type(v) not in (str, bytes, int, type(None), bool) else ascii(v)
    attempt("new(%s)" % address.sub("0x", shown), lambda: _hmac.new(b"k", b"m", v).name)
    attempt("   compute_digest", lambda: len(_hmac.compute_digest(b"k", b"m", v)))
print("===== keys and messages")
class Exports:
    def __buffer__(self, flags): return memoryview(b"exported")
things = {"bytes": b"ab", "bytearray": bytearray(b"ab"), "memoryview": memoryview(b"ab"), "array": array.array("B", b"ab"), "array of ints": array.array("i", [1, 2]), "a slice": memoryview(b"abcd")[::2], "two dimensions": memoryview(b"abcd").cast("B", (2, 2)), "its own buffer": Exports(), "str": "ab", "an empty str": "", "None": None, "int": 5, "list": [1], "a released view": (lambda m: (m.release(), m)[1])(memoryview(b"ab")), "derived from bytes": type("X", (bytes,), {})(b"ab")}
for label, v in things.items():
    attempt("key %s" % label, lambda: _hmac.new(v, b"m", "md5").hexdigest())
    attempt("   message", lambda: _hmac.new(b"k", v, "md5").hexdigest())
    attempt("   update", lambda: (lambda x: (x.update(v), x.hexdigest()))(_hmac.new(b"k", None, "md5")))
    attempt("   compute_digest, key", lambda: _hmac.compute_digest(v, b"m", "md5").hex())
    attempt("   compute_digest, message", lambda: _hmac.compute_digest(b"k", v, "md5").hex())
    attempt("   compute_md5", lambda: (_hmac.compute_md5(v, b"m").hex(), _hmac.compute_md5(b"k", v).hex()))
attempt("which is found wrong first", lambda: _hmac.new("k", "m", "nothing"))
attempt("   the key before the message", lambda: _hmac.new("k", 5, "md5"))
attempt("   in compute_digest", lambda: _hmac.compute_digest("k", 5, "nothing"))
attempt("   in compute_md5", lambda: _hmac.compute_md5(5, "m"))
print("===== how they are called")
for label, f in {"new()": lambda: _hmac.new(), "new(key)": lambda: _hmac.new(b"k"), "new(key, msg)": lambda: _hmac.new(b"k", b"m"), "new(key, None, None)": lambda: _hmac.new(b"k", None, None), "by name": lambda: _hmac.new(key=b"k", msg=b"m", digestmod="md5").name, "digestmod by name": lambda: _hmac.new(b"k", digestmod="md5").name, "four": lambda: _hmac.new(b"k", b"m", "md5", 1), "unknown": lambda: _hmac.new(b"k", x=1), "twice": lambda: _hmac.new(b"k", key=b"k"),
                 "compute_digest()": lambda: _hmac.compute_digest(), "compute_digest: two": lambda: _hmac.compute_digest(b"k", b"m"), "compute_digest: by name": lambda: _hmac.compute_digest(key=b"k", msg=b"m", digest="md5").hex(), "compute_digest: four": lambda: _hmac.compute_digest(b"k", b"m", "md5", 1), "compute_md5()": lambda: _hmac.compute_md5(), "compute_md5: one": lambda: _hmac.compute_md5(b"k"), "compute_md5: three": lambda: _hmac.compute_md5(b"k", b"m", 1), "compute_md5: by name": lambda: _hmac.compute_md5(key=b"k", msg=b"m"),
                 "update()": lambda: h.update(), "update: two": lambda: h.update(b"a", b"b"), "update: by name": lambda: h.copy().update(msg=b"a"), "update: unknown": lambda: h.update(data=b"a"), "digest: one": lambda: h.digest(1), "hexdigest: one": lambda: h.hexdigest(1), "hexdigest: by name": lambda: h.hexdigest(sep=":"), "copy: one": lambda: h.copy(1), "copy: by name": lambda: h.copy(x=1), "HMAC()": lambda: _hmac.HMAC(), "HMAC(key)": lambda: _hmac.HMAC(b"k", b"m", "md5"), "derived from": lambda: type("X", (_hmac.HMAC,), {}),
                 "of something else": lambda: _hmac.HMAC.update(5, b"a"), "__new__": lambda: _hmac.HMAC.__new__(_hmac.HMAC), "object.__new__": lambda: object.__new__(_hmac.HMAC)}.items():
    attempt(label, f)
print("===== the module and the class")
attempt("the module", lambda: (list(vars(_hmac))[-17:], _hmac.__name__, _hmac.__doc__, _hmac._GIL_MINSIZE))
attempt("UnknownHashError", lambda: (_hmac.UnknownHashError, _hmac.UnknownHashError.__mro__, _hmac.UnknownHashError.__module__, _hmac.UnknownHashError.__doc__))
k = _hmac.HMAC
attempt("HMAC", lambda: (repr(k), k.__module__, k.__qualname__, k.__mro__, k.__basicsize__ > 0, k.__dictoffset__, k.__weakrefoffset__, hex(k.__flags__), k.__text_signature__, k.__doc__, sorted(vars(k))))
attempt("set on it", lambda: setattr(k, "x", 1))
for n in sorted(vars(k)):
    v = vars(k)[n]
    attempt("   " + n, lambda: (type(v).__name__, getattr(v, "__doc__", None), getattr(v, "__text_signature__", None), getattr(v, "__qualname__", None)))
for n in sorted(vars(_hmac)):
    v = getattr(_hmac, n)
    if type(v).__name__ == "builtin_function_or_method": attempt(n, lambda: (v.__doc__, v.__text_signature__, v.__module__, repr(v)))
print("===== hmac is over it")
attempt("hmac.new", lambda: [(lambda x: (x.name, x.hexdigest()[:16]))(hmac.new(b"k", b"m", d)) for d in ("sha256", "sha3_256", "blake2b", hashlib.sha256)])
print("done")
