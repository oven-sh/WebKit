import sys
m = sys.monitoring; E = m.events
def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
    finally:
        sys.settrace(None); sys.setprofile(None)
        for t in range(6): m.free_tool_id(t)
        # In CPython what has been disabled outlasts the tool, if the code is not run before the tool is used again.
        m.restart_events()
def attempt(f):
    try: return f()
    except BaseException as e: return type(e).__name__ + ": " + str(e)
def rel(code, line): return line - code.co_firstlineno
def work():
    a = 1
    b = 2
    return a + b
def loop(n):
    t = 0
    for i in range(n):
        t += i
    return t
# ---- DISABLE
def disables():
    log = []
    def on_line(code, line):
        if code is not loop.__code__: return m.DISABLE
        log.append(rel(code, line)); return m.DISABLE
    m.use_tool_id(0, "t"); m.register_callback(0, E.LINE, on_line); m.set_events(0, E.LINE)
    loop(3); a = list(log); log.clear(); loop(3); b = list(log); log.clear()
    m.restart_events(); loop(2); c = list(log); m.set_events(0, 0)
    return (a, b, c)
show("DISABLE lines", disables)
def disables_some():
    log = []
    def on_line(code, line):
        if code is not loop.__code__: return
        log.append(rel(code, line))
        if rel(code, line) == 3: return m.DISABLE
    m.use_tool_id(0, "t"); m.register_callback(0, E.LINE, on_line); m.set_events(0, E.LINE); loop(3); m.set_events(0, 0)
    return log
show("DISABLE one line", disables_some)
def disables_start():
    log = []
    def on(code, offset):
        if code is work.__code__: log.append("start"); return m.DISABLE
    m.use_tool_id(0, "t"); m.register_callback(0, E.PY_START, on); m.set_events(0, E.PY_START)
    work(); work(); m.restart_events(); work(); m.set_events(0, 0)
    return log
show("DISABLE PY_START", disables_start)
def disables_call():
    log = []
    def f():
        len("a"); len("b")
    def on(name):
        def cb(code, offset, callable, arg0):
            if code is f.__code__:
                log.append((name, arg0))
                if name == "CALL" and arg0 == "a": return m.DISABLE
        return cb
    m.use_tool_id(0, "t")
    for n in ("CALL", "C_RETURN"): m.register_callback(0, getattr(E, n), on(n))
    m.set_events(0, E.CALL | E.C_RETURN | E.C_RAISE); f(); f(); m.set_events(0, 0)
    return log
show("DISABLE CALL", disables_call)
def cannot_disable(event, f):
    def go():
        seen = []
        def cb(*a): seen.append(1); return m.DISABLE
        m.use_tool_id(0, "t"); m.register_callback(0, event, cb); m.set_events(0, event)
        r = attempt(f); r2 = attempt(f); m.set_events(0, 0)
        return (r, r2, len(seen), m.register_callback(0, event, None))
    return go
def raiser():
    try: raise KeyError("k")
    except KeyError: return "caught"
def unwinder():
    def u(): raise KeyError("k")
    try: u()
    except KeyError: return "caught"
show("cannot disable RAISE", cannot_disable(E.RAISE, raiser))
show("cannot disable PY_UNWIND", cannot_disable(E.PY_UNWIND, unwinder))
show("cannot disable EXCEPTION_HANDLED", cannot_disable(E.EXCEPTION_HANDLED, raiser))
# ---- local events
def local():
    log = []
    m.use_tool_id(0, "t"); m.register_callback(0, E.LINE, lambda c, l: log.append((c.co_name, rel(c, l))))
    m.set_local_events(0, work.__code__, E.LINE); work(); loop(1); a = list(log); log.clear()
    m.set_local_events(0, work.__code__, 0); work(); return (a, log)
show("local", local)
def local_and_global():
    log = []
    m.use_tool_id(0, "a"); m.use_tool_id(1, "b")
    m.register_callback(0, E.PY_START, lambda c, o: log.append(("a", c.co_name)) if c.co_name in ("work", "loop") else None)
    m.register_callback(1, E.PY_START, lambda c, o: log.append(("b", c.co_name)) if c.co_name in ("work", "loop") else None)
    m.set_events(0, E.PY_START); m.set_local_events(1, work.__code__, E.PY_START); work(); loop(0); m.set_events(0, 0)
    return log
