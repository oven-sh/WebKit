import sys
def tidy(text):
    # Addresses are not the same from one run to the next.
    out = []; i = 0
    while i < len(text):
        if text.startswith("0x", i):
            j = i + 2
            while j < len(text) and text[j] in "0123456789abcdef": j += 1
            out.append("0x"); i = j
        else:
            out.append(text[i]); i += 1
    return "".join(out)
def show(label, f):
    try:
        print(label, "=>", tidy(str(f())))
    except BaseException as e:
        print(label, "!!", type(e).__name__, tidy(str(e)))
def attempt(f):
    try: return f()
    except BaseException as e: return type(e).__name__ + ": " + str(e)
# ---- bool
show("bool operators", lambda: [(a & b, a | b, a ^ b) for a in (True, False) for b in (True, False, 1, 0, 2)])
show("bool reflected", lambda: [(b & a, b | a, b ^ a) for a in (True, False) for b in (1, 0, 2)])
show("bool methods", lambda: (True.__and__(False), True.__and__(1), True.__and__(1.5), True.__rand__(False), True.__rand__(3), True.__or__(False), False.__xor__(True), True.__ror__(2), True.__rxor__(True)))
show("bool own", lambda: sorted(n for n in vars(bool) if n != "__doc__"))
show("bool in place", lambda: (lambda: [x for x in [True] for x in [x & False]])())
class BI(int): pass
show("bool with a derived int", lambda: (True & BI(1), type(True & BI(1)).__name__, BI(1) & True))
# ---- bytes and bytearray
show("bytes rmod", lambda: (b"x".__rmod__(b"%s!"), b"x".__rmod__(bytearray(b"%s!")), b"x".__rmod__("%s"), b"x".__rmod__(1), bytearray(b"x").__rmod__(bytearray(b"%s!")), bytearray(b"x").__rmod__(b"%s!"), (1).__rmod__(b"%d") if False else 0))
show("bytes rmod tuple", lambda: b"%s %s" % (b"a", b"b"))
show("bytes str", lambda: (str(b"a'\x00"), b"a".__str__(), bytearray(b"a").__str__(), "__str__" in vars(bytes), "__str__" in vars(bytearray)))
show("alloc", lambda: (bytearray().__alloc__(), bytearray(b"a").__alloc__(), bytearray(b"abc").__alloc__(), bytearray(10).__alloc__()))
show("alloc wrong", lambda: bytearray().__alloc__(1))
# ---- memoryview
m = memoryview(b"abcabc")
show("memoryview repr", lambda: (repr(m), str(m)))
def released():
    x = memoryview(b"a"); x.release(); return x
show("memoryview released repr", lambda: repr(released()))
show("count", lambda: (m.count(97), m.count(100), m.count("a"), m.count(97.0), m.count(None), memoryview(b"").count(1)))
show("count wrong", lambda: m.count())
show("count released", lambda: released().count(1))
show("index", lambda: (m.index(97), m.index(98), m.index(97, 1), m.index(97, -3), m.index(99, 0, 3), m.index(97.0), m.index(97, 3, 100)))
for a in [(100,), (97, 4), (97, 1, 3), (97, 3, 1), (97, -1), (97, 100), ("a",), (97, 0, 0), (97, -100, -100)]:
    show("index" + repr(a), lambda: m.index(*a))
show("index wrong", lambda: m.index())
show("index wrong start", lambda: m.index(97, "a"))
show("index None start", lambda: m.index(97, None))
show("index big", lambda: m.index(97, -(1 << 70), 1 << 70))
show("index keywords", lambda: m.index(97, start=1))
show("index released", lambda: released().index(1))
show("index released with a bad start", lambda: released().index(1, "a"))
show("index of other formats", lambda: (memoryview(bytearray(8)).cast("i").index(0), memoryview(bytearray(8)).cast("i").count(0), memoryview(bytearray(8)).cast("d").count(0)))
show("from flags", lambda: (bytes(memoryview._from_flags(b"ab", 0)), memoryview._from_flags(b"ab", 0).readonly, memoryview._from_flags(bytearray(b"ab"), 1).readonly, bytes(memoryview._from_flags(m, 0))))
show("from flags writable", lambda: memoryview._from_flags(b"ab", 1))
show("from flags wrong", lambda: memoryview._from_flags(1, 0))
show("from flags wrong flags", lambda: memoryview._from_flags(b"", "a"))
show("from flags too few", lambda: memoryview._from_flags(b""))
# ---- complex.from_number
class HasComplex:
    def __complex__(s): return 3 + 4j
