# The module _csv, and csv, which is written over it.
import _csv
import binascii
import copy
import csv
import io
import itertools
import pickle
import random


def attempt(f, /, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)


def t(label, f):
    r = attempt(f)
    print(label, "=>", r if isinstance(r, str) else ascii(r))


def crc(x):
    return binascii.crc32(ascii(x).encode())


print("---- what there is")
names = sorted(n for n in vars(_csv) if not n.startswith("__"))
t("the module", lambda: (_csv.__name__, _csv.__package__, _csv.__loader__.__name__, _csv.__doc__, names, sorted(n for n in vars(_csv) if n.startswith("__"))))
for name in names:
    x = getattr(_csv, name)
    t(name, lambda: x if isinstance(x, int) else sorted(x) if isinstance(x, dict) else (type(x).__name__, x.__text_signature__, x.__doc__, x.__module__, x.__name__, x.__qualname__))
for T in (_csv.Dialect, _csv.Reader, _csv.Writer, _csv.Error):
    t(T.__name__, lambda: (repr(T), [b.__name__ for b in T.__mro__], [(n, type(v).__name__, getattr(v, "__doc__", None) if n != "__doc__" else None) for n, v in sorted(vars(T).items())], T.__basicsize__, T.__flags__ & 0xFFFFF, attempt(setattr, T, "x", 1), attempt(lambda: type("S", (T,), {}).__name__), attempt(T) if T is not _csv.Dialect and T is not _csv.Error else None, attempt(T.__new__, T) if T is not _csv.Dialect and T is not _csv.Error else None))
t("csv takes them", lambda: [n for n in names if not n.startswith("_") and n not in ("Dialect", "Reader", "Writer") and getattr(csv, n, None) is not getattr(_csv, n)])

print("---- Dialect")
D = _csv.Dialect


def shown(d):
    return (d.delimiter, d.doublequote, d.escapechar, d.lineterminator, d.quotechar, d.quoting, d.skipinitialspace, d.strict) if isinstance(d, D) else d


t("as it comes", lambda: shown(D()))
VALUES = (None, ",", ";", " ", "\t", "\n", "\r", "", "ab", "\xe9", "€", "\U0001F600", "\ud800", "\0", '"', "'", "\\", b",", 5, 0, 1, True, False, 1.5, [], (), type("S", (str,), {})("|"), type("I", (int,), {})(1), 2 ** 70, -1, 6, 3)
for key in ("delimiter", "doublequote", "escapechar", "lineterminator", "quotechar", "quoting", "skipinitialspace", "strict"):
    t(key, lambda: [shown(attempt(D, **{key: v})) for v in VALUES])
t("that clash", lambda: [shown(attempt(D, **k)) for k in ({"delimiter": "x", "quotechar": "x"}, {"delimiter": "x", "escapechar": "x"}, {"quotechar": "x", "escapechar": "x"}, {"delimiter": "x", "lineterminator": "axb"}, {"quotechar": "x", "lineterminator": "x"}, {"escapechar": "x", "lineterminator": "x"}, {"delimiter": " ", "skipinitialspace": True}, {"quotechar": " ", "skipinitialspace": True},
                                                        {"escapechar": " ", "skipinitialspace": True}, {"quotechar": " "}, {"escapechar": " "}, {"quotechar": None}, {"quotechar": None, "quoting": 0}, {"quotechar": None, "quoting": 3}, {"quotechar": None, "quoting": 1}, {"quoting": 3}, {"quotechar": None, "escapechar": None}, {"lineterminator": ""}, {"lineterminator": "abc"})])
t("how it is called", lambda: [shown(attempt(D, *a, **k)) for a, k in (((1,) * 10, {}), ((), {"x": 1}), ((), {"delimitr": ","}), ((None,), {"dialect": None}), ((), {"strict": 1, "x": 2}), ((None,) * 9, {"x": 1}), ((), dict.fromkeys("ABCDEGHIJK")), ((None, ";"), {}), ((None, ";", 0, "\\", "\n", "'", 1, 1, 1), {}), ((None,), {}), ((5,), {}), (("excel",), {}), (("nope",), {}), ((object(),), {}))])


