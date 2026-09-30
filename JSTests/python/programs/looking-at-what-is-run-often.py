# What is run often is compiled for speed, and keeps what it can where it is quickest to get at. Whatever looks at it from outside is to find what it would have found otherwise: its variables, where it has got to, what
# called it. And what is changed from outside is to be changed. Each of these is run until it has been compiled, and looked at then.
import signal
import sys
import traceback


def line_of(frame):
    return frame.f_lineno - frame.f_code.co_firstlineno


def show(label, f, *arguments):
    "The first time, before anything has been compiled, and after it has been run a good many times"
    first = f(*arguments)
    for i in range(400):
        f(*arguments)
    last = f(*arguments)
    print(label, "=>", first if first == last else ("AT FIRST", first, "AND THEN", last))


print("---- the variables of what called")


def peek():
    return dict(sys._getframe(1).f_locals)


def peek_further():
    def inner():
        return dict(sys._getframe(2).f_locals)
    return inner()


def summing(n):
    total = 0
    half = 0.5
    seen = None
    for i in range(n):
        total += i
        half = half * 2.0
        if i == n - 2:
            seen = peek()
    return total, half, seen


def summing_further(n):
    total = 0
    for i in range(n):
        total += i * 2
        name = "x" * (i & 3)
        if i == n - 1:
            return peek_further()


def not_yet_bound(n):
    for i in range(n):
        if i == n - 1:
            before = peek()
    later = 1
    return sorted(before), later


def deleted(n):
    gone = 1
    for i in range(n):
        if i == 1:
            del gone
        if i == n - 1:
            return sorted(peek())


def where():
    frame = sys._getframe(1)
    return line_of(frame), frame.f_lasti >= 0, frame.f_back.f_code.co_name


def stack():
    return [(entry.name, entry.line) for entry in traceback.extract_stack()[-2:]]


def asks_where(n):
    seen = []
    for i in range(n):
        here = where()
        if i == n - 1:
            seen.append(here)
            seen.append(
                where())
            seen.append(stack())
    return seen


show("in a loop", summing, 30)
show("where it has got to", asks_where, 30)
show("from further down", summing_further, 30)
show("one that has nothing yet", not_yet_bound, 30)
show("one that has been deleted", deleted, 30)

print("---- what a variable had, that nothing here looks at again")


def overwritten(n):
    x = n + 5
    seen = peek()
    x = n + 6
    return seen, x


def overwritten_in_a_loop(n):
    x = n * 2
    seen = []
    for i in range(3):
        seen.append(peek().get("x"))
        x = i
    return seen


class Looks:
    def __init__(self):
        self.n = 0
        self.seen = []

    def __iter__(self):
        return self

    def __next__(self):
        self.seen.append(sys._getframe(1).f_locals.get("x"))
        self.n += 1
        if self.n > 3:
            raise StopIteration
        return self.n


def overwritten_first_thing_in_a_loop(n):
    it = Looks()
    x = n + 100
    for i in it:
        x = i * 10
    return it.seen


def never_looked_at(n):
    x = n + 1
    a = peek()
    x = n + 2
    b = peek()
    x = n + 3
    return a["x"], b["x"], peek()["x"]


def overwritten_in_a_handler(n):
    x = n
    try:
        x = n + 1
        [].pop()
    except IndexError:
        x = n + 2
        seen = peek()["x"]
        x = n + 3
    return seen, peek()["x"]


def deleted_and_never_looked_at(n):
    x = n
    a = sorted(peek())
    del x
    b = sorted(peek())
    x = n + 1
    return a, b, sorted(peek())


def in_a_traceback(n):
    def fails():
        x = n + 1
        1 // 0
        x = n + 2
    try:
        fails()
    except ZeroDivisionError as e:
        return e.__traceback__.tb_next.tb_frame.f_locals["x"]


show("before it has something else", overwritten, 1)
show("in a loop", overwritten_in_a_loop, 4)
show("first thing in a loop", overwritten_first_thing_in_a_loop, 1)
show("time and again", never_looked_at, 10)
show("in a handler", overwritten_in_a_handler, 10)
show("deleted", deleted_and_never_looked_at, 10)
show("in a traceback", in_a_traceback, 10)

print("---- changing them")


def poke(name, value):
    sys._getframe(1).f_locals[name] = value


def poked(n):
    x = 1
    total = 0
    for i in range(n):
        if i == n // 2:
            poke("x", 100)
        total += x
    return total, x


def poked_to_another_kind(n):
    x = 1
    parts = []
    for i in range(n):
        if i == n - 3:
            poke("x", 0.5)
        if i == n - 2:
            poke("x", "s")
        parts.append(x * 2)
    return parts[-4:]


