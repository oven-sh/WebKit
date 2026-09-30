# The module termios, as far as it can be tried with no terminal: what there is in it, what it will not take, what it looks at first, and what it says of what is not a terminal.
import os
import sys
import termios


def t(label, f):
    try:
        print(label, "=>", repr(f()))
    except BaseException as e:
        print(label, "=>", type(e).__module__ + "." + type(e).__name__ + ":", e, e.args)


print("---- what there is")
print(sorted(n for n in dir(termios) if not n.startswith("_") and not n.isupper()))
print([(n, getattr(termios, n)) for n in sorted(dir(termios)) if n.isupper() or n[:1] == "B" and n[1:].isdigit()])
print(all(type(getattr(termios, n)) is int for n in dir(termios) if n.isupper()), all(getattr(termios, n) >= 0 for n in dir(termios) if n.startswith("TIO")))
print(termios.__name__, termios.__doc__.split("\n")[0], termios.__spec__.origin)
for name in ("tcgetattr", "tcsetattr", "tcsendbreak", "tcdrain", "tcflush", "tcflow", "tcgetwinsize", "tcsetwinsize"):
    function = getattr(termios, name)
    print(name, function.__text_signature__, function.__module__, function.__doc__.split("\n")[0])

print("---- termios.error")
error = termios.error
print(error, error.__name__, error.__qualname__, error.__module__, error.__bases__, error.__mro__, error.__doc__, sorted(vars(error)))
print(repr(error(1, "x")), str(error(1, "x")), error(1, "x").args, hasattr(error(1, "x"), "errno"), issubclass(error, OSError))
class Derived(error):
    pass
print(Derived.__mro__, repr(Derived("d")))
t("with something of its own", lambda: (lambda e: (setattr(e, "x", 1), e.x, e.__dict__))(error()))
import pickle, copy
t("pickled", lambda: repr(pickle.loads(pickle.dumps(error(1, "x")))))
t("copied", lambda: repr(copy.copy(error(1, "x"))))

print("---- what is not a terminal")
r, w = os.pipe()
NCCS = 20 if sys.platform == "darwin" else 32
attributes = [0, 0, 0, 0, 0, 0, [b"\0"] * NCCS]
t("tcgetattr", lambda: termios.tcgetattr(r))
t("tcsetattr", lambda: termios.tcsetattr(r, termios.TCSANOW, attributes))
t("tcsendbreak", lambda: termios.tcsendbreak(r, 0))
t("tcdrain", lambda: termios.tcdrain(r))
t("tcflush", lambda: termios.tcflush(r, termios.TCIFLUSH))
t("tcflow", lambda: termios.tcflow(r, termios.TCOON))
t("tcgetwinsize", lambda: termios.tcgetwinsize(r))
t("tcsetwinsize", lambda: termios.tcsetwinsize(r, (24, 80)))
with open(os.devnull) as f:
    t("a file", lambda: termios.tcgetattr(f))
t("that is closed", lambda: termios.tcgetattr(f))
t("not open", lambda: termios.tcgetattr(9999))
t("what is caught", lambda: [c.__name__ for c in (termios.error, OSError, Exception) if (lambda c: (lambda: True)() if issubclass(termios.error, c) else False)(c)])

print("---- what they will not take")
for name, more in (("tcgetattr", ()), ("tcsetattr", (0, attributes)), ("tcsendbreak", (0,)), ("tcdrain", ()), ("tcflush", (0,)), ("tcflow", (0,)), ("tcgetwinsize", ()), ("tcsetwinsize", ((1, 2),))):
    function = getattr(termios, name)
    t(name + ": nothing", lambda: function())
    t("too much", lambda: function(r, *more, 1))
    if more:
        t("too little", lambda: function(r))
    t("by name", lambda: function(fd=r))
    for label, descriptor in (("a float", 1.0), ("a string", "x"), ("None", None), ("less than nothing", -1), ("too big", 2 ** 31), ("what has a fileno() that raises", type("F", (), {"fileno": lambda s: 1 / 0})())):
        t(label, lambda: function(descriptor, *more))
