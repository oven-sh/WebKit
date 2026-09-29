# generator.throw(), of a generator that is waiting on another with `yield from`. What is thrown goes to the one that is waited on, as it was given, and those that are waiting are not woken for it. They go on only if
# that one has come to an end. What is watching sees which frames are gone into and what is raised in them, and never anything that is not an exception.
import sys
import _warnings

_warnings.filters.insert(0, ("ignore", None, Warning, None, 0))

# CPython closes a generator as soon as nothing has it, which what is watching would see. So something goes on having every one of them.
kept = []


def keep(value):
    kept.append(value)
    return value


def inner():
    try:
        yield 1
    except KeyError as e:
        yield ("inner caught", e.args)


def returning():
    try:
        yield 1
    except KeyError:
        return "returned"


def raising():
    try:
        yield 1
    except KeyError:
        raise LookupError("from inner")


def middle(iterator):
    return (yield from iterator)


def outer(iterator):
    try:
        x = yield from keep(middle(iterator))
        yield ("outer got", x)
    except (TypeError, LookupError) as e:
        yield ("outer caught", type(e).__name__, str(e))


class Thing:
    def __iter__(self): return self
    def __next__(self): return "thing"
    def throw(self, *a): return ("thing thrown", a)


class ThingThatStops(Thing):
    def throw(self, *a): raise StopIteration("stopped")


class ThingWithoutThrow:
    def __iter__(self): return self
    def __next__(self): return "thing"


class ThingThatIsClosed(ThingWithoutThrow):
    def close(self): print("    closed")


def attempt(make, arguments):
    g = keep(outer(keep(make())))
    next(g)
    states = lambda: (g.gi_running, g.gi_suspended, type(g.gi_yieldfrom).__name__)
    before = states()
    try:
        r = g.throw(*arguments)
    except BaseException as e:
        r = type(e).__name__ + ": " + str(e)
    return r, before, states()


cases = [(make, arguments) for make in (inner, returning, raising, Thing, ThingThatStops, ThingWithoutThrow, ThingThatIsClosed) for arguments in ((KeyError,), (KeyError, (1, 2)), (KeyError(3),), (ValueError,), (None,), (KeyError(1), 2), (KeyError, None, 5), (GeneratorExit,))]
said = lambda arguments: [getattr(a, "__name__", repr(a)) for a in arguments]

print("---- with nothing watching")
for make, arguments in cases:
    print(make.__name__, said(arguments), "=>", attempt(make, arguments))

print("---- traced")
log = []


# Only what is in this file: what a weak reference calls when what it refers to has gone can be run at any time, and importlib has such.
def tracer(frame, event, arg):
    if event in ("exception", "call", "return") and frame.f_code.co_filename == __file__ and frame.f_code.co_name not in ("attempt", "keep", "<lambda>"):
        log.append((event, frame.f_code.co_name, type(arg).__name__ if event != "exception" else (arg[0].__name__, type(arg[1]).__name__)))
    return tracer


for make, arguments in cases:
    del log[:]
    sys.settrace(tracer)
    r = attempt(make, arguments)
    sys.settrace(None)
    print(make.__name__, said(arguments), "=>", r)
    print("   ", log)

print("---- monitored")
M = sys.monitoring
M.use_tool_id(1, "t")
seen = []
# How often something is handled and raised again on its way goes by how what nobody wrote is put together, which is not the same here.
for name in ("PY_THROW", "PY_RESUME", "RAISE", "PY_UNWIND", "PY_YIELD", "PY_RETURN"):
    M.register_callback(1, getattr(M.events, name), (lambda name: lambda code, offset, *value: code.co_filename != __file__ or code.co_name in ("attempt", "keep", "<lambda>") or seen.append((name, code.co_name, *[type(v).__name__ for v in value])))(name))
M.set_events(1, M.events.PY_THROW | M.events.PY_RESUME | M.events.RAISE | M.events.PY_UNWIND | M.events.PY_YIELD | M.events.PY_RETURN)
for make, arguments in cases:
    del seen[:]
    r = attempt(make, arguments)
    print(make.__name__, said(arguments), "=>", r)
    print("   ", seen)
M.set_events(1, 0)

print("---- while it is being thrown into")


def nosy():
    try:
        yield 1
    except KeyError:
        try:
            watched[0].throw(ValueError)
        except ValueError as e:
            yield ("it is busy", str(e), watched[0].gi_running)


watched = [keep(outer(keep(nosy())))]
next(watched[0])
print(watched[0].throw(KeyError), watched[0].gi_running)