show("local and global, and the order of tools", local_and_global)
def cleared_local():
    log = []
    m.use_tool_id(0, "t"); m.register_callback(0, E.PY_START, lambda c, o: log.append(c.co_name)); m.set_local_events(0, work.__code__, E.PY_START)
    work(); m.clear_tool_id(0); work(); m.register_callback(0, E.PY_START, lambda c, o: log.append("again")); work()
    return (log, m.get_local_events(0, work.__code__))
show("clearing forgets local events", cleared_local)
def local_of_a_copy():
    log = []
    F = type(work); g = F(work.__code__, {})
    m.use_tool_id(0, "t"); m.register_callback(0, E.PY_START, lambda c, o: log.append(c is work.__code__)); m.set_local_events(0, work.__code__, E.PY_START)
    g(); work(); return log
show("a function made from the code", local_of_a_copy)
# ---- callbacks
def raising_callback(event, f):
    def go():
        def cb(*a): raise RuntimeError("from the callback")
        m.use_tool_id(0, "t"); m.register_callback(0, event, cb); m.set_local_events(0, work.__code__, event) if event < E.RAISE else m.set_events(0, event)
        r = attempt(f); m.set_events(0, 0); m.set_local_events(0, work.__code__, 0)
        return r
    return go
for n in ("PY_START", "LINE", "PY_RETURN"):
    show("callback raises in " + n, raising_callback(getattr(E, n), work))
show("callback raises in RAISE", raising_callback(E.RAISE, raiser))
show("callback raises in PY_UNWIND", raising_callback(E.PY_UNWIND, unwinder))
def caught_in_frame():
    def cb(code, line):
        if rel(code, line) == 2: raise RuntimeError("at line 2")
    def f():
        try:
            x = 1
        except RuntimeError as e:
            return "caught " + str(e)
        return "not raised"
    m.use_tool_id(0, "t"); m.register_callback(0, E.LINE, cb); m.set_local_events(0, f.__code__, E.LINE)
    return f()
show("what a callback raises can be caught", caught_in_frame)
def not_told_of_itself():
    log = []
    def helper(): return 1
    def cb(code, offset): log.append(code.co_name); helper(); len("a")
    m.use_tool_id(0, "t"); m.register_callback(0, E.PY_START, cb); m.set_events(0, E.PY_START | E.CALL); work(); m.set_events(0, 0)
    return [n for n in log if n in ("work", "helper", "cb")]
show("nothing is told of what a callback does", not_told_of_itself)
def no_callback():
    m.use_tool_id(0, "t"); m.set_events(0, E.LINE | E.PY_START | E.CALL | E.RAISE); r = (work(), raiser()); m.set_events(0, 0); return r
show("events with no callback", no_callback)
def mid_frame():
    log = []
    m.use_tool_id(0, "t"); m.register_callback(0, E.LINE, lambda c, l: log.append(rel(c, l)) if c.co_name == "mid_frame" else None)
    m.set_events(0, E.LINE)
    a = 1
    b = 2
    m.set_events(0, 0)
    c = 3
    return log
show("in the middle of a frame", mid_frame)
def returns_mid_frame():
    log = []
    def inner():
        m.set_events(0, E.PY_RETURN | E.LINE)
        return 5
    m.use_tool_id(0, "t"); m.register_callback(0, E.PY_RETURN, lambda c, o, v: log.append((c.co_name, v))); m.register_callback(0, E.LINE, lambda c, l: log.append((c.co_name, rel(c, l))))
    inner()
    x = 1
    m.set_events(0, 0)
    return log
show("frames that were running", returns_mid_frame)
def branch_both():
    log = []
    def f(a):
        if a: return 1
        return 2
    def cb(code, src, dst): log.append("branch"); return m.DISABLE
    m.use_tool_id(0, "t"); m.register_callback(0, E.BRANCH, cb); m.set_local_events(0, f.__code__, E.BRANCH)
    f(1); f(0); f(1); f(0); return (log, m.get_local_events(0, f.__code__) == E.BRANCH_LEFT | E.BRANCH_RIGHT)
