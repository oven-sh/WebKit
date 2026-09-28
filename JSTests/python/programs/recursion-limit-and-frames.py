import sys
def frames(e):
    n = 0; tb = e.__traceback__
    while tb: n += 1; tb = tb.tb_next
    return n
def deep(n=0): return deep(n + 1)
try: deep()
except RecursionError as e: print("frames in the traceback", frames(e))
# A generator that is resumed one too deep does not go on, and catches nothing.
log = []
def gen():
    try:
        yield 1
        yield 2
    except RecursionError:
        log.append("caught in the generator"); yield 3
    finally:
        log.append("finally")
def resume_at_the_limit(g, n):
    if n: return resume_at_the_limit(g, n - 1)
    return next(g)
def at_limit(f, *a):
    # The most that can be gone down without error, and then one more.
    lo = 1
    while True:
        try: f(*a, lo)
        except RecursionError: return lo
        lo += 1
def fresh(n):
    g = gen(); next(g); return resume_at_the_limit(g, n)
sys.setrecursionlimit(60)
n = at_limit(fresh)
g = gen(); next(g)
try: resume_at_the_limit(g, n)
except RecursionError as e: print("raised", frames(e) - n, log, g.gi_frame is None, list(g))
def starts(n):
    g = gen()
    return resume_at_the_limit(g, n)
n2 = at_limit(starts); print("the same depth for one that has not begun", n2 == n)
events = []
def tracer(frame, event, arg):
    if frame.f_code.co_name == "leaf": events.append(event)
    return tracer
def leaf(): return 1
def down(n): return down(n - 1) if n else leaf()
k = at_limit(down)
sys.settrace(tracer)
try: down(k)
except RecursionError: pass
sys.settrace(None); print("nothing is told of what did not begin", events)
sys.setrecursionlimit(1000)
