# _pickle: what says how a thing is to be written, and what goes wrong with it.
import _pickle, pickle, io, copyreg, collections, sys, types, functools, warnings
import re; address = re.compile('0x[0-9a-f]+')
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
        notes = getattr(e, "__notes__", None)
        if notes: r += " | " + " | ".join(notes)
        c = e.__context__ or e.__cause__
        if c is not None: r += " <- %s: %s" % (type(c).__name__, c)
    print(label, "->", address.sub("0x", ascii(r)))
P = _pickle.Pickler
def each(label, f):
    for proto in (0, 1, 2, 3, 4, 5): attempt("%s %d" % (label, proto), lambda: f(proto))
class R:
    def __init__(self, value): self.value = value
    def __reduce__(self): return self.value
    def __repr__(self): return "R()"
def ident(*a): return a
print("===== what __reduce__ gives")
for label, v in {"None": None, "int": 5, "list": [ident, ()], "empty": (), "one": (ident,), "seven": (ident, (), None, None, None, None, None), "not callable": (5, ()), "args no tuple": (ident, [1]), "args None": (ident, None), "state": (ident, (), {"a": 1}), "state None": (ident, (), None),
                 "listitems no iterator": (ident, (), None, [1]), "listitems": (list, (), None, iter([1, 2, 3])), "one listitem": (list, (), None, iter([1])), "no listitems": (list, (), None, iter([])), "dictitems no iterator": (ident, (), None, None, {1: 2}), "dictitems": (dict, (), None, None, iter({1: 2, 3: 4}.items())),
                 "one dictitem": (dict, (), None, None, iter([(1, 2)])), "dictitems no pairs": (dict, (), None, None, iter([1])), "dictitems of three": (dict, (), None, None, iter([(1, 2, 3)])), "second dictitem no pair": (dict, (), None, None, iter([(1, 2), 3])), "dictitems of lists": (dict, (), None, None, iter([[1, 2]])),
                 "setter not callable": (ident, (), {}, None, None, 5), "setter": (ident, (), {"s": 1}, None, None, ident), "setter and no state": (ident, (), None, None, None, ident), "a str": "ident", "a str that is not there": "nowhere", "a dotted str": "R.__init__", "an empty str": "", "a str subclass": type("S", (str,), {})("ident"),
                 "bytes": b"ident", "unpicklable callable": (lambda: 0, ()), "unpicklable argument": (ident, (lambda: 0,)), "unpicklable state": (ident, (), lambda: 0), "unpicklable listitem": (list, (), None, iter([1, lambda: 0])), "unpicklable dictitem": (dict, (), None, None, iter([(1, 2), ("k", lambda: 0)])),
                 "unpicklable key": (dict, (), None, None, iter([(lambda: 0, 1)])), "unpicklable setter": (ident, (), 1, None, None, lambda a, b: 0), "iterator that raises": (list, (), None, (1 / 0 for _ in [1])), "tuple subclass": collections.namedtuple("N", "a b")(ident, ())}.items():
    each(label, lambda proto: _pickle.dumps(R(v), proto))
print("===== __newobj__ and __newobj_ex__")
class C:
    def __repr__(self): return "C()"
class RC(C):
    def __init__(self, value): self.value = value
    def __reduce__(self): return self.value
    def __repr__(self): return "RC()"
for label, v in {"newobj": (copyreg.__newobj__, (RC,)), "newobj, no arguments": (copyreg.__newobj__, ()), "newobj, no class": (copyreg.__newobj__, (5,)), "newobj, another class": (copyreg.__newobj__, (C,)), "newobj, arguments": (copyreg.__newobj__, (RC, 1, 2)), "newobj, unpicklable argument": (copyreg.__newobj__, (RC, lambda: 0)),
                 "newobj_ex": (copyreg.__newobj_ex__, (RC, (), {})), "newobj_ex, two": (copyreg.__newobj_ex__, (RC, ())), "newobj_ex, four": (copyreg.__newobj_ex__, (RC, (), {}, 1)), "newobj_ex, no class": (copyreg.__newobj_ex__, (5, (), {})), "newobj_ex, no tuple": (copyreg.__newobj_ex__, (RC, [], {})),
                 "newobj_ex, no dict": (copyreg.__newobj_ex__, (RC, (), [])), "newobj_ex, arguments": (copyreg.__newobj_ex__, (RC, (1,), {"k": 2})), "newobj_ex, another class": (copyreg.__newobj_ex__, (C, (), {})), "newobj_ex, unpicklable": (copyreg.__newobj_ex__, (RC, (lambda: 0,), {})),
                 "newobj_ex, unpicklable keyword": (copyreg.__newobj_ex__, (RC, (), {"k": lambda: 0})), "something else called __newobj__": (type("F", (), {"__call__": lambda s, *a: 0, "__name__": "__newobj__", "__reduce__": lambda s: "ident"})(), (RC,))}.items():
    each(label, lambda proto: _pickle.dumps(RC(v), proto))
print("===== which is asked")
log = []
class Both:
    def __reduce__(self): log.append("reduce"); return (ident, ())
    def __reduce_ex__(self, p): log.append(("reduce_ex", p)); return (ident, (p,))
