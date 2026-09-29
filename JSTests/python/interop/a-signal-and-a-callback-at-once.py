# A signal has come, and something that was weakly referred to has been collected, and both are found out at once. The handler is seen to first, as by _Py_HandlePending(), so that
# what it raises is raised where the program is. In the callback it would be shown and forgotten.
import _signal
import os
import weakref

import js


class Thing:
    pass


log = []


def handler(*_):
    log.append("the handler")
    raise KeyError("from the handler")


def callback(_):
    log.append("the callback")


# All in one, with no code of Python's run between: the first thing that any does is to see what there is to see to.
both = js.eval("(unblock, how, which) => { fullGC(); unblock(how, which); return 'nothing was raised'; }")

_signal.signal(_signal.SIGUSR1, handler)
for attempt in range(3):
    del log[:]
    thing = Thing()
    reference = weakref.ref(thing, callback)
    _signal.pthread_sigmask(_signal.SIG_BLOCK, [_signal.SIGUSR1])
    os.kill(os.getpid(), _signal.SIGUSR1)
    del thing
    try:
        result = both(_signal.pthread_sigmask, _signal.SIG_UNBLOCK, [_signal.SIGUSR1])
    except KeyError as e:
        result = "caught " + repr(e)
    print(result, log, reference() is None)
