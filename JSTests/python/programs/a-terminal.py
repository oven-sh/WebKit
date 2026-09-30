# termios, fcntl, tty and what os has for terminals, with a terminal to try them on: the two ends of a pseudo-terminal.
import errno
import fcntl
import os
import select
import struct
import sys
import termios
import tty


def show(e):
    return type(e).__module__ + "." + type(e).__name__ + ": " + str(e)


def attempt(f, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return show(e)


def t(label, f):
    r = attempt(f)
    print(label, "=>", r if isinstance(r, str) else ascii(r))
    sys.stdout.flush()


def flags(value, names):
    "Which of them are set, by name, and what is left over"
    found = [n for n in names if getattr(termios, n) and value & getattr(termios, n) == getattr(termios, n)]
    for n in found:
        value &= ~getattr(termios, n)
    return found, value


INPUT = ("BRKINT", "ICRNL", "IGNBRK", "IGNCR", "IGNPAR", "INLCR", "INPCK", "ISTRIP", "IXANY", "IXOFF", "IXON", "PARMRK", "IMAXBEL", "IUTF8")
OUTPUT = ("OPOST", "ONLCR", "OCRNL", "ONOCR", "ONLRET")
LOCAL = ("ECHO", "ECHOE", "ECHOK", "ECHONL", "ICANON", "IEXTEN", "ISIG", "NOFLSH", "TOSTOP", "ECHOCTL", "ECHOKE")
CONTROL = ("CS8", "CREAD", "CSTOPB", "HUPCL", "PARENB", "PARODD", "CLOCAL")
IFLAG, OFLAG, CFLAG, LFLAG, ISPEED, OSPEED, CC = range(7)


def described(mode):
    return flags(mode[IFLAG], INPUT), flags(mode[OFLAG], OUTPUT), flags(mode[CFLAG] & ~termios.CBAUD, CONTROL), flags(mode[LFLAG], LOCAL), mode[ISPEED] == termios.B38400, mode[OSPEED] == termios.B38400


def available(fd, count):
    "So many bytes of what comes out of it. The system passes them across when it gets round to it, and not all at once."
    data = b""
    while len(data) < count and select.select([fd], [], [], 5)[0]:
        more = os.read(fd, 1000)
        if not more:
            break
        data += more
    return data


def rest(fd):
    "Whatever else there is, which is thrown away"
    while select.select([fd], [], [], 0.05)[0]:
        os.read(fd, 1000)


master, slave = os.openpty()

print("---- what it is")
t("a terminal", lambda: (os.isatty(master), os.isatty(slave), os.ttyname(slave).startswith("/dev/pts/"), os.ttyname(slave) == os.ptsname(master), os.get_inheritable(master), os.get_inheritable(slave)))
t("as it comes", lambda: described(termios.tcgetattr(slave)))
t("what tcgetattr() gives", lambda: [(type(m).__name__, len(m), [type(x).__name__ for x in m], len(m[CC]) == termios.NCCS, sorted({type(c).__name__ for c in m[CC]}), all(len(c) == 1 for c in m[CC])) for m in [termios.tcgetattr(slave)]])
t("the characters", lambda: [(n, termios.tcgetattr(slave)[CC][getattr(termios, n)]) for n in ("VINTR", "VQUIT", "VERASE", "VKILL", "VEOF", "VSTART", "VSTOP", "VSUSP", "VEOL", "VREPRINT", "VWERASE", "VLNEXT", "VMIN", "VTIME")])
t("each time another list", lambda: (termios.tcgetattr(slave) == termios.tcgetattr(slave), termios.tcgetattr(slave) is not termios.tcgetattr(slave), termios.tcgetattr(master) == termios.tcgetattr(slave)))
t("something with fileno()", lambda: [termios.tcgetattr(f) == termios.tcgetattr(slave) for f in [os.fdopen(slave, "rb", buffering=0, closefd=False)]])

print("---- setting it")
original = termios.tcgetattr(slave)


def changed(**k):
    mode = termios.tcgetattr(slave)
    for name, value in k.items():
        mode[globals()[name]] = value(mode[globals()[name]])
    return mode


t("no echo", lambda: (termios.tcsetattr(slave, termios.TCSANOW, changed(LFLAG=lambda v: v & ~termios.ECHO)), described(termios.tcgetattr(slave))[3]))
t("when", lambda: [(termios.tcsetattr(slave, when, original), termios.tcgetattr(slave) == original) for when in (termios.TCSANOW, termios.TCSADRAIN, termios.TCSAFLUSH)] + [attempt(termios.tcsetattr, slave, w, original) for w in (99, -1, "a", None, 2 ** 31)])
t("how fast", lambda: [(termios.tcsetattr(slave, termios.TCSANOW, changed(ISPEED=lambda v: s, OSPEED=lambda v: s)), termios.tcgetattr(slave)[ISPEED:CC] == [s, s]) for s in (termios.B9600, termios.B115200, termios.B0, termios.B38400)])
t("at no speed that there is", lambda: [attempt(termios.tcsetattr, slave, termios.TCSANOW, changed(ISPEED=lambda v: s)) for s in (12345, -1, 2 ** 40)] + [attempt(termios.tcsetattr, slave, termios.TCSANOW, changed(OSPEED=lambda v: s)) for s in (12345, -1, 2 ** 40)])
termios.tcsetattr(slave, termios.TCSANOW, original)
t("not a line at a time", lambda: (termios.tcsetattr(slave, termios.TCSANOW, changed(LFLAG=lambda v: v & ~termios.ICANON)), [(type(c).__name__, c) for c in (termios.tcgetattr(slave)[CC][termios.VMIN], termios.tcgetattr(slave)[CC][termios.VTIME])],
                                   sorted({type(c).__name__ for c in termios.tcgetattr(slave)[CC]})))


def with_characters(**k):
    mode = termios.tcgetattr(slave)
    for name, value in k.items():
        mode[CC][getattr(termios, name)] = value
    return mode


t("how many, and how long", lambda: (termios.tcsetattr(slave, termios.TCSANOW, with_characters(VMIN=3, VTIME=7)), termios.tcgetattr(slave)[CC][termios.VMIN], termios.tcgetattr(slave)[CC][termios.VTIME], termios.tcsetattr(slave, termios.TCSANOW, with_characters(VMIN=b"\x02", VTIME=b"\x00")),
                                     termios.tcgetattr(slave)[CC][termios.VMIN], termios.tcgetattr(slave)[CC][termios.VTIME]))
termios.tcsetattr(slave, termios.TCSANOW, original)
t("a character", lambda: [(attempt(termios.tcsetattr, slave, termios.TCSANOW, with_characters(VINTR=c)), termios.tcgetattr(slave)[CC][termios.VINTR]) for c in (b"\x01", 2, 255, 256, 257, -1, True, b"", b"ab", "a", None, 1.5, bytearray(b"a"), 2 ** 64, type("I", (), {"__index__": lambda s: 5})(), type("B", (bytes,), {})(b"\x07"), type("N", (int,), {})(8))])
termios.tcsetattr(slave, termios.TCSANOW, original)
t("what is not seven things", lambda: [attempt(termios.tcsetattr, slave, termios.TCSANOW, x) for x in ([], original[:6], original + [0], tuple(original), None, 5, "1234567", type("L", (list,), {})(original))])
t("what is not a number", lambda: [attempt(termios.tcsetattr, slave, termios.TCSANOW, original[:i] + [x] + original[i + 1:]) for i in range(6) for x in ("a", None, 1.5, 2 ** 64)])
t("numbers that are too much are cut down", lambda: [(attempt(termios.tcsetattr, slave, termios.TCSANOW, original[:i] + [original[i] + 2 ** 32] + original[i + 1:]), termios.tcgetattr(slave) == original) for i in range(4)])
t("not the right number of characters", lambda: [attempt(termios.tcsetattr, slave, termios.TCSANOW, original[:6] + [x]) for x in ([], original[CC][:-1], original[CC] + [b"a"], tuple(original[CC]), None, b"a" * termios.NCCS)])
t("it is as it was", lambda: termios.tcgetattr(slave) == original)

print("---- tty")
t("raw", lambda: (tty.setraw(slave) == original, described(termios.tcgetattr(slave)), termios.tcgetattr(slave)[CC][termios.VMIN], termios.tcgetattr(slave)[CC][termios.VTIME]))
termios.tcsetattr(slave, termios.TCSANOW, original)
t("a character at a time", lambda: (tty.setcbreak(slave) == original, described(termios.tcgetattr(slave)), termios.tcgetattr(slave)[CC][termios.VMIN], termios.tcgetattr(slave)[CC][termios.VTIME]))
termios.tcsetattr(slave, termios.TCSANOW, original)
t("worked out and not set", lambda: [(tty.cfmakeraw(m), described(m), tty.cfmakecbreak(n), described(n), termios.tcgetattr(slave) == original) for m, n in [(termios.tcgetattr(slave), termios.tcgetattr(slave))]])

print("---- what goes through it")
t("a line at a time, and shown as it is typed", lambda: (os.write(master, b"hel"), available(master, 3), select.select([slave], [], [], 0.05)[0], os.write(master, b"lo\n"), available(master, 4), available(slave, 6)))
t("with what is rubbed out", lambda: (os.write(master, b"abcx\x7fd\n"), available(master, 10), available(slave, 5)))
t("the whole line rubbed out", lambda: (os.write(master, b"abc\x15de\n"), available(slave, 3), rest(master)))
t("the end of it", lambda: (os.write(master, b"ab\x04"), available(slave, 2), os.write(master, b"\x04"), os.read(slave, 10), available(master, 2)))
t("what is written to it", lambda: (os.write(slave, b"one\ntwo\r\n"), available(master, 11)))
tty.setraw(slave)
t("raw: as it is", lambda: (os.write(master, b"a\x7f\x03\r\n\x04"), available(slave, 6), select.select([master], [], [], 0.05)[0], os.write(slave, b"one\n"), available(master, 4)))
termios.tcsetattr(slave, termios.TCSANOW, original)

print("---- what is waiting")
t("how much", lambda: (os.write(master, b"waiting\n"), available(master, 9), select.select([slave], [], [], 5)[0] == [slave], struct.unpack("i", fcntl.ioctl(slave, termios.FIONREAD, bytes(4)))[0]))
t("thrown away", lambda: (termios.tcflush(slave, termios.TCIFLUSH), struct.unpack("i", fcntl.ioctl(slave, termios.FIONREAD, bytes(4)))[0], select.select([slave], [], [], 0)[0]))
t("either way, and both", lambda: [termios.tcflush(slave, q) for q in (termios.TCIFLUSH, termios.TCOFLUSH, termios.TCIOFLUSH)] + [attempt(termios.tcflush, slave, q) for q in (99, -1, "a", None, 2 ** 31)])
t("held up and let go", lambda: [termios.tcflow(slave, a) for a in (termios.TCOOFF, termios.TCOON)] + [attempt(termios.tcflow, slave, a) for a in (99, -1, "a", None)])
t("until it has gone", lambda: (os.write(slave, b"x"), termios.tcdrain(slave), available(master, 1)))
t("a break", lambda: (termios.tcsendbreak(slave, 0), attempt(termios.tcsendbreak, slave, "a"), attempt(termios.tcsendbreak, slave, 2 ** 31)))

print("---- how big it is")
t("as it comes", lambda: (termios.tcgetwinsize(slave), termios.tcgetwinsize(master), type(termios.tcgetwinsize(slave)).__name__, attempt(os.get_terminal_size, slave)))
t("set", lambda: (termios.tcsetwinsize(slave, (24, 80)), termios.tcgetwinsize(slave), termios.tcgetwinsize(master), os.get_terminal_size(slave), tuple(os.get_terminal_size(master))))
t("anything with two things in it", lambda: [(termios.tcsetwinsize(slave, x), termios.tcgetwinsize(slave)) for x in ([1, 2], (3, 4), range(5, 7), b"\x07\x08", type("I", (), {"__index__": lambda s: 9})() and [True, False], (65535, 0))])
t("what will not do", lambda: [attempt(termios.tcsetwinsize, slave, x) for x in ((), (1,), (1, 2, 3), 5, None, "ab", {1, 2}, {1: 2, 3: 4}, iter([1, 2]), (65536, 0), (0, 65536), (-1, 0), ("a", 0), (0, None), (1.5, 0), (2 ** 64, 0))] + [termios.tcgetwinsize(slave)])
t("the rest of it is left as it was", lambda: (fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 10, 20, 30, 40)), termios.tcsetwinsize(slave, (11, 21)), struct.unpack("HHHH", fcntl.ioctl(slave, termios.TIOCGWINSZ, bytes(8)))))

