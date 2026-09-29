# A signal that comes is seen to, however many come and whatever is going on. Each loop here ends only if it is: one that was lost would leave the program going round for ever.
import os
import signal
import sys
import weakref

count = 0


def handler(number, frame):
    global count
    count += 1


signal.signal(signal.SIGUSR1, handler)
me = os.getpid()

# One at a time, with nothing being done but waiting for it
for i in range(20000):
    before = count
    os.kill(me, signal.SIGUSR1)
    while count == before:
        pass
print("one at a time:", count)


# While what says whether there is anything to see to is being changed for other reasons: the recursion limit, being watched, and weak references that go
class Thing:
    pass


def deep(n):
    return n and deep(n - 1)


count = 0
gone = []
for i in range(3000):
    before = count
    os.kill(me, signal.SIGUSR1)
    sys.setrecursionlimit(1000 + i % 7)
    sys.settrace(None)
    sys.setprofile(lambda *a: None)
    sys.setprofile(None)
    weakref.ref(Thing(), gone.append)
    deep(20)
    while count == before:
        pass
print("with the rest going on:", count)

# As fast as a timer can send them, in calls, loops, generators and comprehensions
count = 0
signal.signal(signal.SIGALRM, handler)
signal.setitimer(signal.ITIMER_REAL, 0.0001, 0.0001)
total = 0
while count < 2000:
    total += sum(x for x in range(50)) + len([deep(3) for _ in range(5)])
signal.setitimer(signal.ITIMER_REAL, 0)
print("from a timer:", count >= 2000, total > 0)


# A function that raises, caught over and over
class Stop(Exception):
    pass


def raiser(number, frame):
    raise Stop


signal.signal(signal.SIGUSR1, raiser)
caught = 0
for i in range(5000):
    try:
        os.kill(me, signal.SIGUSR1)
        while True:
            pass
    except Stop:
        caught += 1
print("raised and caught:", caught)
