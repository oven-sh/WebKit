import _contextvars
from _contextvars import Context, ContextVar, Token, copy_context
def scrub(s):
    out = []; i = 0
    while i < len(s):
        if s.startswith(" at 0x", i):
            out.append(" at 0x"); i += 6
            while i < len(s) and s[i] in "0123456789abcdefABCDEF": i += 1
        else: out.append(s[i]); i += 1
    return "".join(out)
def show(label, f):
    try:
        print(label, "=>", scrub(repr(f())))
    except BaseException as e:
        print(label, "!!", type(e).__name__, scrub(str(e)))
def attempt(f):
    try: return f()
    except BaseException as e: return type(e).__name__ + ": " + str(e)
# ---- the module and the classes
show("module", lambda: (_contextvars.__name__, _contextvars.__doc__, sorted(n for n in dir(_contextvars) if not n.startswith("__"))))
for c in (Context, ContextVar, Token, type(Token.MISSING)):
    show("class " + c.__name__, lambda: (c.__module__, c.__qualname__, c.__mro__, c.__doc__, c.__flags__ & (1 << 10) != 0, c.__hash__ is None, sorted((n, type(v).__name__) for n, v in vars(c).items())))
show("cannot be derived from", lambda: [attempt(lambda: type("X", (c,), {})) for c in (Context, ContextVar, Token)])
show("generic", lambda: (ContextVar[int], Token[int], attempt(lambda: Context[int])))
# ---- ContextVar
v = ContextVar("v"); d = ContextVar("d", default=5); n = ContextVar("n", default=None)
show("repr", lambda: (v, d, n, ContextVar("q'\"", default="s")))
show("name", lambda: (v.name, attempt(lambda: setattr(v, "name", "x")), attempt(lambda: delattr(v, "name"))))
for a, k in (((), {}), ((1,), {}), ((None,), {}), (("a", 1), {}), (("a",), {"default": 1}), (("a",), {"x": 1}), ((), {"name": "a"}), ((), {"default": 1}), (("a",), {"default": 1, "x": 2}), ((b"a",), {})):
    show("ContextVar" + repr((a, k)), lambda: ContextVar(*a, **k))
class S(str): pass
show("a name that is a subclass of str", lambda: (ContextVar(S("s")).name, type(ContextVar(S("s")).name).__name__))
show("get", lambda: (attempt(v.get), v.get(1), v.get(None), d.get(), d.get(1), n.get(), attempt(lambda: v.get(1, 2)), attempt(lambda: v.get(default=1))))
def catch(f):
    try: f()
    except BaseException as e: return e
show("LookupError has the variable", lambda: (lambda e: (type(e).__name__, e.args[0] is v, len(e.args)))(catch(v.get)))
show("hash and equality", lambda: (hash(v) == hash(v), hash(v) != hash(d), v == v, v != d, v == ContextVar("v"), isinstance(hash(v), int), {v: 1}[v]))
show("no other attributes", lambda: (attempt(lambda: setattr(v, "x", 1)), hasattr(v, "__dict__"), attempt(lambda: v.default)))
# ---- set, reset and Token
def in_new(f): return Context().run(f)
def sets():
    t = v.set(1); a = v.get(); t2 = v.set(2); b = v.get(); v.reset(t2); c = v.get(); v.reset(t); return (a, b, c, attempt(v.get))
show("set and reset", lambda: in_new(sets))
def tokens():
    t = v.set(1); t2 = v.set(2)
    return (t, t.var is v, t.old_value, t.old_value is Token.MISSING, t2.old_value, Token.MISSING, type(Token.MISSING).__name__)
show("tokens", lambda: in_new(tokens))
def used():
    t = v.set(1); r0 = repr(t); v.reset(t); return (r0, t, attempt(lambda: v.reset(t)))
show("a token is used once", lambda: in_new(used))
def wrong_var():
    t = v.set(1); return attempt(lambda: d.reset(t))
show("another variable's token", lambda: in_new(wrong_var))
def wrong_context():
    t = v.set(1); return attempt(lambda: Context().run(v.reset, t))
