# _pickle: Pickler, and what goes wrong with writing.
import _pickle, pickle, io, copyreg, collections, sys, types, functools, warnings
warnings.simplefilter('ignore')
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
for t in (_pickle.Pickler, _pickle.Unpickler, _pickle.PickleBuffer, type(P(io.BytesIO()).memo), type(_pickle.Unpickler(io.BytesIO()).memo), _pickle.PickleError, _pickle.PicklingError, _pickle.UnpicklingError):
    print(t, t.__name__, t.__qualname__, t.__module__, t.__mro__, sorted(n for n in vars(t)), bool(t.__flags__ & (1 << 10)), t.__text_signature__, t.__basicsize__, t.__dictoffset__, t.__weakrefoffset__ != 0)
print("===== making one")
attempt("no arguments", lambda: P())
attempt("no write", lambda: P(5))
attempt("write that raises", lambda: P(type("F", (), {"write": property(lambda s: 1 / 0)})()))
for proto in (None, 0, 1, 5, 6, -1, -100, 2**70, -2**70, "2", 2.0, True, [], 255, 256):
    attempt("protocol %r" % (proto,), lambda: (lambda p: (p.bin, p.fast))(P(io.BytesIO(), proto)))
    attempt("dumps, protocol %r" % (proto,), lambda: _pickle.dumps(1, proto))
attempt("keywords", lambda: P(file=io.BytesIO(), protocol=2, fix_imports=False, buffer_callback=None).bin)
attempt("unknown keyword", lambda: P(io.BytesIO(), zzz=1))
attempt("too many", lambda: P(io.BytesIO(), 1, 1, None, 1))
attempt("buffer_callback and protocol 4", lambda: P(io.BytesIO(), 4, buffer_callback=print))
attempt("buffer_callback and no protocol", lambda: P(io.BytesIO(), buffer_callback=print).bin)
attempt("dumps: buffer_callback and protocol 4", lambda: _pickle.dumps(1, 4, buffer_callback=print))
attempt("dumps: positional fix_imports", lambda: _pickle.dumps(1, 2, True))
attempt("dumps: nothing", lambda: _pickle.dumps())
attempt("dump: no file", lambda: _pickle.dump(1))
attempt("dump: not a file", lambda: _pickle.dump(1, 2))
attempt("dump", lambda: (lambda f: (_pickle.dump([1], f, 2), f.getvalue()))(io.BytesIO()))
attempt("dump, by keyword", lambda: (lambda f: (_pickle.dump(obj=[1], file=f, protocol=0, fix_imports=False), f.getvalue()))(io.BytesIO()))
class NoInit(P):
    def __init__(self): pass
attempt("__init__ not called", lambda: NoInit().dump(1))
attempt("__init__ not called: clear_memo", lambda: NoInit().clear_memo())
attempt("__init__ not called: attributes", lambda: (NoInit().bin, NoInit().fast))
attempt("__init__ not called: dispatch_table", lambda: NoInit().dispatch_table)
attempt("__new__ alone", lambda: P.__new__(P).dump(1))
def twice():
    f, g = io.BytesIO(), io.BytesIO()
    p = P(f, 2); p.dump([1]); p.__init__(g, 0); p.dump([1]); return f.getvalue(), g.getvalue(), p.memo.copy() == {}
attempt("__init__ twice", twice)
print("===== attributes")
p = P(io.BytesIO(), 2)
attempt("bin, fast", lambda: (p.bin, p.fast))
attempt("set fast", lambda: (setattr(p, "fast", 5), p.fast)[1])
attempt("set fast to True", lambda: (setattr(p, "fast", True), p.fast)[1])
attempt("set fast to a str", lambda: setattr(p, "fast", "x"))
attempt("set fast to a float", lambda: setattr(p, "fast", 1.5))
attempt("set fast too large", lambda: (setattr(p, "fast", 2**40), p.fast))
attempt("delete fast", lambda: delattr(p, "fast"))
attempt("set bin", lambda: (setattr(p, "bin", 0), p.bin)[1])
attempt("dispatch_table, unset", lambda: P(io.BytesIO()).dispatch_table)
attempt("delete dispatch_table, unset", lambda: delattr(P(io.BytesIO()), "dispatch_table"))
attempt("set dispatch_table", lambda: (lambda q: (setattr(q, "dispatch_table", {1: 2}), q.dispatch_table)[1])(P(io.BytesIO())))
attempt("delete dispatch_table", lambda: (lambda q: (setattr(q, "dispatch_table", {}), delattr(q, "dispatch_table"), hasattr(q, "dispatch_table"))[2])(P(io.BytesIO())))
attempt("another attribute", lambda: setattr(P(io.BytesIO()), "zzz", 1))
attempt("get another attribute", lambda: P(io.BytesIO()).zzz)
attempt("persistent_id", lambda: P(io.BytesIO()).persistent_id(5))
attempt("persistent_id: no argument", lambda: P(io.BytesIO()).persistent_id())
attempt("set persistent_id", lambda: (lambda q: (setattr(q, "persistent_id", len), q.persistent_id is len)[1])(P(io.BytesIO())))
attempt("delete persistent_id", lambda: (lambda q: (setattr(q, "persistent_id", len), delattr(q, "persistent_id"), q.persistent_id(1)))(P(io.BytesIO())))
attempt("delete persistent_id, unset", lambda: delattr(P(io.BytesIO()), "persistent_id"))
attempt("hash, eq", lambda: (hash(p) == hash(p), p == p))
attempt("a name that is no str", lambda: getattr(p, 5))
attempt("setattr with a name that is no str", lambda: p.__setattr__(5, 1))
attempt("subclass has a __dict__", lambda: (lambda q: (setattr(q, "zzz", 1), q.zzz)[1])(type("Q", (P,), {})(io.BytesIO())))
print("===== the memo")
def memo():
    f = io.BytesIO(); q = P(f, 2); x = [1]; q.dump([x, x, "abc"])
    m = q.memo; c = m.copy()
    return type(m).__name__, sorted((v[0], v[1]) for v in c.values()), all(k == id(v[1]) for k, v in c.items()), m is q.memo
