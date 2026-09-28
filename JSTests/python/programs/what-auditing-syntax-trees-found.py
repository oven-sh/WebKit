# What was found to be wrong, with what has nothing to do with syntax trees, by compiling and running a million syntax trees that were made wrongly: audits/syntax-trees.py.

import _warnings
import sys

_warnings.filters.insert(0, ("ignore", None, Warning, None, 0))


def t(label, f):
    try:
        r = f()
    except BaseException as e:
        r = type(e).__name__ + ": " + str(e)
    print(label, "=>", r)


# ---- A function that has annotations, and a variable of the function that it is in
def f():
    def g() -> int:
        nonlocal b
        b += 1
        return b
    b = 1
    return g


g = f()
print(g(), g.__annotations__)


class C:
    def m(self):
        def h(x: str) -> int:
            nonlocal y
            return __class__, y
        y = 2
        return h


h = C().m()
print(h(1), h.__annotations__)


def outer():
    class K:
        x: int

        def m(self):
            nonlocal w
            return w
    w = 1
    return K


print(outer().__annotations__, outer()().m())

# ---- Annotations of a module that have no names in them
variables = {}
exec("x: 1 = 1\ny: 'str'\nz = __annotate__(1)", variables)
print(variables["z"])

# ---- What evaluates annotations, and what it is called
def annotated(x: int): pass


class Annotated:
    y: int
    def m(self, a: int): pass


def enclosing():
    def inner(x: int): pass
    return inner


for o in (annotated, Annotated, Annotated.m, enclosing()):
    a = o.__annotate__
    print(o.__qualname__, "|", a.__qualname__, a.__name__, a.__code__.co_qualname, a.__code__.co_firstlineno - o.__annotate__.__code__.co_firstlineno)


def names(code, depth=0):
    print("  " * depth + code.co_qualname)
    for constant in code.co_consts:
        if hasattr(constant, "co_code"):
            names(constant, depth + 1)


names(compile("x: int\ndef f(a: int): pass\nclass C:\n  y: int\n  def m(self) -> int: pass\ndef g():\n  def h(b: int): pass\n  k = lambda: 0", "<test>", "exec"))
print([list(compile(source, "<test>", mode).co_positions())[0] for source, mode in (("a", "exec"), ("a", "eval"), ("a", "single"), ("\n\na", "exec"), ("", "exec"))], list(compile("a", "<test>", "exec").replace(co_firstlineno=7).co_positions())[0])

for s in ["for x in y: pass\nelse: break", "while x: pass\nelse: break", "while x: pass\nelse: continue", "for a in b:\n for x in y: pass\n else: break", "async def f():\n async for x in y: pass\n else: break", "for x in y:\n try: pass\n finally: break", "while 1:\n def f(): break", "while 1:\n class C: break", "for x in y: pass\nelse:\n for z in w: break",
          "class C(m=1, m=2): pass", "match x:\n case -123456789012345678901234567890: pass", "match x:\n case 123456789012345678901234567890: pass", "match x:\n case {123456789012345678901234567890: 1, 123456789012345678901234567890: 2}: pass", "match x:\n case {-123456789012345678901234567890: 1, 0x18ee90ff6c373e0ee4e3f0ad2: 2}: pass", "match x:\n case {123456789012345678901234567890: 1, 0x18ee90ff6c373e0ee4e3f0ad2: 2}: pass", "match x:\n case 123456789012345678901234567890 + 1j: pass", "match x:\n case {18446744073709551616: 1, 18446744073709551616.0: 2}: pass"]:
    t(ascii(s), lambda: compile(s, "<t>", "exec") and "ok")
t("set star", lambda: {1, *2}); t("list star", lambda: [1, *2]); t("tuple star", lambda: (1, *2)); t("call star", lambda: print(*2)); t("dict star", lambda: {**2}); t("set star none", lambda: {*None})
def c1():
    class C(**1): pass
def c2():
    class C(*1): pass
def c3():
    class C(**{1: 2}): pass
def c4():
    class C(metaclass=type, **{"metaclass": type}): pass