show("another context's token", lambda: in_new(wrong_context))
show("not a token", lambda: [attempt(lambda: v.reset(x)) for x in (1, None, "t", Token)])
show("wrong numbers", lambda: (attempt(v.set), attempt(lambda: v.set(1, 2)), attempt(v.reset), attempt(lambda: v.set(value=1)), attempt(lambda: v.reset(token=1))))
show("Token()", lambda: (attempt(Token), attempt(lambda: Token(1)), attempt(lambda: hash(in_new(lambda: v.set(1)))), attempt(lambda: setattr(in_new(lambda: v.set(1)), "var", 1))))
def out_of_order():
    a = v.set(1); b = v.set(2); v.reset(a); r = attempt(v.get); v.reset(b); return (r, v.get())
show("reset out of order", lambda: in_new(out_of_order))
def with_token():
    v.set(0)
    with v.set(1) as t: a = (v.get(), type(t).__name__)
    return (a, v.get(), t)
show("a token in `with`", lambda: in_new(with_token))
def with_token_raises():
    v.set(0)
    try:
        with v.set(1): raise KeyError("k")
    except KeyError: return v.get()
show("a token in `with`, and something is raised", lambda: in_new(with_token_raises))
def with_token_twice():
    t = v.set(1)
    with t: pass
    return attempt(lambda: t.__exit__(None, None, None))
show("a token in `with`, twice", lambda: in_new(with_token_twice))
show("set to the default", lambda: in_new(lambda: (d.set(5).old_value, d.get())))
# ---- Context
show("Context()", lambda: (len(Context()), list(Context()), attempt(lambda: Context(1)), attempt(lambda: Context(a=1)), attempt(lambda: hash(Context())), bool(Context())))
show("repr of a context", lambda: Context())
def filled():
    c = Context(); c.run(lambda: (v.set(1), d.set(2))); return c
c = filled()
show("as a mapping", lambda: (len(c), sorted(k.name for k in c), c[v], c[d], v in c, n in c, c.get(v), c.get(n), c.get(n, 9), sorted(k.name for k in c.keys()), sorted(c.values()), sorted((k.name, x) for k, x in c.items())))
show("the views", lambda: (type(c.keys()).__name__, type(c.values()).__name__, type(c.items()).__name__, type(iter(c)).__name__, iter(c.keys()) is not None, attempt(lambda: len(c.keys()))))
show("a view is gone through once", lambda: (lambda k: (len(list(k)), len(list(k))))(c.keys()))
show("missing", lambda: (attempt(lambda: c[n]), catch(lambda: c[n]).args[0] is n))
show("keys that are no variables", lambda: [(attempt(lambda: c[x]), attempt(lambda: x in c), attempt(lambda: c.get(x))) for x in (1, "v", None)])
show("cannot be changed", lambda: (attempt(lambda: c.__setitem__(v, 1)), attempt(lambda: c.__delitem__(v)), hasattr(c, "update"), hasattr(c, "pop")))
show("equality", lambda: (c == c, c == filled(), c != filled(), c == Context(), Context() == Context(), c == 1, c != 1, c.__eq__(1), attempt(lambda: c < c), c == c.copy()))
class Eq:
    def __init__(s, x): s.x = x
    def __eq__(s, o): return s.x == o.x
def holding(x):
    k = Context(); k.run(v.set, x); return k
show("equality goes by ==", lambda: (holding(Eq(1)) == holding(Eq(1)), holding(Eq(1)) == holding(Eq(2)), holding(1) == holding(1.0)))
class BadEq:
    def __eq__(s, o): raise RuntimeError("eq")
show("equality that raises", lambda: attempt(lambda: holding(BadEq()) == holding(BadEq())))
show("copy", lambda: (lambda k: (k is not c, k == c, k.run(v.set, 9) and None, k[v], c[v]))(c.copy()))
show("wrong numbers for the methods", lambda: (attempt(lambda: c.copy(1)), attempt(lambda: c.keys(1)), attempt(lambda: c.get()), attempt(lambda: c.get(v, 1, 2)), attempt(lambda: c.get(key=v))))
# ---- run
show("run", lambda: (Context().run(lambda *a, **k: (a, k), 1, 2, x=3), attempt(lambda: Context().run()), attempt(lambda: Context().run(1)), attempt(lambda: Context().run(callable=len))))
def isolated():
    v.set("outer"); k = Context(); r = k.run(lambda: (attempt(v.get), v.set("inner"), v.get())[::2]); return (r, v.get(), k[v])
