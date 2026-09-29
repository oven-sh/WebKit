# What there is when a program begins.
import sys

t = lambda label, f: print(label, "=>", ascii(f()).replace(__file__, "main.py").replace(sys.path[0], "HERE"))
main = sys.modules["__main__"]
# What is written in C in CPython and has not been written here yet has something written in Python in its place, or is not needed to begin with. What the engine has of its own is its own.
OURS = {"_frame", "_framelocals", "js", "_py_abc", "_weakrefset"}
THEIRS = {"_abc", "_stat", "_signal", "time", "zipimport", "_distutils_hack", "sitecustomize", "usercustomize"}
EXPECTED = ["__main__", "_codecs", "_collections_abc", "_frozen_importlib", "_frozen_importlib_external", "_imp", "_io", "_sitebuiltins", "_thread", "_warnings", "_weakref", "abc", "builtins", "codecs", "encodings", "encodings.aliases", "encodings.utf_8", "errno", "genericpath", "marshal", "os", "os.path", "posix", "posixpath", "site", "stat", "sys"]
t("what has been imported", lambda: [name for name in EXPECTED if name not in sys.modules])
t("__main__", lambda: (main.__name__, main.__file__, main.__cached__, main.__package__, main.__spec__, main.__doc__, type(main.__loader__).__name__, main.__loader__.name, main.__loader__.path, main.__builtins__ is sys.modules["builtins"], sorted(k for k in vars(main) if k.startswith("__"))))
t("its loader can be asked for it", lambda: (main.__loader__.get_filename(), main.__loader__.get_data(__file__)[:38], main.__loader__.is_package("__main__"), type(main.__loader__.get_code("__main__")).__name__))
t("sys.path[0]", lambda: (sys.path[0], __file__.startswith(sys.path[0])))
for name in ("stdin", "stdout", "stderr"):
    t("sys." + name, lambda: [(type(f).__name__, f.name, f.mode, f.encoding, f.errors, f.newlines, f.fileno(), f.closed, f.readable(), f.writable(), f is getattr(sys, "__%s__" % name), repr(f), type(getattr(f.buffer, "raw", f.buffer)).__name__, getattr(f.buffer, "raw", f.buffer).name, getattr(f.buffer, "raw", f.buffer).mode, getattr(f.buffer, "raw", f.buffer).closefd) for f in [getattr(sys, name)]])
t("what is read is buffered", lambda: (type(sys.stdin.buffer).__name__, sys.stdin.line_buffering))
t("open", lambda: (open is sys.modules["_io"].open, open is __import__("io").open, open.__module__, open.__name__, open.__qualname__))
t("what site adds", lambda: [(name, type(getattr(sys.modules["builtins"], name)).__name__, type(getattr(sys.modules["builtins"], name)).__module__) for name in ("exit", "quit", "copyright", "credits", "license", "help")])
t("exit", lambda: (repr(exit), repr(quit)))
t("encodings", lambda: (sys.getdefaultencoding(), sys.getfilesystemencoding(), sys.getfilesystemencodeerrors(), sys.flags.utf8_mode))
t("codecs by name", lambda: [("h\xe9".encode(name, "replace"), b"h\xe9".decode(name, "replace")) for name in ("utf-8", "UTF8", "u8", "latin-1", "L1", "iso-8859-15", "cp1252", "cp437", "utf-16-le", "utf_8_sig", "mac-roman", "koi8-r", "cp850", "utf-7", "unicode_escape", "raw-unicode-escape", "utf-32-be", "ascii", "646")])
t("codecs that are not of text", lambda: __import__("codecs").encode("abc", "rot13"))


def _catch(f):
    try:
        f()
    except Exception as e:
        return e


t("no such codec", lambda: [(type(e).__name__, str(e)) for e in [_catch(lambda: "a".encode("no-such-codec")), _catch(lambda: b"a".decode("no such")), _catch(lambda: "a".encode("rot13")), _catch(lambda: open(__file__, encoding="no-such"))]])
t("open, with nothing said about how", lambda: [(f.encoding, f.errors, f.mode, f.name, f.line_buffering, type(f.buffer).__name__, f.readline(), f.close()) for f in [open(__file__)]])
t("finders", lambda: [f.__name__ for f in sys.meta_path if getattr(f, "__module__", "").startswith("_frozen_importlib")])
t("importlib", lambda: [(m.__name__, m._bootstrap is sys.modules["_frozen_importlib"], m._bootstrap.__name__, m._bootstrap.__package__, m._bootstrap_external.__name__, sys.modules["importlib._bootstrap"] is m._bootstrap, m.import_module("os.path").__name__) for m in [__import__("importlib")]])
t("__future__", lambda: [(m.__name__, m.annotations, m.__file__.rpartition("/")[2], m.__doc__[:40]) for m in [__import__("__future__")]])
t("sys.flags", lambda: {k: getattr(sys.flags, k) for k in ("debug", "inspect", "interactive", "optimize", "no_user_site", "no_site", "ignore_environment", "verbose", "bytes_warning", "quiet", "isolated", "dev_mode", "safe_path", "int_max_str_digits", "warn_default_encoding")})
t("what is known of the library", lambda: (type(sys.stdlib_module_names).__name__, len(sys.stdlib_module_names), sorted(sys.stdlib_module_names)[:5], "os" in sys.stdlib_module_names, "numpy" in sys.stdlib_module_names))
t("sys._getframemodulename", lambda: (sys._getframemodulename(), sys._getframemodulename(0), sys._getframemodulename(1), sys._getframemodulename(2), sys._getframemodulename(100), sys._getframemodulename(depth=1), (lambda: sys._getframemodulename(-1))()))
sys.stdout.write("written to sys.stdout\n")
sys.stdout.flush()
sys.stdout.buffer.write(b"written to sys.stdout.buffer\n")
sys.stdout.flush()
print("printed", file=sys.stdout, flush=True)
