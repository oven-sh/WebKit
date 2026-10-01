# What is all on the line that it begins on begins no line, to sys.monitoring. sys.settrace() is told of one all the same.
import sys
M = sys.monitoring; E = M.events
def one(): pass
def one_r(): return 1
def two():
    pass
def deco(f): return f
@deco
def decorated(): pass
@deco
def decorated2():
    pass
lam = lambda: 1
def gen(): yield 1
def gen2():
    yield 1
class Holder:
    def m(self): return 1
def cls():
    class A: x = 1
    class B:
        x = 1
def comp(): return [i for i in (1, 2)]
def loop():
    for i in (1, 2): pass
def one_loop(): [None for _ in ()]; x = 1
async def co(): return 1
def run():
    one(); one_r(); two(); decorated(); decorated2(); lam(); list(gen()); list(gen2()); Holder().m(); cls(); comp(); loop(); one_loop()
    try: co().send(None)
    except StopIteration: pass
    exec("x = 1"); exec("\nx = 1"); eval("1")
names = ("one", "one_r", "two", "decorated", "decorated2", "<lambda>", "gen", "gen2", "m", "cls", "A", "B", "comp", "loop", "one_loop", "co", "<module>")
seen = []
def line(code, n):
    if code.co_name in names: seen.append((code.co_name, n - code.co_firstlineno))
M.use_tool_id(1, "a"); M.register_callback(1, E.LINE, line); M.set_events(1, E.LINE)
run()
M.set_events(1, 0); M.register_callback(1, E.LINE, None); M.free_tool_id(1)
print(seen)
seen = []
def tracer(frame, event, arg):
    if frame.f_code.co_name in names: seen.append((frame.f_code.co_name, event, frame.f_lineno - frame.f_code.co_firstlineno))
    return tracer
sys.settrace(tracer); run(); sys.settrace(None)
print(seen)
