import sys
def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
def attempt(f):
    try: return f()
    except BaseException as e: return type(e).__name__ + ": " + str(e)
class Err:
    def __init__(s): s.text = []
    def write(s, t): s.text.append(t)
    def flush(s): pass
def captured(f):
    old = sys.stderr; sys.stderr = e = Err()
    try: r = f()
    finally: sys.stderr = old
    return (r, "".join(e.text))
# ---- the limit on digits
show("limit", lambda: (sys.get_int_max_str_digits(), sys.int_info.default_max_str_digits, sys.int_info.str_digits_check_threshold))
big = 10 ** 4300; ok = 10 ** 4299
show("str", lambda: (len(str(ok)), len(repr(-ok)), attempt(lambda: str(big)), attempt(lambda: repr(-big)), attempt(lambda: str(10 ** 100000))))
show("format", lambda: [attempt(f) if not isinstance(attempt(f), str) or attempt(f).startswith("ValueError") else len(attempt(f)) for f in (lambda: "%d" % big, lambda: f"{big}", lambda: format(big, "d"), lambda: format(big, ","), lambda: "{}".format(big), lambda: "%s" % big, lambda: "%r" % big, lambda: format(big, "x"), lambda: hex(big), lambda: oct(big), lambda: bin(big), lambda: "%x" % big, lambda: format(ok, "d"), lambda: format(big, "e"))])
show("int", lambda: (int("1" * 4300) > 0, attempt(lambda: int("1" * 4301)), attempt(lambda: int("-" + "1" * 4301)), attempt(lambda: int(" " + "1" * 4301 + " ")), attempt(lambda: int("1_" * 4300 + "1")), int("1_" * 4299 + "1") > 0, attempt(lambda: int(b"1" * 4301)), attempt(lambda: int("1" * 5000, 10))))
show("other bases", lambda: [attempt(lambda: int("1" * 5000, b) > 0) for b in (2, 3, 4, 5, 7, 8, 9, 16, 32, 36)])
show("containers", lambda: [attempt(f)[:60] for f in (lambda: repr([big]), lambda: repr({1: big}), lambda: repr((big,)), lambda: repr(range(big)), lambda: str(slice(big)), lambda: repr({big}))])
show("literal", lambda: (attempt(lambda: eval("1" * 4300) > 0), attempt(lambda: eval("1" * 4301)), attempt(lambda: eval("0x" + "1" * 5000) > 0), attempt(lambda: eval("1_" * 4300 + "1")), attempt(lambda: compile("x = 1\ny = " + "1" * 4301, "f", "exec"))))
def literal_error():
    try: compile("x = 1\ny = " + "1" * 4301 + "\n", "f", "exec")
    except SyntaxError as e: return (e.lineno, e.offset, e.end_lineno, e.end_offset, e.filename, len(e.text or ""))