for name in ("tcsendbreak", "tcflush", "tcflow"):
    function = getattr(termios, name)
    for label, value in (("a float", 1.0), ("a string", "x"), ("None", None), ("too big", 2 ** 31), ("too small", -2 ** 31 - 1), ("what has __index__", type("I", (), {"__index__": lambda s: 0})()), ("no such thing", 12345)):
        t(name + ": " + label, lambda: function(r, value))

print("---- tcsetattr(): what is looked at, and in what order")
for label, when, value in (("not a list", 0, ()), ("a tuple of seven", 0, tuple(attributes)), ("too short", 0, attributes[:6]), ("too long", 0, attributes + [0]), ("None", 0, None), ("a list of a class of one's own", 0, type("L", (list,), {})(attributes)),
                           ("when is a float", 1.0, attributes), ("when is too big", 2 ** 31, attributes), ("when is a float and it is not a list", 1.0, ()), ("what is in it is not looked at before the terminal is", 0, ["x"] * 7)):
    t(label, lambda: termios.tcsetattr(r, when, value))
t("not open, and not a list", lambda: termios.tcsetattr(9999, 0, ()))
t("not open", lambda: termios.tcsetattr(9999, 0, attributes))

print("---- tcsetwinsize(): what is looked at, and in what order")
class Sequence:
    def __init__(self, *items): self.items = items
    def __len__(self): return len(self.items)
    def __getitem__(self, i): return self.items[i]
for label, value in (("a list", [24, 80]), ("a sequence of one's own", Sequence(24, 80)), ("a string", "ab"), ("bytes", b"ab"), ("a range", range(2)), ("one", (1,)), ("three", (1, 2, 3)), ("none", ()), ("a number", 1), ("None", None), ("a set", {1, 2}), ("a dict", {1: 2, 3: 4}), ("an iterator", iter((1, 2))),
                     ("floats", (1.0, 2.0)), ("the second is a float", (1, 2.0)), ("strings", ("a", "b")), ("what has __index__", (type("I", (), {"__index__": lambda s: 5})(), 6)), ("too big for a long", (2 ** 63, 1)), ("the second is", (1, 2 ** 63)), ("too big for what it is kept in", (65536, 1)), ("less than nothing", (-1, 1)),
                     ("whose length raises", type("S", (), {"__len__": lambda s: 1 / 0, "__getitem__": lambda s, i: 1})()), ("whose length is less than nothing", type("S", (), {"__len__": lambda s: -1, "__getitem__": lambda s, i: 1})())):
    t(label, lambda: termios.tcsetwinsize(r, value))
t("not open, and not a pair", lambda: termios.tcsetwinsize(9999, 1))
t("not open, and floats", lambda: termios.tcsetwinsize(9999, (1.0, 2.0)))
t("not open, and too big", lambda: termios.tcsetwinsize(9999, (65536, 1)))

print("---- what is written over it")
import tty
print(sorted(n for n in dir(tty) if not n.startswith("_") and n.islower())[:8], tty.IFLAG, tty.CC)
mode = [termios.BRKINT | termios.ICRNL, termios.OPOST, termios.CS8, termios.ECHO | termios.ICANON | termios.ISIG, termios.B9600, termios.B9600, [b"\x03"] * NCCS]
t("cfmakeraw", lambda: (tty.cfmakeraw(mode), mode[:6], mode[6][termios.VMIN], mode[6][termios.VTIME])[1:])
mode = [termios.BRKINT | termios.ICRNL, termios.OPOST, termios.CS8, termios.ECHO | termios.ICANON | termios.ISIG, termios.B9600, termios.B9600, [b"\x03"] * NCCS]
t("cfmakecbreak", lambda: (tty.cfmakecbreak(mode), mode[:6], mode[6][termios.VMIN], mode[6][termios.VTIME])[1:])
t("setraw", lambda: tty.setraw(r))
import getpass
print(getpass.getpass.__name__, getpass.unix_getpass is getpass.getpass)
os.close(r)
os.close(w)
