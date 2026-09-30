# For signals.mjs.
import os
import signal

signal.alarm(0)
signal.pthread_sigmask(signal.SIG_SETMASK, [])
signal.signal(signal.SIGINT, signal.default_int_handler)

USR1 = int(signal.SIGUSR1)
INT = int(signal.SIGINT)
set_handler = signal.signal
get_handler = signal.getsignal
raise_signal = signal.raise_signal
seen = []


def handler(number, frame):
    seen.append((signal.Signals(number).name, frame and frame.f_code.co_name))


def name(number):
    return signal.Signals(number).name


def took():
    out = ascii(seen)
    seen.clear()
    return out


def kill():
    os.kill(os.getpid(), USR1)


def call(f, *a):
    return f(*a)


def catching(f):
    try:
        return f()
    except BaseException as e:
        return "Python caught " + type(e).__name__ + ": " + str(e)


def spin_until(f):
    while not f():
        pass


def raises(number, frame):
    raise ValueError("from Python's function")
