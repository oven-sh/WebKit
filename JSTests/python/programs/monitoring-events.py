import sys
m = sys.monitoring; E = m.events
TOOL = 2
NAMES = {v: n for n, v in vars(E).items() if v}
log = []
def short(a):
    if a is m.MISSING: return "MISSING"
    if a is None or isinstance(a, (int, str, tuple, list, bool)): return a
    if isinstance(a, BaseException): return type(a).__name__ + repr(a.args)
    return getattr(a, "__qualname__", None) or type(a).__name__
def line_of(code, offset):
    for s, e, l in code.co_lines():
        if s <= offset < e: return None if l is None else l - code.co_firstlineno
def recorder(event):
    name = NAMES[event]
    def callback(code, second, *rest):
        if code.co_filename != __file__ or code.co_name in ("record", "stop"): return
        if name == "LINE": log.append((code.co_name, name, second - code.co_firstlineno))
        elif name in ("BRANCH_LEFT", "BRANCH_RIGHT", "JUMP"): log.append((code.co_name, name, line_of(code, second), line_of(code, rest[0])))
        # Where what catches an exception is, and what sends it on, is for each implementation to say.
        elif name in ("PY_UNWIND", "RERAISE", "EXCEPTION_HANDLED"): log.append((code.co_name, name) + tuple(short(r) for r in rest))
        else: log.append((code.co_name, name, line_of(code, second)) + tuple(short(r) for r in rest))
    return callback
def stop(): m.set_events(TOOL, 0)
def record(events, f, *a):
    log.clear(); m.use_tool_id(TOOL, "test")
    for i in range(19):
        if events >> i & 1 and (1 << i) != E.BRANCH: m.register_callback(TOOL, 1 << i, recorder(1 << i))
    try:
        m.set_events(TOOL, events)
        try: f(*a)
        except Exception: pass
        stop()
    finally:
        m.free_tool_id(TOOL)
    return list(log)
def show(label, events, f, *a):
    print(label, f.__name__)
    for e in record(events, f, *a): print("   ", *e)
FRAMES = E.PY_START | E.PY_RESUME | E.PY_RETURN | E.PY_YIELD | E.PY_UNWIND | E.PY_THROW
ERRORS = E.RAISE | E.RERAISE | E.EXCEPTION_HANDLED | E.PY_UNWIND | E.STOP_ITERATION
CALLS = E.CALL | E.C_RETURN | E.C_RAISE
def py(x=0): return x
def simple():
    a = py(1)
    return a
def gen():
    x = yield 1
    yield x
    return 3
def generators():
    g = gen()
    next(g)
    g.send("s")
    for _ in g: pass
def throws():
    g = gen()
    next(g)
    try: g.throw(ValueError("t"))
    except ValueError: pass
    g2 = gen()
    next(g2)
    g2.close()
def raises():
    raise KeyError("k")
def propagates():
    raises()
def catches():
    try:
        propagates()
    except KeyError:
        pass
def finallies():
    try:
        try:
            raise ValueError("v")
        finally:
            x = 1
    except ValueError:
        y = 2
def bare():
    try:
        try:
            raise ValueError("v")
        except ValueError:
            raise
    except ValueError:
        pass
def named():
    try:
        raise ValueError("v")
    except ValueError as e:
        z = 1
def no_match():
    try:
        try:
            raise ValueError("v")
        except KeyError:
            pass
    except ValueError:
        pass
def withs():
    try:
        with M():
            raise ValueError("v")
    except ValueError:
        pass
    with M(True):
        raise ValueError("w")
class M:
    def __init__(s, r=False): s.r = r
    def __enter__(s): return s
    def __exit__(s, *a): return s.r
class It:
    def __iter__(s): return s
    def __next__(s): raise StopIteration
def iterators():
    for _ in It(): pass
    for _ in gen(): pass
    for _ in [1]: pass
def delegates():
    def outer():
        r = yield from gen()
        return r
    list(outer())
def calls():
    py()
    py(1)
    len("a")
    [].append(1)
    list.append([], 1)
    int("1")
    M()
    M().__enter__()
    py(x=1)
    py(*(1,))
    len(*["a"])
    print(end="")
    try: int("x")
    except ValueError: pass
def callable_objects():
    class K:
        def __call__(s, a): return a
    K()(1)
    (lambda: 0)()
    b = M().__enter__
    b()
def branches(a):
    if a:
        x = 1
    else:
        x = 2
    y = 1 if a else 2
    while a:
        a -= 1
    for i in range(2):
        pass
    z = a and 1
    w = a or 1
    if a is None:
        pass
    if not a:
        pass
    return z
def jumps(a):
    for i in range(2):
        if i:
            continue
        x = 1
    while True:
        break
    if a:
        x = 1
    else:
        x = 2
    return x
def lines():
    a = 1
    for i in range(2):
        a += i
    while a > 2: a -= 1
    return a
for f in (simple, generators, throws, catches, delegates): show("frames", FRAMES, f)
for f in (catches, bare, named, no_match, iterators, delegates): show("errors", ERRORS, f)
# How many times an exception is caught and sent on by what nobody wrote differs.
for f in (finallies, withs, throws): show("raised", E.RAISE | E.PY_UNWIND | E.STOP_ITERATION | E.PY_THROW, f)
for f in (calls, callable_objects, generators): show("calls", CALLS, f)
show("call only", E.CALL, simple)
for a in (0, 2): show("branches", E.BRANCH_LEFT | E.BRANCH_RIGHT, branches, a)
show("lines", E.LINE, lines)
show("everything", FRAMES | ERRORS | CALLS | E.LINE, catches)
