# x * n, where x is a bytes, a bytearray or a str. What has been copied so far is copied again until there is enough, so what matters is how many times over, and how much is left at the end.
import hashlib

COUNTS = list(range(0, 40)) + [63, 64, 65, 127, 128, 129, 1000, 1023, 1024, 1025]
wrong = []
tried = 0
for size in range(0, 12):
    piece = bytes(range(65, 65 + size))
    for count in COUNTS:
        expected = b"".join([piece] * count)
        in_place = bytearray(piece)
        in_place *= count
        for label, got in (("bytes", piece * count), ("the other way about", count * piece), ("bytearray", bytes(bytearray(piece) * count)), ("in place", bytes(in_place)), ("a view", bytes(memoryview(piece)) * count)):
            tried += 1
            if got != expected:
                wrong.append((label, size, count))
for alphabet in ("abcdefghijkl", "\xe9\xe8\xe7abcdefghi", "€Жabcdefghij", "\U0001f600a\U0001f601bcdefghij", "\ud800abcdefghijk"):
    for size in range(0, 12):
        piece = alphabet[:size]
        for count in COUNTS:
            tried += 1
            got = piece * count
            if got != "".join([piece] * count) or len(got) != size * count or count * piece != got:
                wrong.append((ascii(piece), count))
print(tried, wrong)

print(b"ab" * 3, bytearray(b"ab") * 3, "ab" * 3, b"a" * 5, "€" * 3, b"ab" * -1, "ab" * 0, bytearray(b"ab") * True, type(b"ab" * 1), (b"ab" * 1) is not None)
for piece, count in ((b"a", 5_000_003), (b"abc", 1_000_001), (b"0123456789abcdef", 300_007), (bytes(range(256)), 20_011)):
    print(len(piece), count, hashlib.sha256(piece * count).hexdigest()[:16], hashlib.sha256(bytearray(piece) * count).hexdigest()[:16])
for piece, count in (("a", 5_000_003), ("abc", 1_000_001), ("€b", 700_001), ("\U0001f600", 300_007)):
    print(ascii(piece), count, hashlib.sha256((piece * count).encode()).hexdigest()[:16])

# What is repeated is itself what is written to.
b = bytearray(b"abc")
b *= 4
b *= 2
print(b, len(b))
b *= 1
print(b)
b *= 0
print(b, b * 5)
