# _suggestions, which is how traceback.py finds which keyword may have been meant.
import _suggestions, keyword, traceback
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
    print(label, "->", r)
g = _suggestions._generate_suggestions
words = ["mat", "of", "fro", "whille", "retrun", "imprt", "clas", "Class", "TRUE", "none", "els", "elf", "asynch", "awiat", "lamda", "x", "", "zzzzzz", "pas", "brk", "contine", "yeild", "nonlocl", "globl", "asert", "rais", "exept", "finaly", "wth", "i", "n", "iff", "a" * 50, "d\xe9f", "def"]
for w in words:
    attempt(repr(w)[:20], lambda: g(keyword.kwlist, w))
attempt("soft", lambda: [g(keyword.softkwlist, w) for w in ("mach", "cas", "typ", "_", "__")])
attempt("a tuple", lambda: g(("a",), "a"))
attempt("a list subclass", lambda: g(type("L", (list,), {})(["ab"]), "a"))
attempt("no str in it", lambda: g(["a", 1], "a"))
attempt("item no str", lambda: g(["a"], 1))
attempt("bytes", lambda: g(["a"], b"a"))
attempt("empty", lambda: g([], "a"))
attempt("too many", lambda: g(["ab%d" % i for i in range(750)], "ab"))
attempt("nearly too many", lambda: g(["ab%d" % i for i in range(749)], "ab"))
attempt("surrogate in the item", lambda: g(["a"], "\ud800"))
attempt("surrogate in a candidate", lambda: g(["\ud800"], "a"))
class S(str): pass
c = [S("abc"), "abd"]
attempt("it is the one in the list", lambda: (g(c, "abx") is c[0], type(g(c, "abx")).__name__))
attempt("the first of two as near", lambda: g(["abd", "abe"], "abc"))
attempt("case", lambda: g(["ABC", "abd"], "abc"))
attempt("no arguments", lambda: g())
attempt("keyword", lambda: g(candidates=[], item=""))
print(g.__text_signature__, g.__doc__, _suggestions.__name__, _suggestions.__doc__)
for source in ("[\nx for x\nin range(3)\nof x\n]", "fro x import y", "whille True: pass", "a = 1\nfor x im range(3): pass", "clas A: pass", "if x:\n  pass\nelf y:\n  pass", "asynch def f(): pass", "imprt os", "retrun 5", "x = lamda: 1", "wth a: pass", "def f():\n  yeild 1", "x = 1 iff y else 2", "raisee X", "assrt x"):
    try:
        compile(source, "<t>", "exec")
    except SyntaxError as e:
        print(repr(source), "->", traceback.format_exception_only(e)[-1].strip())
