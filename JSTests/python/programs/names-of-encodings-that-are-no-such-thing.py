# The name of an encoding, and of a way of dealing with errors, that is not what such names are made of.
import codecs, io
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, ascii(str(e)))
    print(label, "->", ascii(r))
names = ["utf-8", "UTF_8", "utf 8", "utf-8\udce2\udc80\udc9d", "\udcfe", "utf-8\xe9", "utf-8€", "\xe9", "", " ", "utf-8 ", " utf-8", "utf--8", "utf.8", "u\x00tf", "utf-8\x00", "latin¹1", "ascii１", "UTF-8\n", "utf8", "U8", "l1", "ｕtf-8", "a" * 300, "utıf"]
for n in names:
    print("=====", ascii(n)[:40])
    attempt("lookup", lambda: codecs.lookup(n).name)
    attempt("decode", lambda: b"abc".decode(n))
    attempt("encode", lambda: "abc".encode(n))
    attempt("str()", lambda: str(b"abc", n))
    attempt("bytes()", lambda: bytes("abc", n))
    attempt("bytearray()", lambda: bytearray("abc", n))
    attempt("codecs.decode", lambda: codecs.decode(b"abc", n))
    attempt("codecs.encode", lambda: codecs.encode("abc", n))
    attempt("TextIOWrapper", lambda: io.TextIOWrapper(io.BytesIO(), encoding=n).encoding)
    attempt("reconfigure", lambda: (lambda t: (t.reconfigure(encoding=n), t.encoding)[1])(io.TextIOWrapper(io.BytesIO(), encoding="ascii")))
    attempt("open", lambda: open(__file__, encoding=n).close())
    attempt("getincrementaldecoder", lambda: codecs.getincrementaldecoder(n).__name__)
    attempt("errors: decode", lambda: b"\xff".decode("ascii", n))
    attempt("errors: encode", lambda: "\xff".encode("ascii", n))
    attempt("errors: decode, none wrong", lambda: b"a".decode("ascii", n))
    attempt("errors: lookup_error", lambda: codecs.lookup_error(n).__name__)
    attempt("errors: TextIOWrapper", lambda: io.TextIOWrapper(io.BytesIO(), encoding="ascii", errors=n).errors)
    attempt("errors: reconfigure", lambda: (lambda t: (t.reconfigure(errors=n), t.errors)[1])(io.TextIOWrapper(io.BytesIO(), encoding="ascii")))
    attempt("newline", lambda: io.TextIOWrapper(io.BytesIO(), encoding="ascii", newline=n).newlines)
    attempt("compile", lambda: type(compile(("# coding: " + n + "\n").encode("utf-8", "surrogatepass"), "<t>", "exec")).__name__)
