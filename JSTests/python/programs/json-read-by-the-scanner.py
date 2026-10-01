# The scanner and the encoder of _json, asked directly and by way of json.
import _json, json, json.decoder, json.encoder, json.scanner, decimal, enum, collections, math, sys, fractions, types
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
        notes = getattr(e, "__notes__", None)
        if notes: r += " | " + " | ".join(notes)
    print(label, "->", ascii(r))
class S(str): pass
print("===== the scanner")
d = json.JSONDecoder()
sc = _json.make_scanner(d)
print(type(sc).__name__, sc.strict, sc.object_hook, sc.object_pairs_hook, sc.parse_float, sc.parse_int, sc.parse_constant == d.parse_constant)
attempt("set a member", lambda: setattr(sc, "strict", False))
attempt("delete a member", lambda: delattr(sc, "strict"))
attempt("another attribute", lambda: setattr(sc, "zzz", 1))
attempt("a context with nothing", lambda: _json.make_scanner(object()))
for missing in ("strict", "object_hook", "object_pairs_hook", "parse_float", "parse_int", "parse_constant"):
    ns = types.SimpleNamespace(strict=1, object_hook=None, object_pairs_hook=None, parse_float=float, parse_int=int, parse_constant=None)
    delattr(ns, missing)
    attempt("without " + missing, lambda: _json.make_scanner(ns))
attempt("by keyword", lambda: type(_json.make_scanner(context=d)).__name__)
attempt("call: none", lambda: sc())
attempt("call: one", lambda: sc("1"))
attempt("call: three", lambda: sc("1", 0, 0))
attempt("call: keywords", lambda: sc(string="12", idx=1))
attempt("call: unknown keyword", lambda: sc("12", index=1))
attempt("call: bytes", lambda: sc(b"1", 0))
attempt("call: subclass", lambda: sc(S("[1]"), 0))
for idx in (-1, 0, 1, 2, 3, 100, 2**70, 1.5, "0", None, True):
    attempt("idx %r" % (idx,), lambda: sc("12 ", idx))
documents = ["1", "-1", "0", "-0", "01", "-", "--1", "+1", "1.", "1.5", ".5", "1e5", "1E5", "1e+5", "1e-5", "1e", "1e+", "1.5e", "1.5e3x", "-0.0", "1e400", "-1e400", "1e-400", "123456789012345678", "1234567890123456789", "-1234567890123456789", "9223372036854775807", "9223372036854775808",
             "-9223372036854775808", "1" * 40, "0.1", "1.7976931348623157e308", "5e-324", "2.5e-324", "123456789.123456789", "null", "nul", "nulll", "true", "tru", "false", "fals", "NaN", "Na", "Infinity", "Infinit", "-Infinity", "-Infinit", "-I", "nan", "None", "True",
             '""', '"a"', '"', "[]", "[ ]", "[1]", "[1,2]", "[1, 2 ]", "[1,]", "[,1]", "[1 2]", "[", "[1", "[1,", "[1, ", "[[", "[]]", "{}", "{ }", '{"a":1}', '{"a" : 1 , "b":2}', '{"a":1,}', '{,}', '{"a"}', '{"a":}', '{"a":1', '{"a":1 ', '{"a":1,', '{"a"', '{"a":', "{", "{1:2}", "{'a':1}", '{"a":1 "b":2}',
             '{"a":1,"a":2}', '[{"a":[{"b":[]}]}]', " 1", "1 ", "\t1", "\n[\n1\n,\n2\n]\n", "\x0b1", "\xa01", "[\xa0]", "[1,\u20282]", "", " ", "x", "\ufeff1", '"\U0001f600"', '["\U0001f600", 1]', '["\U0001f600", x]', '{"\U0001f600": "\U0001f600", "k": x}', '["caf\xe9", x]', '["\u20ac", x]',
             "[1e5, 1E5, -1.5e-3]", "\uff11", "[\uff11]", "1\uff11", "1.\uff11", "1e\uff11", "-\uff11", "/", "//", "/* */1", "[1] # c", "1_000", "0x10", "0b1", "1j", "'a'", "[Infinity, -Infinity, NaN]", "[-]", "[-,", "-x", "[0", "0]", "00", "-00", "0.", "0.e1", "0e0", "0e", "-0e-0"]
for doc in documents:
    attempt("scan " + ascii(doc)[:44], lambda: sc(doc, 0))
    attempt("   loads", lambda: json.loads(doc))