show("what is set in one is not seen outside", lambda: in_new(isolated))
def copied():
    v.set("outer"); k = copy_context(); r = k.run(lambda: (v.get(), v.set("inner"), v.get())[::2]); return (r, v.get(), k[v])
show("copy_context()", lambda: in_new(copied))
def copy_is_a_snapshot():
    v.set(1); k = copy_context(); v.set(2); return (k[v], v.get(), k.run(v.get))
show("a copy is of how things were", lambda: in_new(copy_is_a_snapshot))
def reentered():
    k = Context(); return k.run(lambda: attempt(lambda: k.run(len, "")))
show("entered twice", reentered)
def entered_again_afterwards():
    k = Context(); k.run(v.set, 1); return k.run(v.get)
show("entered again afterwards", entered_again_afterwards)
def raises_inside():
    k = Context()
    def f(): v.set(1); raise KeyError("k")
    return (attempt(lambda: k.run(f)), k[v], attempt(v.get), k.run(v.get))
show("something is raised inside", lambda: in_new(raises_inside))
def nested():
    a = Context(); b = Context()
    return a.run(lambda: (v.set("a"), b.run(lambda: (attempt(v.get), v.set("b"), v.get())[::2]), v.get())[1:])
show("one in another", nested)
show("copy_context() takes nothing", lambda: (attempt(lambda: copy_context(1)), type(copy_context()).__name__, copy_context() is not copy_context()))
def seen_while_entered():
    k = Context()
    def f(): v.set(1); return (k[v], len(k), copy_context() == k, copy_context() is k)
    return k.run(f)
show("a context that is entered has what is set in it", seen_while_entered)
def reset_removes():
    k = Context()
    def f(): t = v.set(1); v.reset(t); return (len(k), v in k)
    return k.run(f)
show("reset takes it out again", reset_removes)
# ---- generators and coroutines have no context of their own
def gen():
    v.set("in the generator"); yield v.get(); yield v.get()
def generators():
    v.set("outside"); g = gen(); a = next(g); b = v.get(); v.set("outside again"); return (a, b, next(g))
show("a generator runs in that of what resumes it", lambda: in_new(generators))
async def co(): v.set("in the coroutine"); return v.get()
def coroutines():
    v.set("outside"); k = co()
    try: k.send(None)
    except StopIteration as e: return (e.value, v.get())
show("and so does a coroutine", lambda: in_new(coroutines))
def generator_across_contexts():
    g = gen(); a = Context(); b = Context(); return (a.run(next, g), b.run(next, g) if False else attempt(lambda: b.run(next, g)), a.get(v), b.get(v))
show("a generator resumed in another", generator_across_contexts)
# ---- many
def many(count):
    vs = [ContextVar("m%d" % i) for i in range(count)]; k = Context()
    def f():
        for i, x in enumerate(vs): x.set(i)
        a = all(x.get() == i for i, x in enumerate(vs))
        for x in vs[::2]: x.set(-1)
        b = sum(x.get() for x in vs)
        return (a, b)
    r = k.run(f); return (r, len(k), len(list(k)), len(set(k)), sorted(k.values())[:3], sum(1 for x in vs if x in k))
for count in (1, 5, 33, 200, 3000): show("many %d" % count, lambda: many(count))
def many_reset(count):
    vs = [ContextVar("r%d" % i) for i in range(count)]; k = Context()
    def f():
        ts = [x.set(i) for i, x in enumerate(vs)]
        for x, t in list(zip(vs, ts))[::3]: x.reset(t)
        return (len(k), sum(1 for x in vs if x in k), sum(k.values()))
    return k.run(f)
for count in (4, 40, 1000): show("many, and some taken out %d" % count, lambda: many_reset(count))
def snapshots():
    x = ContextVar("x"); ks = []
    def f():
        for i in range(50): x.set(i); ks.append(copy_context())
    Context().run(f); return [k[x] for k in ks] == list(range(50))
show("each copy keeps what it had", snapshots)