show("literal error", literal_error)
show("float and others are not limited", lambda: (float("1" * 5000), len(str(float(10 ** 300))), (big + 1 - big), big.bit_length(), big // ok, len(big.to_bytes(2000, "big")), hash(big) != 0))
for v in (0, 640, 639, 1, -1, 5000, "a", 1.5, None, 1 << 40, True):
    show("set " + repr(v), lambda: (sys.set_int_max_str_digits(v), sys.get_int_max_str_digits()))
sys.set_int_max_str_digits(700)
show("lower", lambda: (len(str(10 ** 699)), attempt(lambda: str(10 ** 700)), attempt(lambda: int("1" * 701)), attempt(lambda: eval("1" * 701))))
sys.set_int_max_str_digits(0)
show("none", lambda: (len(str(10 ** 20000)), int("1" * 20000) > 0, eval("1" * 20000) > 0))
sys.set_int_max_str_digits(4300)
show("set keyword", lambda: sys.set_int_max_str_digits(maxdigits=4300))
show("set nothing", lambda: sys.set_int_max_str_digits())
show("flags do not follow", lambda: sys.flags.int_max_str_digits)
# ---- audit hooks
events = []
def hook(event, args):
    if event.startswith("test.") or event in wanted: events.append((event, args))
wanted = set()
show("audit without hooks", lambda: (sys.audit("test.none"), sys.audit("test.none", 1, 2), sys.audit(5) if False else None))
show("audit nothing", lambda: sys.audit())
sys.addaudithook(hook)
show("audit", lambda: (sys.audit("test.a"), sys.audit("test.b", 1, "x"), list(events), events.clear()))
show("audit not a str", lambda: sys.audit(5))
show("audit keywords", lambda: sys.audit("test.k", x=1))
def refusing(event, args):
    if event == "sys.addaudithook" and refuse: raise refuse[0]("no more")
    if event == "test.raise": raise ValueError("from the hook")
refuse = []
sys.addaudithook(refusing)
show("hook raises", lambda: sys.audit("test.raise"))
events.clear()
refuse.append(RuntimeError)
show("refused", lambda: (sys.addaudithook(lambda e, a: events.append("never")), sys.audit("test.c"), list(events), events.clear()))
refuse[0] = KeyboardInterrupt
show("refused hard", lambda: sys.addaudithook(lambda e, a: None))
refuse.clear()
show("add nothing", lambda: sys.addaudithook())
show("add keyword", lambda: sys.addaudithook(hook=lambda e, a: None))
# ---- excepthook
def raised():
    try:
        raise ValueError("boom")
    except BaseException as e: return e
show("excepthook", lambda: (sys.excepthook is sys.__excepthook__, type(sys.excepthook).__name__, sys.excepthook.__name__))
show("excepthook prints", lambda: captured(lambda: (lambda e: sys.excepthook(type(e), e, e.__traceback__))(raised()))[1].replace(__file__, "FILE"))
show("excepthook without a traceback", lambda: captured(lambda: sys.excepthook(ValueError, ValueError("x"), None))[1])
show("excepthook gives a traceback", lambda: captured(lambda: sys.excepthook(ValueError, ValueError("x"), raised().__traceback__))[1].replace(__file__, "FILE"))
show("excepthook of what is not one", lambda: captured(lambda: sys.excepthook(int, 5, None))[1])
show("excepthook of None", lambda: captured(lambda: sys.excepthook(None, None, None))[1])
show("excepthook wrong", lambda: sys.excepthook(1))
# ---- unraisablehook
U = []
def keep(u): U.append(u)
class R:
    def __buffer__(s, f): return memoryview(b"x")
    def __release_buffer__(s, m):
        raise KeyError("in release")
sys.unraisablehook = keep
bytes(R())
sys.unraisablehook = sys.__unraisablehook__
u = U[0]
show("unraisable", lambda: (type(u).__name__, type(u).__module__, len(u), u.exc_type, repr(u.exc_value), type(u.exc_traceback).__name__, u.err_msg, u.object, type(u).__match_args__))
show("unraisable default", lambda: captured(lambda: bytes(R()))[1].replace(__file__, "FILE"))
UA = type(u)
show("unraisable made", lambda: captured(lambda: sys.unraisablehook(UA((ValueError, ValueError("v"), None, None, None))))[1])
show("unraisable with a message", lambda: captured(lambda: sys.unraisablehook(UA((ValueError, ValueError("v"), None, "the message", None))))[1])
show("unraisable with an object", lambda: captured(lambda: sys.unraisablehook(UA((ValueError, ValueError("v"), None, None, [1, 2]))))[1])
show("unraisable with both", lambda: captured(lambda: sys.unraisablehook(UA((ValueError, ValueError("v"), None, "the message", [1, 2]))))[1])
show("unraisable without a value", lambda: captured(lambda: sys.unraisablehook(UA((ValueError, None, None, None, None))))[1])
show("unraisable without a type", lambda: captured(lambda: sys.unraisablehook(UA((None, None, None, "m", None))))[1])
class BadRepr:
    def __repr__(s): raise RuntimeError("no repr")
class BadStr(Exception):
    def __str__(s): raise RuntimeError("no str")
show("unraisable that cannot be shown", lambda: captured(lambda: sys.unraisablehook(UA((BadStr, BadStr(), None, None, BadRepr()))))[1])
class Mine(Exception): pass
Mine.__module__ = "some.where"
show("unraisable from a module", lambda: captured(lambda: sys.unraisablehook(UA((Mine, Mine("m"), None, None, None))))[1])
show("unraisable wrong", lambda: sys.unraisablehook((1, 2, 3, 4, 5)))
def failing(u):
    raise OSError("hook failed")
sys.unraisablehook = failing
show("hook fails", lambda: captured(lambda: bytes(R()))[1].replace(__file__, "FILE").replace(hex(id(failing)), "0x"))
sys.unraisablehook = None
show("hook is None", lambda: captured(lambda: bytes(R()))[1].replace(__file__, "FILE"))
del sys.unraisablehook
show("hook is gone", lambda: captured(lambda: bytes(R()))[1].replace(__file__, "FILE"))
sys.unraisablehook = sys.__unraisablehook__
# ---- breakpoint
calls = []
sys.breakpointhook = lambda *a, **k: calls.append((a, k)) or "returned"
show("breakpoint", lambda: (breakpoint(), breakpoint(1, 2, x=3), calls))
del sys.breakpointhook
show("breakpoint lost", lambda: breakpoint())
sys.breakpointhook = sys.__breakpointhook__
show("breakpoint kinds", lambda: (type(breakpoint).__name__, breakpoint.__name__, breakpoint.__module__, sys.breakpointhook.__name__))
# ---- objects
show("intern", lambda: (sys.intern("abc"), sys.intern("a" + "b") is sys.intern("ab"), sys._is_interned(sys.intern("xyzzy"))))
class S(str): pass
show("intern wrong", lambda: [attempt(lambda: sys.intern(x)) for x in (1, None, b"a", S("a"))])
show("is_interned wrong", lambda: sys._is_interned(1))
class C: pass
class Sl:
    __slots__ = ("a",)
show("getsizeof", lambda: [sys.getsizeof(x) for x in (0, 2 ** 70, "", "abc", b"", (), (1, 2), [], [1], {}, set(), None, 1.5, object(), C(), Sl(), int, C, range(1), 1j, b"abc", bytearray(b"a"), frozenset())])
class NoSize:
    __sizeof__ = None
class BadSize:
    def __init__(s, v): s.v = v
    def __sizeof__(s): return s.v
show("getsizeof default", lambda: (sys.getsizeof(BadSize("x"), "d"), sys.getsizeof(1, "d"), sys.getsizeof(BadSize(5)) - sys.getsizeof(BadSize(0)), sys.getsizeof(object=1), sys.getsizeof(BadSize("x"), default=7)))
for v in ("x", -1, 1.5, 1 << 70, None, True):
    show("getsizeof of " + repr(v), lambda: sys.getsizeof(BadSize(v)) > 0)
show("getsizeof -1 with a default", lambda: sys.getsizeof(BadSize(-1), 5))
show("getsizeof None method", lambda: sys.getsizeof(NoSize()))
show("getsizeof nothing", lambda: sys.getsizeof())
show("refcount", lambda: (type(sys.getrefcount(object())).__name__, sys.getrefcount(object()) > 0, sys.getrefcount(None) > 1000))
# ---- settings
show("recursion limit", lambda: (sys.getrecursionlimit(), sys.setrecursionlimit(2000), sys.getrecursionlimit(), sys.setrecursionlimit(1000)))
for v in (0, -1, "a", 1.5, 1 << 40):
    show("recursion limit " + repr(v), lambda: sys.setrecursionlimit(v))
show("switch interval", lambda: (sys.getswitchinterval(), sys.setswitchinterval(0.5), sys.getswitchinterval(), sys.setswitchinterval(1), sys.getswitchinterval(), sys.setswitchinterval(0.005)))
for v in (0, -1, "a"):
    show("switch interval " + repr(v), lambda: sys.setswitchinterval(v))
show("encodings", lambda: (sys.getdefaultencoding(), sys.getfilesystemencoding(), sys.getfilesystemencodeerrors()))
show("odds", lambda: (sys.is_finalizing(), sys.is_stack_trampoline_active(), sys.deactivate_stack_trampoline(), sys._is_gil_enabled(), sys._get_cpu_count_config(), type(sys.getallocatedblocks()).__name__, sys.call_tracing(divmod, (7, 2)), sys._jit.is_available() in (True, False), sys._jit.is_enabled(), sys._jit.is_active(), sys._clear_type_cache() if False else None, sys._clear_internal_caches()))
show("call_tracing wrong", lambda: sys.call_tracing(divmod, [7, 2]))
show("current", lambda: (len(sys._current_frames()), [type(f).__name__ for f in sys._current_frames().values()], list(sys._current_exceptions().values())))
def handling():
    try: 1 / 0
    except ZeroDivisionError: return [repr(v) for v in sys._current_exceptions().values()]
show("current while handling", handling)
show("data", lambda: (sys.maxsize, sys.maxunicode, sys.byteorder, sys.float_repr_style, sys.api_version, sys.abiflags, sys.platlibdir, sys.warnoptions, sys._xoptions, sys.pycache_prefix, sys.dont_write_bytecode, type(sys.orig_argv).__name__, sys.copyright.split("\n")[0]))
# ---- coroutines and asynchronous generators
async def co(): pass
def make(): return co()
def outer(): return make()
show("origin off", lambda: (sys.get_coroutine_origin_tracking_depth(), close(outer())))
def close(c):
    o = c.cr_origin; c.close()
    return o and tuple((f.replace(__file__, "FILE"), n) for f, l, n in o)
show("origin off", lambda: (sys.get_coroutine_origin_tracking_depth(), close(outer())))
for depth in (1, 2, 3, 100):
    sys.set_coroutine_origin_tracking_depth(depth)
    show("origin " + str(depth), lambda: close(outer())[:4])
def with_lines():
    c = co(); o = c.cr_origin; c.close(); return o[0][1] - with_lines.__code__.co_firstlineno
show("origin line", with_lines)
sys.set_coroutine_origin_tracking_depth(0)
for v in (-1, "a", 1 << 40):
    show("origin depth " + repr(v), lambda: sys.set_coroutine_origin_tracking_depth(v))
show("origin keyword", lambda: sys.set_coroutine_origin_tracking_depth(depth=0))
async def ag():
    yield 1
    yield 2
seen = []
show("hooks", lambda: (sys.get_asyncgen_hooks(), sys.get_asyncgen_hooks().firstiter, len(sys.get_asyncgen_hooks())))
first = lambda g: seen.append(("first", type(g).__name__))
final = lambda g: seen.append(("final", type(g).__name__))
sys.set_asyncgen_hooks(first, final)
show("hooks set", lambda: (sys.get_asyncgen_hooks().firstiter is first, sys.get_asyncgen_hooks().finalizer is final))
def drive(aw):
    try: aw.send(None)
    except StopIteration as e: return ("value", e.value)
    except StopAsyncIteration: return "end"
def used():
    g = ag(); a = list(seen); r = drive(g.__anext__()); b = list(seen); drive(g.__anext__()); drive(g.asend(None)); c = list(seen); seen.clear()
    return (a, r, b, c)
show("first iteration", used)
for name, use in [("asend", lambda g: g.asend(None)), ("athrow", lambda g: g.athrow(ValueError)), ("aclose", lambda g: g.aclose()), ("anext", lambda g: anext(g))]:
    def once():
        g = ag(); aw = use(g); r = list(seen); seen.clear(); attempt(lambda: aw.close()); return r
    show("first by " + name, once)
sys.set_asyncgen_hooks(firstiter=None)
show("one unset", lambda: (sys.get_asyncgen_hooks().firstiter, sys.get_asyncgen_hooks().finalizer is final))
sys.set_asyncgen_hooks(finalizer=None)
show("both unset", lambda: sys.get_asyncgen_hooks())
show("hooks nothing", lambda: (sys.set_asyncgen_hooks(), sys.get_asyncgen_hooks()))
show("hooks wrong", lambda: sys.set_asyncgen_hooks(1))
show("hooks wrong 2", lambda: sys.set_asyncgen_hooks(None, 1))
show("hooks wrong both", lambda: sys.set_asyncgen_hooks(1, 2))
show("hooks too many", lambda: sys.set_asyncgen_hooks(None, None, None))
def failing_first(g): raise ValueError("first failed")
sys.set_asyncgen_hooks(failing_first)
show("first fails", lambda: ag().__anext__())
sys.set_asyncgen_hooks(None, None)