class Like:
    delimiter = ";"
    quotechar = "'"
    strict = True


class Raises:
    def __getattr__(self, name): raise KeyError(name)


class Bad:
    delimiter = 5


t("from what has what one has", lambda: [shown(attempt(D, *a, **k)) for a, k in (((Like,), {}), ((Like(),), {}), ((Like,), {"delimiter": ","}), ((Raises(),), {}), ((Bad,), {}), ((Bad,), {"delimiter": ","}), ((csv.excel,), {}), ((csv.excel_tab,), {}), ((csv.unix_dialect,), {}), ((csv.excel(),), {}), ((type("E", (), {}),), {}))])
t("of one", lambda: [(D(d) is d, D(dialect=d) is d, D(d, strict=False) is d, shown(D(d, delimiter="|")), type("S", (D,), {})(d) is d, D(type("S", (D,), {})(delimiter="!")).delimiter) for d in [D(delimiter=";")]])
t("what it has", lambda: [(attempt(setattr, d, n, 1)) for d in [D()] for n in ("delimiter", "strict", "quoting", "x")] + [attempt(delattr, D(), "delimiter"), attempt(getattr, D(), "__dict__"), attempt(hash, D()) != 0, D() == D(), repr(D()).split(" at ")[0]])
t("which is not pickled", lambda: [attempt(f, D()) for f in (pickle.dumps, copy.copy, copy.deepcopy, lambda d: d.__reduce__(), lambda d: d.__reduce_ex__(2), lambda d: d.__reduce__(1, 2, 3), lambda d: d.__reduce_ex__())] + [attempt(pickle.dumps, D(), p) for p in range(pickle.HIGHEST_PROTOCOL + 1)] + [attempt(copy.copy, type("Sub", (D,), {})())])

print("---- the dialects that have names")
t("as they come", lambda: (sorted(csv.list_dialects()), sorted(_csv._dialects), [shown(csv.get_dialect(n)) for n in sorted(csv.list_dialects())], type(csv.list_dialects()).__name__))
t("registered", lambda: (csv.register_dialect("mine", delimiter="|"), shown(csv.get_dialect("mine")), "mine" in csv.list_dialects(), csv.get_dialect("mine") is csv.get_dialect("mine"), list(csv.reader(["a|b"], "mine")), list(csv.reader(["a|b;c"], "mine", delimiter=";")), csv.register_dialect("mine", Like), shown(csv.get_dialect("mine")), csv.unregister_dialect("mine"), "mine" in csv.list_dialects(), attempt(csv.get_dialect, "mine"), attempt(csv.unregister_dialect, "mine")))
t("what goes wrong", lambda: [attempt(f, *a, **k) for f, a, k in ((csv.register_dialect, (), {}), (csv.register_dialect, (5,), {}), (csv.register_dialect, (None,), {}), (csv.register_dialect, ("a", None, None), {}), (csv.register_dialect, ("a",), {"delimiter": "ab"}), (csv.register_dialect, ("a", Bad), {}), (csv.register_dialect, ("a",), {"x": 1}), (csv.register_dialect, ("a", "nope"), {}), (csv.register_dialect, (), {"name": "a"}),
                                                                   (csv.get_dialect, (), {}), (csv.get_dialect, (5,), {}), (csv.get_dialect, ([],), {}), (csv.get_dialect, ("a", "b"), {}), (csv.get_dialect, (None,), {}), (csv.unregister_dialect, (), {}), (csv.unregister_dialect, (5,), {}), (csv.unregister_dialect, ([],), {}), (csv.list_dialects, (1,), {}), (csv.list_dialects, (), {"x": 1}))] + [shown(csv.get_dialect(name="excel")), "a" in csv.list_dialects()])
