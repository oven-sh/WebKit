log = []
def gen():
    try:
        yield 1
        yield 2
    except GeneratorExit:
        log.append("GeneratorExit")
        raise
    finally:
        log.append("finally")
def ignores():
    try:
        yield 1
    except GeneratorExit:
        yield 2
def returns_value():
    try:
        yield 1
    except GeneratorExit:
        return "value"
def take(): r = list(log); log.clear(); return r
class Transaction:
    def __enter__(self): return self
    def __exit__(self, kind, value, traceback):
        log.append("rolled back on " + kind.__name__ if kind else "committed")
        return False
def in_transaction():
    with Transaction():
        yield 1
        yield 2
def close(g): return g.close()
def send(g, v): return g.send(v)
def throw(g, e): return g.throw(e)
def kind(g): return (type(g).__name__, type(g) is type(gen()))
def go_through(g): return list(g)
def first_of(g):
    for x in g: return x
def echo():
    got = yield "ready"
    while True: got = yield got
def look(g):
    out = []
    for n in ("__name__", "__qualname__", "gi_code", "gi_frame", "gi_running", "gi_suspended", "gi_yieldfrom"):
        try: out.append((n, type(getattr(g, n)).__name__))
        except BaseException as e: out.append((n, type(e).__name__ + ": " + str(e)))
    r = repr(g)
    out.append(r[:r.index(" at 0x")] if " at 0x" in r else r)
    return out
