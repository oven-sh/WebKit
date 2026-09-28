import sys
events = []
def tracer(frame, event, arg):
    if frame.f_code.co_filename != __file__: return None
    a = arg if event in ("return",) else (arg[0].__name__ if event == "exception" else None)
    events.append((frame.f_code.co_name, event, frame.f_lineno - base(frame), a if isinstance(a, (int, str, list, tuple, type(None))) else type(a).__name__))
    return tracer
def base(frame): return frame.f_code.co_firstlineno
def trace(f, *a):
    events.clear(); sys.settrace(tracer)
    try:
        try: f(*a)
        except Exception as e: pass
    finally: sys.settrace(None)
    return [e for e in events if e[0] not in ("trace",)]
def show(f, *a):
    print(f.__name__)
    for e in trace(f, *a): print("   ", *e)
def simple():
    x = 1
    y = 2
    return x + y
def one_line(): return 1
def branches(a):
    if a:
        x = 1
    else:
        x = 2
    return x
def loop():
    t = 0
    for i in range(2):
        t += i
    return t
def while_loop():
    n = 2
    while n:
        n -= 1
    else:
        n = 9
    return n
def same_line_loop():
    n = 3
    while n: n -= 1
    for i in range(2): pass
    return n
def calls():
    a = simple()
    return a
def multi_line():
    x = [1,
         2,
         3]
    y = max(1,
            2)
    return (x,
            y)
def passes():
    pass
    pass
    "string"
    ...
def breaks():
    for i in range(5):
        if i == 1:
            continue
        if i == 2:
            break
    else:
        return "no"
    return i
def tries():
    try:
        x = 1
        raise ValueError("v")
    except KeyError:
        x = 2
    except ValueError as e:
        x = 3
    else:
        x = 4
    finally:
        x = 5
    return x
def raises():
    x = 1
    raise KeyError("k")
def propagates():
    raises()
    return 1
def catches():
    try:
        propagates()
    except KeyError:
        return "caught"
def gen():
    yield 1
    x = yield 2
    return 3
def uses_gen():
    g = gen()
    a = next(g)
    b = next(g)
    for c in g:
        pass
    return a
def with_block():
    with cm() as c:
        x = 1
    return x
class cm:
    def __enter__(self):
        return self
    def __exit__(self, *a):
        return False
def klass():
    class K:
        a = 1
        def m(self): pass
    return K
def lambdas():
    f = lambda: 1
    return f()
def comps():
    a = [i for i in range(2)]
    b = {i: i
         for i in range(2)}
    return a
def nested():
    def inner():
        return 1
    return inner()
def ternary(a):
    x = (1 if a
         else 2)
    y = (a and
         1 or
         2)
    return x
def deco(f): return f
def decorated():
    @deco
    @deco
    def h():
        pass
    return h
def semis():
    a = 1; b = 2; c = 3
    return a
def dels():
    x = 1
    del x
    global G
    G = 1
    assert G
    import sys as s
def match_stmt(v):
    match v:
        case 1:
            return "one"
        case [a, b]:
            return "pair"
        case _:
            return "other"
for f, a in [(simple, ()), (one_line, ()), (branches, (1,)), (branches, (0,)), (loop, ()), (while_loop, ()), (same_line_loop, ()), (calls, ()), (multi_line, ()), (passes, ()), (breaks, ()), (tries, ()), (raises, ()), (catches, ()), (uses_gen, ()), (with_block, ()), (klass, ()), (lambdas, ()), (comps, ()), (nested, ()), (ternary, (1,)), (ternary, (0,)), (decorated, ()), (semis, ()), (dels, ()), (match_stmt, (1,)), (match_stmt, ([1, 2],)), (match_stmt, (5,))]:
    show(f, *a)
