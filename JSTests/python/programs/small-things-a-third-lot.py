# Small things, a third lot.
import ast, contextvars, types, warnings, sys, collections
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
    print(label, "->", r)

print("== names that come to be True, False and None")
for source in (b"Tru\xe1\xb5\x89", b"Fal\xc5\xbfe", b"N\xc2\xbane", "Truᵉ = 1".encode(), "def Truᵉ(): pass".encode(), "x.Truᵉ".encode(), "f(Truᵉ=1)".encode(), "import Truᵉ".encode(), "Ｔrue".encode(), "__debug＿_".encode(), "ｘ = 1".encode()):
    for mode in ("eval", "exec"):
        attempt("%a %s" % (source, mode), lambda: ast.dump(ast.parse(source, mode=mode))[:90])
        attempt("  compile", lambda: type(compile(source, "<t>", mode)).__name__)

print("== what is raised while something is being handled has that for its context")
closed = __import__("io").StringIO(); closed.close()
def context_of(f):
    try:
        1 / 0
    except ZeroDivisionError:
        try:
            f()
        except BaseException as e:
            return type(e).__name__, type(e.__context__).__name__
    return "nothing raised"
for label, f in (("literal_eval", lambda: ast.literal_eval(r"'\U'")), ("compile", lambda: compile("(", "<t>", "exec")), ("eval", lambda: eval("1 +")), ("exec", lambda: exec("def")), ("ast.parse", lambda: ast.parse("a b")),
                 ("indentation", lambda: compile("if 1:\nx", "<t>", "exec")), ("tab", lambda: compile("if 1:\n\tx\n        y", "<t>", "exec")), ("symtable", lambda: compile("def f():\n  x = 1\n  global x", "<t>", "exec")),
                 ("codegen", lambda: compile("break", "<t>", "exec")), ("tree", lambda: compile(ast.Module([ast.Expr(ast.Name("True", ast.Load()))], []), "<t>", "exec")), ("null", lambda: compile("a\0b", "<t>", "exec")),
                 ("encoding", lambda: compile(b"# coding: nope\n", "<t>", "exec")), ("int", lambda: int("x")), ("import", lambda: __import__("no_such_module_at_all")), ("key", lambda: {}[1]), ("attribute", lambda: (1).x),
                 ("recursion", lambda: (lambda f: f(f))(lambda f: f(f))), ("not callable", lambda: (lambda x: x())(1)), ("ContextVar.get", lambda: contextvars.ContextVar("v").get()), ("Context[var]", lambda: contextvars.Context()[contextvars.ContextVar("v")]),
                 ("unsupported", lambda: __import__("io").BytesIO().fileno()), ("from import", lambda: exec("from sys import no_such_name")), ("warning as error", lambda: (warnings.simplefilter("error"), warnings.warn("w"))),
                 ("tokenize", lambda: list(__import__("tokenize").generate_tokens(iter(["(\n"]).__next__))), ("tokenize 2", lambda: list(__import__("tokenize").generate_tokens(iter(["'abc\n"]).__next__))),
                 ("frozen", lambda: __import__("_imp").get_frozen_object("no_such_frozen")), ("unbound", lambda: exec("def f():\n  x\n  x = 1\nf()")), ("name", lambda: no_such_name), ("index", lambda: [][0]), ("unpack", lambda: exec("a, b = 1,")),
                 ("format", lambda: "%d" % "x"), ("overflow", lambda: 2.0 ** 10000), ("memory", lambda: "x" * (1 << 62)), ("os", lambda: open("/no/such/file")), ("unicode", lambda: "\ud800".encode()), ("json", lambda: __import__("json").loads("{")),
                 ("struct", lambda: __import__("struct").pack("i", "x")), ("re", lambda: __import__("re").compile("(")), ("generator", lambda: (lambda g: (next(g), g.throw(KeyError)))((i for i in (1,)))), ("close", lambda: __import__("io").StringIO().close() or __import__("io").StringIO.read(closed)), ("stop", lambda: next(iter(()))), ("decode", lambda: b"\xff".decode()), ("assert", lambda: exec("assert 0"))):
    print(label, context_of(f))
