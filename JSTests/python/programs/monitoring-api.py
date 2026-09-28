import sys
m = sys.monitoring; E = m.events
def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
def attempt(f):
    try: return f()
    except BaseException as e: return type(e).__name__ + ": " + str(e)
show("module", lambda: (type(m).__name__, m.__name__, sorted(n for n in dir(m) if not n.startswith("__"))))
show("events", lambda: sorted((v, n) for n, v in vars(E).items()))
show("ids", lambda: (m.DEBUGGER_ID, m.COVERAGE_ID, m.PROFILER_ID, m.OPTIMIZER_ID))
show("sentinels", lambda: (type(m.DISABLE).__name__, type(m.MISSING).__name__, m.DISABLE is m.MISSING, type(E).__name__))
# ---- tools
show("no tool", lambda: [m.get_tool(i) for i in range(6)])
show("use", lambda: (m.use_tool_id(1, "one"), m.get_tool(1), attempt(lambda: m.use_tool_id(1, "again")), m.free_tool_id(1), m.get_tool(1), m.use_tool_id(1, "two"), m.get_tool(1), m.free_tool_id(1)))
for t in (-1, 6, 7, 8, 100, "a", None, 1.5, 1 << 40):
    show("tool " + repr(t), lambda: (attempt(lambda: m.use_tool_id(t, "n")), attempt(lambda: m.get_tool(t)), attempt(lambda: m.free_tool_id(t)), attempt(lambda: m.clear_tool_id(t)), attempt(lambda: m.get_events(t)), attempt(lambda: m.set_events(t, 0)), attempt(lambda: m.register_callback(t, E.LINE, None))))
show("name", lambda: [attempt(lambda: m.use_tool_id(2, n)) for n in (1, None, b"n")])
show("free what is free", lambda: (m.free_tool_id(3), m.clear_tool_id(3)))
show("not in use", lambda: (attempt(lambda: m.set_events(3, E.LINE)), attempt(lambda: m.set_local_events(3, show.__code__, E.LINE)), m.get_events(3), m.get_local_events(3, show.__code__), m.register_callback(3, E.LINE, None)))
# ---- events
m.use_tool_id(0, "t")
def sets(v): m.set_events(0, v); r = m.get_events(0); m.set_events(0, 0); return r
show("set", lambda: [sets(v) for v in (0, E.LINE, E.PY_START | E.PY_RETURN, E.CALL, E.CALL | E.C_RAISE | E.C_RETURN, E.BRANCH, E.BRANCH_LEFT, E.BRANCH | E.LINE, E.RAISE | E.RERAISE | E.PY_UNWIND | E.PY_THROW | E.EXCEPTION_HANDLED | E.STOP_ITERATION, (1 << 19) - 1)])
for v in (E.C_RETURN, E.C_RAISE, E.C_RETURN | E.LINE, E.CALL | E.C_RETURN, E.CALL | E.C_RAISE, -1, 1 << 19, 1 << 25, "a", None, 1 << 40):
    show("set " + repr(v), lambda: sets(v))
show("_all_events", lambda: (m._all_events(), m.set_events(0, E.LINE | E.CALL), m._all_events(), m.set_events(0, 0), m._all_events()))
code = show.__code__
def sets_local(v): m.set_local_events(0, code, v); r = m.get_local_events(0, code); m.set_local_events(0, code, 0); return r
show("set local", lambda: [sets_local(v) for v in (0, E.LINE, E.PY_START | E.PY_RETURN, E.CALL | E.C_RETURN | E.C_RAISE, E.BRANCH, E.STOP_ITERATION, (1 << 11) - 1)])
for v in (E.RAISE, E.PY_UNWIND, E.C_RETURN, 1 << 11, 1 << 19, -1, "a"):
    show("set local " + repr(v), lambda: sets_local(v))
show("not code", lambda: [(attempt(lambda: m.set_local_events(0, c, 0)), attempt(lambda: m.get_local_events(0, c))) for c in (1, None, show)])
show("order of checks", lambda: (attempt(lambda: m.set_local_events(9, 1, 0)), attempt(lambda: m.get_local_events(9, 1)), attempt(lambda: m.set_local_events("a", 1, 0)), attempt(lambda: m.set_local_events(9, code, E.RAISE)), attempt(lambda: m.set_local_events(0, 1, "a"))))
# ---- callbacks
f1 = lambda *a: None; f2 = lambda *a: None
show("register", lambda: (m.register_callback(0, E.LINE, f1), m.register_callback(0, E.LINE, f2) is f1, m.register_callback(0, E.LINE, None) is f2, m.register_callback(0, E.LINE, None)))
for v in (0, E.LINE | E.CALL, 1 << 19, 1 << 30, -1, "a"):
    show("register " + repr(v), lambda: m.register_callback(0, v, f1))
show("register each", lambda: [(m.register_callback(0, 1 << i, f1), m.register_callback(0, 1 << i, None) is f1) for i in range(19)])
show("branch and its two", lambda: (m.register_callback(0, E.BRANCH, f1), m.register_callback(0, E.BRANCH_LEFT, None) is f1, m.register_callback(0, E.BRANCH_RIGHT, None) is f1, m.register_callback(0, E.BRANCH, None)))
show("not callable", lambda: (m.register_callback(0, E.LINE, 5), m.register_callback(0, E.LINE, None)))
show("clear", lambda: (m.register_callback(0, E.LINE, f1), m.set_events(0, E.LINE), m.set_local_events(0, code, E.CALL), m.clear_tool_id(0), m.get_tool(0), m.get_events(0), m.get_local_events(0, code), m.register_callback(0, E.LINE, None)))
show("free", lambda: (m.register_callback(0, E.LINE, f1), m.set_events(0, E.LINE), m.free_tool_id(0), m.get_tool(0), m.get_events(0), m.use_tool_id(0, "t"), m.register_callback(0, E.LINE, None), m.get_events(0)))
show("restart", lambda: m.restart_events())
show("wrong numbers", lambda: [attempt(f) for f in (lambda: m.use_tool_id(), lambda: m.use_tool_id(1), lambda: m.get_tool(), lambda: m.set_events(0), lambda: m.register_callback(0, E.LINE), lambda: m.restart_events(1), lambda: m.get_local_events(0), lambda: m.use_tool_id(tool_id=1, name="n"))])
events = []
sys.addaudithook(lambda e, a: events.append((e, len(a))) if e.startswith("sys.monitoring") or e in ("sys.settrace", "sys.setprofile") else None)
show("audited", lambda: (m.register_callback(0, E.LINE, f1), m.register_callback(0, E.LINE, None) is f1, sys.settrace(None), sys.setprofile(None), events))
m.free_tool_id(0)
# ---- settrace and setprofile
show("get", lambda: (sys.gettrace(), sys.getprofile()))
t = lambda *a: None
show("set and get", lambda: (sys.settrace(t), sys.gettrace() is t, sys.settrace(None), sys.gettrace(), sys.setprofile(t), sys.getprofile() is t, sys.setprofile(None), sys.getprofile()))
show("tools 6 and 7 are not seen", lambda: (sys.settrace(t), sys.setprofile(t), m._all_events(), [m.get_tool(i) for i in range(6)], sys.settrace(None), sys.setprofile(None), m._all_events()))
show("not callable", lambda: (sys.settrace(5), sys.gettrace(), sys.settrace(None)))
show("nothing", lambda: (attempt(sys.settrace), attempt(sys.setprofile), attempt(lambda: sys.gettrace(1))))
