import sys
log = []
def tracer(frame, event, arg):
    what = arg[0].__name__ if event == "exception" else (arg if event == "return" and isinstance(arg, (int, str, type(None))) else None)
    log.append((frame.f_code.co_name, event, frame.f_lineno - frame.f_code.co_firstlineno, what))
    return tracer
def profiler(frame, event, arg):
    log.append((frame.f_code.co_name, event, getattr(arg, "__name__", None) or getattr(arg, "name", None)))
def start(): log.clear(); sys.settrace(tracer)
def start_profile(): log.clear(); sys.setprofile(profiler)
def stop():
    sys.settrace(None); sys.setprofile(None)
    return [e for e in log if e[0] not in ("stop", "start", "start_profile")]
def leaf(x):
    return x + 1
def calls_back(f, x):
    y = f(x)
    return y
def catches(f):
    try:
        f()
    except Exception as e:
        return type(e).__name__
def lets_through(f):
    f()
    return "not reached"
def raises():
    raise KeyError("k")
def backs():
    names = []; f = sys._getframe()
    while f: names.append(f.f_code.co_name); f = f.f_back
    return names
def depth():
    for n in range(1, 100):
        try: sys.setrecursionlimit(n)
        except RecursionError: continue
        sys.setrecursionlimit(1000); return n
def gen():
    yield 1
    yield 2
def recurse_with(f, n): return f(n)