print("---- ioctl(), given somewhere to write")
t("bytes are copied", lambda: [(fcntl.ioctl(slave, termios.TIOCGWINSZ, b), b) for b in [bytes(8)]])
t("a bytearray is written in", lambda: [(fcntl.ioctl(slave, termios.TIOCGWINSZ, b), bytes(b)) for b in [bytearray(8)]])
t("unless it is said not to be", lambda: [(fcntl.ioctl(slave, termios.TIOCGWINSZ, b, False), bytes(b)) for b in [bytearray(8)]])
t("or it is said to be", lambda: [(fcntl.ioctl(slave, termios.TIOCGWINSZ, b, True), bytes(b)) for b in [bytearray(8)]] + [attempt(fcntl.ioctl, slave, termios.TIOCGWINSZ, bytes(8), True)])
t("a memoryview, and an array", lambda: [(fcntl.ioctl(slave, termios.TIOCGWINSZ, memoryview(b)), bytes(b)) for b in [bytearray(8)]] + [(fcntl.ioctl(slave, termios.TIOCGWINSZ, a), list(a)) for a in [__import__("array").array("H", [0] * 4)]])
t("more room than is wanted", lambda: [(fcntl.ioctl(slave, termios.TIOCGWINSZ, b), bytes(b[:10]), len(b)) for b in [bytearray(b"\xff" * 2000)]] + [(lambda r: (r[:10], len(r)))(fcntl.ioctl(slave, termios.TIOCGWINSZ, b"\xff" * 1024)), attempt(fcntl.ioctl, slave, termios.TIOCGWINSZ, b"\xff" * 1025)])
t("a str", lambda: fcntl.ioctl(slave, termios.TIOCGWINSZ, "\0" * 8))
t("of what is no terminal", lambda: [[(attempt(fcntl.ioctl, r, termios.TIOCGWINSZ, bytes(8)), attempt(termios.tcgetattr, r), attempt(termios.tcgetwinsize, r), attempt(termios.tcdrain, r), attempt(tty.setraw, r), os.close(r), os.close(w)) for r, w in [os.pipe()]]])

