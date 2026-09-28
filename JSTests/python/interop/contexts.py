from _contextvars import Context, ContextVar, copy_context
v = ContextVar("v", default="unset")
w = ContextVar("w", default="unset")
def get(): return v.get()
def get_whatever(*a): return v.get()
def put(x): return v.set(x)
def reset(t): v.reset(t)
def get_w(): return w.get()
def put_w(x): return w.set(x)
def in_new(f, *a): return Context().run(f, *a)
def in_copy(f, *a): return copy_context().run(f, *a)
def snapshot(): return copy_context()
def size(): return len(copy_context())
async def sets_and_waits(x, wait):
    before = v.get(); v.set(x); await wait(); middle = v.get(); await wait(); return (before, middle, v.get())
async def reads(wait):
    a = v.get(); await wait(); return (a, v.get())
async def token_across(x, wait):
    t = v.set(x); await wait(); v.reset(t); return v.get()
async def calls(f, wait):
    v.set("from the coroutine"); a = f(); await wait(); return (a, f())
async def awaits_inner(wait):
    v.set("outer coroutine")
    r = await sets_and_waits("inner coroutine", wait)
    return (r, v.get())
async def agen(wait):
    v.set("in the generator"); yield v.get(); await wait(); yield v.get()
