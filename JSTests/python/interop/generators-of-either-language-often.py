# A generator is gone on with in the same way whichever language it is written in, and whichever language the loop is written in. Each of these is run until it has been compiled.
import js
import sys


def show(label, f, *arguments):
    def outcome():
        try:
            return f(*arguments)
        except BaseException as e:
            return type(e).__name__, str(e)
    first = outcome()
    for i in range(700):
        now = outcome()
        if now != first:
            print(label, "=>", ("AT FIRST", first, "AND THEN", now))
            return
    print(label, "=>", first)


js_count = js.eval("(function* jsCount(n) { for (let i = 0; i < n; ++i) yield i; return 'returned'; })")
js_fails = js.eval("(function* jsFails(n) { for (let i = 0; i < n; ++i) yield i; throw new RangeError('from JavaScript'); })")
js_cleans_up = js.eval("(function* jsCleansUp(log) { try { yield 1; yield 2; yield 3; } finally { log.push('cleaned up'); } })")
js_through = js.eval("(function* jsThrough(g) { for (const v of g) yield v; })")
js_delegates = js.eval("(function* jsDelegates(g) { return yield* g; })")
js_sum = js.eval("(function jsSum(g) { let s = 0; for (const v of g) s += v; return s; })")
js_calls_back = js.eval("(function* jsCallsBack(f, n) { for (let i = 0; i < n; ++i) yield f(i); })")


def count(n):
    for i in range(n):
        yield i


def handling():
    e = sys.exception()
    return type(e).__name__ if e else None


print("---- a loop in Python, a generator of JavaScript's")
show("what it yields", lambda: [v for v in js_count(5)])
show("added up", lambda: sum(v for v in js_count(50)))
show("gone through again", lambda: (lambda g: ([v for v in g], [v for v in g]))(js_count(3)))
show("what gets out of it", lambda: [v for v in js_fails(2)])


def caught(n):
    seen = []
    try:
        for v in js_fails(n):
            seen.append(v)
    except Exception as e:
        return seen, type(e).__name__, str(e), isinstance(e, js.RangeError)


def left():
    log = js.Array()
    g = js_cleans_up(log)
    for v in g:
        break
    return v, list(log), next(g), list(log)


def while_handling(n):
    try:
        raise KeyError
    except KeyError:
        return [(v, handling()) for v in js_calls_back(lambda i: handling(), n)], handling()


show("caught", caught, 3)
show("left on the way", left)
show("that calls back, while something is being handled", while_handling, 2)

print("---- one language and then the other")
show("Python, JavaScript, Python", lambda: [v for v in js_through(count(4))])
show("and JavaScript again", lambda: js_sum(js_through(v * 2 for v in js_count(5))))
show("by way of yield*", lambda: [v for v in js_delegates(count(3))])


def fails(n):
    for i in range(n):
        yield i
    raise ValueError("from Python")


def stops(n):
    for i in range(n):
        yield i
    raise StopIteration("from Python")


show("what gets out, all the way", lambda: [v for v in js_through(fails(2))])
show("a StopIteration", lambda: [v for v in js_through(stops(2))])

print("---- a loop in JavaScript, a generator of Python's")
show("added up", lambda: js_sum(count(50)))
show("what gets out of it", lambda: js_sum(fails(3)))
