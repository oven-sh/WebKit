# Whatever goes wrong in getting something to await from what __anext__() gave is the cause of a TypeError that says so.
def run(coro):
    try: coro.send(None)
    except StopIteration as e: return ("returned", e.value)
    except BaseException as e: return (type(e).__name__, str(e), repr(e.__cause__), repr(e.__context__), e.__suppress_context__)
class F:
    def __init__(self, what): self.what = what
    def __aiter__(self): return self
    def __anext__(self): return self
    def __await__(self):
        if isinstance(self.what, BaseException): raise self.what
        return self.what
async def main(what):
    async for _ in F(what): pass
for what in (ZeroDivisionError("z"), TypeError("t"), KeyboardInterrupt("k"), StopIteration("s"), 5, None):
    print(type(what).__name__, run(main(what)))
class NoAwait:
    def __aiter__(self): return self
    def __anext__(self): return 5
async def main2():
    async for _ in NoAwait(): pass
print(run(main2()))
