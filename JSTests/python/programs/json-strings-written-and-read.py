# _json, which json is quicker for, asked directly and by way of json.
import _json, json, json.decoder, json.encoder, json.scanner, decimal, enum, collections, math, sys
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
        notes = getattr(e, "__notes__", None)
        if notes: r += " | " + " | ".join(notes)
    print(label, "->", ascii(r))
print(json.decoder.c_scanstring is _json.scanstring, json.scanner.c_make_scanner is _json.make_scanner, json.encoder.c_make_encoder is _json.make_encoder,
      json.encoder.c_encode_basestring is _json.encode_basestring, json.encoder.c_encode_basestring_ascii is _json.encode_basestring_ascii)
print(sorted(n for n in vars(_json) if not n.startswith("__")), _json.__doc__)
for t in (_json.make_scanner, _json.make_encoder):
    print(t, t.__name__, t.__qualname__, t.__module__, t.__doc__, t.__mro__, sorted(n for n in vars(t)), t.__flags__ & (1 << 10), t.__basicsize__)
    attempt("derive", lambda: type("X", (t,), {}))
    attempt("no arguments", lambda: t())
    attempt("too many", lambda: t(*range(12)))
    attempt("unknown keyword", lambda: t(zzz=1))
print("===== strings written")
texts = ["", "a", "abc def", "\"", "\\", "/", "\b\f\n\r\t", "\x00\x01\x1f\x7f\x80\xff", "caf\xe9", "\u2028\u2029", "\u20ac", "\U0001f600", "a\U0001f600b", "\ud800", "\udc00", "x" * 50 + "\n" + "y" * 50, "\ud800x", "~}|", " "]
class S(str): pass
for t in texts:
    attempt(ascii(t)[:30], lambda: (_json.encode_basestring_ascii(t), _json.encode_basestring(t), _json.encode_basestring_ascii(t) == json.encoder.py_encode_basestring_ascii(t), _json.encode_basestring(t) == json.encoder.py_encode_basestring(t)))
for bad in (1, b"a", None, ["a"], S("q\n")):
    attempt("ascii of %r" % (bad,), lambda: (_json.encode_basestring_ascii(bad), type(_json.encode_basestring_ascii(bad)).__name__))
    attempt("plain of %r" % (bad,), lambda: _json.encode_basestring(bad))
attempt("no arguments", lambda: _json.encode_basestring())
attempt("two", lambda: _json.encode_basestring("a", "b"))
attempt("keyword", lambda: _json.encode_basestring(string="a"))
print("===== scanstring")
cases = ['"', 'abc"', 'abc', '', 'a\\nb"', 'a\\', 'a\\x"', 'a\\u00e9"', 'a\\u00e"', 'a\\u00', 'a\\uzzzz"', '\\ud83d\\ude00"', '\\ud83d\\ude00', '\\ud83d\\ude00"x', '\\ud83d"', '\\ud83dx"', '\\ud83d\\n"', '\\ud83d\\u0041"', '\\ud83d\\uzzzz"x', '\\ude00\\ud83d"',
         'a\nb"', 'a\tb"', 'a\x00b"', '\U0001f600"', '\U0001f600\\n\U0001f600"tail', 'caf\xe9"', '\u20ac\\"\u20ac"', '\\/\\b\\f\\r\\t\\\\\\""', '\\U0001f600"', '\\u+123"', '\\u 123"', '\\u0x12"', '\\u\uff11234"', '\\u12\xe94"']
for c in cases:
    for strict in (True, False):
        attempt("%s strict=%s" % (ascii(c), strict), lambda: _json.scanstring(c, 0, strict))
        attempt("   as Python has it", lambda: json.decoder.py_scanstring(c, 0, strict))
for end in (-1, 0, 1, 3, 4, 5, 6, 100, 2**70, -2**70, 1.0, "1", None, True):
    attempt("end %r" % (end,), lambda: _json.scanstring('ab"cd"', end))
    attempt("end %r, astral" % (end,), lambda: _json.scanstring('\U0001f600b"\U0001f600d"', end))
attempt("not a str", lambda: _json.scanstring(b'a"', 0))
attempt("a str subclass", lambda: _json.scanstring(S('a"'), 0))
attempt("one argument", lambda: _json.scanstring('a"'))
attempt("four", lambda: _json.scanstring('a"', 0, 1, 2))
attempt("keyword", lambda: _json.scanstring('a"', end=0))
attempt("strict is anything", lambda: (_json.scanstring('a\nb"', 0, []), _json.scanstring('a\nb"', 0, "")))
class Bad:
    def __bool__(self): raise ZeroDivisionError("bool")
attempt("strict that raises", lambda: _json.scanstring('a"', 0, Bad()))
attempt("both wrong", lambda: _json.scanstring(5, "x"))