t("a name of a class derived from str", lambda: (csv.register_dialect(type("S", (str,), {})("sub")), "sub" in csv.list_dialects(), shown(D("sub")), csv.unregister_dialect("sub")))

print("---- field_size_limit")
t("field_size_limit", lambda: (csv.field_size_limit(), csv.field_size_limit(10), csv.field_size_limit(), list(csv.reader(["a" * 10])), attempt(list, csv.reader(["a" * 11])), attempt(list, csv.reader(['"' + "a" * 11 + '"'])), csv.field_size_limit(0), attempt(list, csv.reader(["a"])), list(csv.reader([",,"])), csv.field_size_limit(-1), attempt(list, csv.reader(["a"])), csv.field_size_limit(2 ** 63 - 1), csv.field_size_limit(new_limit=131072), csv.field_size_limit()))
t("what it is given", lambda: [attempt(csv.field_size_limit, *a, **k) for a, k in (((None,), {}), (("a",), {}), ((1.5,), {}), ((True,), {}), ((2 ** 63,), {}), ((-2 ** 63 - 1,), {}), ((1, 2), {}), ((), {"x": 1}), ((type("I", (int,), {})(5),), {}))] + [csv.field_size_limit()])

print("---- reader")
R = csv.reader
t("what it is", lambda: [(type(r).__name__, iter(r) is r, r.line_num, shown(r.dialect), next(r), r.line_num, next(r), r.line_num, attempt(next, r), r.line_num, attempt(next, r), attempt(setattr, r, "line_num", 5), attempt(setattr, r, "dialect", None), attempt(setattr, r, "x", 1), repr(r).split(" at ")[0], attempt(len, r), attempt(pickle.dumps, r), attempt(copy.copy, r)) for r in [R(["a,b", "c"])]])
t("how it is made", lambda: [attempt(lambda: list(R(*a, **k))) for a, k in (((), {}), ((5,), {}), ((None,), {}), (([], None, None), {}), (([],), {"x": 1}), (([],), {"delimitr": ","}), (([], "nope"), {}), (([], 5), {}), (([],), {"delimiter": "ab"}), ((), {"iterable": []}), ((["a;b"], Like), {}), ((["a;b"],), {"dialect": Like}), ((["a;b"], "excel"), {"dialect": Like}), (("ab",), {}), ((iter(["a"]),), {}), ((io.StringIO("a,b\r\nc\r\n"),), {}), (({"a,b": 1},), {}), (((x for x in ("a", "b")),), {}))])
t("what it is given for lines", lambda: [attempt(lambda: list(R(l))) for l in ([b"a"], [5], [None], [["a"]], ["a", b"b"], [type("S", (str,), {})("a,b")], [""], ["", ""], ["\n"], ["\r\n"], ["\r"], ["\n\n"], ["a\n\n"], ["a\nb"], ["a\rb"], ["a\r\nb"], ["a\n", "b"], ["a\r\n\r\n"], ['"a\nb"'], ['"a', 'b"'], ['"a\n', 'b"'], ['"a'], ['"'], ['""'], ['"""'], ['""""'], ["\0"], ["a\0b"], ["\ud800"], ["\U0001F600,\xe9"])])


class Lines:
    def __init__(self, *lines): self.lines = list(lines)
    def __iter__(self): return self
    def __next__(self):
        if not self.lines:
            raise StopIteration
        line = self.lines.pop(0)
        if isinstance(line, BaseException):
            raise line
        return line() if callable(line) else line


