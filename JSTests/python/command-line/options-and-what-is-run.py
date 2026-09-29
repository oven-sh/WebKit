# The command line of `python`, and what it looks at in its environment. Each thing is tried in a process of its own, which is started as this was: by sys.executable.
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.realpath(tempfile.mkdtemp())
os.chdir(HERE)
# What is not the same from one Python to another, or from one place to another
PLACES = sorted({os.path.realpath(sys.prefix), sys.prefix, os.path.realpath(sys.base_prefix)}, key=len, reverse=True)


def tidy(text):
    text = text.replace(HERE, "HERE")
    for place in PLACES:
        text = text.replace(place, "PREFIX")
    text = re.sub(r" at 0x[0-9a-f]+", " at 0x", text)
    text = re.sub(r"Current thread 0x[0-9a-f]+", "Current thread 0x", text)
    text = re.sub(r"(?m)^Python 3\.14\.\d+$", "Python 3.14.N", text)
    # What is in the library is in the program itself in one Python, which then has no source to show for it, and in a file in another. It is not what is being looked at.
    text = re.sub(r'(?m)^  File "(?:<frozen [\w.]+>|PREFIX/[^"]+)", line \d+, in .*\n(?:    .*\n)*', "  (in the library)\n", text)
    text = re.sub(r"(?:  \(in the library\)\n)+", "  (in the library)\n", text)
    text = re.sub(r"(?m)^usage: \S+ ", "usage: PYTHON ", text)
    text = re.sub(r"(?m)^\S*[Pp]ython[\d.]*: (can't open file|can't find|Error while finding|No module named|No code object|'HERE)", r"PYTHON: \1", text)
    text = re.sub(r"(?m)^\S*/[Pp]ython[\d.]*: ", "PYTHON: ", text)
    return text


def run(*arguments, env={}, input=None, cwd=None):
    "What comes of `python arguments`: the status it ends with, what it writes, and what it says has gone wrong"
    environment = {"PATH": os.environ.get("PATH", ""), "HOME": HERE, "LC_ALL": "en_US.UTF-8", "PYTHONUTF8": "1"}
    environment.update(env)
    done = subprocess.run([sys.executable, *arguments], env={k: v for k, v in environment.items() if v is not None}, input=input, capture_output=True, text=True, errors="backslashreplace", cwd=cwd, timeout=60)
    return done.returncode, tidy(done.stdout), tidy(done.stderr)


def t(label, *arguments, **k):
    print(label, "=>", ascii(run(*arguments, **k)))


def write(name, text):
    os.makedirs(os.path.dirname(os.path.join(HERE, name)) or HERE, exist_ok=True)
    with open(os.path.join(HERE, name), "w") as f:
        f.write(text)
    return name


SHOW = "import sys; print(sys.argv, sys.path[0] if sys.path else None, __name__, globals().get('__file__'), type(__loader__).__name__ if isinstance(__loader__, object) and not isinstance(__loader__, type) else __loader__.__name__, __spec__ and __spec__.name, __package__)"

