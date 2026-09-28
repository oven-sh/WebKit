import js
log = []
def take_log():
    out = list(log); log.clear(); return out
class CM:
    def __init__(self, name, gives=None, suppress=False): self.name = name; self.gives = gives; self.suppress = suppress
    def __enter__(self): log.append("enter " + self.name); return self.gives if self.gives is not None else self
    def __exit__(self, t, v, tb):
        log.append("exit " + self.name + (" " + t.__name__ + ":" + str(v) if t else "")); return self.suppress
class Transaction:
    def __enter__(self): log.append("begin"); return self
    def __exit__(self, t, v, tb): log.append("rollback" if t else "commit")
class FailsToEnter:
    def __enter__(self): raise ValueError("cannot enter")
    def __exit__(self, *a): log.append("exit must not be called")
class FailsToExit:
    def __enter__(self): return self
    def __exit__(self, *a): raise KeyError("in exit")
class OnlyExit:
    def __exit__(self, *a): pass
class FromGenerator:
    "What contextlib.contextmanager makes."
    def __init__(self, gen): self.gen = gen
    def __enter__(self): return next(self.gen)
    def __exit__(self, t, v, tb):
        if t is None:
            try: next(self.gen)
            except StopIteration: return False
            raise RuntimeError("generator didn't stop")
        try: self.gen.throw(v)
        except StopIteration: return True
        except BaseException as e:
            if e is v: return False
            raise
        raise RuntimeError("generator didn't stop after throw()")
def session(swallow=False):
    def g():
        log.append("open")
        try: yield "the connection"
        except BaseException as e:
            log.append("saw " + type(e).__name__)
            if not swallow: raise
        finally: log.append("close")
    return FromGenerator(g())
class ACM:
    def __init__(self, name, suppress=False): self.name = name; self.suppress = suppress
    async def __aenter__(self): log.append("aenter " + self.name); await js.Promise.resolve(); return "entered " + self.name
    async def __aexit__(self, t, v, tb): await js.Promise.resolve(); log.append("aexit " + self.name + (" " + t.__name__ if t else "")); return [] if not self.suppress else [1]
# ---- with, over what is JavaScript's
def use_with(d):
    with d as v:
        log.append("body")
        return v is d
def use_with_raise(d):
    try:
        with d: raise ValueError("in body")
    except ValueError as e: return "got out: " + str(e)
def use_two(a, b):
    with a, b: log.append("body")
async def use_async_with(d):
    async with d as v:
        log.append("body")
        return v is d
def has_protocol(o): return (hasattr(o, "__enter__"), hasattr(o, "__exit__"))