t("class **", c1); t("class *", c2); t("class ** key", c3); t("class dup", c4)
for v, f in [("a", "-5"), ("a", "+5"), ("a", " 5"), ("a", "=5"), ("a", "05"), ("a", "<05"), ("a", ",")  , ("a", "_"), ("a", "#"), ("a", "z"), ("a", "5.2"), ("a", ".0"), ("a", "5s"), ("a", "5d"), ("a", "^5"), ("a", "x^5"), ("a", "=^5"), ("a", "0=5"), ("a", "-<5"), ("a", "<-5")]:
    t("format %r %r" % (v, f), lambda: repr(format(v, f)))
def big(x):
    match x:
        case -123456789012345678901234567890: return "neg"
        case 123456789012345678901234567890: return "pos"
        case {123456789012345678901234567890: v}: return v
    return None
t("big", lambda: (big(-123456789012345678901234567890), big(123456789012345678901234567890), big({123456789012345678901234567890: 5}), big(1)))

# ---- Deleting and setting what cannot be
H = 1 << 40; G = 1 << 100
for a in (0, 1, -1, 5, -5, 1 << 70, -(1 << 70)):
    for b in (0, 1, 31, 32, 63, 64, 100, H, G):
        t("%d >> %d" % (a, b), lambda: a >> b)
    for b in (H, G):
        if a == 0: t("%d << %d" % (a, b), lambda: a << b)
t("1 << G", lambda: 1 << G); t("True >> H", lambda: True >> H)
class S: __slots__ = ("q",)
class D: pass
class R:
    @property
    def p(self): return 1
def dl(o, n):
    delattr(o, n)
    t("del %s.x" % label, lambda: dl(o, "x")); t("set %s.x" % label, lambda: setattr(o, "x", 1))
t("del slots.q", lambda: dl(S(), "q")); t("del property.p", lambda: dl(R(), "p")); t("del int.real", lambda: dl(1, "real")); t("del int.__add__", lambda: dl(1, "__add__")); t("del int.__class__", lambda: dl(1, "__class__")); t("del D().__dict__", lambda: dl(D(), "__dict__"))
def di(o):
    del o[0]
def si(o):
    o[0] = 1
def gi(o):
    return o[0]
for label, o in (("int", 1), ("str", "s"), ("tuple", (1,)), ("None", None), ("object", object()), ("set", {1}), ("bytes", b"a"), ("range", range(1)), ("function", di), ("type", int), ("frozenset", frozenset()), ("gen", (i for i in ()))):
    t("del %s[0]" % label, lambda: di(o)); t("set %s[0]" % label, lambda: si(o)); t("get %s[0]" % label, lambda: gi(o))

class D: pass
class I(int): pass
class L:
    def __len__(self): return 0
class Ix:
    def __index__(self): return 0
class G:
    def __getitem__(self, k): return k
def di(o, k):
    del o[k]
def si(o, k):
    o[k] = 1
things = [("int", 1), ("D", D()), ("I", I()), ("L", L()), ("G", G()), ("str", "s"), ("tuple", (1,)), ("set", {1}), ("keys", {}.keys()), ("values", {}.values()), ("items", {}.items()), ("alias", list[int]), ("union", int | str), ("template", t""), ("interp", t"{1}".interpolations[0]), ("enumerate", enumerate([])), ("slice", slice(1)), ("proxy", type.__dict__), ("range", range(1)), ("bytes", b"a"), ("iter", iter([])), ("module", sys), ("code", di.__code__), ("frame", sys._getframe()), ("method", D().__init__), ("float", 1.5), ("complex", 1j), ("bool", True), ("ellipsis", ...), ("NotImplemented", NotImplemented), ("super", super(D, D())), ("property", property()), ("staticmethod", staticmethod(di)), ("zip", zip()), ("map", map(di, [])), ("filter", filter(None, [])), ("reversed", reversed([])), ("exception", ValueError()), ("group", ExceptionGroup("g", [ValueError()])), ("type", int), ("class", D), ("memoryview", memoryview(b"a")), ("frozenset", frozenset()), ("coroutine type", type(None)), ("simple namespace", sys.implementation), ("version", sys.version_info), ("flags", sys.flags)]
for label, o in things:
    for kl, k in (("0", 0), ("'a'", "a"), ("Ix", Ix()), ("True", True), ("1.5", 1.5), ("slice", slice(0, 1)), ("big", 1 << 100)):
        t("del %s[%s]" % (label, kl), lambda: di(o, k)); t("set %s[%s]" % (label, kl), lambda: si(o, k))
