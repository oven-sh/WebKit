def gen(): yield 1
async def co(): pass
async def ag(): yield 1
class K:
    def m(self): yield 1
c = co()
for x in (gen(), c, ag(), K().m(), (i for i in ())):
    r = repr(x); print(r[:r.index(" at 0x")], str(x) == r, x.__name__, x.__qualname__)
c.close()