def poked_the_loop_variable(n):
    seen = []
    for i in range(n):
        if i == 3:
            poke("i", -1)
        seen.append(i)
    return seen[:6]


def poked_an_argument(n, step):
    total = 0
    for i in range(n):
        if i == n - 2:
            poke("step", 1000)
        total += step
    return total


def poked_something_new(n):
    for i in range(n):
        if i == n - 1:
            poke("made_up", 5)
            return sorted(peek()), sys._getframe().f_locals["made_up"]


show("a variable", poked, 30)
show("to another kind", poked_to_another_kind, 30)
show("what goes round the loop", poked_the_loop_variable, 30)
show("an argument", poked_an_argument, 30, 1)
show("one that there is not", poked_something_new, 30)

print("---- what an exception remembers")


def fails(depth, x):
    local = depth * 2
    if depth:
        return fails(depth - 1, x) + local
    return 1 // x


def where_it_went(e):
    steps = []
    tb = e.__traceback__
    while tb:
        steps.append((tb.tb_frame.f_code.co_name, tb.tb_lineno - tb.tb_frame.f_code.co_firstlineno, sorted((k, v) for k, v in tb.tb_frame.f_locals.items() if isinstance(v, (int, float)))))
        tb = tb.tb_next
    return steps


def catches(x):
    kept = 3.5
    try:
        return fails(3, x)
    except ZeroDivisionError as e:
        return where_it_went(e), kept


def catches_in_a_loop(n):
    caught = 0
    last = None
    for i in range(n):
        try:
            caught += fails(1, i & 1)
        except ZeroDivisionError as e:
            last = where_it_went(e)
    return caught, last


def passes_it_on(n):
    for i in range(n):
        value = i * 1.5
        if i == n - 1:
            [].pop()


def from_something_built_in(n):
    try:
        passes_it_on(n)
    except IndexError as e:
        return str(e), where_it_went(e)


def raised_again(n):
    try:
        try:
            fails(1, 0)
        except ZeroDivisionError:
            marker = n
            raise
    except ZeroDivisionError as e:
        return where_it_went(e)


def cleans_up(n):
    log = []
    for i in range(n):
        try:
            try:
                if i == n - 1:
                    fails(0, 0)
            finally:
                log.append(i)
        except ZeroDivisionError as e:
            return log[-2:], where_it_went(e)


show("with nothing wrong", catches, 1)
show("all the way down", catches, 0)
show("every other time round", catches_in_a_loop, 20)
show("out of something built in", from_something_built_in, 20)
show("raised again", raised_again, 7)
show("with something to be done on the way", cleans_up, 20)

print("---- a frame that is kept")


def own_frame(n):
    total = 0
    for i in range(n):
        total += i
    frame = sys._getframe()
    total += 1000
    return frame


def look_at_own_frame(n):
    frame = own_frame(n)
    return sorted((k, v) for k, v in frame.f_locals.items() if k != "frame"), line_of(frame), frame.f_back is None, frame.f_code.co_name


def frame_taken_by_another(n):
    def take():
        return sys._getframe(1)
    x = 0.25
    for i in range(n):
        x += 1.0
        if i == 2:
            frame = take()
    return frame


def look_at_frame_taken(n):
    frame = frame_taken_by_another(n)
    return sorted((k, v) for k, v in frame.f_locals.items() if isinstance(v, (int, float))), line_of(frame)


def only_now_and_then(n, when):
    total = 0
    for i in range(n):
        total += i
    return (total, sys._getframe()) if when else (total, None)


show("its own", look_at_own_frame, 20)
show("that another took", look_at_frame_taken, 20)
frames = [only_now_and_then(10, i % 100 == 99)[1] for i in range(1000)]
print("only now and then =>", [sorted(f.f_locals.items(), key=str)[:3] for f in frames if f][-1], sum(1 for f in frames if f))

print("---- generators")


def counting(n):
    total = 0
    for i in range(n):
        total += i
        yield total


def look_at_generator(n):
    g = counting(n)
    for k in range(n - 2):
        next(g)
    suspended = dict(g.gi_frame.f_locals), line_of(g.gi_frame)
    g.gi_frame.f_locals["total"] = 1000
    return suspended, next(g), list(g), g.gi_frame


def generator_that_looks(n):
    for i in range(n):
        yield sorted(sys._getframe().f_locals.items()), sys._getframe(1).f_code.co_name


def look_from_generator(n):
    return list(generator_that_looks(n))[-1]