print("---- a command")
t("-c", "-c", SHOW)
t("with arguments", "-c", SHOW, "a", "-b", "--c", "-c", "d")
t("stuck to the option", "-c" + SHOW, "a")
t("after other options", "-B", "-s", "-c", SHOW, "-B")
t("options together", "-Bsc", SHOW, "x")
t("of several lines", "-c", "x = 1\nif x:\n    print('yes')\nprint(x)")
t("all indented", "-c", "    x = 1\n    if x:\n        print('yes')\n    print(x)")
t("indented with tabs", "-c", "\tx = 1\n\tprint(x)")
t("indented one way and another", "-c", "  x = 1\n\tprint(x)")
t("with blank lines among them", "-c", "    x = 1\n\n  \n    print(x)\n")
t("some not indented", "-c", "    x = 1\nprint(x)")
t("empty", "-c", "")
t("blank", "-c", "   ")
t("a comment", "-c", "# nothing")
t("not ASCII", "-c", "print('\xe9\u20ac\U0001F600', len('\U0001F600'))")
t("with a coding line, which is not gone by", "-c", "# coding: latin-1\nprint('\xe9')")
t("what is wrong with it", "-c", "x = = 1")
t("on the second line", "-c", "x = 1\ny = (")
t("what it raises", "-c", "def f():\n    return 1 / 0\nf()")
t("from what was indented", "-c", "    def f():\n        return 1 / 0\n    f()")
t("what it has for source", "-c", "import linecache, sys; print(linecache.getlines('<string>')); print(sys._getframe().f_code.co_filename)")
t("__main__", "-c", "import __main__, sys; print(sys.modules['__main__'] is __main__, sorted(vars(__main__)))")
t("none given", "-c")
print("---- how it ends")
for code in ("sys.exit()", "sys.exit(0)", "sys.exit(3)", "sys.exit(255)", "sys.exit(256)", "sys.exit(-1)", "sys.exit(2 ** 40)", "sys.exit(None)", "sys.exit(True)", "sys.exit('a message')", "sys.exit(['a', 1])", "sys.exit(1.5)", "sys.exit((3,))", "raise SystemExit", "raise SystemExit(4)", "raise SystemExit(4, 5)", "raise ValueError('v')", "raise KeyboardInterrupt", "os._exit(7)", "raise SystemExit('\xe9')",
             "class E(SystemExit): pass\nraise E(9)", "class E(Exception):\n    def __str__(self): return 1 / 0\nraise E", "sys.stdout.close()", "print('x'); sys.stdout = None", "print('x'); sys.stdout = open(os.devnull, 'w')", "sys.stderr = None; 1 / 0", "print('e', file=sys.stderr); sys.stderr = None", "import atexit; atexit.register(print, 'at exit'); sys.exit(2)", "import atexit; atexit.register(lambda: 1 / 0)", "os.close(1); print('x')", "sys.excepthook = lambda *a: print('hooked', a[0].__name__); 1 / 0", "sys.excepthook = lambda *a: 2 / 0; 1 / 0", "del sys.excepthook; 1 / 0"):
    t(code.replace("\n", "; "), "-c", "import sys, os\n" + code)

print("---- a file")
write("script.py", SHOW + "\n")
write("dir/inner.py", SHOW + "\n")
t("by its name", "script.py")
t("with arguments", "script.py", "a", "-b", "-c")
t("from here", "./script.py")
t("in a directory", "dir/inner.py")
t("up and down", "dir/../script.py")
t("by all of its name", os.path.join(HERE, "script.py"))
t("from elsewhere", "../script.py", cwd=os.path.join(HERE, "dir"))
os.symlink(os.path.join(HERE, "dir", "inner.py"), os.path.join(HERE, "link.py"))
os.symlink("dir/inner.py", os.path.join(HERE, "relative-link.py"))
t("by a link", "link.py")
t("by a link that is relative", "relative-link.py")
t("after --", "--", "script.py", "-x")
t("whose name begins with a dash", "--", write("-odd.py", SHOW + "\n"))
t("that is not there", "nope.py")
t("in a directory that is not there", "nope/nope.py")
t("that cannot be read", write("secret.py", "print(1)") and (os.chmod("secret.py", 0) or "secret.py"))
t("that is empty", write("empty.py", ""))
t("that does not end in .py", write("plain", "print('plain', __file__)"))
t("what is wrong with it", write("wrong.py", "x = 1\ny = = 2\n"))
t("what it raises", write("raises.py", "def f():\n    return 1 / 0\n\nf()\n"))
t("with a coding line", write("coded.py", "# coding: latin-1\nprint(ascii('\xe9'))\n"))
t("with a zero in it", write("zero.py", "print(1)\0"))
t("-x", "-x", write("first-line.py", "#!not python at all (\nimport sys; print(sys._getframe().f_lineno)\n"))
t("-x, of one line", "-x", write("one-line.py", "not python"))
t("not -x", "first-line.py")
os.mkdir("bare")
t("a directory", "bare")
write("package/__main__.py", SHOW + "\n")
write("package/helper.py", "value = 5\n")
t("a directory with __main__.py in it", "package", "a")
t("which finds what is beside it", write("package2/__main__.py", "import helper; print(helper.value)") and write("package2/helper.py", "value = 6\n") and "package2")
import zipfile
with zipfile.ZipFile("archive.zip", "w") as z:
    z.writestr("__main__.py", SHOW + "\nimport inside; print(inside.value)\n")
    z.writestr("inside.py", "value = 7\n")
