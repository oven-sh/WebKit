# A signal that has been kept back is let through, and nothing has been said of what is to be done with it. That is the end of the process, as it would have been had it not been kept back.
import signal
import subprocess
import sys

PROGRAM = """
import os, signal, sys
signal.pthread_sigmask(signal.SIG_BLOCK, [signal.%(name)s])
%(before)s
os.kill(os.getpid(), signal.%(name)s)
print("it is still there", sorted(s.name for s in signal.sigpending()), flush=True)
signal.pthread_sigmask(signal.%(how)s, %(mask)s)
print("it has been let through", flush=True)
"""

for label, values in (
    ("let through", {"name": "SIGUSR2", "before": "", "how": "SIG_UNBLOCK", "mask": "[signal.SIGUSR2]"}),
    ("with all that are kept back said at once", {"name": "SIGUSR2", "before": "", "how": "SIG_SETMASK", "mask": "[]"}),
    ("another", {"name": "SIGTERM", "before": "", "how": "SIG_UNBLOCK", "mask": "[signal.SIGTERM]"}),
    ("a function was to be called, and no longer is", {"name": "SIGUSR2", "before": "signal.signal(signal.SIGUSR2, print); signal.signal(signal.SIGUSR2, signal.SIG_DFL)", "how": "SIG_UNBLOCK", "mask": "[signal.SIGUSR2]"}),
    ("one that nothing is done about as a rule", {"name": "SIGCHLD", "before": "", "how": "SIG_UNBLOCK", "mask": "[signal.SIGCHLD]"}),
    ("one that is to be ignored", {"name": "SIGUSR2", "before": "signal.signal(signal.SIGUSR2, signal.SIG_IGN)", "how": "SIG_UNBLOCK", "mask": "[signal.SIGUSR2]"}),
    ("some other is let through", {"name": "SIGUSR2", "before": "", "how": "SIG_UNBLOCK", "mask": "[signal.SIGUSR1]"}),
):
    done = subprocess.run([sys.executable, "-c", PROGRAM % values], capture_output=True, timeout=60)
    print(label, "=>", (done.stdout.decode().splitlines(), done.stderr.decode(), signal.Signals(-done.returncode).name if done.returncode < 0 else done.returncode))