print("---- fcntl()")
t("whether it waits", lambda: (fcntl.fcntl(slave, fcntl.F_GETFL) & os.O_NONBLOCK, fcntl.fcntl(slave, fcntl.F_SETFL, fcntl.fcntl(slave, fcntl.F_GETFL) | os.O_NONBLOCK), fcntl.fcntl(slave, fcntl.F_GETFL) & os.O_NONBLOCK == os.O_NONBLOCK, os.get_blocking(slave), attempt(os.read, slave, 1), os.set_blocking(slave, True),
                               fcntl.fcntl(slave, fcntl.F_GETFL) & os.O_NONBLOCK))
t("whether it is passed on", lambda: (fcntl.fcntl(slave, fcntl.F_GETFD), fcntl.fcntl(slave, fcntl.F_SETFD, 0), os.get_inheritable(slave), fcntl.fcntl(slave, fcntl.F_SETFD, fcntl.FD_CLOEXEC), os.get_inheritable(slave)))

print("---- whose it is")
t("no one's", lambda: (attempt(os.tcgetpgrp, slave), attempt(os.tcsetpgrp, slave, os.getpgrp())))

print("---- when the other end has gone")
os.close(master)
t("nothing more comes", lambda: (attempt(os.read, slave, 1), attempt(os.write, slave, b"a"), len(termios.tcgetattr(slave)), os.isatty(slave)))
os.close(slave)
t("nor is there anything to ask", lambda: (attempt(termios.tcgetattr, slave), attempt(termios.tcgetwinsize, slave), attempt(fcntl.ioctl, slave, termios.TIOCGWINSZ, bytes(8)), attempt(fcntl.fcntl, slave, fcntl.F_GETFL), os.isatty(slave)))