class HasFloat:
    def __float__(s): return 2.5
class HasIndex:
    def __index__(s): return 7
class CC(complex): pass
show("from_number", lambda: [complex.from_number(x) for x in (1, 1.5, 1j, True, HasComplex(), HasFloat(), HasIndex(), CC(1, 2), 1 << 60)])
show("from_number same", lambda: (lambda z: (complex.from_number(z) is z, complex.from_number(CC(1)) is not None, type(complex.from_number(CC(1))).__name__))(1j))
show("from_number derived", lambda: (type(CC.from_number(1)).__name__, CC.from_number(1j), type(CC.from_number(CC(2))).__name__))
for bad in ("1", None, [], b"1", "1j"):
    show("from_number " + repr(bad), lambda: complex.from_number(bad))
show("from_number too big", lambda: complex.from_number(1 << 2000))
show("from_number nothing", lambda: complex.from_number())
show("from_number kind", lambda: (type(vars(complex)["from_number"]).__name__, complex.from_number.__self__ is complex))
# ---- super
class A:
    def f(s): return "A.f"
    @classmethod
    def c(cls): return "A.c of " + cls.__name__
class B(A):
    def f(s): return "B.f then " + super().f()
    def me(s): return super()
    @classmethod
    def c(cls): return "B.c then " + super().c()
    @classmethod
    def mec(cls): return super()
b = B()
show("super repr", lambda: (repr(b.me()), repr(B.mec()), repr(super(B)), repr(super(B, b)), repr(super(B, B)), repr(super(A, b)), str(super(B))))
show("super without init", lambda: repr(super.__new__(super)))
show("super fields", lambda: [(s.__thisclass__, s.__self__ is b or s.__self__, s.__self_class__) for s in (super(B, b), super(B), super(B, B))])
show("super of None", lambda: (repr(super(B, None)), super(B, None).__self__))
for a in [(B, 1), (B, A()), (B, A), (B, int), (int, "s"), (B, object())]:
    show("super" + tidy(repr(a)), lambda: super(*a))
show("super wrong", lambda: super(1))
show("super wrong 2", lambda: super(1, 2))
show("super three", lambda: super(B, b, 1))
show("super keywords", lambda: super(type=B))
show("super outside", lambda: super())
class Proxy:
    __class__ = property(lambda s: B)
show("super of a stand-in", lambda: (repr(super(B, Proxy())), super(B, Proxy()).__self_class__))
class U:
    sup = super(B)
class UB(B):
    sup = super(B)
show("unbound super as an attribute", lambda: (repr(UB().sup), UB().sup.f(), repr(UB.sup)))
show("unbound super of another class", lambda: U().sup)
show("super get", lambda: (repr(super(B).__get__(b)), repr(super(B).__get__(b, B)), super(B).__get__(None, B) is not None, repr(super(B, b).__get__(B())), super(B, b).__get__(1) is not None))
show("super get nothing", lambda: super(B).__get__(None))
show("super get none", lambda: super(B).__get__())
class MySuper(super):
    def __init__(s, *a): log.append(a and a[0].__name__); super().__init__(*a)
log = []
show("derived super get", lambda: (type(MySuper(B).__get__(b)).__name__, log))
show("super own", lambda: sorted(n for n in vars(super) if n != "__doc__"))
# ---- staticmethod and classmethod
def fn(): pass
show("wrappers repr", lambda: (repr(staticmethod(fn)), repr(classmethod(fn)), repr(staticmethod(1)), repr(classmethod("s")), str(staticmethod(len))))
class SM(staticmethod): pass
class CM(classmethod): pass
show("derived wrappers repr", lambda: (repr(SM(fn)), repr(CM(fn))))
show("wrappers without init", lambda: (attempt(lambda: repr(staticmethod.__new__(staticmethod))), attempt(lambda: repr(classmethod.__new__(classmethod)))))
# ---- methods
show("method get", lambda: (b.f.__get__(1) is not None, b.f.__get__(1).__self__ is b, b.f.__get__(None, int).__self__ is b, b.f.__get__(1)() , "__get__" in vars(type(b.f))))
show("method get nothing", lambda: b.f.__get__(None))
show("method get none", lambda: b.f.__get__())
class Holder:
    bound = b.f
