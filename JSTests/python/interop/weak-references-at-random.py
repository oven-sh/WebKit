# Objects and weak references to them are made and let go of at random, with the collector run in between, and what there is is compared with what there should be. What refers to what is kept in lists that the collector
# is not told of, so what would go wrong here is not an answer but the engine.
import js
from _weakref import ref, proxy, getweakrefcount, getweakrefs

state = [12345]


def random(n):
    state[0] = (state[0] * 1103515245 + 12345) % 2147483648
    return (state[0] >> 8) % n


class C:
    def __init__(self, n): self.n = n


class Derived(ref):
    pass


called = []
expected_calls = []
objects = {}    # number -> object
held = {}       # number -> the references to it that are kept, whether or not it is
serial = [0]
wrong = []


def make_reference(n):
    o = objects[n]
    kind = random(5)
    if kind == 0:
        r = ref(o)
    elif kind == 1:
        r = proxy(o)
    elif kind == 2:
        serial[0] += 1
        r = ref(o, lambda r, s=serial[0]: called.append(s))
        serials.append((r, serial[0]))
    elif kind == 3:
        r = Derived(o)
    else:
        serial[0] += 1
        r = proxy(o, lambda r, s=serial[0]: called.append(s))
        serials.append((r, serial[0]))
    if not any(r is other for other in held[n]):
        held[n].append(r)


serials = []    # Each reference that has a callback and is kept, and which callback that is.


def serial_of(r):
    for other, s in serials:
        if other is r:
            return s


def check(step):
    for n, o in objects.items():
        if getweakrefcount(o) != len(held[n]):
            wrong.append((step, "count", n, getweakrefcount(o), len(held[n])))
        found = getweakrefs(o)
        if len(found) != len(held[n]) or not all(any(f is h for h in held[n]) for f in found):
            wrong.append((step, "which", n))
        for r in held[n]:
            if type(r) in (ref, Derived) and r() is not o:
                wrong.append((step, "refers", n))
    for n, references in held.items():
        if n not in objects:
            for r in references:
                if type(r) in (ref, Derived) and r() is not None:
                    wrong.append((step, "still there", n))
    if sorted(called) != sorted(expected_calls):
        wrong.append((step, "callbacks", len(called), len(expected_calls)))
    # Only now, when what has gone has been found to have: a reference that goes before that goes with what it refers to, and its callback is not called.
    for n in [n for n in held if n not in objects]:
        for r in held[n]:
            serials[:] = [pair for pair in serials if pair[0] is not r]
        del held[n]


def step(i):
    what = random(10)
    if what < 3 or not objects:
        n = i
        objects[n] = C(n)
        held[n] = []
    elif what < 7:
        make_reference(list(objects)[random(len(objects))])
    elif what < 8:
        # A reference is let go of. Its callback is not to be called.
        n = list(held)[random(len(held))]
        if held[n] and n in objects:
            r = held[n].pop(random(len(held[n])))
            serials[:] = [pair for pair in serials if pair[0] is not r]
    elif what < 9:
        n = list(objects)[random(len(objects))]
        del objects[n]
        for r in held[n]:
            if serial_of(r) is not None:
                expected_calls.append(serial_of(r))
    else:
        # References that nothing keeps.
        n = list(objects)[random(len(objects))]
        for again in range(random(20)):
            ref(objects[n], lambda r: called.append("never"))
    if what == 5:
        js.edenGC()


# What has been let go of is gone once the collector finds that nothing has it, and this is about what there is then, not about how soon that is. Two things can put it off.
# The collector takes whatever it finds on the stack for a reference, and a function that has only just begun has registers that it has given nothing yet, which have in them what the last function to be called from
# the same place left there. So it is run from here, and not from a function that this calls.
# And while a function is being compiled, on another thread, the compiler keeps what the function had in its variables when that was begun. So that is waited for.
for i in range(6000):
    step(i)
    if i % 150 == 149:
        getattr(js, "$vm").completeAllJITPlans()
        js.fullGC()
        check(i)
getattr(js, "$vm").completeAllJITPlans()
js.fullGC()
check("the end")
print("objects", len(objects), "references", sum(len(v) for v in held.values()), "callbacks", len(called), "wrong", wrong[:5])