t("what gives the lines raises", lambda: [(attempt(next, r), attempt(next, r), attempt(next, r), r.line_num) for r in [R(Lines("a", KeyError("k"), "b"))]] + [(attempt(next, r), attempt(next, r), r.line_num) for r in [R(Lines('"a', KeyError("k"), 'b"'))]])
t("what gives the lines asks for a record", lambda: [(attempt(next, r), attempt(next, r), attempt(next, r), r.line_num) for box in [[]] for r in [R(Lines("a", lambda: (box.append(attempt(next, box[0])), "b")[1], "c", "d"))] for _ in [box.append(r)]])
CASES = ("a,b,c", "a, b", " a,b ", ",", ",,", "a,", ",a", '"a"', '"a",b', 'a,"b"', '"a,b"', '"a""b"', '"a"b', '"a"b,c', 'a"b', 'a"b"c', '"a" ,b', '"a", "b"', ' "a"', '"a\\"b"', "a\\,b", "a\\", "\\", "\\\\", '"a\\', "a\\\nb", "a\\\rb", "a\\\r\nb", '"a\\\nb"', "1,2.5,x", '1,"2",', "1,,3", '"",', "'a,b'", "'a''b'", "a;b", "a\tb", "a|b", "  ", " , ", "a b", '"a b" c', "a  b", "1e5,inf,nan,-0", " 1", "1 ", "0x1", "1_0", "١")
DIALECTS = ({}, {"delimiter": ";"}, {"delimiter": "\t"}, {"delimiter": " "}, {"delimiter": " ", "skipinitialspace": True}, {"skipinitialspace": True}, {"quotechar": "'"}, {"quotechar": None}, {"escapechar": "\\"}, {"escapechar": "\\", "quoting": 3}, {"escapechar": "\\", "doublequote": False}, {"doublequote": False}, {"strict": True}, {"strict": True, "escapechar": "\\"}, {"strict": True, "doublequote": False},
            {"quoting": 1}, {"quoting": 2}, {"quoting": 3}, {"quoting": 4}, {"quoting": 5}, {"quoting": 2, "strict": True}, {"quoting": 4, "skipinitialspace": True}, {"lineterminator": "x"}, {"delimiter": "\U0001F600"}, {"quotechar": "\xe9", "escapechar": "€"})
for k in DIALECTS:
    t("read with %r" % (k,), lambda: [attempt(lambda: list(R([c], **k))) for c in CASES])

rng = random.Random(20260930)
ALPHABET = ['a', 'b', '1', '.', ',', ',', ';', '"', '"', "'", '\\', ' ', ' ', '\t', '\n', '\r', '\xe9', '\U0001F600', '|', '-']
inputs = [["".join(rng.choice(ALPHABET) for _ in range(rng.randrange(0, 14))) for _ in range(rng.randrange(1, 4))] for _ in range(1500)]
for k in DIALECTS:
    results = [attempt(lambda: list(R(lines, **k))) for lines in inputs]
    t("at random, read with %r" % (k,), lambda: (crc(results), sum(isinstance(r, str) for r in results), sorted({r[:60] for r in results if isinstance(r, str)})[:8]))

print("---- writer")


class Sink:
    def __init__(self): self.written = []
    def write(self, s):
        self.written.append(s)
        return len(self.written)


def written(rows, **k):
    s = Sink()
    w = csv.writer(s, **k)
    out = [attempt(w.writerow, r) for r in rows]
    return s.written, out