print("===== hooks")
def decoder(**k): return json.JSONDecoder(**k)
doc = '{"a": 1, "b": [1.5, {"c": NaN, "d": -Infinity}], "a": 2, "e": {}}'
attempt("object_hook", lambda: decoder(object_hook=lambda o: sorted(o.items(), key=str)).decode(doc))
attempt("object_pairs_hook", lambda: decoder(object_pairs_hook=lambda p: p).decode(doc))
attempt("both", lambda: decoder(object_hook=lambda o: "hook", object_pairs_hook=lambda p: ("pairs", len(p))).decode(doc))
attempt("OrderedDict", lambda: decoder(object_pairs_hook=collections.OrderedDict).decode(doc))
attempt("parse_float", lambda: decoder(parse_float=decimal.Decimal).decode("[1.10, 1e5, 1, -0.0]"))
attempt("parse_float gets", lambda: decoder(parse_float=lambda s: (type(s).__name__, s)).decode("[1.10, 1E+5, -0.0e0]"))
attempt("parse_int gets", lambda: decoder(parse_int=lambda s: (type(s).__name__, s)).decode("[1, -0, 123456789012345678901234567890, 1.5]"))
attempt("parse_int is float", lambda: decoder(parse_int=float).decode("[1, 2.5]"))
attempt("parse_float is int", lambda: decoder(parse_float=int).decode("[1.5]"))
attempt("parse_constant gets", lambda: decoder(parse_constant=lambda s: (type(s).__name__, s)).decode("[NaN, Infinity, -Infinity, null, true]"))
attempt("parse_constant raises", lambda: decoder(parse_constant=lambda s: 1 / 0).decode("[NaN]"))
attempt("a hook that raises", lambda: decoder(object_hook=lambda o: 1 / 0).decode(doc))
attempt("a hook that is not callable", lambda: decoder(object_hook=5).decode(doc))
attempt("a subclass of int for parse_int", lambda: [type(x).__name__ for x in decoder(parse_int=type("I", (int,), {})).decode("[1]")])
attempt("strict", lambda: decoder(strict=True).decode('"a\nb"'))
attempt("not strict", lambda: decoder(strict=False).decode('["a\nb", {"c\td": "\\u0000\x00"}]'))
attempt("the same key is the one str", lambda: (lambda r: [a is b for a, b in zip(r[0], r[1])])(json.loads('[{"key one": 1, "key two": 2}, {"key one": 3, "key two": 4}]')))
attempt("values are not", lambda: (lambda r: r[0] is r[1])(json.loads('["value one", "value one"]')))
attempt("raw_decode", lambda: d.raw_decode('{"a": 1} tail'))
attempt("raw_decode from", lambda: d.raw_decode('xx[1, 2]yy', 2))
attempt("raw_decode astral", lambda: d.raw_decode('"\U0001f600\U0001f600" tail'))
attempt("raw_decode astral from", lambda: d.raw_decode('\U0001f600[1, "\U0001f600"]\U0001f600', 1))
attempt("extra data", lambda: json.loads('[1] [2]'))
attempt("extra data, astral", lambda: json.loads('["\U0001f600"] x'))
def position(doc):
    try: json.loads(doc)
    except json.JSONDecodeError as e: return e.msg, e.pos, e.lineno, e.colno, e.doc is doc
attempt("where", lambda: position('[1,\n 2,\n  "\U0001f600", x]'))
attempt("where, wide", lambda: position('[1,\n 2,\n  "\u20ac", x]'))
attempt("digits", lambda: len(str(json.loads("1" * 4300))))
attempt("too many digits", lambda: json.loads("1" * 4301))
attempt("too many digits, negative", lambda: json.loads("[-" + "1" * 4301 + "]"))
attempt("many digits in a float", lambda: json.loads("1" * 5000 + ".5"))
attempt("many digits after the point", lambda: json.loads("0." + "1" * 5000))
attempt("deep", lambda: (lambda r: 1)(json.loads("[" * 500 + "]" * 500)))
def too_deep(text):
    try: json.loads(text)
    except RecursionError as e: return str(e)[str(e).find("while"):]
attempt("too deep, arrays", lambda: too_deep("[" * 500000))
attempt("too deep, objects", lambda: too_deep('{"a":' * 500000))
attempt("bytes", lambda: json.loads(b'[1, "\xc3\xa9"]'))
attempt("utf-16", lambda: json.loads('[1, "\xe9"]'.encode("utf-16")))
attempt("types", lambda: [type(x).__name__ for x in json.loads('[1, 1.0, "a", null, true, [], {}, 1e2, 12345678901234567890]')])