show("BRANCH, and disabling it", branch_both)
def branches_agree():
    def f(a):
        if a:
            x = 1
        for i in range(2):
            pass
        while a:
            a -= 1
        return a and 1
    seen = set()
    m.use_tool_id(0, "t")
    m.register_callback(0, E.BRANCH_LEFT, lambda c, s, d: seen.add((s, "L", d))); m.register_callback(0, E.BRANCH_RIGHT, lambda c, s, d: seen.add((s, "R", d)))
    m.set_local_events(0, f.__code__, E.BRANCH_LEFT | E.BRANCH_RIGHT); f(0); f(2)
    table = {s: (l, r) for s, l, r in f.__code__.co_branches()}
    return (len(table), all(s in table and table[s][0 if k == "L" else 1] == d for s, k, d in seen), len(seen) == 2 * len(table))
show("co_branches() agrees with what is told", branches_agree)
def jump_events():
    log = []
    def f():
        for i in range(2):
            pass
        return 1
    m.use_tool_id(0, "t"); m.register_callback(0, E.JUMP, lambda c, s, d: log.append(d < s)); m.set_local_events(0, f.__code__, E.JUMP); f()
    return log
show("JUMP back", jump_events)
def instruction_events():
    n = []
    m.use_tool_id(0, "t"); m.register_callback(0, E.INSTRUCTION, lambda c, o: n.append(o)); m.set_local_events(0, work.__code__, E.INSTRUCTION); work()
    return (len(n) > 2, n == sorted(n))
show("INSTRUCTION", instruction_events)
# ---- sys.settrace()
def traced(tracer, f, *a):
    sys.settrace(tracer)
    try: return f(*a)
    finally: sys.settrace(None)
def only_calls():
    log = []
    def t(frame, event, arg): log.append((frame.f_code.co_name, event)); return None
    traced(t, work); return [e for e in log if e[0] == "work"]
show("a tracer that returns None", only_calls)
def changes_tracer():
    log = []
    def second(frame, event, arg): log.append(("second", event, rel(frame.f_code, frame.f_lineno))); return second
    def first(frame, event, arg):
        if frame.f_code is not work.__code__: return None
        log.append(("first", event, rel(frame.f_code, frame.f_lineno)))
        return second if event == "line" else first
    traced(first, work); return log
show("a tracer that returns another", changes_tracer)
def stops_lines():
    log = []
    def t(frame, event, arg):
        if frame.f_code is not work.__code__: return None
        log.append((event, rel(frame.f_code, frame.f_lineno)))
        if event == "line": frame.f_trace_lines = False
        return t
    traced(t, work); return log
show("f_trace_lines", stops_lines)
def tracer_raises():
    def t(frame, event, arg):
        if frame.f_code is work.__code__ and event == "line": raise RuntimeError("tracer")
        return t
    r = attempt(lambda: traced(t, work)); return (r, sys.gettrace())
show("a tracer that raises", tracer_raises)
def tracer_raises_on_call():
    def t(frame, event, arg):
        if frame.f_code is work.__code__: raise RuntimeError("on call")
        return t
    sys.settrace(t); r = attempt(work); return (r, sys.gettrace())
show("a tracer that raises at the call", tracer_raises_on_call)
def like_pdb():
    log = []
    def t(frame, event, arg): log.append((frame.f_code.co_name, event, rel(frame.f_code, frame.f_lineno))); return t
    def set_trace():
        frame = sys._getframe().f_back
        while frame:
            frame.f_trace = t; frame = frame.f_back
        sys.settrace(t)
    def inner():
        x = 1
        set_trace()
        y = 2
        return y
    def outer():
        inner()
        z = 3
        return z
    outer(); sys.settrace(None)
    f = sys._getframe()
    while f: f.f_trace = None; f = f.f_back
    return [e for e in log if e[0] in ("inner", "outer")]
