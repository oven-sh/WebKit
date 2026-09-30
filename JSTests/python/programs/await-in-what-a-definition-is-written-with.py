# The defaults, the decorators and the bases of a definition are part of what the definition is in, and may await if that may.
import asyncio
async def value(x): return x
async def outer():
    def g(a=await value(1), *, b=await value(2)): return a, b
    async def h(a=await value(3)): return a
    k = lambda a=await value(4): a
    @(await value(lambda f: f))
    def decorated(): return "decorated"
    class C((await value(object))): x = 5
    return g(), await h(), k(), decorated(), C.x
try: print(asyncio.run(outer()))
except BaseException as e: print(type(e).__name__, e)

for source in ("def f():\n    def g(a=await x): pass", "def f():\n    @(await x)\n    def g(): pass", "def f():\n    class C((await x)): pass", "async def f():\n    def g():\n        def h(a=await x): pass", "async def f():\n    g = lambda: (lambda a=await x: a)"):
    try:
        compile(source, "<test>", "exec")
        print(repr(source), "compiles")
    except SyntaxError as e:
        print(repr(source), e.msg, e.lineno, e.offset)
