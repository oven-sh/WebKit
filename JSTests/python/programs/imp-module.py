# The module _imp
import _imp
import marshal
import sys


def show(e):
    more = " | name=%r" % (e.name,) if isinstance(e, ImportError) else ""
    return type(e).__name__ + ": " + str(e) + more


def attempt(f, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return show(e)


def t(label, f):
    r = attempt(f)
    print(label, "=>", r if isinstance(r, str) else ascii(r))


# There is no loading what is written in C, so what is for that is not there, as it is not where CPython cannot either.
t("what there is", lambda: sorted(n for n in dir(_imp) if n not in ("create_dynamic", "exec_dynamic")))
t("the module", lambda: (_imp.__name__, _imp.__doc__, _imp.__package__, _imp.__loader__.__name__, _imp.__spec__.origin, _imp.check_hash_based_pycs, type(_imp.pyc_magic_number_token).__name__, _imp.pyc_magic_number_token.to_bytes(4, "little")[2:]))
for name in sorted(n for n in dir(_imp) if callable(getattr(_imp, n)) and n not in ("create_dynamic", "exec_dynamic", "__loader__")):
    t(name, lambda: (getattr(_imp, name).__text_signature__, getattr(_imp, name).__doc__))

print("---- the lock")
t("lock", lambda: (_imp.lock_held(), _imp.acquire_lock(), _imp.lock_held(), _imp.acquire_lock(), _imp.release_lock(), _imp.lock_held(), _imp.release_lock(), _imp.lock_held(), attempt(_imp.release_lock), attempt(_imp.lock_held, 1), attempt(_imp.acquire_lock, 1)))

print("---- what is written in C")
for arg in ("sys", "builtins", "_imp", "marshal", "_io", "os", "", "nothing", "SYS", "sys\0", 5, None, b"sys"):
    t("is_builtin(%r)" % (arg,), lambda: _imp.is_builtin(arg))


class Spec:
    def __init__(self, name): self.name = name


for arg in ("sys", "builtins", "marshal", "nothing", "", 5, None, b"sys"):
    t("create_builtin of %r" % (arg,), lambda: (lambda m: m if m is None else (type(m).__name__, m.__name__, m is sys.modules.get(m.__name__)))(_imp.create_builtin(Spec(arg))))
t("create_builtin of what has no name", lambda: (attempt(_imp.create_builtin, object()), attempt(_imp.create_builtin, None), attempt(_imp.create_builtin), attempt(_imp.create_builtin, Spec("sys"), 1)))
t("another each time", lambda: [(a is b, a.dumps is b.dumps, a.dumps(1) == b.dumps(1), a.__spec__, a.__loader__) for a in [_imp.create_builtin(Spec("marshal"))] for b in [_imp.create_builtin(Spec("marshal"))]])
t("exec_builtin", lambda: (_imp.exec_builtin(marshal), _imp.exec_builtin(sys), _imp.exec_builtin(5), _imp.exec_builtin(None), attempt(_imp.exec_builtin), attempt(_imp.exec_builtin, 1, 2)))
t("extension_suffixes", lambda: (type(_imp.extension_suffixes()).__name__, _imp.extension_suffixes() is _imp.extension_suffixes(), attempt(_imp.extension_suffixes, 1)))

print("---- what is frozen")
NAMES = ("_frozen_importlib", "_frozen_importlib_external", "importlib._bootstrap", "sys", "nothing", "", "_frozen_importlib\0", 5, None, b"_frozen_importlib")
for arg in NAMES:
    t("is_frozen(%r)" % (arg,), lambda: _imp.is_frozen(arg))
    t("is_frozen_package(%r)" % (arg,), lambda: _imp.is_frozen_package(arg))
    t("find_frozen(%r)" % (arg,), lambda: _imp.find_frozen(arg))
    t("get_frozen_object(%r)" % (arg,), lambda: (lambda c: (type(c).__name__, c.co_name, c.co_filename))(_imp.get_frozen_object(arg)))
    if arg not in ("_frozen_importlib", "_frozen_importlib_external"):
        t("init_frozen(%r)" % (arg,), lambda: _imp.init_frozen(arg))
t("with the data", lambda: [(type(data).__name__, data.readonly, data.format, len(data) > 1000, package, name, [(type(c).__name__, c.co_filename, c.co_name, "_find_and_load" in c.co_names) for c in [marshal.loads(data), _imp.get_frozen_object("_frozen_importlib", data), _imp.get_frozen_object("any name", data), _imp.get_frozen_object("any name", bytes(data))]]) for data, package, name in [_imp.find_frozen("_frozen_importlib", withdata=True)]])
t("withdata", lambda: (_imp.find_frozen("_frozen_importlib", withdata=False), _imp.find_frozen("_frozen_importlib", withdata=0), type(_imp.find_frozen("_frozen_importlib", withdata=[1])[0]).__name__, attempt(_imp.find_frozen, "_frozen_importlib", True), attempt(lambda: _imp.find_frozen("_frozen_importlib", other=1)), attempt(lambda: _imp.find_frozen(name="_frozen_importlib")), attempt(_imp.find_frozen)))
for label, data in (("b''", b""), ("what is not marshalled", b"nonsense"), ("an int, marshalled", marshal.dumps(5)), ("None, marshalled", marshal.dumps(None)), ("a str", "abc"), ("an int", 5), ("a list", [1]), ("a bytearray", bytearray(marshal.dumps(compile("1", "f", "eval")))), ("cut short", marshal.dumps(compile("1", "f", "eval"))[:-3])):
    t("get_frozen_object with " + label, lambda: type(_imp.get_frozen_object("the name", data)).__name__)
t("get_frozen_object()", lambda: (attempt(_imp.get_frozen_object), attempt(_imp.get_frozen_object, "a", None, 1), type(_imp.get_frozen_object("_frozen_importlib", None)).__name__))
t("_frozen_module_names", lambda: [(type(names).__name__, names[:2], all(isinstance(n, str) for n in names), all(_imp.is_frozen(n) for n in names), _imp._frozen_module_names() is names, attempt(_imp._frozen_module_names, 1)) for names in [_imp._frozen_module_names()]])
t("without those that can be done without", lambda: [(_imp._override_frozen_modules_for_tests(-1), _imp._frozen_module_names()[:2], len(_imp._frozen_module_names()) <= len(before), _imp.is_frozen("_frozen_importlib"), _imp._override_frozen_modules_for_tests(1), len(_imp._frozen_module_names()) == len(before), _imp._override_frozen_modules_for_tests(0), _imp._frozen_module_names() == before) for before in [_imp._frozen_module_names()]])
for arg in ("a", None, 1.5, 2 ** 40):
    t("_override_frozen_modules_for_tests(%r)" % (arg,), lambda: _imp._override_frozen_modules_for_tests(arg))
t("_override_multi_interp_extensions_check", lambda: (attempt(_imp._override_multi_interp_extensions_check, 0), attempt(_imp._override_multi_interp_extensions_check, "a"), attempt(_imp._override_multi_interp_extensions_check)))

print("---- source_hash")
for key, source in ((0, b""), (0, b"a"), (1, b"a"), (-1, b"a"), (3627, b"print('hello')\n"), (2 ** 63 - 1, b"abcdefg"), (-2 ** 63, b"abcdefgh"), (5, b"abcdefghi"), (5, bytes(range(256))), (5, bytearray(b"xyz")), (5, memoryview(b"xyz")), (True, b"x")):
    t("source_hash(%r, %r)" % (key, bytes(source)[:20]), lambda: _imp.source_hash(key, source))
t("of every length", lambda: [_imp.source_hash(7, bytes(range(n))).hex() for n in range(20)])
for args, kwargs in (((), {}), ((1,), {}), ((1, "a"), {}), ((1, 5), {}), ((1, None), {}), (("a", b"a"), {}), ((1.5, b"a"), {}), ((2 ** 63, b"a"), {}), ((None, b"a"), {}), ((1, b"a", 2), {}), ((), {"key": 1, "source": b"a"}), ((1,), {"source": b"a"}), ((1, b"a"), {"other": 1})):
    t("source_hash(*%r, **%r)" % (args, kwargs), lambda: _imp.source_hash(*args, **kwargs))

print("---- _fix_co_filename")
SOURCE = "def f():\n    def g(): pass\n    return g\nclass C:\n    def m(self): pass\nh = lambda: (x for x in ())\n"


def names(code):
    return [code.co_filename] + [n for c in code.co_consts if isinstance(c, type(code)) for n in names(c)]


t("it and all that is in it", lambda: [(names(code), _imp._fix_co_filename(code, "new"), names(code)) for code in [compile(SOURCE, "old", "exec")]])
t("what has been made from it", lambda: [(exec(code, space), _imp._fix_co_filename(code, "new"), space["f"].__code__.co_filename, space["f"]().__code__.co_filename, space["C"].m.__code__.co_filename) for code in [compile(SOURCE, "old", "exec")] for space in [{}]])
t("what has been read", lambda: [(names(code), _imp._fix_co_filename(code, "new"), names(code)) for code in [marshal.loads(marshal.dumps(compile(SOURCE, "old", "exec")))]])
t("the same name", lambda: [(_imp._fix_co_filename(code, "old"), names(code)[:2]) for code in [compile(SOURCE, "old", "exec")]])

def attempt_exception(code):
    try:
        exec(code)
    except ZeroDivisionError as e:
        return e


t("in a traceback", lambda: [(_imp._fix_co_filename(code, "renamed.py"), attempt_exception(code).__traceback__.tb_next.tb_frame.f_code.co_filename) for code in [compile("1 / 0", "old", "exec")]])
code = compile("1", "old", "eval")
for args in ((), (code,), (code, 5), (code, None), (code, b"new"), (5, "new"), (None, "new"), ("code", "new"), (code, "new", 1), (code, ""), (code, "a\0b"), (code, "\ud800")):
    t("_fix_co_filename%r" % (tuple("code" if a is code else a for a in args),), lambda: (_imp._fix_co_filename(*args), code.co_filename, type(code.co_filename).__name__))