show("as pdb.set_trace() does", like_pdb)
def running_frame_is_not_traced():
    log = []
    def t(frame, event, arg): log.append((frame.f_code.co_name, event)); return t
    def f():
        sys.settrace(t)
        x = 1
        work()
        sys.settrace(None)
    f(); return [e for e in log if e[0] in ("f", "work")]
show("a frame that was running is not traced", running_frame_is_not_traced)
def f_trace_attribute():
    f = sys._getframe(); t = lambda *a: None
    return (f.f_trace, f.f_trace_lines, f.f_trace_opcodes, setattr(f, "f_trace", t), f.f_trace is t, delattr(f, "f_trace"), f.f_trace, attempt(lambda: setattr(f, "f_trace_lines", 1)), attempt(lambda: setattr(f, "f_trace_opcodes", "a")), attempt(lambda: delattr(f, "f_trace_lines")))
show("f_trace", f_trace_attribute)
def opcodes():
    log = []
    def t(frame, event, arg):
        if frame.f_code is not work.__code__: return None
        frame.f_trace_opcodes = True; log.append(event); return t
    traced(t, work); return (log[0], log[-1], log.count("opcode") > 2, log.count("line"))
show("f_trace_opcodes", opcodes)
def both():
    log = []
    def t(frame, event, arg):
        if frame.f_code is work.__code__: log.append(("trace", event))
        return t
    def p(frame, event, arg):
        if frame.f_code is work.__code__: log.append(("profile", event))
    sys.setprofile(p); traced(t, work); sys.setprofile(None); return log
show("trace and profile", both)
def with_a_tool():
    log = []
    def t(frame, event, arg):
        if frame.f_code is work.__code__: log.append(("trace", event))
        return t
    m.use_tool_id(5, "t"); m.register_callback(5, E.LINE, lambda c, l: log.append(("tool", rel(c, l))) if c is work.__code__ else None); m.set_events(5, E.LINE)
    traced(t, work); m.set_events(5, 0); return log
show("trace and a tool", with_a_tool)
def profiler_raises():
    def p(frame, event, arg):
        if frame.f_code is work.__code__: raise RuntimeError("profiler")
    sys.setprofile(p); r = attempt(work); return (r, sys.getprofile())
show("a profiler that raises", profiler_raises)
def not_traced_in_tracer():
    log = []
    def helper(): return 1
    def t(frame, event, arg): log.append(frame.f_code.co_name); helper(); return t
    traced(t, work); return "helper" in log
show("a tracer is not traced", not_traced_in_tracer)
def call_tracing():
    log = []
    def inner(): return 1
    def t(frame, event, arg):
        log.append((frame.f_code.co_name, event))
        if frame.f_code is work.__code__ and event == "call": sys.call_tracing(inner, ())
        return t
    traced(t, work); return [e for e in log if e[0] == "inner"]
show("sys.call_tracing()", call_tracing)
def locals_at_call():
    log = []
    def f(a, b=2, *c, d=4, **e):
        def g(): return a
        return g
    def t(frame, event, arg):
        if frame.f_code is f.__code__ and event == "call": log.append(sorted(frame.f_locals.items()))
        return None
    traced(t, f, 1, 5, 6, x=7); return log
show("the arguments are there at the call", locals_at_call)
def return_values():
    log = []
    def f(): return [1]
    def g():
        yield "y"
    def t(frame, event, arg):
        if event == "return" and frame.f_code.co_name in ("f", "g"): log.append((frame.f_code.co_name, arg))
        return t
    traced(t, lambda: (f(), list(g()))); return log
show("what is returned", return_values)
def exception_argument():
    log = []
    def t(frame, event, arg):
        if event == "exception": log.append((frame.f_code.co_name, arg[0].__name__, str(arg[1]), type(arg[2]).__name__, arg[2].tb_frame is frame))
        return t
    traced(t, unwinder); return log
show("what an exception event is given", exception_argument)
def depth_is_kept():
    def t(frame, event, arg): return t
    def deep(n): return n and deep(n - 1)
    old = sys.getrecursionlimit()
    a = attempt(lambda: traced(t, deep, 2000))
    return (a, sys.getrecursionlimit() == old, deep(500))
show("the recursion limit while tracing", depth_is_kept)
