# The codecs that are written in C, given what comes to hand.
import _codecs
import _warnings

_warnings._acquire_lock()
_warnings.filters.insert(0, ("ignore", None, Warning, None, 0))
_warnings._filters_mutated_lock_held()
_warnings._release_lock()
state = [521288629]


def random(n):
    v = state[0]
    v ^= (v << 13) & 0xFFFFFFFF
    v ^= v >> 17
    v ^= (v << 5) & 0xFFFFFFFF
    state[0] = v
    return v % n


def units(text):
    "A str as code units of 16 bits, so that half a surrogate pair beside the other half is the same thing as the pair."
    return _codecs.utf_16_le_encode(text, "surrogatepass")[0]


def add(digest, data):
    for b in data:
        digest = (digest * 31 + b) % 1000000007
    return digest


_codecs.register_error("test.mark", lambda e: ("<%d:%d>" % (e.start, e.end), e.end))
_codecs.register_error("test.skip", lambda e: ("", min(e.end + 1, len(e.object))))
_codecs.register_error("test.bytes", lambda e: (b"\x00\x00\x00\x00", e.end) if isinstance(e, UnicodeEncodeError) else ("?", e.end))
HANDLERS = ("strict", "ignore", "replace", "backslashreplace", "surrogateescape", "surrogatepass", "test.mark", "test.skip", "test.bytes")
# Bytes that are likely to be the beginning, the middle or the end of something.
POOLS = {
    "utf_8": bytes(range(0x7E, 0x82)) + bytes(range(0xBE, 0xC4)) + bytes(range(0xDF, 0xE2)) + b"\xec\xed\xee\xef\xf0\xf1\xf3\xf4\xf5\xf7\xf8\xff\x8f\x90\x9f\xa0a",
    "utf_16": b"\x00\x00\x00a\xd8\xd9\xdb\xdc\xdd\xdf\xfe\xff\x20\xac",
    "utf_32": b"\x00\x00\x00\x00\x00\x01\x10\x11a\xd8\xdc\xdf\xfe\xff",
    "utf_7": b"+-+-AAAGEIKw2D3e/09az!~\\ \x80",
    "escape": b"\\\\\\\\xuUN{}0123789afAFgz\n'\"tn \xe9",
}
DECODERS = (("utf_8", "utf_8"), ("utf_16", "utf_16"), ("utf_16_le", "utf_16"), ("utf_16_be", "utf_16"), ("utf_32", "utf_32"), ("utf_32_le", "utf_32"), ("utf_32_be", "utf_32"), ("utf_7", "utf_7"), ("unicode_escape", "escape"), ("raw_unicode_escape", "escape"), ("ascii", "utf_8"), ("latin_1", "utf_8"))
for name, pool in DECODERS:
    decode = getattr(_codecs, name + "_decode")
    pool = POOLS[pool]
    for errors in HANDLERS:
        digest = 0
        for i in range(1500):
            data = bytes(pool[random(len(pool))] for j in range(random(14)))
            for final in ((False, True) if name not in ("ascii", "latin_1") else ((),)):
                try:
                    text, used = decode(data, errors, *((final,) if final != () else ()))
                    digest = add(add(digest, units(text)), (used, 1))
                except UnicodeDecodeError as e:
                    digest = add(digest, (e.start, e.end, len(e.reason), 2))
                except (TypeError, IndexError) as e:
                    digest = add(digest, (len(str(e)) % 251, 3))
        print(name + "_decode", errors, digest)
CHARACTERS = ["a", "z", "+", "-", "\\", "~", "\x00", "\x7f", "\x80", "\xff", "Ā", "߿", "ࠀ", "€", "퟿", "\ud800", "\udbff", "", "﻿", "￿", "\U00010000", "\U0001F600", "\U0010FFFF", "\udc80", "\udcff", "\udc00"]
TABLE = _codecs.charmap_build("".join(chr(i) for i in range(128)) + "€￾Ā" + "￾" * 125)
for name in ("utf_7", "utf_8", "utf_16", "utf_16_le", "utf_16_be", "utf_32", "utf_32_le", "utf_32_be", "unicode_escape", "raw_unicode_escape", "ascii", "latin_1", "charmap"):
    encode = getattr(_codecs, name + "_encode")
    for errors in HANDLERS + ("xmlcharrefreplace", "namereplace"):
        digest = 0
        for i in range(1500):
            text = ""
            for j in range(random(9)):
                c = CHARACTERS[random(len(CHARACTERS))]
                # Not the first half of a pair and then the second.
                if not (text and "\ud800" <= text[-1] <= "\udbff" and "\udc00" <= c <= "\udfff"):
                    text += c
            try:
                data, used = encode(text, errors, *((TABLE,) if name == "charmap" else ()))
                digest = add(add(digest, data), (used, 1))
            except UnicodeEncodeError as e:
                digest = add(digest, (e.start, e.end, len(e.reason), 2))
            except (TypeError, IndexError) as e:
                digest = add(digest, (len(str(e)) % 251, 3))
        print(name + "_encode", errors, digest)
wrong = []
for name in ("utf_7", "utf_8", "utf_16", "utf_16_le", "utf_16_be", "utf_32", "utf_32_le", "utf_32_be", "unicode_escape", "raw_unicode_escape"):
    for i in range(2000):
        text = "".join(c for c in (CHARACTERS[random(len(CHARACTERS))] for j in range(random(12))) if not "\ud800" <= c <= "\udfff")
        data = getattr(_codecs, name + "_encode")(text)[0]
        if getattr(_codecs, name + "_decode")(data, "strict", True) != (text, len(data)):
            wrong.append((name, text))
print("there and back", wrong[:3])
