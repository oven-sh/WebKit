# A loop that goes through a generator goes on with it each time round. Once the loop has been compiled that is one piece of compiled code calling another, with nothing written in C++ between them, and everything that goes
# with going on with a generator is to be as it was: what is being handled, what is running, what called what, what comes of an exception that gets out.
import sys


def show(label, f, *arguments):
    "The first time, before anything has been compiled, and after it has been run a good many times"
    def outcome():
        try:
            return f(*arguments)
        except BaseException as e:
            return type(e).__name__, str(e), type(e.__cause__).__name__, type(e.__context__).__name__
    first = outcome()
    for i in range(700):
        outcome()
    last = outcome()
    print(label, "=>", first if first == last else ("AT FIRST", first, "AND THEN", last))


def count(n):
    for i in range(n):
        yield i


print("---- what it yields")


def total(n):
    s = 0
    for v in count(n):
        s += v
    return s


def kinds(n):
    def mixed():
        for i in range(n):
            yield i
            yield i * 0.5
            yield str(i)
            yield None
            yield (i,)
    return [type(v).__name__ for v in mixed()][-5:], sum(1 for v in mixed())


def nested(n):
    def outer():
        for a in count(n):
            for b in count(a):
                yield a, b
    return len(list(outer())), [p for p in outer()][-1]


def delegated(n):
    def outer():
        got = yield from inner()
        yield "got", got
    def inner():
        for v in count(n):
            yield v
        return "returned"
    return [v for v in outer()][-2:]


def returns_something(n):
    def g():
        yield 1
        return "not seen by a loop"
    seen = []
    for k in range(n):
        for v in g():
            seen.append(v)
    return len(seen)


def nothing_at_all(n):
    def g():
        return
        yield
    return [v for k in range(n) for v in g()]


show("numbers", total, 20)
show("all sorts", kinds, 4)
show("one in another", nested, 6)
show("by way of another", delegated, 5)
show("what it returns", returns_something, 5)
show("nothing", nothing_at_all, 5)
show("in a comprehension", lambda: ([v * 2 for v in count(5)], {v for v in count(3)}, {v: v for v in count(2)}, list(v for v in count(3)), sum(v for v in count(10) if v % 2)))

print("---- when it has come to an end, or has not begun, or is running")


def again(n):
    g = count(n)
    return [v for v in g], [v for v in g], [v for v in g]


def part_of_the_way(n):
    g = count(n)
    first = []
    for v in g:
        first.append(v)
        if v == 2:
            break
    return first, next(g), [v for v in g], next(g, "no more")


def itself():
    def g():
        yield 1
        for v in me:
            yield v
    me = g()
    return [v for v in me]


def closed(n):
    g = count(n)
    for v in g:
        g.close()
    return v, [v for v in g]


def sent_to(n):
    def g():
        got = None
        for i in range(n):
            got = yield i, got
    it = g()
    seen = []
    for v in it:
        seen.append(v)
        if v[0] == 1:
            seen.append(it.send("sent"))
    return seen


def thrown_into(n):
    def g():
        for i in range(n):
            try:
                yield i
            except KeyError as e:
                yield "caught", str(e)
    it = g()
    seen = []
    for v in it:
        seen.append(v)
        if v == 1:
            seen.append(it.throw(KeyError("thrown")))
    return seen


show("gone through again", again, 3)
show("left, and gone on with", part_of_the_way, 6)
show("from inside itself", itself)
show("closed on the way", closed, 5)
show("sent something on the way", sent_to, 4)
show("thrown something on the way", thrown_into, 4)

print("---- what gets out of it")


def fails_at(n, what):
    for i in range(n):
        yield i
    raise what


def gets_out(what):
    seen = []
    try:
        for v in fails_at(3, what):
            seen.append(v)
    except Exception as e:
        return seen, type(e).__name__, str(e), type(e.__cause__).__name__
    return seen


def afterwards(what):
    g = fails_at(2, what)
    try:
        for v in g:
            pass
    except Exception:
        pass
    return [v for v in g], g.gi_frame, g.gi_running


def where_it_went(e):
    steps = []
    tb = e.__traceback__
    while tb:
        steps.append((tb.tb_frame.f_code.co_name, tb.tb_lineno - tb.tb_frame.f_code.co_firstlineno))
        tb = tb.tb_next
    return steps


def traced_back():
    def inner():
        yield 1
        1 // 0
    def outer():
        for v in inner():
            yield v
    try:
        for v in outer():
            pass
    except ZeroDivisionError as e:
        return where_it_went(e)


show("an exception", gets_out, ValueError("x"))
show("StopIteration", gets_out, StopIteration("y"))
show("one derived from it", gets_out, type("Mine", (StopIteration,), {})("z"))
show("StopAsyncIteration", gets_out, StopAsyncIteration("w"))
show("and it is over", afterwards, ValueError("x"))
show("where it has been", traced_back)
show("not caught here", lambda: [v for v in fails_at(2, KeyError("k"))])

