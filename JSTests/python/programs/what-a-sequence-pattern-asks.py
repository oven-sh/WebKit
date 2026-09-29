# What a sequence pattern asks of what it is matched against: whether it asks how long it is, and what it gets, and in what order. [*_] matches however many there are, and does not ask.
import collections.abc
log = []
class S(collections.abc.Sequence):
    def __init__(self, n): self.n = n
    def __len__(self):
        log.append("len"); return self.n
    def __getitem__(self, i):
        log.append("get %r" % (i,))
        if isinstance(i, slice): return list(range(self.n))[i]
        if not -self.n <= i < self.n: raise IndexError(i)
        return i
    def __iter__(self):
        log.append("iter"); return iter(range(self.n))
def m(x):
    log.clear()
    match x:
        case [*_]: r = "star wildcard"
    return r, log[:]
def m2(x):
    log.clear()
    match x:
        case [*rest]: r = ("star", rest)
    return r, log[:]
def m3(x):
    log.clear()
    match x:
        case [_, *_]: r = "one and star"
        case _: r = "no"
    return r, log[:]
def m4(x):
    log.clear()
    match x:
        case []: r = "none"
        case [_]: r = "one"
        case [_, _]: r = "two"
        case [a, *_, b]: r = ("ends", a, b)
    return r, log[:]
def m5(x):
    log.clear()
    match x:
        case [a, *_]: r = ("first", a)
        case _: r = "no"
    return r, log[:]
def m6(x):
    log.clear()
    match x:
        case [*_, a]: r = ("last", a)
        case _: r = "no"
    return r, log[:]
for f in (m, m2, m3, m4, m5, m6):
    print(f.__name__, [f(S(n)) for n in (0, 1, 2, 3)])
class NoLen: pass
collections.abc.Sequence.register(NoLen)
def attempt(f, *a):
    try: return f(*a)
    except BaseException as e: return type(e).__name__ + ": " + str(e)
print([attempt(f, NoLen()) for f in (m, m2, m3, m4, m5, m6)])
