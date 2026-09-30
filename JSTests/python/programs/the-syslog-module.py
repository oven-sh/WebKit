# The module syslog. What is sent to the log is not to be had back. What can be seen is what the hooks of sys.addaudithook() are told, and what LOG_PERROR has written to the standard error as well.
import os
import sys
import syslog
import tempfile


def t(label, f):
    try:
        print(label, "=>", f())
    except Exception as e:
        print(label, "=>", type(e).__name__, e)


print("---- what there is")
COMMON = ("LOG_EMERG", "LOG_ALERT", "LOG_CRIT", "LOG_ERR", "LOG_WARNING", "LOG_NOTICE", "LOG_INFO", "LOG_DEBUG", "LOG_PID", "LOG_CONS", "LOG_NDELAY", "LOG_ODELAY", "LOG_NOWAIT", "LOG_PERROR", "LOG_KERN", "LOG_USER", "LOG_MAIL", "LOG_DAEMON", "LOG_AUTH",
          "LOG_LPR", "LOG_LOCAL0", "LOG_LOCAL1", "LOG_LOCAL2", "LOG_LOCAL3", "LOG_LOCAL4", "LOG_LOCAL5", "LOG_LOCAL6", "LOG_LOCAL7", "LOG_SYSLOG", "LOG_CRON", "LOG_UUCP", "LOG_NEWS", "LOG_AUTHPRIV", "LOG_FTP")
print([(n, getattr(syslog, n)) for n in COMMON])
print(sorted(n for n in vars(syslog) if not n.startswith("__") and not isinstance(vars(syslog)[n], int)), syslog.__doc__, syslog.__spec__.origin, {type(v).__name__ for n, v in vars(syslog).items() if n in COMMON})
for n in ("openlog", "closelog", "syslog", "setlogmask", "LOG_MASK", "LOG_UPTO"):
    f = getattr(syslog, n)
    print("   ", n, f.__text_signature__, f.__module__, repr(f.__doc__))

print("---- masks")
# To shift by less than nothing, or by more than there is room for, is more than C says anything about. All but LOG_MASK(-1) come to the same wherever CPython has been compiled.
print([syslog.LOG_MASK(n) for n in (0, 1, 7, 29, 30, 31, 32, 33, 63, 64, -2, -32, 2 ** 40, 2 ** 62, 2 ** 63 - 1, True)])
print([syslog.LOG_UPTO(n) for n in (0, 1, 7, 29, 30, 31, 32, 33, 63, 64, -1, -2, -32, 2 ** 40, 2 ** 62, 2 ** 63 - 1, True)])
for f in (syslog.LOG_MASK, syslog.LOG_UPTO, syslog.setlogmask):
    for label, g in (("none", lambda: f()), ("two", lambda: f(1, 2)), ("by name", lambda: f(pri=1)), ("a str", lambda: f("1")), ("a float", lambda: f(1.0)), ("None", lambda: f(None)), ("too great", lambda: f(2 ** 63)), ("too little", lambda: f(-2 ** 63 - 1)),
                     ("what says what it is", lambda: type(f(type("I", (), {"__index__": lambda self: 0})())).__name__)):
        t("%s %s" % (f.__name__, label), g)
# What macOS says the mask was, of a log that has not been opened, is not what it says that it is.
syslog.openlog("ident")
before = syslog.setlogmask(syslog.LOG_UPTO(syslog.LOG_WARNING))
print(before, syslog.setlogmask(0), syslog.setlogmask(syslog.LOG_MASK(syslog.LOG_ERR)), syslog.setlogmask(before), syslog.setlogmask(0) == before)

print("---- what the hooks are told")
events = []
sys.addaudithook(lambda event, arguments: events.append((event, arguments)) if event.startswith("syslog.") else None)


def told(f):
    del events[:]
    try:
        result = f()
    except Exception as e:
        result = type(e).__name__, str(e)
    return result, events[:]


saved = sys.argv
for label, f in (("openlog()", lambda: syslog.openlog()), ("an ident", lambda: syslog.openlog("ident")), ("all three", lambda: syslog.openlog("ident", syslog.LOG_PID, syslog.LOG_MAIL)), ("by name", lambda: syslog.openlog(facility=syslog.LOG_LOCAL3, logoption=syslog.LOG_NDELAY, ident="é€😀")),
                 ("an empty one", lambda: syslog.openlog("")), ("a str of another class", lambda: syslog.openlog(type("S", (str,), {})("s"))), ("great numbers", lambda: syslog.openlog("i", 2 ** 40, -2 ** 40)), ("syslog(message)", lambda: syslog.syslog("a message")),
                 ("with a priority", lambda: syslog.syslog(syslog.LOG_DEBUG, "a message")), ("and a facility", lambda: syslog.syslog(syslog.LOG_DEBUG | syslog.LOG_LOCAL0, "é€😀 100% %s %n")), ("nothing to say", lambda: syslog.syslog("")), ("closelog()", lambda: syslog.closelog()), ("again", lambda: syslog.closelog()),
                 ("syslog() when it is not open", lambda: syslog.syslog("opens it")), ("and again", lambda: syslog.syslog("it is open")), ("setlogmask()", lambda: syslog.setlogmask(syslog.setlogmask(0)))):
    print(label, "=>", told(f))
