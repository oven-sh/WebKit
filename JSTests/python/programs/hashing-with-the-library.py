# What the library makes of the modules that hash: hashlib, hmac, and what goes by those. CPython has OpenSSL to go by for most of this and the engine by itself has not, so this is about what comes to the same either way.
import hashlib
import hmac
import io
import random
import uuid


def t(label, f):
    try:
        print(label, "=>", f())
    except Exception as e:
        print(label, "=>", type(e).__name__, e)


data = bytes((i * 37 + 11) & 0xFF for i in range(10000))
FIXED = sorted(hashlib.algorithms_guaranteed - {"shake_128", "shake_256"})

print("---- hashlib")
print(sorted(hashlib.algorithms_guaranteed), hashlib.algorithms_guaranteed <= hashlib.algorithms_available)
for name in FIXED:
    by_name = hashlib.new(name, data)
    by_function = getattr(hashlib, name)(data)
    print(name, by_name.hexdigest() == by_function.hexdigest(), by_name.name, by_name.digest_size, by_name.block_size, by_function.hexdigest())
for name in ("shake_128", "shake_256"):
    print(name, hashlib.new(name, data).hexdigest(20), getattr(hashlib, name)(data).hexdigest(20), hashlib.new(name).digest_size, hashlib.new(name).block_size)
t("in capitals", lambda: hashlib.new("SHA256", b"x").hexdigest()[:16])
t("not for security", lambda: [hashlib.new(name, b"x", usedforsecurity=False).hexdigest()[:8] for name in FIXED])
t("one that there is not", lambda: hashlib.new("sha1024", b"x"))
t("a str", lambda: hashlib.sha256("x"))
t("BLAKE2 by name, told things", lambda: hashlib.new("blake2b", b"x", digest_size=9, key=b"k").hexdigest())
t("and by function", lambda: hashlib.blake2s(b"x", digest_size=9, person=b"p").hexdigest())

print("---- of a file")
for name in ("md5", "sha256", "sha3_256", "blake2b"):
    t(name, lambda: hashlib.file_digest(io.BytesIO(data * 30), name).hexdigest() == hashlib.new(name, data * 30).hexdigest())
t("with something that makes one", lambda: hashlib.file_digest(io.BytesIO(data), lambda: hashlib.blake2s(digest_size=5)).hexdigest())
t("of this", lambda: [f := open(__file__, "rb"), hashlib.file_digest(f, "sha1").hexdigest() == hashlib.sha1(open(__file__, "rb").read()).hexdigest(), f.close()][1])
try:
    hashlib.file_digest(io.StringIO("x"), "md5")
except ValueError as e:
    print("of text =>", str(e).split(" object at ")[0], str(e).split(">'")[1])

print("---- hmac")
for name in FIXED:
    for key in (b"", b"key", data[:64], data[:65], data[:128], data[:129], data[:144], data[:145], data[:1000]):
        h = hmac.new(key, data[:300], name)
        pieces = hmac.new(key, digestmod=name)
        for i in range(0, 300, 7):
            pieces.update(data[i:min(i + 7, 300)])
        print(name, len(key), h.hexdigest()[:32], pieces.digest() == h.digest(), hmac.digest(key, data[:300], name) == h.digest(), h.copy().hexdigest() == h.hexdigest(), h.name, h.digest_size, h.block_size)
t("given what makes one", lambda: hmac.new(b"k", b"m", hashlib.sha256).hexdigest())
t("given a function of a module", lambda: hmac.new(b"k", b"m", lambda d=b"": hashlib.blake2b(d, digest_size=16)).hexdigest())
t("with none", lambda: hmac.new(b"k", b"m"))
t("with a str for a key", lambda: hmac.new("k", b"m", "md5"))
t("with a str to hash", lambda: hmac.new(b"k", "m", "md5"))
t("with one that there is not", lambda: type(hmac.new(b"k", b"m", "nothing")).__name__)
t("compare_digest", lambda: [hmac.compare_digest(a, b) for a, b in ((b"a", b"a"), (b"a", b"b"), (b"a", b"ab"), ("a", "a"), ("a", "b"), (bytearray(b"a"), b"a"))])
t("of a str and bytes", lambda: hmac.compare_digest("a", b"a"))
t("of what is not ASCII", lambda: hmac.compare_digest("é", "é"))
# RFC 4231, the second case
print(hmac.new(b"Jefe", b"what do ya want for nothing?", "sha256").hexdigest())
print(hmac.new(b"Jefe", b"what do ya want for nothing?", "sha512").hexdigest())

print("---- what goes by them")
print(uuid.uuid3(uuid.NAMESPACE_DNS, "python.org"), uuid.uuid5(uuid.NAMESPACE_DNS, "python.org"), uuid.uuid5(uuid.NAMESPACE_URL, "é"), uuid.uuid3(uuid.NAMESPACE_OID, b"bytes"))
for seed in ("a str", b"bytes", bytearray(b"a bytearray"), "", "é" * 100):
    r = random.Random(seed)
    print(repr(seed)[:20], [r.randrange(1000) for i in range(5)], r.random())
random.seed("again")
print(random.random(), random.getrandbits(70))