warnings.resetwarnings()

print("== the name of a ContextVar is asked for its hash")
class weird(str):
    def __eq__(self, other): pass
class counts(str):
    n = 0
    def __hash__(self): counts.n += 1; return 5
attempt("unhashable", lambda: contextvars.ContextVar(weird()))
attempt("counted", lambda: (contextvars.ContextVar(counts("v")).name, counts.n))
attempt("no str", lambda: contextvars.ContextVar(5))
attempt("hash raises", lambda: contextvars.ContextVar(type("R", (str,), {"__hash__": lambda s: 1 / 0})()))

print("== what has itself in it, shown")
d = {}; d[42] = d.values(); attempt("values", lambda: repr(d))
d = {}; d[42] = d.items(); attempt("items", lambda: repr(d))
d = {}; d[42] = d.keys(); attempt("keys", lambda: repr(d))
d = {}; v = d.values(); d[1] = v; attempt("the view", lambda: repr(v))
d = {}; v = d.items(); d[1] = v; attempt("the items", lambda: repr(v))
d = {}; d[1] = types.MappingProxyType(d); attempt("proxy", lambda: repr(d))
dq = collections.deque(); dq.append(dq); attempt("deque", lambda: repr(dq))
dd = collections.defaultdict(list); dd[1] = dd; attempt("defaultdict", lambda: repr(dd))
l = []; l.append(iter(l)); attempt("iterator", lambda: repr(l).split(" at ")[0])

print("== an alias whose arguments are a list that is changed while it is shown")
lst = []
class X:
    def __repr__(self): lst.clear(); return "x"
lst += [X(), 1]
attempt("cleared", lambda: repr(types.GenericAlias(int, lst)))
lst2 = []
class Y:
    def __repr__(self): lst2.append(1); return "y"
lst2 += [Y()]
attempt("added to", lambda: repr(types.GenericAlias(int, (lst2,))))
lst3 = []
class Z:
    def __repr__(self): lst3.clear(); return "z"
lst3 += [Z(), 1]
attempt("cleared, in a tuple", lambda: repr(types.GenericAlias(int, (lst3,))))

print("== how many times a source is warned of")
for source in (r"f'\{{'", r"'\d'", r"f'\d'", r"f'\d{1}\d'", r"f'{1:\d}'", r"b'\d'", r"'\d' '\d'", r"f'\{{{1}\}}'", "1 is 1", "(1)()", "[1][1,]", r"f'''\{{'''", r"rf'\{{'", r"'\777'", "0if 1else 2", r"t'\{{'", r"f'{f'\d'}'"):
    def run():
        with warnings.catch_warnings(record=True) as w:
            warnings.simplefilter("always")
            try:
                compile(source, "<t>", "eval")
            except SyntaxError as e:
                return "SyntaxError", len(w)
        return [(x.category.__name__, str(x.message)[:40], x.lineno) for x in w]
    attempt(source, run)

print("== bytes that there is only one of")
e = b""
print(bytes() is e, b"a"[0:0] is e, bytes(0) is e, b"".join([]) is e, e + e is e, e * 5 is e, b"abc".strip(b"abc") is e, bytes(bytearray()) is e, e.lower() is e, e[:] is e, bytes(e) is e, b"ab"[5:] is e, bytes.fromhex("") is e, "".encode() is e)
def constant(v):
    ns = {}
    tree = ast.parse("x = 0"); tree.body[0].value = ast.Constant(v); ast.fix_missing_locations(tree)
    exec(compile(tree, "<t>", "exec"), ns)
    return ns["x"]
for v in (None, False, True, Ellipsis, b"", (), "", 0, "a"):
    print(repr(v), constant(v) is v)