t("an archive with __main__.py in it", "archive.zip", "a")
with zipfile.ZipFile("no-main.zip", "w") as z:
    z.writestr("other.py", "")
t("an archive without", "no-main.zip")

print("---- a module")
write("mod.py", SHOW + "\n")
write("pkg/__init__.py", "print('pkg is imported')\n")
write("pkg/__main__.py", SHOW + "\n")
write("pkg/sub.py", SHOW + "\n")
write("nomain/__init__.py", "")
t("-m", "-m", "mod")
t("with arguments", "-m", "mod", "a", "-b", "-m", "c")
t("stuck to the option", "-mmod", "a")
t("a package", "-m", "pkg")
t("in a package", "-m", "pkg.sub", "x")
t("a package with no __main__", "-m", "nomain")
t("that is not there", "-m", "nope")
t("in a package that is not there", "-m", "nope.sub")
t("in what is not a package", "-m", "mod.sub")
t("that is built in", "-m", "sys")
t("with .py on the end", "-m", "mod.py")
t("with no name", "-m", "")
t("relative", "-m", ".mod")
t("none given", "-m")
t("that raises", "-m", write("raising.py", "1 / 0\n")[:-3])
t("that is wrong", "-m", write("mistaken.py", "x = = 1\n")[:-3])
t("that exits", "-m", write("exiting.py", "import sys; sys.exit(5)\n")[:-3])
t("of the library", "-m", "json.tool", input='{"a": [1, 2]}')
t("-m and -c", "-m", "mod", "-c", "print(1)")
t("-c and -m", "-c", "print(1)", "-m", "mod")

print("---- what comes in")
t("with nothing said", input=SHOW + "\n")
t("-", "-", input=SHOW + "\n")
t("- and arguments", "-", "a", "-b", input=SHOW + "\n")
t("nothing at all", input="")
t("what is wrong with it", input="x = = 1\n")
t("what it raises", input="def f():\n    return 1 / 0\nf()\n")
t("that exits", input="import sys\nsys.exit(6)\n")
t("that reads the rest", input="import sys\nprint(repr(sys.stdin.read()))\nand this is not python\n")

print("---- options")
# hash_randomization and dont_write_bytecode are as this Python has them whatever is said, and utf8_mode is on unless it is turned off.
FLAGS = "import sys; f = sys.flags; print([(n, getattr(f, n)) for n in ('debug', 'inspect', 'interactive', 'optimize', 'no_user_site', 'no_site', 'ignore_environment', 'verbose', 'bytes_warning', 'quiet', 'isolated', 'dev_mode', 'warn_default_encoding', 'safe_path', 'int_max_str_digits', 'gil', 'thread_inherit_context', 'context_aware_warnings') if getattr(f, n)], sys.warnoptions, sys._xoptions, sys.argv)"
for options in ((), ("-b",), ("-bb",), ("-b", "-b", "-b"), ("-B",), ("-d",), ("-dd",), ("-E",), ("-I",), ("-O",), ("-OO",), ("-OOO",), ("-P",), ("-q",), ("-qq",), ("-R",), ("-s",), ("-S",), ("-t",), ("-u",), ("-x",), ("-bBdEOPqRsStux",), ("-IE",), ("-sE",), ("-Is",),
                ("-W", "error"), ("-Werror",), ("-W", "ignore", "-W", "default"), ("-W", "error", "-W", "error"), ("-Wd", "-b"), ("-W", "ignore::DeprecationWarning"), ("-W", ""), ("-b", "-W", "default::BytesWarning"),
                ("-X", "dev"), ("-Xdev",), ("-X", "a"), ("-X", "a=b"), ("-X", "a=b=c"), ("-X", "a="), ("-X", "=b"), ("-X", ""), ("-X", "a", "-X", "a=2"), ("-X", "a=1", "-X", "b"), ("-X", "dev", "-W", "error"), ("-X", "dev=0"), ("-X", "warn_default_encoding"), ("-X", "int_max_str_digits=0"), ("-X", "int_max_str_digits=640"), ("-X", "int_max_str_digits=100000"),
                ("-X", "thread_inherit_context=1"), ("-X", "context_aware_warnings=1"), ("-X", "thread_inherit_context=0"), ("-X", "gil=1"), ("-X", "importtime=0") if False else ("-X", "no_debug_ranges"), ("-X", "frozen_modules"), ("-X", "frozen_modules=on"), ("-X", "cpu_count=3"), ("-X", "cpu_count=default"), ("-X", "pycache_prefix=/x"), ("-X", "pycache_prefix="), ("-X", "pycache_prefix"), ("-X", "utf8"), ("-X", "utf8=1"), ("-X", "faulthandler") if False else ("-X", "showrefcount"), ("-X", "\xe9=\u20ac")):
    t(" ".join(options) or "none", *options, "-c", FLAGS)