each("__reduce_ex__ first", lambda proto: (_pickle.dumps(Both(), proto)[-12:], log.pop()))
class ExNone: __reduce_ex__ = None
class BothNone: __reduce_ex__ = None; __reduce__ = None
each("__reduce_ex__ is None", lambda proto: _pickle.dumps(ExNone(), proto))
class Raises:
    def __reduce_ex__(self, p): raise ZeroDivisionError("in reduce")
each("it raises", lambda proto: _pickle.dumps([Raises()], proto))
class GetAttr:
    def __getattribute__(self, n):
        if n.startswith("__reduce"): raise AttributeError(n)
        return object.__getattribute__(self, n)
each("neither can be got", lambda proto: _pickle.dumps(GetAttr(), proto))
class GetAttrRaises:
    def __getattribute__(self, n):
        if n.startswith("__reduce"): raise ZeroDivisionError(n)
        return object.__getattribute__(self, n)
each("getting it raises", lambda proto: _pickle.dumps(GetAttrRaises(), proto))
print("===== dispatch tables")
class X:
    def __repr__(self): return "X()"
def with_table(table, v, proto=2, cls=P):
    f = io.BytesIO(); q = cls(f, proto); q.dispatch_table = table; q.dump(v); return f.getvalue()
attempt("its own", lambda: with_table({X: lambda o: (ident, ("own",))}, X()))
attempt("not in it", lambda: with_table({}, 1j))
attempt("copyreg's is not looked in then", lambda: with_table({}, 1j) == _pickle.dumps(1j, 2))
attempt("a ChainMap", lambda: with_table(collections.ChainMap({X: lambda o: (ident, ("chain",))}, copyreg.dispatch_table), [X(), 1j]))
attempt("not a mapping", lambda: with_table(5, X()))
class BadMap:
    def __getitem__(self, k): raise ZeroDivisionError("getitem")
attempt("a mapping that raises", lambda: with_table(BadMap(), X()))
class KeyMap:
    def __getitem__(self, k): raise KeyError(k)
attempt("a mapping that has nothing", lambda: with_table(KeyMap(), X())[-20:])
attempt("None in it", lambda: with_table({X: None}, X()))
attempt("for a class", lambda: with_table({type: lambda o: (ident, ("a class",))}, X))
attempt("for a function", lambda: with_table({types.FunctionType: lambda o: (ident, ("a function",))}, ident))
attempt("for a list", lambda: with_table({list: lambda o: (ident, ("a list",))}, [1]))
class PT(P): dispatch_table = {X: lambda o: (ident, ("of the class",))}
attempt("of a class derived from Pickler", lambda: (lambda f: (PT(f, 2).dump(X()), f.getvalue())[1])(io.BytesIO()))
copyreg.pickle(X, lambda o: (ident, ("copyreg",)))
attempt("copyreg", lambda: _pickle.dumps(X(), 2))
del copyreg.dispatch_table[X]
print("===== reducer_override")
class RO(P):
    def reducer_override(self, o):
        log.append(type(o).__name__)
        if isinstance(o, X): return (len, ("overridden",))
        if o is ident: return (len, ("a function",))
        if o is X: return "ident"
        if isinstance(o, complex): return 5
        if isinstance(o, range): raise ZeroDivisionError("override")
        return NotImplemented
def override(v, proto=2):
    log.clear(); f = io.BytesIO(); RO(f, proto).dump(v); return f.getvalue(), list(log)
for label, v in {"an instance": X(), "a function": ident, "a class": X, "what it leaves": C, "built in": [1, "a", (2,), {3: 4}, {5}, b"6", None, 1.5], "gives an int": 1j, "raises": range(3), "another function": attempt, "an instance that it leaves": C()}.items():
    attempt(label, lambda: override(v))
attempt("set on an instance of a class derived from it", lambda: (lambda f, q: (setattr(q, "reducer_override", lambda o: (len, ("set",)) if isinstance(o, X) else NotImplemented), q.dump(X()), f.getvalue())[2])(*(lambda f: (f, type("Q", (P,), {})(f, 2)))(io.BytesIO())))
print("===== persistent_id")
class PP(P):
    def persistent_id(self, o):
        if isinstance(o, X): return "an X"
        if isinstance(o, C): return ("a", "tuple")
        if isinstance(o, complex): return "\xe9"
        if isinstance(o, range): raise ZeroDivisionError("pid")
        if isinstance(o, frozenset): return 0
        if isinstance(o, bytes): return X()
        return None
def persistent(v, proto):
    f = io.BytesIO(); PP(f, proto).dump(v); return f.getvalue()
for label, v in {"a str": X(), "a tuple": C(), "not ASCII": 1j, "raises": range(2), "0": frozenset(), "what is itself persistent": b"x", "inside": [X(), {1: X()}, (X(),)], "nothing": [1]}.items():
    each(label, lambda proto: persistent(v, proto))
def set_pid(proto):
    f = io.BytesIO(); q = P(f, proto); q.persistent_id = lambda o: "id" if isinstance(o, X) else None; q.dump([X()]); return f.getvalue()
each("set on the instance", set_pid)
attempt("set to what is not callable", lambda: (lambda q: (setattr(q, "persistent_id", 5), q.dump(1)))(P(io.BytesIO())))