attempt("copy", memo)
attempt("clear", lambda: (lambda q: (q.dump([1]), q.memo.clear(), q.memo.copy())[2])(P(io.BytesIO())))
attempt("clear_memo", lambda: (lambda q: (q.dump([1]), q.clear_memo(), q.memo.copy())[2])(P(io.BytesIO())))
attempt("__reduce__", lambda: (lambda q: q.memo.__reduce__())(P(io.BytesIO())))
attempt("hash", lambda: hash(P(io.BytesIO()).memo))
attempt("len", lambda: len(P(io.BytesIO()).memo))
attempt("getitem", lambda: P(io.BytesIO()).memo[0])
attempt("delete", lambda: delattr(P(io.BytesIO()), "memo"))
attempt("set to a list", lambda: setattr(P(io.BytesIO()), "memo", []))
attempt("set to None", lambda: setattr(P(io.BytesIO()), "memo", None))
attempt("set: values that are no tuples", lambda: setattr(P(io.BytesIO()), "memo", {1: 2}))
attempt("set: tuples of three", lambda: setattr(P(io.BytesIO()), "memo", {1: (1, 2, 3)}))
attempt("set: no int", lambda: setattr(P(io.BytesIO()), "memo", {1: ("a", [])}))
attempt("set: too large", lambda: setattr(P(io.BytesIO()), "memo", {1: (2**70, [])}))
def primed():
    x = ["shared"]; f = io.BytesIO(); q = P(f, 2); q.memo = {0: (7, x)}; q.dump([x, x]); return f.getvalue(), sorted(v[0] for v in q.memo.copy().values())
attempt("set to a dict", primed)
def from_proxy():
    x = ["shared"]; a = P(io.BytesIO(), 2); a.dump(x); f = io.BytesIO(); b = P(f, 2); b.memo = a.memo; b.dump([x]); return f.getvalue(), len(a.memo.copy()), len(b.memo.copy())
attempt("set to a proxy", from_proxy)
def reused():
    f = io.BytesIO(); q = P(f, 2); x = [1]; q.dump(x); n = f.tell(); q.dump(x); return f.getvalue()[:n], f.getvalue()[n:]
attempt("twice with one Pickler", reused)
def large_memo(proto):
    v = [[i] for i in range(300)]; v += v; d = _pickle.dumps(v, proto); return len(d), d[-30:], _pickle.loads(d) == v
for proto in range(6): attempt("more than 256 in the memo, %d" % proto, lambda: large_memo(proto))
print("===== in use")
class Reenter:
    def __init__(self, f): self.f = f
    def __reduce__(self): return (self.f(), ())
def reenter(what):
    q = P(io.BytesIO())
    return q.dump(Reenter(lambda: what(q)))
attempt("dump inside dump", lambda: reenter(lambda q: q.dump(1)))
attempt("__init__ inside dump", lambda: reenter(lambda q: q.__init__(io.BytesIO())))
attempt("clear_memo inside dump", lambda: reenter(lambda q: q.clear_memo() or list))
attempt("after it failed", lambda: (lambda q: ([attempt("  first", lambda: q.dump(lambda: 0))], q.dump(1)))(P(io.BytesIO())))
print("===== fast")
def fast(v, proto=2):
    f = io.BytesIO(); q = P(f, proto); q.fast = 1; q.dump(v); return f.getvalue()
x = [1]
attempt("nothing is remembered", lambda: fast([x, x, "a", "a", (1, 2), {1: x}]))
attempt("protocol 4", lambda: fast([x, x, frozenset({1}), {2}], 4))
a = []; a.append(a)
def no_address(f):
    try: f()
    except ValueError as e: return str(e)[:str(e).find(" at ")]
attempt("a list in itself", lambda: no_address(lambda: fast(a)))
b = {}; b[1] = b
attempt("a dict in itself", lambda: no_address(lambda: fast(b)))
deep = 1
for i in range(60): deep = [deep]
attempt("deep", lambda: len(fast(deep)))
attempt("deep, and then again", lambda: (lambda q: (setattr(q, "fast", 1), q.dump(deep), q.dump(deep)))(P(io.BytesIO())))
