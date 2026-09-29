# Where `python` finds itself and its library, which is worked out from how it was started and what is beside it.
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.realpath(tempfile.mkdtemp())
os.chdir(HERE)
PREFIX = os.path.realpath(sys.base_prefix)
# The program itself, whatever it was started by way of
REAL = os.path.realpath(sys._base_executable)
BIN = os.path.dirname(sys._base_executable)


def tidy(text):
    text = text.replace(HERE, "HERE")
    for place, name in sorted(((REAL, "PYTHON"), (sys._base_executable, "PYTHON"), (BIN, "BIN"), (os.path.dirname(REAL), "BIN"), (PREFIX, "PREFIX"), (sys.base_prefix, "PREFIX")), key=lambda p: -len(p[0])):
        text = text.replace(place, name)
    return re.sub(r"BIN/[Pp]ython[\d.]*\b", "PYTHON", text)


def run(program, *arguments, env={}, cwd=None, argv0=None):
    environment = {"PATH": "/usr/bin:/bin", "HOME": HERE, "LC_ALL": "en_US.UTF-8", "PYTHONUTF8": "1"}
    environment.update(env)
    done = subprocess.run([argv0 or program, *arguments], executable=program, env=environment, capture_output=True, text=True, cwd=cwd, timeout=60)
    return done.returncode, tidy(done.stdout), tidy(done.stderr)


def t(label, *arguments, **k):
    print(label, "=>", ascii(run(*arguments, **k)))


WHERE = "import sys; print(sys.executable, sys._base_executable, sys.prefix, sys.exec_prefix, sys.base_prefix, sys.base_exec_prefix, sys._stdlib_dir, sys.platlibdir, getattr(sys, '_home', 'no _home')); print([p for p in sys.path if not p.endswith('site-packages') or 'env' in p or 'bare' in p], sum(p.endswith('site-packages') for p in sys.path))"

print("---- how it is started")
t("by all of its name", sys.executable, "-c", WHERE)
t("from where it is", "./" + os.path.basename(sys._base_executable), "-c", WHERE, cwd=BIN)
t("from above where it is", os.path.join(os.path.basename(BIN), os.path.basename(sys._base_executable)), "-c", WHERE, cwd=os.path.dirname(BIN))
t("by looking for it", sys.executable, "-c", WHERE, argv0=os.path.basename(sys._base_executable), env={"PATH": BIN + ":/usr/bin:/bin"})
t("by a name that is nowhere to be found", sys.executable, "-c", WHERE, argv0="python-that-is-not-there")
t("with dots in the way", os.path.join(BIN, "..", os.path.basename(BIN), ".", os.path.basename(sys._base_executable)), "-c", WHERE)
# What it takes itself to be then is not the same in every build of CPython. Where the library is is.
LIBRARY = "import sys, os; print(sys.prefix, sys.exec_prefix, sys._stdlib_dir, os.__file__)"
os.symlink(sys._base_executable, "python-by-a-link")
t("by a link", os.path.join(HERE, "python-by-a-link"), "-c", LIBRARY)
os.symlink("python-by-a-link", "python-by-two-links")
t("by a link to a link", os.path.join(HERE, "python-by-two-links"), "-c", LIBRARY)

print("---- what it is told")
t("PYTHONHOME", sys.executable, "-c", WHERE, env={"PYTHONHOME": PREFIX})
t("PYTHONHOME, in two parts", sys.executable, "-c", WHERE, env={"PYTHONHOME": PREFIX + ":" + PREFIX})
t("PYTHONHOME, of which the second is nowhere", sys.executable, "-c", WHERE, env={"PYTHONHOME": PREFIX + ":/nowhere"})
t("PYTHONHOME, and -E", sys.executable, "-E", "-c", WHERE, env={"PYTHONHOME": "/nowhere"})
t("PYTHONPATH", sys.executable, "-c", WHERE, env={"PYTHONPATH": "/a:b:"})
t("PYTHONPATH, and -I", sys.executable, "-I", "-c", WHERE, env={"PYTHONPATH": "/a"})
t("-S", sys.executable, "-S", "-c", LIBRARY + "; print(hasattr(sys, '_home'), sys.path)")
t("-s", sys.executable, "-s", "-c", WHERE)

print("---- a virtual environment")
import venv
venv.create("env", with_pip=False, symlinks=True)
print(sorted(os.listdir("env")), sorted(os.listdir("env/bin"))[:3] != [], [tidy(l.strip()) for l in open("env/pyvenv.cfg") if not l.startswith(("version", "command"))])
inside = os.path.join(HERE, "env", "bin", "python")
t("its python", inside, "-c", WHERE)
t("-I", inside, "-I", "-c", WHERE)
t("-S", inside, "-S", "-c", WHERE)
t("what it starts", inside, "-c", "import subprocess, sys; print(subprocess.run([sys.executable, '-c', 'import sys; print(sys.prefix, sys.executable)'], capture_output=True, text=True).stdout)")
packages = [p for p in eval(run(inside, "-c", "import site; print(site.getsitepackages())")[1].replace("HERE", HERE))]
print([tidy(p) for p in packages])
with open(os.path.join(packages[0], "installed.py"), "w") as f:
    f.write("value = 'from the environment'\n")
t("finds what is installed in it", inside, "-c", "import installed; print(installed.value)")
t("and what is outside does not", sys.executable, "-c", "import installed")
with open(os.path.join(packages[0], "more.pth"), "w") as f:
    f.write(HERE + "/extra\nimport sys; sys.from_pth = 1\n# a comment\n/nowhere/at/all\n")
os.mkdir("extra")
t("a .pth file", inside, "-c", "import sys; print(sys.from_pth, [p for p in sys.path if 'extra' in p or 'nowhere' in p])")
venv.create("open-env", with_pip=False, symlinks=True, system_site_packages=True)
t("one that sees what is outside", os.path.join(HERE, "open-env", "bin", "python"), "-c", WHERE)
os.makedirs("bare/bin")
os.symlink(sys._base_executable, "bare/bin/python")
with open("bare/pyvenv.cfg", "w") as f:
    f.write("home = %s\n" % BIN)
t("with nothing said but where home is", os.path.join(HERE, "bare", "bin", "python"), "-c", WHERE)
with open("bare/pyvenv.cfg", "w") as f:
    f.write("# nothing\n")
t("with nothing said", os.path.join(HERE, "bare", "bin", "python"), "-c", LIBRARY + "; print(sys.executable, sys.base_prefix, sys._home)")
with open("bare/pyvenv.cfg", "w") as f:
    f.write("HOME=%s\ninclude-system-site-packages = TRUE\n" % BIN)
t("said another way", os.path.join(HERE, "bare", "bin", "python"), "-c", WHERE)