t("after the command they are not options", "-c", FLAGS, "-O", "-X", "dev")
t("after the file they are not options", write("flags.py", FLAGS + "\n"), "-O", "-X", "dev")
t("--", "-O", "--", "flags.py", "-O")

print("---- what they do")
t("-O", "-O", "-c", "assert False, 'not seen'\nprint(__debug__)\ndef f():\n    'doc'\nprint(f.__doc__)")
t("-OO", "-OO", "-c", "assert False, 'not seen'\nprint(__debug__)\ndef f():\n    'doc'\nprint(f.__doc__)")
t("neither", "-c", "print(__debug__)\ndef f():\n    'doc'\nprint(f.__doc__)\nassert False, 'seen'")
t("what builtins has", "-c", "import builtins; print(builtins.__debug__, [(n, vars(builtins)[n]) for n in ('None', 'True', 'False', 'Ellipsis', 'NotImplemented')], getattr(builtins, 'None'))")
t("with -O", "-O", "-c", "import builtins; print(builtins.__debug__)")
t("-O, of what is imported", "-O", "-c", "import asserting", cwd=os.path.dirname(os.path.join(HERE, write("o/asserting.py", "assert False\nprint('imported', __debug__)\n"))))
t("-OO, of what is compiled", "-OO", "-c", "exec(compile('assert 0\\nprint(__debug__)', 'f', 'exec')); exec(compile('assert 0, 5', 'f', 'exec', optimize=0))")
t("-b", "-b", "-c", "print(str(b'a'), b'a' == 'a', bytearray(b'a') == 'a', b'a' == 1, {b'a': 1, 'a': 2} and 1)")
t("-bb", "-bb", "-c", "str(b'a')")
t("-bb, compared", "-bb", "-c", "b'a' == 'a'")
t("without", "-c", "print(str(b'a'), b'a' == 'a')")
t("-B", "-B", "-c", "import sys; print(sys.dont_write_bytecode)")
t("-E", "-E", "-c", "import sys; print(sys.flags.optimize, sys.warnoptions)", env={"PYTHONOPTIMIZE": "2", "PYTHONWARNINGS": "error"})
t("-I", "-I", "-c", "import sys; print(sys.flags.optimize, '' in sys.path, HERE in sys.path)".replace("HERE", repr(HERE)), env={"PYTHONOPTIMIZE": "2", "PYTHONPATH": HERE})
t("-P", "-P", "-c", "import sys; print('' in sys.path)")
t("-P, of a file", "-P", "script.py")
t("-P, of a module", "-P", "-m", "mod")
t("-P, of a directory", "-P", "package")
t("-s", "-s", "-c", "import site; print(site.ENABLE_USER_SITE)")
t("-S", "-S", "-c", "import sys; print('site' in sys.modules, hasattr(__builtins__, 'exit'), [p for p in sys.path if 'site-packages' in p])")
t("not -S", "-c", "import sys; print('site' in sys.modules, hasattr(__builtins__, 'exit'))")
STREAMS = "import sys; print([(s.name, s.encoding, s.errors, s.line_buffering, s.write_through, type(s.buffer).__name__) for s in (sys.stdin, sys.stdout, sys.stderr)])"
t("streams", "-c", STREAMS)
t("-u", "-u", "-c", STREAMS)
t("-u, in what order", "-u", "-c", "import sys, os; sys.stdout.write('a'); os.write(1, b'b'); sys.stdout.write('c')")
t("not -u", "-c", "import sys, os; sys.stdout.write('a'); os.write(1, b'b'); sys.stdout.write('c')")
t("-q", "-q", "-c", "print(1)")
t("-W error", "-W", "error", "-c", "import warnings; warnings.warn('w')")
t("-W ignore", "-W", "ignore", "-c", "import warnings; warnings.warn('w'); print('went on')")
t("-W default", "-W", "default", "-c", "import warnings\nfor i in range(2): warnings.warn('w', DeprecationWarning)")
t("-W always", "-W", "always", "-c", "import warnings\nfor i in range(2): warnings.warn('w', DeprecationWarning)")
t("-W once", "-W", "once", "-c", "import warnings\nfor i in range(2): warnings.warn('w')\nwarnings.warn('w')")
t("none", "-c", "import warnings; warnings.warn('d', DeprecationWarning); warnings.warn('u'); warnings.warn('p', PendingDeprecationWarning); warnings.warn('i', ImportWarning); warnings.warn('r', ResourceWarning)")
t("of a category", "-W", "error::DeprecationWarning", "-c", "import warnings; warnings.warn('u'); warnings.warn('d', DeprecationWarning)")
t("of a message", "-W", "error:this", "-c", "import warnings; warnings.warn('that'); warnings.warn('this one')")
t("of a module and a line", "-W", "error:::__main__:1", "-c", "import warnings; warnings.warn('one')")
t("the last wins", "-W", "error", "-W", "ignore", "-c", "import warnings; warnings.warn('w'); print('went on')")
for bad in ("nope", "error:::x:y", "error::Nope", "error::os.nope", "a:b:c:d:e:f", "error::int", ":", "error:::::", "e", "i", "ign", "error::builtins.DeprecationWarning", "error::nope.Nope"):
    t("-W " + bad, "-W", bad, "-c", "import warnings; print([f[:1] + f[2:3] for f in warnings.filters[:2]])")
