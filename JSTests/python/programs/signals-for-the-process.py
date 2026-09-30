# A signal that is sent to the process, and not to a thread of it, is given to any thread that will have it. There is the one thread in CPython. Here the engine has others, which are to have nothing to do with it.
import os
import signal
import sys
import time

U1, U2 = signal.SIGUSR1, signal.SIGUSR2
BLOCK, UNBLOCK, SETMASK = signal.SIG_BLOCK, signal.SIG_UNBLOCK, signal.SIG_SETMASK
me = os.getpid()
called = []


def t(label, f):
    try:
        result = f()
    except BaseException as e:
        result = type(e).__name__ + ": " + str(e)
    print(label, "=>", result)
    sys.stdout.flush()


def names(signals):
    return sorted(s.name for s in signals)


def handler(number, frame):
    called.append(signal.Signals(number).name)


def took():
    result = called[:]
    del called[:]
    return result


print("---- kept back, with nothing said of what is to be done with it")
for attempt in range(20):
    signal.pthread_sigmask(BLOCK, [U2])
    os.kill(me, U2)
    if (names(signal.sigpending()), signal.sigwait([U2]).name, names(signal.sigpending())) != (["SIGUSR2"], "SIGUSR2", []):
        print("not so, at attempt", attempt)
    signal.pthread_sigmask(UNBLOCK, [U2])
t("it is there to be asked after, and to be waited for", lambda: (names(signal.pthread_sigmask(BLOCK, [U2])), os.kill(me, U2), names(signal.sigpending()), signal.sigwait([U2]).name, names(signal.sigpending()), names(signal.pthread_sigmask(UNBLOCK, [U2]))))
t("what is to be done with it is as it was said to be", lambda: (signal.pthread_sigmask(BLOCK, [U2]) and None, signal.getsignal(U2).name, signal.pthread_sigmask(UNBLOCK, [U2]) and None, signal.getsignal(U2).name))
t("two of them", lambda: (signal.pthread_sigmask(BLOCK, [U1, U2]) and None, os.kill(me, U2), os.kill(me, U1), names(signal.sigpending()), sorted([signal.sigwait([U1, U2]).name, signal.sigwait([U1, U2]).name]), names(signal.sigpending()), names(signal.pthread_sigmask(UNBLOCK, [U1, U2]))))
t("with all that are kept back said at once", lambda: (names(signal.pthread_sigmask(SETMASK, [U2])), os.kill(me, U2), names(signal.sigpending()), signal.sigwait([U2]).name, names(signal.pthread_sigmask(SETMASK, []))))

print("---- kept back, with a function to be called")
signal.signal(U1, handler)
t("it is not called until it is let through", lambda: (signal.pthread_sigmask(BLOCK, [U1]) and None, os.kill(me, U1), took(), names(signal.sigpending()), signal.pthread_sigmask(UNBLOCK, [U1]) and None, took(), names(signal.sigpending())))
t("or it is waited for, and is not called at all", lambda: (signal.pthread_sigmask(BLOCK, [U1]) and None, os.kill(me, U1), signal.sigwait([U1]).name, signal.pthread_sigmask(UNBLOCK, [U1]) and None, took()))
t("not kept back", lambda: (os.kill(me, U1), took()))
t("the function is said while it is kept back", lambda: (signal.pthread_sigmask(BLOCK, [U2]) and None, os.kill(me, U2), signal.signal(U2, handler).name, took(), signal.pthread_sigmask(UNBLOCK, [U2]) and None, took()))
t("and is taken away while it is kept back", lambda: (signal.pthread_sigmask(BLOCK, [U2]) and None, signal.signal(U2, signal.SIG_DFL) is handler, os.kill(me, U2), names(signal.sigpending()), signal.sigwait([U2]).name, signal.pthread_sigmask(UNBLOCK, [U2]) and None, took()))

print("---- kept back, and to be ignored")
# Whether one that is to be ignored is kept at all is for the system to say.
t("said first", lambda: (signal.signal(U2, signal.SIG_IGN).name, signal.pthread_sigmask(BLOCK, [U2]) and None, os.kill(me, U2), names(signal.sigpending()), signal.pthread_sigmask(UNBLOCK, [U2]) and None, names(signal.sigpending()), signal.getsignal(U2).name))
t("said afterwards", lambda: (signal.signal(U2, signal.SIG_DFL).name, signal.pthread_sigmask(BLOCK, [U2]) and None, os.kill(me, U2), names(signal.sigpending()), signal.signal(U2, signal.SIG_IGN).name, names(signal.sigpending()), signal.pthread_sigmask(UNBLOCK, [U2]) and None, "still here"))
signal.signal(U2, signal.SIG_DFL)

print("---- sent by another process")


def sent(name):
    "It has been sent by the time that this returns. When it comes is another matter"
    return os.waitpid(os.posix_spawnp("kill", ["kill", "-" + name, str(me)], os.environ), 0)[1]


def until_called():
    while not called:
        time.sleep(0.001)
    return took()


for attempt in range(10):
    signal.pthread_sigmask(BLOCK, [U2])
    if (sent("USR2"), signal.sigwait([U2]).name) != (0, "SIGUSR2"):
        print("not so, at attempt", attempt)
    signal.pthread_sigmask(UNBLOCK, [U2])
t("kept back, with nothing said of what is to be done with it", lambda: (signal.pthread_sigmask(BLOCK, [U2]) and None, sent("USR2"), signal.sigwait([U2]).name, names(signal.sigpending()), signal.pthread_sigmask(UNBLOCK, [U2]) and None))
t("kept back, with a function to be called", lambda: (signal.pthread_sigmask(BLOCK, [U1]) and None, sent("USR1"), time.sleep(0.01), took(), signal.pthread_sigmask(UNBLOCK, [U1]) and None, until_called()))
t("which is not called if it is waited for", lambda: (signal.pthread_sigmask(BLOCK, [U1]) and None, sent("USR1"), signal.sigwait([U1]).name, signal.pthread_sigmask(UNBLOCK, [U1]) and None, time.sleep(0.01), took()))
t("not kept back", lambda: (sent("USR1"), until_called()))
t("it interrupts what is waiting", lambda: [(os.posix_spawnp("sh", ["sh", "-c", "sleep 0.05; kill -USR1 %d" % me], os.environ) > 0, time.sleep(0.5), took(), os.wait()[1]) for _ in [0]])

print("---- sent by the process to itself, as the last thing in a try")
# What is called for it raises, and that is to be caught by what the sending is inside of. The signal has come by the time that it has been sent, so it is where it was sent that it is raised.


class Came(Exception):
    pass


def raises(number, frame):
    raise Came(signal.Signals(number).name)


def where(send):
    try:
        send()
        time.sleep(5)
    except Came as e:
        return str(e), e.__traceback__.tb_lineno - where.__code__.co_firstlineno
    return "nothing came"


signal.signal(U1, raises)
t("kill", lambda: where(lambda: os.kill(me, U1)))
t("raise_signal", lambda: where(lambda: signal.raise_signal(U1)))
t("pthread_kill", lambda: where(lambda: signal.pthread_kill(__import__("_thread").get_ident(), U1)))
# In a group of its own, so that no one else is sent it.
os.setpgid(0, 0)
t("killpg", lambda: where(lambda: os.killpg(os.getpgrp(), U1)))
t("kill, of the group", lambda: where(lambda: os.kill(0, U1)))
signal.signal(U1, signal.SIG_DFL)

