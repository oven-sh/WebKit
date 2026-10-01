# json as programs use it, and what is done to it meanwhile.
import _json, json, json.encoder, enum, collections, math, sys, io, dataclasses
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
        notes = getattr(e, "__notes__", None)
        if notes: r += " | " + " | ".join(notes)
    print(label, "->", ascii(r))
data = {"name": "caf\xe9 € \U0001f600", "n": [1, 2.5, -3, 1e100, None, True, False], "nested": {"a": {"b": {"c": [[], {}, [{}]]}}}, "": "", "k\n": "v\t"}
for label, k in {"plain": {}, "indent 2": {"indent": 2}, "indent 0": {"indent": 0}, "indent -1": {"indent": -1}, "indent tab": {"indent": "\t"}, "indent str": {"indent": "--"}, "sorted": {"sort_keys": True}, "not ascii": {"ensure_ascii": False}, "separators": {"separators": (",", ":")},
                 "odd separators": {"separators": (" ; ", " = ")}, "indent and separators": {"indent": 1, "separators": (",", ":")}, "no check": {"check_circular": False}, "all": {"indent": 3, "sort_keys": True, "ensure_ascii": False, "check_circular": False, "allow_nan": False, "skipkeys": True}}.items():
    attempt(label, lambda: json.dumps(data, **k))
    attempt(label + ", and back", lambda: json.loads(json.dumps(data, **k)) == data)
    attempt(label + ", a piece at a time", lambda: "".join(json.JSONEncoder(**k).iterencode(data)) == json.dumps(data, **k))
    attempt(label + ", to a file", lambda: (lambda f: (json.dump(data, f, **k), f.getvalue() == json.dumps(data, **k))[1])(io.StringIO()))
print("===== circles")
a = []; a.append(a)
attempt("a list in itself", lambda: json.dumps(a))
b = {}; b["b"] = b
attempt("a dict in itself", lambda: json.dumps(b))
c = [[{"x": None}]]; c[0][0]["x"] = c
attempt("by way of others", lambda: json.dumps(c))
attempt("the same twice is no circle", lambda: (lambda x: json.dumps([x, x, {"a": x}]))([1]))
class Loop:
    pass
attempt("default gives itself", lambda: json.dumps(Loop(), default=lambda o: o))
attempt("default gives what has it", lambda: json.dumps(Loop(), default=lambda o: [o]))
def depth(f):
    try: f()
    except RecursionError as e: return "RecursionError", len(getattr(e, "__notes__", [])) > 0
attempt("unchecked, a list", lambda: depth(lambda: json.dumps(a, check_circular=False)))
attempt("unchecked, a dict", lambda: depth(lambda: json.dumps(b, check_circular=False)))
attempt("unchecked, default", lambda: depth(lambda: json.dumps(Loop(), default=lambda o: [o], check_circular=False)))
deep = []
for i in range(400): deep = [deep]
attempt("deep", lambda: len(json.dumps(deep)))
attempt("markers are left empty", lambda: (lambda m: (_json.make_encoder(m, None, _json.encode_basestring, None, ":", ",", 0, 0, 0)([[1], {"a": [2]}], 0), m))({}))
def left(m):
    try: _json.make_encoder(m, None, _json.encode_basestring, None, ":", ",", 0, 0, 0)([[1, {2}]], 0)
    except TypeError: return len(m)
attempt("and not when it fails", lambda: left({}))
class Forgetful(dict):
    def __setitem__(self, k, v): pass
attempt("markers of a class derived from dict", lambda: _json.make_encoder(Forgetful(), None, _json.encode_basestring, None, ":", ",", 0, 0, 0)([[1]], 0))
print("===== default")
@dataclasses.dataclass
class P: x: int; y: list
attempt("default", lambda: json.dumps([P(1, [P(2, [])])], default=dataclasses.asdict))
attempt("default gives a str", lambda: json.dumps({"a": {1, 2}}, default=lambda o: "set"))
attempt("default gives None", lambda: json.dumps([object()], default=lambda o: None))
attempt("default raises", lambda: json.dumps({"a": [1, {"b": object()}]}, default=lambda o: 1 / 0))
attempt("no default", lambda: json.dumps({"a": [1, {"b": (2, {3})}]}))
the_set = {1}
attempt("default, then what cannot be", lambda: json.dumps([Loop()], default=lambda o: {"k": the_set}))
class E(json.JSONEncoder):
    def default(self, o): return {"<%s>" % type(o).__name__: sorted(o)} if isinstance(o, (set, frozenset)) else super().default(o)
attempt("cls", lambda: json.dumps([{1, 2}, frozenset("ab"), {"k": {3}}], cls=E))
attempt("cls, and what it cannot", lambda: json.dumps([{1}, b"x"], cls=E))
attempt("a note for each level", lambda: json.dumps({"a": [0, (1, {"b": collections.OrderedDict(c=[b""])})]}))
class R:
    def __repr__(self): raise ZeroDivisionError("repr")
    def __hash__(self): return 1
attempt("a key whose repr raises", lambda: json.dumps({1: {2: b"x"}}))
print("===== meddling")
def shrinking():
    l = [1, 2, 3, 4]
    class X: pass
    l[1] = X()
    def d(o): del l[2:]; return "x"
    return json.dumps(l, default=d)
attempt("a list that shrinks", shrinking)
def growing():
    l = [1, None]
    class X: pass
    l[1] = X()
    def d(o):
        if len(l) < 5: l.append(len(l))
        return "x"
    return json.dumps(l, default=d)
attempt("a list that grows", growing)
def dict_cleared():
    class X: pass
    m = {"a": X(), "b": 2, "c": 3}
    def d(o): m.clear(); return "x"
    return json.dumps(m, default=d)
attempt("a dict that is cleared", dict_cleared)
def sorted_meddled():
    class X: pass
    m = {"a": X(), "b": 2, "c": 3}
    def d(o): m.clear(); return "x"
    return json.dumps(m, default=d, sort_keys=True)
attempt("sorted, and cleared", sorted_meddled)
print("===== odd encoders")
mk = lambda enc, **k: _json.make_encoder(None, None, enc, k.get("indent"), ": ", ", ", 0, 0, 1)
attempt("gives bytes", lambda: mk(lambda s: b"x")(["a"], 0))
attempt("gives None", lambda: mk(lambda s: None)({"a": 1}, 0))
attempt("gives a subclass", lambda: mk(lambda s: type("S", (str,), {})("<>"))(["a", {"b": "c"}], 0))
attempt("raises", lambda: mk(lambda s: 1 / 0)([1, ["a"]], 0))
attempt("not callable", lambda: mk(5)(["a"], 0))
attempt("str", lambda: mk(str)(["a", {1: "b", None: 2.5}], 0))
attempt("the one in Python", lambda: mk(json.encoder.py_encode_basestring_ascii)(["a\xe9"], 0))
attempt("indent: an int", lambda: mk(str, indent=2)([1], 0))
attempt("indent: an int, at a level", lambda: mk(str, indent=2)([1], 1))
attempt("indent: an int, and nothing to indent", lambda: mk(str, indent=2)(1, 0))
attempt("indent: an int, and empty", lambda: mk(str, indent=2)([], 0))
attempt("indent: bytes", lambda: mk(str, indent=b" ")([1], 0))
attempt("indent: a subclass", lambda: mk(str, indent=type("S", (str,), {})(".."))([1, [2]], 1))
attempt("digits", lambda: len(json.dumps(10 ** 4299)))
attempt("too many digits", lambda: json.dumps([10 ** 4300]))
attempt("too many digits in a key", lambda: json.dumps({10 ** 4300: 1}))
