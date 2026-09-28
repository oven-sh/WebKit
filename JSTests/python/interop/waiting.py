import js
ValueError = ValueError; KeyError = KeyError
log = []
async def double(x): return x * 2
async def fails(): raise ValueError("from coroutine")
async def awaits(p): return ("got", await p)
async def catches(p):
    try: return ("no error", await p)
    except BaseException as e: return (type(e).__name__, str(e))
async def catches_anything(p):
    try: await p
    except: return "bare except caught it"
async def finally_runs(p):
    try: await p
    finally: log.append("finally ran")
async def nested(n): return n if n == 0 else 1 + await nested(n - 1)
async def worker(name, n):
    for i in range(n):
        log.append(name + str(i))
        await js.Promise.resolve()
    return name
async def yields_turn(name):
    log.append(name + " before"); await Turn(); log.append(name + " after")
class Turn:
    def __await__(self): yield
class Custom:
    def __init__(self, p): self.p = p
    def __await__(self):
        v = yield self.p
        return ("custom", v)
class BadYield:
    def __await__(self):
        try: yield 5
        except RuntimeError as e: return str(e)
class NotIterator:
    def __await__(self): return 5
async def await_obj(o): return await o
async def gather(*cs): return list(await js.Promise.all(list(cs)))
async def returns_coroutine(): return double(4)
async def sleeps(ms): await js.Promise.new(lambda resolve, reject: js.setTimeout(resolve, ms)); return "slept"
def start(c): return js.Promise.resolve(c)
def is_promise(p): return (type(p).__name__, type(p) is js.Promise, isinstance(p, js.Promise), hasattr(p, "__await__"), hasattr(js.Object.new(), "__await__"))
def by_hand(p):
    c = awaits(p); y = c.send(None); same = y is p
    try: c.send("sent by hand")
    except StopIteration as e: return (same, e.value)
def awaiter(p):
    a = p.__await__(); r = [type(a).__name__, iter(a) is a, next(a) is p]
    try: a.send(1)
    except StopIteration as e: r.append(e.value)
    try: next(a)
    except RuntimeError as e: r.append(str(e))
    return r
async def exits(): raise SystemExit(3)
def take_log():
    out = list(log); log.clear(); return out
# ---- asynchronous iteration
async def agen(n):
    try:
        for i in range(n):
            got = yield i
            if got is not None: log.append(("sent", got))
    finally:
        log.append("agen closed")
async def agen_fails():
    yield 1
    raise KeyError("in agen")
async def agen_awaits():
    for i in range(2): yield await js.Promise.resolve(i * 10)
class AIter:
    def __init__(self): self.n = 0
    def __aiter__(self): return self
    async def __anext__(self):
        self.n += 1
        if self.n > 2: raise StopAsyncIteration
        return "item" + str(self.n)
async def consume(ai): return [v async for v in ai]
async def consume_some(ai):
    async for v in ai:
        if v == "b": break
    return v
async def consume_catch(ai):
    out = []
    try:
        async for v in ai: out.append(v)
    except BaseException as e: out.append(type(e).__name__ + ": " + str(e))
    return out
async def with_builtins(ai):
    i = aiter(ai); return [await anext(i), await anext(i), await anext(i, "default")]