FILTERS = "import warnings; print([(a, getattr(m, 'pattern', m), c.__name__, getattr(o, 'pattern', o), l) for a, m, c, o, l in warnings.filters])"
t("the filters", "-c", FILTERS)
t("the filters, with -X dev", "-X", "dev", "-c", FILTERS)
t("the filters, with -b", "-b", "-c", FILTERS)
t("the filters, with -W", "-Wi", "-We::UserWarning", "-c", FILTERS)
t("a warning about the source", "-W", "error", "-c", "x = 1 is 1")
t("and not raised", "-c", "x = 1 is 1\nprint(x)")
t("-X int_max_str_digits", "-X", "int_max_str_digits=640", "-c", "import sys; print(sys.get_int_max_str_digits()); print(len(str(10 ** 639))); str(10 ** 640)")
t("in the source too", "-X", "int_max_str_digits=640", "-c", "x = " + "1" * 641)
t("-X no_debug_ranges", "-X", "no_debug_ranges", write("ranges.py", "def f():\n    return 1 / 0\nf()\n"))
t("-X no_debug_ranges, in the code", "-X", "no_debug_ranges", "-c", "def f(): return 1 / 0\nprint(list(f.__code__.co_positions())[:3])")
t("-X warn_default_encoding", "-X", "warn_default_encoding", "-W", "default", "-c", "open(%r).close()" % os.path.join(HERE, "script.py"))
t("-X dev", "-X", "dev", "-c", "import sys, warnings; print(sys.flags.dev_mode); warnings.warn('d', DeprecationWarning); warnings.warn('r', ResourceWarning)")
CHECKS = """
import io
def t(f):
    try:
        print(repr(f()))
    except Exception as e:
        print(type(e).__name__, e)
t(lambda: b''.decode('nope'))
t(lambda: b''.decode('utf-8', 'nope'))
t(lambda: b'a'.decode('utf-8', 'nope'))
t(lambda: ''.encode('nope'))
t(lambda: ''.encode('utf-8', 'nope'))
t(lambda: 'a'.encode('ascii', 'nope'))
t(lambda: str(b'', 'nope'))
t(lambda: str(b'', 'utf-8', 'nope'))
t(lambda: str(bytearray(), 'latin-1', 'nope'))
t(lambda: bytes('', 'nope'))
t(lambda: bytearray('', 'utf-8', 'nope'))
t(lambda: b''.decode('latin-1', 'ignore'))
t(lambda: io.TextIOWrapper(io.BytesIO(), 'utf-8', 'nope').errors)
t(lambda: io.TextIOWrapper(io.BytesIO(), 'utf-8', None).errors)
t(lambda: open(__file__, errors='nope').errors)
"""
t("what is looked into with -X dev", "-X", "dev", write("checks.py", CHECKS))
t("and without", "checks.py")
t("-X cpu_count", "-X", "cpu_count=3", "-c", "import os; print(os.cpu_count(), os.process_cpu_count())")
t("-X pycache_prefix", "-X", "pycache_prefix=/some/where", "-c", "import sys, importlib.util; print(sys.pycache_prefix, importlib.util.cache_from_source('/a/b.py').startswith('/some/where/a/'))")
t("-X frozen_modules=off", "-X", "frozen_modules=off", "-c", "import os; print(os.__spec__.origin != 'frozen')")
def import_times(*arguments, **k):
    code, out, err = run(*arguments, **k)
    lines = err.split("\n")
    return code, lines[0], [re.sub(r" +\d+", " N", l) for l in lines if l.endswith(("timed", "timed.inner", "timed.other"))], all(re.fullmatch(r"import time: +(\d+|cached) +\| +(\d+|cached) +\| +[\w.]+", l) for l in lines[1:] if l)