def generator_that_fails(n):
    for i in range(n):
        yield i
    yield 1 // 0


def failing_generator(n):
    try:
        return sum(generator_that_fails(n))
    except ZeroDivisionError as e:
        return where_it_went(e)


show("one that is waiting", look_at_generator, 12)
show("from inside one", look_from_generator, 12)
show("one that fails", failing_generator, 12)

print("---- too deep")


def deeper(n):
    return deeper(n + 1) + 1


def how_deep():
    depth = 0
    def down():
        nonlocal depth
        depth += 1
        down()
    try:
        down()
    except RecursionError as e:
        return depth, str(e)


def survives(n):
    if not n:
        return 0
    try:
        return survives(n - 1) + 1
    except RecursionError:
        return -1000


limit = sys.getrecursionlimit()
sys.setrecursionlimit(120)
show("how far it gets", how_deep)
show("within it", survives, 60)
show("and past it", lambda: survives(500) < 0)
try:
    deeper(0)
except RecursionError as e:
    print("the traceback is", len(where_it_went(e)), "long, of", sorted({name for name, line, variables in where_it_went(e)}))
sys.setrecursionlimit(limit)
show("and after there is room again", survives, 300)

print("---- being told of what is run, beginning in the middle of it")
events = []


def tracer(frame, event, argument):
    if frame.f_code.co_filename == __file__:
        events.append((frame.f_code.co_name, event, line_of(frame), argument if event == "return" else None))
    return tracer


def begin_tracing():
    sys.settrace(tracer)
    sys._getframe(1).f_trace = tracer


def helper(x):
    return x + 1


def traced_from_the_middle(n):
    total = 0
    for i in range(n):
        if i == n - 2:
            begin_tracing()
        total += helper(i)
    return total


def run_traced(n):
    del events[:]
    try:
        result = traced_from_the_middle(n)
    finally:
        sys.settrace(None)
    return result, list(events)


show("with settrace()", run_traced, 25)


def profiler(frame, event, argument):
    if frame.f_code.co_filename == __file__:
        events.append((frame.f_code.co_name, event))


def profiled_from_the_middle(n):
    total = 0
    for i in range(n):
        if i == n - 2:
            sys.setprofile(profiler)
        total += helper(i)
    return total


def run_profiled(n):
    del events[:]
    try:
        result = profiled_from_the_middle(n)
    finally:
        sys.setprofile(None)
    return result, [e for e in events if e[0] != "run_profiled"]


show("with setprofile()", run_profiled, 25)

monitoring = sys.monitoring
TOOL = 3


def on_line(code, line):
    if code.co_filename == __file__:
        events.append((code.co_name, line - code.co_firstlineno))


def monitored_from_the_middle(n):
    total = 0
    for i in range(n):
        if i == n - 2:
            monitoring.set_events(TOOL, monitoring.events.LINE)
        total += helper(i)
    return total


def run_monitored(n):
    del events[:]
    monitoring.use_tool_id(TOOL, "test")
    monitoring.register_callback(TOOL, monitoring.events.LINE, on_line)
    try:
        result = monitored_from_the_middle(n)
    finally:
        monitoring.set_events(TOOL, 0)
        monitoring.register_callback(TOOL, monitoring.events.LINE, None)
        monitoring.free_tool_id(TOOL)
    return result, [e for e in events if e[0] != "run_monitored"]


show("with sys.monitoring", run_monitored, 25)
show("and with nothing again", summing, 30)

print("---- a signal")
handled = []


def handler(number, frame):
    handled.append((frame.f_code.co_name, frame.f_locals.get("i"), frame.f_locals.get("total")))


def interrupted(n):
    total = 0
    for i in range(n):
        if i == n - 3:
            signal.raise_signal(signal.SIGUSR1)
        total += 1
    return total


def handler_that_raises(number, frame):
    raise KeyError("from the handler")


def interrupted_by_an_exception(n):
    total = 0
    try:
        for i in range(n):
            if i == n - 3:
                signal.raise_signal(signal.SIGUSR1)
            total += 1
    except KeyError as e:
        return total in (n - 3, n - 2), str(e), where_it_went(e)[-1][0]
    return "nothing was raised"


def run_interrupted(n):
    del handled[:]
    return interrupted(n), [(name, i) for name, i, total in handled]


previous = signal.signal(signal.SIGUSR1, handler)
show("in a loop", run_interrupted, 25)
signal.signal(signal.SIGUSR1, handler_that_raises)
show("whose handler raises", interrupted_by_an_exception, 25)
signal.signal(signal.SIGUSR1, previous)