show("a method as an attribute", lambda: (Holder().bound(), Holder.bound(), Holder().bound.__self__ is b))
# ---- modules
M = type(sys)
show("module", lambda: (lambda x: (x.__name__, x.__doc__, sorted(vars(x)), repr(x)))(M("m", "d")))
show("module without init", lambda: (lambda x: (vars(x), attempt(lambda: x.__name__), repr(x), dir(x)))(M.__new__(M)))
show("module new takes anything", lambda: vars(M.__new__(M, 1, 2, x=3)))
show("module init again", lambda: (lambda x: (setattr(x, "y", 1), setattr(x, "__spec__", "s"), x.__init__("n"), sorted(vars(x).items(), key=str)))(M("m", "d")))
show("module init wrong", lambda: M("m").__init__())
show("module init wrong 2", lambda: M("m").__init__(1))
show("module init None", lambda: M(None))
show("module init keywords", lambda: (lambda x: (x.__name__, x.__doc__))(M(doc="d", name="n")))
class St(str): pass
show("module named by a derived str", lambda: type(M(St("m")).__name__).__name__)
class MM(M):
    def __init__(s, n): super().__init__(n + "!"); s.extra = 1
show("derived module", lambda: (MM("m").__name__, MM("m").extra, repr(MM("m")), type(MM("m")).__name__))
show("module getattribute", lambda: (M.__getattribute__(sys, "maxsize") == sys.maxsize, "__getattribute__" in vars(M), "__init__" in vars(M)))
show("module getattribute missing", lambda: M.__getattribute__(M("m"), "nope"))
def with_getattr():
    x = M("m"); x.__getattr__ = lambda n: "got " + n; return (x.nope, M.__getattribute__(x, "nope"), attempt(lambda: object.__getattribute__(x, "nope")))
show("module getattr", with_getattr)
show("module getattribute of another", lambda: M.__getattribute__(1, "real"))
# ---- tracebacks
def tb():
    try: 1 / 0
    except ZeroDivisionError as e: return e.__traceback__
T = type(tb())
t = tb()
show("traceback new", lambda: (lambda n: (n.tb_next, n.tb_frame is t.tb_frame, n.tb_lasti, n.tb_lineno))(T(None, t.tb_frame, 5, 42)))
show("traceback chained", lambda: (lambda n: (n.tb_next is t, n.tb_lineno))(T(t, t.tb_frame, 0, 7)))
show("traceback keywords", lambda: T(tb_next=None, tb_frame=t.tb_frame, tb_lasti=1, tb_lineno=2).tb_lineno)
for a in [(), (None,), (None, 1, 2, 3), (1, t.tb_frame, 2, 3), (None, t.tb_frame, "a", 3), (None, t.tb_frame, 2, "a"), (None, t.tb_frame, 2, 1.5), (None, t.tb_frame, 1 << 40, 3), (None, t.tb_frame, 2, -(1 << 40)), (None, t.tb_frame, 1 << 70, 3), (None, None, 1, 2), (1, 1, 1, 1), (None, t.tb_frame, 1, 2, 3)]:
    show("traceback" + repr(tuple("frame" if x is t.tb_frame else x for x in a)), lambda: T(*a))
show("traceback dir", lambda: (dir(t), t.__dir__()))
show("traceback negative line", lambda: T(None, t.tb_frame, 0, -5).tb_lineno)
show("traceback own", lambda: sorted(n for n in vars(T) if n != "__doc__"))
def raised_with():
    try: raise ValueError("x").with_traceback(T(None, t.tb_frame, 0, 3))
    except ValueError as e: return [(x.tb_lineno) for x in walk(e.__traceback__)][1:]
def walk(x):
    while x: yield x; x = x.tb_next
show("traceback in use", raised_with)