print("---- what is being handled")


def handling():
    e = sys.exception()
    return type(e).__name__ if e else None


def looks(n):
    for i in range(n):
        yield handling()


def yields_while_handling(n):
    for i in range(n):
        try:
            raise KeyError(i)
        except KeyError:
            yield handling()
            yield handling()
        yield handling()


def from_nothing(n):
    return [v for v in looks(n)], handling()


def from_a_handler(n):
    try:
        raise ValueError
    except ValueError:
        return [v for v in looks(n)], handling()


def its_own(n):
    seen = []
    for v in yields_while_handling(n):
        seen.append((v, handling()))
    return seen, handling()


def its_own_from_a_handler(n):
    try:
        raise ValueError
    except ValueError:
        seen = []
        for v in yields_while_handling(n):
            seen.append((v, handling()))
        return seen, handling()


def raises_again():
    def g():
        try:
            raise KeyError("first")
        except KeyError:
            yield 1
            raise
    try:
        for v in g():
            try:
                raise ValueError("meanwhile")
            except ValueError:
                pass
    except KeyError as e:
        return str(e), type(e.__context__).__name__, handling()


def has_for_context():
    def g():
        yield 1
        raise KeyError("in the generator")
    try:
        raise ValueError("outside")
    except ValueError:
        try:
            for v in g():
                pass
        except KeyError as e:
            return type(e.__context__).__name__, handling()


def two_at_once(n):
    a, b = yields_while_handling(n), looks(n * 3)
    return [(x, next(b)) for x in a], handling()


def left_while_handling():
    g = yields_while_handling(2)
    for v in g:
        break
    return v, handling(), next(g), handling(), [v for v in g], handling()


show("nothing", from_nothing, 3)
show("what whoever goes through it is handling", from_a_handler, 3)
show("its own, and only while it runs", its_own, 2)
show("its own, before that of whoever goes through it", its_own_from_a_handler, 2)
show("raised again after something else has been", raises_again)
show("what a new one has for its context", has_for_context)
show("two at once", two_at_once, 2)
show("left in the middle of it", left_while_handling)

print("---- who called whom")


def says_who(n):
    for i in range(n):
        frame = sys._getframe()
        yield frame.f_back.f_code.co_name, frame.f_back.f_lineno - frame.f_back.f_code.co_firstlineno, frame.f_code.co_name


def asks_who(n):
    for v in says_who(n):
        last = v
    return last


def is_it_running(n):
    def g():
        for i in range(n):
            yield me.gi_running, me.gi_frame.f_back is not None
    me = g()
    return [v for v in me][-1], me.gi_running, me.gi_frame


def waiting(n):
    g = count(n)
    for v in g:
        if v == 1:
            return g.gi_running, g.gi_frame.f_back, g.gi_frame.f_lineno - g.gi_code.co_firstlineno, dict(g.gi_frame.f_locals)


def changes_the_caller(n):
    def g():
        for i in range(n):
            sys._getframe(1).f_locals["changed"] = i
            yield i
    changed = None
    for v in g():
        pass
    return changed


show("what went on with it", asks_who, 3)
show("whether it is running", is_it_running, 3)
show("while it waits", waiting, 4)
show("what it changes there", changes_the_caller, 4)

print("---- one that goes through itself, all the way down")


def walk(tree):
    if isinstance(tree, list):
        for branch in tree:
            for leaf in walk(branch):
                yield leaf
    else:
        yield tree


def deep(n):
    tree = 0
    for i in range(n):
        tree = [tree, i]
    return tree


show("a tree", lambda: sum(walk(deep(30))))
limit = sys.getrecursionlimit()
sys.setrecursionlimit(90)
show("too deep a one", lambda: sum(walk(deep(200))))
sys.setrecursionlimit(limit)
show("and after there is room again", lambda: sum(walk(deep(150))))

print("---- being told of what is run, beginning in the middle of it")
events = []


def tracer(frame, event, argument):
    if frame.f_code.co_filename == __file__:
        events.append((frame.f_code.co_name, event, frame.f_lineno - frame.f_code.co_firstlineno, repr(argument) if event in ("return", "exception") and not isinstance(argument, tuple) else type(argument).__name__))
    return tracer


def traced_from_the_middle(n):
    s = 0
    for v in count(n):
        if v == n - 2:
            sys.settrace(tracer)
            sys._getframe().f_trace = tracer
        s += v
    return s


def run_traced(n):
    del events[:]
    try:
        result = traced_from_the_middle(n)
    finally:
        sys.settrace(None)
    return result, list(events)


show("with settrace()", run_traced, 12)
show("and with nothing again", total, 20)