t("what it is", lambda: [(type(w).__name__, shown(w.dialect), w.writerow(["a", 1]), w.writerows([["b"], ["c", None]]), s.written, attempt(setattr, w, "dialect", None), attempt(setattr, w, "x", 1), repr(w).split(" at ")[0], attempt(iter, w), attempt(pickle.dumps, w), attempt(copy.copy, w)) for s in [Sink()] for w in [csv.writer(s)]])
t("how it is made", lambda: [attempt(lambda: type(csv.writer(*a, **k)).__name__) for a, k in (((), {}), ((5,), {}), ((None,), {}), ((Sink(), None, None), {}), ((Sink(),), {"x": 1}), ((Sink(), "nope"), {}), ((Sink(),), {"delimiter": "ab"}), ((), {"fileobj": Sink()}), ((type("W", (), {"write": 5})(),), {}), ((type("W", (), {"write": None})(),), {}), ((type("W", (), {"write": property(lambda s: 1 / 0)})(),), {}), ((type("W", (), {"write": len})(),), {}), ((io.StringIO(),), {}), ((Sink(), Like), {}), ((Sink(),), {"dialect": "unix"}))])
t("what a row is", lambda: [written([r]) for r in ([], (), "", "ab", ["a"], ("a", "b"), iter(["a"]), {"a": 1, "b": 2}, {"a"}, range(3), (x for x in "ab"), 5, None, 1.5, object, [[]], [["a"]], [()], [{}], b"ab", [b"ab"], Lines("a", "b"), Lines("a", KeyError("k")), type("I", (), {"__iter__": lambda s: 1 / 0})(), type("I", (), {"__iter__": lambda s: 5})(), type("G", (), {"__getitem__": lambda s, i: "ab"[i]})())])
t("how they are called", lambda: [attempt(f, *a, **k) for w in [csv.writer(Sink())] for f, a, k in ((w.writerow, (), {}), (w.writerow, ([], []), {}), (w.writerow, (), {"row": []}), (w.writerows, (), {}), (w.writerows, (5,), {}), (w.writerows, (None,), {}), (w.writerows, ([5],), {}), (w.writerows, ([[], 5],), {}), (w.writerows, ("ab",), {}), (w.writerows, ([],), {}), (w.writerows, (Lines(["a"], KeyError("k")),), {}))])
t("what write() does", lambda: (attempt(csv.writer(type("W", (), {"write": lambda s, x: 1 / 0})()).writerow, ["a"]), csv.writer(type("W", (), {"write": lambda s, x: ("given", x)})()).writerow(["a"]), attempt(csv.writer(type("W", (), {"write": lambda s: 0})()).writerow, ["a"]), attempt(csv.writer(type("W", (), {"write": lambda s, x: 1 / 0})()).writerows, [["a"]])))


class Str:
    def __init__(self, v): self.v = v
    def __str__(self): return self.v() if callable(self.v) else self.v


class Num:
    def __float__(self): return 1.5
    def __str__(self): return "num"


FIELDS = ("", "a", "a b", " a", "a ", " ", ",", "a,b", '"', 'a"b', '""', "'", "\\", "a\\b", "\n", "\r", "\r\n", "a\nb", ";", "\t", "|", "x", None, 0, 1, -1, 1.5, -0.0, 1e100, float("inf"), float("nan"), True, False, 2 ** 70, 1j, b"a", [], (), {}, [1, 2], Str("s"), Str("a,b"), Str(""), Str('"'), Num(), type("S", (str,), {})("sub"), type("I", (int,), {})(5), "\xe9", "\U0001F600", "\ud800", "\0", "1", "1.5")
for k in DIALECTS:
    t("written with %r" % (k,), lambda: (written([[f] for f in FIELDS], **k), written([[f, f] for f in FIELDS], **k)[0], written([["a", f, "b"] for f in FIELDS], **k)[0]))
t("what will not be shown", lambda: written([[Str(lambda: 1 / 0)], ["a", Str(lambda: 1 / 0)], [Str(5)], [Str(None)], ["b"]]))
rows = [[rng.choice((None, 1, 2.5, "")) if rng.random() < 0.15 else "".join(rng.choice(ALPHABET) for _ in range(rng.randrange(0, 8))) for _ in range(rng.randrange(0, 5))] for _ in range(1500)]
for k in DIALECTS:
    got = written(rows, **k)
    t("at random, written with %r" % (k,), lambda: (crc(got[0]), sum(isinstance(r, str) for r in got[1]), sorted({r for r in got[1] if isinstance(r, str)})))
