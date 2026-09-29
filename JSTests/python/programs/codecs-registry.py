# The registry of codecs: what is asked for one by name, and what is done with what it answers.
import _codecs
import _warnings
import sys

_warnings._acquire_lock()
_warnings.filters.insert(0, ("error", None, Warning, None, 0))
_warnings._filters_mutated_lock_held()
_warnings._release_lock()


def show(e):
    notes = getattr(e, "__notes__", None)
    return type(e).__name__ + ": " + str(e) + (" | notes %r" % (notes,) if notes else "")


def attempt(f, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return show(e)


def t(label, f):
    r = attempt(f)
    print(label, "=>", r if isinstance(r, str) else ascii(r))


asked = []
known = {}


def search(name):
    if name.startswith("test"):
        asked.append(name)
    return known.get(name)


t("register", lambda: (_codecs.register(search), attempt(_codecs.register, 5), attempt(_codecs.register, None), attempt(_codecs.register), attempt(_codecs.register, search, search), attempt(lambda: _codecs.register(search_function=search))))
up = lambda s, errors="strict": (s.upper().encode("ascii"), len(s))
down = lambda b, errors="strict": (bytes(b).decode("ascii").lower(), len(b))
known["test_up"] = (up, down, None, None)
t("lookup", lambda: (_codecs.lookup("test_up") is known["test_up"], asked))
t("what has been found is kept", lambda: (asked.clear(), _codecs.lookup("test_up") is known["test_up"], _codecs.lookup("TEST-UP") is known["test_up"], _codecs.lookup("Test Up") is known["test_up"], asked))
for name in ("test-A", "TEST_B", "test c", "test  d", "test--e", "test-_ f", "-test-g-", "  test h  ", "test.i", "test:j", "test\xe9k", "test€l", "test/m\\n", "Test_O", "test\to", "__test__p__", "test(q)"):
    t("what is asked for %r" % name, lambda: (asked.clear(), attempt(_codecs.lookup, name), asked))
for arg in (5, None, b"test_up", "test\0up", ""):
    t("lookup(%r)" % (arg,), lambda: _codecs.lookup(arg))
t("lookup()", lambda: (attempt(_codecs.lookup), attempt(_codecs.lookup, "a", "b")))
for label, value in (("a list", [up, down, None, None]), ("three", (up, down, None)), ("five", (up, down, None, None, None)), ("an int", 5), ("a str", "abcd"), ("()", ()), ("False", False), ("0", 0), ("derived from tuple", type("T", (tuple,), {})((up, down, None, None))), ("four of anything", (1, 2, 3, 4))):
    known["test_returns"] = value
    t("a search function that returns " + label, lambda: type(_codecs.lookup("test_returns")).__name__)
    _codecs.unregister(search)
    _codecs.register(search)


def raising(name):
    if name == "test_raises":
        raise KeyError("from the search function")


t("one that raises", lambda: (_codecs.register(raising), attempt(_codecs.lookup, "test_raises"), attempt("a".encode, "test_raises"), attempt(b"a".decode, "test_raises"), _codecs.unregister(raising)))
order = []
first = lambda name: order.append(("first", name)) if name.startswith("test") else None
second = lambda name: (order.append(("second", name)), known["test_up"])[1] if name == "test_order" else None
third = lambda name: order.append(("third", name)) if name.startswith("test") else None
t("in the order in which they were registered, until one answers", lambda: (_codecs.register(first), _codecs.register(second), _codecs.register(third), _codecs.lookup("test_order") is known["test_up"], order))
t("unregister", lambda: (order.clear(), _codecs.unregister(second), attempt(_codecs.lookup, "test_order"), order, _codecs.unregister(second), _codecs.unregister(first), _codecs.unregister(third), attempt(_codecs.unregister, 5), attempt(_codecs.unregister), _codecs.unregister(len)))
t("it forgets what had been found", lambda: (asked.clear(), _codecs.lookup("test_up") is known["test_up"], asked, asked.clear(), _codecs.register(len.__class__), _codecs.unregister(len.__class__), _codecs.lookup("test_up") is known["test_up"], asked))
t("registered twice", lambda: (order.clear(), _codecs.register(first), _codecs.register(first), attempt(_codecs.lookup, "test_twice"), order, _codecs.unregister(first), order.clear(), attempt(_codecs.lookup, "test_twice"), order, _codecs.unregister(first)))

print("---- encode and decode")
t("encode", lambda: (_codecs.encode("abc", "test_up"), _codecs.encode("abc", "test_up", "ignore"), _codecs.encode(obj="abc", encoding="test-up", errors="x"), _codecs.decode(b"ABC", "test_up"), _codecs.decode(obj=b"ABC", encoding="TEST UP")))
given = []
known["test_args"] = (lambda *a, **k: (given.append(("encode", a, k)), (b"", 0))[1], lambda *a, **k: (given.append(("decode", a, k)), ("", 0))[1], None, None)
t("what the codec is given", lambda: (_codecs.encode(5, "test_args"), _codecs.encode(5, "test_args", "e"), _codecs.decode(None, "test_args"), _codecs.decode([1], "test_args", "e"), given))
for args, kwargs in (((), {}), (("a", 5), {}), (("a", None), {}), (("a", "test_up", 5), {}), (("a", "test_up", None), {}), (("a", "test\0up"), {}), (("a", "test_up", "a\0b"), {}), (("a", "test_up", "e", 1), {}), (("a",), {"other": 1}), (("a", "test_none_such"), {})):
    t("encode(*%r, **%r)" % (args, kwargs), lambda: _codecs.encode(*args, **kwargs))
    t("decode of the same", lambda: _codecs.decode(*args, **kwargs))
for label, value in (("None", None), ("a list", [b"", 0]), ("one thing", (b"",)), ("three", (b"", 0, 0)), ("bytes", b"ab"), ("anything, and anything", (5, "x")), ("derived from tuple", type("T", (tuple,), {})((b"t", 1)))):
    known["test_result"] = (lambda *a: value, lambda *a: value, None, None)
    _codecs.unregister(search)
    _codecs.register(search)
    t("a codec that returns " + label, lambda: (attempt(_codecs.encode, "a", "test_result"), attempt(_codecs.decode, b"a", "test_result"), attempt("a".encode, "test_result"), attempt(b"a".decode, "test_result")))


def fails(*a):
    raise ValueError("from the codec")


known["test_fails"] = (fails, fails, None, None)
t("one that raises", lambda: (attempt(_codecs.encode, "a", "test_fails"), attempt(_codecs.decode, b"a", "Test-Fails"), attempt("a".encode, "test_fails"), attempt(b"a".decode, "test_fails"), attempt(bytearray(b"a").decode, "test_fails"), attempt(str, b"a", "test_fails"), attempt(bytes, "a", "test_fails")))
known["test_not_callable"] = (5, None, None, None)
t("one that cannot be called", lambda: (attempt(_codecs.encode, "a", "test_not_callable"), attempt(_codecs.decode, b"a", "test_not_callable")))

print("---- str.encode() and bytes.decode()")
t("by way of the registry", lambda: ("abc".encode("test_up"), "abc".encode("Test-Up", "ignore"), "abc".encode(encoding="test up"), b"ABC".decode("test_up"), bytearray(b"ABC").decode("test_up"), str(b"ABC", "test_up"), str(memoryview(b"ABC"), "test_up"), bytes("abc", "test_up"), bytearray("abc", "test_up")))
given.clear()
t("what the codec is given", lambda: ("xy".encode("test_args"), "xy".encode("test_args", "e"), b"xy".decode("test_args"), b"xy".decode("test_args", "e"), [(k, tuple((type(x).__name__, bytes(x) if isinstance(x, memoryview) else x, getattr(x, "readonly", None)) for x in a), kw) for k, a, kw in given]))
t("of nothing", lambda: (given.clear(), "".encode("test_args"), b"".decode("test_args"), b"".decode("test_none_such"), attempt("".encode, "test_none_such"), [g[0] for g in given]))
for label, value in (("a str", "s"), ("bytes", b"b"), ("a bytearray", bytearray(b"ba")), ("None", None), ("an int", 5), ("derived from bytes", type("B", (bytes,), {})(b"db")), ("derived from str", type("S", (str,), {})("ds")), ("derived from bytearray", type("BA", (bytearray,), {})(b"dba"))):
    known["test_kind"] = (lambda *a: (value, 0), lambda *a: (value, 0), None, None)
    _codecs.unregister(search)
    _codecs.register(search)
    t("a codec that gives " + label, lambda: [(r if isinstance(r, str) and ":" in r else (type(r).__name__, r)) for r in (attempt("a".encode, "test_kind"), attempt(b"a".decode, "test_kind"), attempt(_codecs.encode, "a", "test_kind"), attempt(_codecs.decode, b"a", "test_kind"))])


class Info(tuple):
    pass


for label, flag in (("says that it is of text", True), ("says that it is not", False), ("says 0", 0), ("says 1", 1), ("says None", None), ("says ''", ""), ("does not say", ...)):
    info = Info((up, down, None, None))
    if flag is not ...:
        info._is_text_encoding = flag
    known["test_text"] = info
    _codecs.unregister(search)
    _codecs.register(search)
    t("one that " + label, lambda: (attempt("a".encode, "test_text"), attempt(b"A".decode, "Test-Text"), attempt(_codecs.encode, "a", "test_text"), attempt(_codecs.decode, b"A", "test_text"), attempt(str, b"A", "test_text"), attempt(bytes, "a", "test_text")))


class Odd(tuple):
    @property
    def _is_text_encoding(self): raise KeyError("from the property")


known["test_odd"] = Odd((up, down, None, None))
t("one that raises when it is asked", lambda: (attempt("a".encode, "test_odd"), attempt(_codecs.encode, "a", "test_odd")))


class Truth:
    def __bool__(self): raise KeyError("from __bool__")


info = Info((up, down, None, None))
info._is_text_encoding = Truth()
known["test_truth"] = info
t("one whose answer cannot be told", lambda: attempt("a".encode, "test_truth"))

print("---- the names that are known without asking")
for name in ("utf-8", "UTF-8", "utf8", "utf_8", "UTF8", "utf 8", "utf--8", "-utf-8-", "utf-16", "utf16", "UTF_16", "utf-32", "utf32", "ascii", "ASCII", "us-ascii", "US_ASCII", "latin-1", "latin1", "LATIN_1", "iso-8859-1", "iso8859-1", "ISO_8859_1", "iso 8859 1"):
    t(repr(name), lambda: (asked.clear(), "a\xe9".encode(name, "replace"), b"a\xe9\x00\x00".decode(name, "replace"), asked))
t("the errors of one that is not asked for", lambda: ("abc".encode("utf-8", "no such"), b"abc".decode("ascii", "no such"), attempt("\ud800".encode, "utf-8", "no such"), attempt(b"\xff".decode, "ascii", "no such")))
_codecs.unregister(search)