write("timed/__init__.py", "from . import inner\nimport timed.other\nimport timed.inner\n")
write("timed/inner.py", "")
write("timed/other.py", "import timed.inner\n")
print("-X importtime =>", ascii(import_times("-X", "importtime", "-c", "import timed; import timed")))
print("-X importtime=1 =>", ascii(import_times("-X", "importtime=1", "-c", "import timed")))
print("-X importtime=2 =>", ascii(import_times("-X", "importtime=2", "-c", "import timed; import timed")))
print("-X importtime=x =>", ascii(import_times("-X", "importtime=x", "-c", "import timed")))
print("-X importtime=0 =>", ascii(run("-X", "importtime=0", "-c", "import timed")))
print("PYTHONPROFILEIMPORTTIME =>", ascii(import_times("-c", "import timed", env={"PYTHONPROFILEIMPORTTIME": "1"})))
print("of what is not there =>", ascii(import_times("-X", "importtime", "-c", "try:\n    import timed.nope\nexcept ImportError: pass")))

print("---- what is in the environment")
for name, values in (("PYTHONOPTIMIZE", ("1", "2", "0", "x", "-1", "", "5", " 1", "1 ")), ("PYTHONDEBUG", ("1", "3", "x")), ("PYTHONVERBOSE", ("0",)), ("PYTHONDONTWRITEBYTECODE", ("1", "0", "x")), ("PYTHONNOUSERSITE", ("1", "0", "x")), ("PYTHONUNBUFFERED", ("1", "0", "x")), ("PYTHONSAFEPATH", ("1", "0", "")), ("PYTHONDEVMODE", ("1", "0", "")), ("PYTHONWARNDEFAULTENCODING", ("1", "")),
                     ("PYTHONWARNINGS", ("error", "ignore,default", "error,,ignore", ",", "error::DeprecationWarning,ignore", " error")), ("PYTHONINTMAXSTRDIGITS", ("0", "640", "639", "x", "-1", "", "100000", "99999999999")), ("PYTHONHASHSEED", ("0", "1", "random", "4294967295", "4294967296", "x", "-1", "", " 1", "1x")), ("PYTHON_GIL", ("1", "0", "2", "x", "")),
                     ("PYTHON_CPU_COUNT", ("2", "default", "0", "x", "-1")), ("PYTHONTRACEMALLOC", ("x", "-1")), ("PYTHONPROFILEIMPORTTIME", ("3", "-1")), ("PYTHON_FROZEN_MODULES", ("on", "x", "")), ("PYTHON_THREAD_INHERIT_CONTEXT", ("1", "0", "2", "x")), ("PYTHON_CONTEXT_AWARE_WARNINGS", ("1", "0", "2", "x")), ("PYTHONUTF8", ("2", "x")), ("PYTHONNODEBUGRANGES", ("1",)), ("PYTHONPYCACHEPREFIX", ("/x",)), ("PYTHONPLATLIBDIR", ("lib64",))):
    for value in values:
        t("%s=%s" % (name, value), "-c", FLAGS + "; print(sys.pycache_prefix, sys.platlibdir)", env={name: value})
