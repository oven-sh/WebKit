# pdb.set_trace() stops on the line that it is written on, since what it asks to be told of is the next instruction, and that is what comes after the call.
import sys, io, os, linecache, textwrap, pdb, asyncio

class WithoutTheDirectory:
    "What is written says where this file is, which is not the same everywhere."
    def __init__(self, out): self.out = out
    def write(self, text): return self.out.write(text.replace(os.path.dirname(os.path.abspath(__file__)) + os.sep, ""))
    def flush(self): self.out.flush()
sys.stdout = WithoutTheDirectory(sys.stdout)

class Typed:
    def __init__(self, lines): self.lines = list(lines)
    def readline(self):
        line = self.lines.pop(0)
        print(line)
        return line + "\n"

count = 0
def session(source, commands, call="f()"):
    global count
    count += 1
    name = "<session %d>" % count
    source = textwrap.dedent(source)
    linecache.cache[name] = (len(source), None, source.splitlines(True), name)
    print("=====", count)
    real = sys.stdin
    sys.stdin = Typed(commands)
    try:
        ns = {"__name__": "session"}
        exec(compile(source, name, "exec"), ns)
        if call:
            exec(call, ns)
    except BaseException as e:
        print("raised", type(e).__name__, e)
    finally:
        sys.stdin = real
        sys.settrace(None)

SET = "import pdb; pdb.Pdb(nosigint=True, readrc=False).set_trace()"

session(f"""
    def f():
        x = 1
        {SET}
        y = 2
        return x + y
""", ["p x", "n", "p x", "n", "p y", "c"])

session(f"""
    def f():
        x = 1
        {SET}
""", ["p x", "n", "c"])

session(f"""
    def f():
        for i in range(2):
            {SET}
        return i
""", ["p i", "c", "p i", "n", "n", "c"])

session(f"""
    def g():
        {SET}
        return 5
    def f():
        a = g()
        return a
""", ["w 2", "n", "n", "n", "p a", "c"])

session(f"""
    def stop():
        {SET}
    def f():
        a = 1
        stop()
        b = 2
""", ["u", "p a", "d", "r", "n", "n", "p b", "c"])

session(f"""
    def f():
        p = pdb.Pdb(nosigint=True, readrc=False)
        r = (1, p.set_trace(), 3)
        return r
    import pdb
""", ["l 1, 6", "n", "p r", "c"])

session(f"""
    def f():
        return [pdb.Pdb(nosigint=True, readrc=False).set_trace() or x for x in (7,)]
    import pdb
""", ["p x", "p [x, x]", "c"])

session(f"""
    def gen():
        yield 1
        {SET}
        yield 2
    def f():
        return list(gen())
""", ["n", "n", "n", "c"])

session(f"""
    x = 1
    {SET}
    y = 2
""", ["p x", "n", "n", "c"], call=None)

session(f"""
    def f():
        x = 1
        breakpoint()
        y = 2
""", ["p x", "s", "p x", "c"])

session(f"""
    def h(a):
        return a * 2
    def f():
        {SET}
        b = h(3)
        return b
""", ["n", "s", "a", "r", "n", "p b", "c"])

session(f"""
    def f():
        try:
            {SET}
            1 / 0
        except ZeroDivisionError:
            z = 1
        return z
""", ["n", "n", "n", "n", "p z", "c"])

session(f"""
    def f():
        x = 1
        {SET}
        x = 2
        x = 3
        return x
""", ["j 6", "n", "j 6", "n", "p x", "c"])

session(f"""
    class A:
        def __repr__(self):
            {SET}
            return "A()"
    def f():
        return repr(A())
""", ["w 2", "n", "n", "c"])

session(f"""
    def f():
        with open(__import__("os").devnull) as d:
            {SET}
        return 1
""", ["n", "n", "c"])

session(f"""
    def f():
        x = 1
        {SET}; x = 2
        return x
""", ["p x", "n", "p x", "c"])

session(f"""
    def f():
        x = len([1]) + (pdb.Pdb(nosigint=True, readrc=False).set_trace() or 1) + len([2, 3])
        return x
    import pdb
""", ["n", "p x", "c"])

print("===== await, at the prompt of a debugger that a coroutine stopped in")
session("""
    import asyncio, pdb
    async def get(): return 42
    async def main():
        x = 1
        await pdb.Pdb(nosigint=True, readrc=False).set_trace_async()
        return x
    def f():
        print(asyncio.run(main()))
""", ["p x", "y = await get()", "p y", "await asyncio.sleep(0)", "x = await get()", "p x", "c"])

print("===== the end of a call of what is not written in Python is told of when it ends")
events = []
def profiler(frame, event, arg):
    if frame.f_code.co_name == "profiled":
        events.append((event, frame.f_lineno - frame.f_code.co_firstlineno, getattr(arg, "__name__", None)))
def other(): pass
def profiled():
    a = len("abc")
    b = [len("a"),
         other(),
         max(1, 2)]
    try:
        int("x")
    except ValueError:
        pass
    c = abs(-1) if a else 0
    for i in range(1):
        d = min(1, 2)
    return sorted([len("x")])
sys.setprofile(profiler); profiled(); sys.setprofile(None)
for e in events: print(" ", e)

M = sys.monitoring
told = []
E = M.events
names = {E.CALL: "CALL", E.C_RETURN: "C_RETURN", E.C_RAISE: "C_RAISE", E.LINE: "LINE", E.PY_RETURN: "PY_RETURN", E.PY_START: "PY_START", E.RAISE: "RAISE", E.EXCEPTION_HANDLED: "EXCEPTION_HANDLED"}
M.use_tool_id(3, "t")
def make(event):
    def callback(code, *args):
        if code.co_name == "profiled":
            what = args[0] - code.co_firstlineno if event == E.LINE else getattr(args[1], "__name__", None) if event in (E.CALL, E.C_RETURN, E.C_RAISE) else None
            told.append((names[event], what))
    return callback
for e in names: M.register_callback(3, e, make(e))
M.set_events(3, sum(e for e in names if e not in (E.C_RETURN, E.C_RAISE)))
profiled()
M.set_events(3, 0)
for e in names: M.register_callback(3, e, None)
M.free_tool_id(3)
for t in told: print(" ", t)

print("===== the line that the first instruction after a call is on")
lines = []
def tracer(frame, event, arg):
    if frame.f_code.co_name != "watched":
        return None
    if event == "opcode":
        n = frame.f_lineno - frame.f_code.co_firstlineno
        if not lines or lines[-1] != n:
            lines.append(n)
    return tracer
def begin():
    f = sys._getframe(1)
    f.f_trace = tracer
    f.f_trace_opcodes = True
    f.f_trace_lines = False
    sys.settrace(tracer)
def watched():
    a = 1
    begin()
    b = 2
    c = (3,
         len("x"),
         5)
    return a
watched(); sys.settrace(None)
print(lines)
