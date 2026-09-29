# What an iterator says as it ends, when something else is going through it.
def returns():
    return "returned"
    yield


def returns_none():
    return
    yield


class Raises:
    def __iter__(self): return self
    def __next__(self): raise StopIteration("raised", 2)


class RaisesDerived:
    class Stop(StopIteration): pass
    def __iter__(self): return self
    def __next__(self): raise self.Stop("derived")


WRAPPERS = (("itself", lambda it: it), ("iter", iter), ("filter", lambda it: filter(None, it)), ("filter with a function", lambda it: filter(abs, it)), ("map", lambda it: map(abs, it)), ("map of two", lambda it: map(max, [1], it)), ("map of two, the other way", lambda it: map(max, it, [1])),
            ("zip", lambda it: zip(it)), ("zip of two", lambda it: zip([1], it)), ("zip of two, the other way", lambda it: zip(it, [1])), ("enumerate", enumerate), ("strict zip", lambda it: zip(it, strict=True)), ("strict zip of two", lambda it: zip(it, [], strict=True)), ("strict map", lambda it: map(abs, it, strict=True)),
            ("filter of map of zip", lambda it: filter(None, map(len, zip(it)))))
for make in (returns, returns_none, Raises, RaisesDerived):
    for label, wrap in WRAPPERS:
        it = wrap(make())
        out = []
        for attempt in range(2):
            try:
                out.append(next(it))
            except StopIteration as e:
                out.append((type(e).__name__, e.args, e.value))
        print(make.__name__, label, out, next(wrap(make()), "the default"), list(wrap(make())), [x for x in wrap(make())])


def delegates(it):
    got = yield from it
    print("yield from got", got)


for make in (returns, Raises):
    for label, wrap in WRAPPERS[:6]:
        print(make.__name__, label, list(delegates(wrap(make()))))