t("and the option as well", "-O", "-c", FLAGS, env={"PYTHONOPTIMIZE": "2"})
t("the more of the two", "-OOO", "-c", FLAGS, env={"PYTHONOPTIMIZE": "2"})
t("warnings from both", "-W", "ignore", "-c", FLAGS, env={"PYTHONWARNINGS": "error,default"})
t("the same from both", "-W", "error", "-c", FLAGS, env={"PYTHONWARNINGS": "error"})
t("digits from both", "-X", "int_max_str_digits=700", "-c", FLAGS, env={"PYTHONINTMAXSTRDIGITS": "800"})
t("wrong in one and right in the other", "-X", "int_max_str_digits=700", "-c", FLAGS, env={"PYTHONINTMAXSTRDIGITS": "x"})
t("PYTHONPATH", "-c", "import sys; print(sys.path[:4])", env={"PYTHONPATH": "/a:/b::/c"})
t("PYTHONPATH, empty", "-c", "import sys; print(sys.path[:2])", env={"PYTHONPATH": ""})
t("PYTHONPATH, used", "-c", "import helper; print(helper.value)", env={"PYTHONPATH": os.path.join(HERE, "package")})
for value in ("latin-1", "latin-1:replace", ":replace", "utf-8:", ":", "ascii:backslashreplace", "nope", "utf-8:nope", "UTF8", "ascii"):
    t("PYTHONIOENCODING=" + value, "-c", STREAMS + "; print('\\xe9')", env={"PYTHONIOENCODING": value})
t("PYTHONHOME, where there is nothing", "-c", "print(1)", env={"PYTHONHOME": "/nowhere"})
t("PYTHONHOME, where it is", "-c", "import sys; print(sys.prefix, sys.exec_prefix)", env={"PYTHONHOME": sys.prefix})
t("PYTHONSTARTUP is for what is typed", "-c", "print(1)", env={"PYTHONSTARTUP": os.path.join(HERE, "script.py")})

print("---- what makes no sense")
for arguments in (("-Z",), ("-c", ), ("-m",), ("-W",), ("-X",), ("-bZ",), ("--nope",), ("--",), ("-",), ("--check-hash-based-pycs",), ("--check-hash-based-pycs", "nope"), ("--check-hash-based-pycs", "always", "-c", "print(1)"), ("--check-hash-based-pycs=always", "-c", "print(1)"), ("-\xe9",), ("---",), ("--help-nope",), ("-:",), ("-c:",), ("-1",), ("- ",),
                  ("-X", "utf8=2"), ("-X", "utf8=x"), ("-X", "int_max_str_digits"), ("-X", "int_max_str_digits="), ("-X", "int_max_str_digits=x"), ("-X", "int_max_str_digits=639"), ("-X", "int_max_str_digits=-1"), ("-X", "cpu_count"), ("-X", "cpu_count=0"), ("-X", "cpu_count=x"), ("-X", "importtime=3"), ("-X", "importtime=-1"), ("-X", "tracemalloc=x"), ("-X", "tracemalloc=-1"), ("-X", "frozen_modules=x"), ("-X", "gil=0"), ("-X", "gil=2"), ("-X", "gil"),
                  ("-X", "thread_inherit_context"), ("-X", "thread_inherit_context=2"), ("-X", "context_aware_warnings=x")):
    t(" ".join(arguments), *arguments, input="")

print("---- being told things")
t("-V", "-V")
t("--version", "--version")
t("-V and more", "-V", "-c", "print(1)")
t("-c and -V", "-c", "print(1)", "-V")
for option in ("-h", "-?", "--help", "--help-env", "--help-xoptions", "--help-all"):
    # What it says is not the same from one release to the next. It is taken from CPython's source as it is, so all that is looked at here is which parts of it there are.
    code, out, err = run(option)
    print(option, "=>", code, ascii(err), ascii(out.split("\n")[0]), [mark for mark in ("usage: PYTHON", "\n-b     : ", "\n-X opt : ", "\n--help-all", "\nEnvironment variables that change behavior:", "\nPYTHONPATH      : ':'-separated", "<prefix>/lib/pythonX.X", "\nThe following implementation-specific options are available:", "\n-X dev : ", "\n-X utf8") if mark in out], out.endswith("\n"), out.count("\n") > 5)
t("-h and more", "-h", "-Z")
t("-Z and -h", "-Z", "-h")