syslog.closelog()
for argv in (["/a/b/c.py"], ["c.py", "x"], ["/a/b/"], ["/"], [""], [], [5], [b"x"], ("t.py",), None, ["é/€"], [type("S", (str,), {})("/x/s.py")]):
    sys.argv = argv
    print("with sys.argv", ascii(argv), "=>", told(lambda: syslog.openlog())[1])
del sys.argv
print("with none =>", told(lambda: syslog.openlog()))
sys.argv = saved
syslog.closelog()

print("---- what will not do")
for label, f in (("ident an int", lambda: syslog.openlog(5)), ("bytes", lambda: syslog.openlog(b"x")), ("None", lambda: syslog.openlog(None)), ("half a character", lambda: syslog.openlog("\ud800")), ("with a zero in it", lambda: syslog.openlog("a\0b")), ("logoption a str", lambda: syslog.openlog("i", "1")),
                 ("too great", lambda: syslog.openlog("i", 2 ** 63)), ("facility a float", lambda: syslog.openlog("i", 0, 1.0)), ("four", lambda: syslog.openlog("i", 0, 0, 0)), ("something else", lambda: syslog.openlog(other=1)), ("ident twice", lambda: syslog.openlog("i", ident="j")),
                 ("syslog()", lambda: syslog.syslog()), ("three", lambda: syslog.syslog(1, "m", 2)), ("by name", lambda: syslog.syslog(message="m")), ("both by name", lambda: syslog.syslog(priority=1, message="m")), ("an int", lambda: syslog.syslog(5)), ("bytes", lambda: syslog.syslog(b"m")), ("None", lambda: syslog.syslog(None)),
                 ("with a zero in it", lambda: syslog.syslog("a\0b")), ("half a character", lambda: syslog.syslog("\ud800")), ("the wrong way round", lambda: syslog.syslog("m", 1)), ("priority a float", lambda: syslog.syslog(1.0, "m")), ("priority None", lambda: syslog.syslog(None, "m")),
                 ("priority too great", lambda: syslog.syslog(2 ** 31, "m")), ("too little", lambda: syslog.syslog(-2 ** 31 - 1, "m")), ("far too great", lambda: syslog.syslog(2 ** 70, "m")), ("message an int", lambda: syslog.syslog(1, 2)), ("closelog(1)", lambda: syslog.closelog(1))):
    print(label, "=>", told(f))


def refuses(event, arguments):
    if event.startswith("syslog.") and refusing:
        raise PermissionError(event)


refusing = True
sys.addaudithook(refuses)
for label, f in (("openlog()", lambda: syslog.openlog("i")), ("syslog()", lambda: syslog.syslog("m")), ("closelog()", lambda: syslog.closelog()), ("setlogmask()", lambda: syslog.setlogmask(0))):
    t("a hook refuses " + label, f)
refusing = False

print("---- what is written to the standard error as well")
sys.stdout.flush()
with tempfile.TemporaryFile() as file:
    kept = os.dup(2)
    os.dup2(file.fileno(), 2)
    try:
        syslog.openlog("the ident", syslog.LOG_PERROR)
        syslog.syslog("a message")
        syslog.syslog(syslog.LOG_ERR, "é€😀 100% %s %d %n")
        syslog.setlogmask(syslog.LOG_UPTO(syslog.LOG_WARNING))
        syslog.syslog(syslog.LOG_INFO, "not important enough")
        syslog.syslog(syslog.LOG_CRIT, "important enough")
        syslog.setlogmask(before)
        # What it was given to go by is kept for as long as it goes by it.
        for i in range(2000):
            "some other str %d" % i
        syslog.syslog("later")
        syslog.openlog("another", syslog.LOG_PERROR)
        syslog.syslog("by another name")
        syslog.closelog()
        sys.argv = ["/somewhere/program.py"]
        syslog.openlog(logoption=syslog.LOG_PERROR)
        syslog.syslog("by the name of the program")
        syslog.closelog()
        sys.argv = saved
    finally:
        os.dup2(kept, 2)
        os.close(kept)
    file.seek(0)
    # How a line begins differs: with the time, and what process it is, or with neither.
    for line in file.read().decode().splitlines():
        head, message = line.rsplit(": ", 1)
        print([name for name in ("the ident", "another", "program.py") if name in head], repr(message))