t("and read back", lambda: [(k, sum(1 for row in rows if row and all(isinstance(f, str) for f in row) and not (len(row) == 1 and not row[0]) for w in [written([row], **k)] if w[0] and attempt(lambda: list(R(io.StringIO(w[0][0], newline=""), **k))) != [row])) for k in ({}, {"quoting": 1}, {"escapechar": "\\", "doublequote": False}, {"delimiter": ";", "quotechar": "'"}, {"lineterminator": "\n"})])
t("a great deal", lambda: (lambda s: (csv.writer(s).writerows([[i, "x" * (i % 50), i * 0.5, None] for i in range(20000)]), crc(s.getvalue()), len(list(R(io.StringIO(s.getvalue(), newline=""))))))(io.StringIO(newline="")))
t("a long field", lambda: (csv.field_size_limit(10 ** 7), len(next(R(['"' + "a," * 10 ** 6 + '"']))[0]), len(written([["b\n" * 10 ** 6]])[0][0]), csv.field_size_limit(131072))[1:3])

print("---- csv, which is written over it")
t("DictReader", lambda: (list(csv.DictReader(["a,b", "1,2", "3", "4,5,6", ""])), list(csv.DictReader(["1,2"], fieldnames=["x", "y"])), list(csv.DictReader(["a,b", "1,2,3"], restkey="rest")), list(csv.DictReader(["a,b", "1"], restval="none")), csv.DictReader(["a,b"]).fieldnames, csv.DictReader([]).fieldnames, [(r.line_num, next(r), r.line_num, shown(r.dialect) if False else r.dialect) for r in [csv.DictReader(["a", "1"])]]))
t("DictWriter", lambda: [(w.writeheader(), w.writerow({"a": 1, "b": 2}), w.writerow({"a": 1}), w.writerows([{"b": 3}]), attempt(w.writerow, {"c": 1}), s.getvalue()) for s in [io.StringIO()] for w in [csv.DictWriter(s, ["a", "b"], restval="-")]] + [[(w.writerow({"a": 1, "c": 2}), s.getvalue()) for s in [io.StringIO()] for w in [csv.DictWriter(s, ["a"], extrasaction="ignore")]], attempt(csv.DictWriter, io.StringIO(), ["a"], extrasaction="x")])
t("Sniffer", lambda: [(shown(_csv.Dialect(d)), attempt(csv.Sniffer().has_header, s)) if not isinstance(d, str) else d for s in ("a,b,c\n1,2,3\n4,5,6\n", "a;b;c\n1;2;3\n", "'a','b'\n'c','d'\n", "a\tb\n1\t2\n", '"a", "b"\n"c", "d"\n', "abc", "", "a|b|c\nd|e|f\n", "name,age\nx,1\ny,2\n") for d in [attempt(csv.Sniffer().sniff, s)]])
t("the classes", lambda: [(d.__name__, d.delimiter, d.quotechar, d.escapechar, d.doublequote, d.skipinitialspace, d.lineterminator, d.quoting, d._valid, d._name) for d in (csv.excel, csv.excel_tab, csv.unix_dialect)] + [attempt(csv.Dialect), attempt(type("B", (csv.Dialect,), {"delimiter": "ab"})), attempt(type("B", (csv.excel,), {"quoting": 9}))])
t("with a file", lambda: (lambda p: ([(csv.writer(f).writerows([["a", "b\nc"], ["\xe9", 1]]), f.close()) for f in [open(p, "w", newline="", encoding="utf-8")]], [(list(R(f)), f.close())[0] for f in [open(p, newline="", encoding="utf-8")]], open(p, "rb").read(), __import__("os").remove(p))[1:3])(__import__("tempfile").mktemp()))

print("---- Error")
import weakref
E = _csv.Error
e = E("a", 1)
print(repr(e), str(e), e.args, isinstance(e, Exception), attempt(weakref.ref, e), e.__dict__, attempt(setattr, e, "x", 1), e.x, repr(pickle.loads(pickle.dumps(e))), repr(copy.copy(e)), attempt(setattr, E, "y", 1), E.y, type("S", (E,), {})("z").args, E.__module__, E.__qualname__, csv.Error is E)
try:
    try: 1 / 0
    except ZeroDivisionError: list(csv.reader(["a\rb"]))
except E as x: print(type(x).__name__, x, repr(x.__context__), x.__traceback__ is not None)
