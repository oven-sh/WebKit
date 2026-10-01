# What locals(), vars(), dir(), eval(), exec() and f_locals make of the variable of a comprehension, from inside it.
import sys, textwrap

def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
    print(label, "->", r)

bodies = {
    "locals": '[sorted(k for k in locals() if len(k) < 3) for x in l]',
    "locals x": '[locals()["x"] for x in l]',
    "vars": '[vars()["x"] for x in l]',
    "dir": '[[k for k in dir() if len(k) < 3] for x in l]',
    "eval": '[eval("x") for x in l]',
    "eval y": '[eval("y") for x in l]',
    "eval both": '[eval("x + y") for x in l]',
    "exec": '[exec("y = x") for x in l] and y',
    "exec x": '[(exec("x = 9"), x)[1] for x in l]',
    "f_locals": '[sorted(k for k in frame().f_locals if len(k) < 3) for x in l]',
    "f_locals x": '[frame().f_locals["x"] for x in l]',
    "f_locals type": '[type(frame().f_locals).__name__ for x in l]',
    "locals type": '[type(locals()).__name__ for x in l]',
    "locals is": '[locals() is locals() for x in l]',
    "afterwards": '"x" in [frame().f_locals for x in l][0]',
    "afterwards locals": '"x" in [locals() for x in l][0]',
    "set through f_locals": '[(frame().f_locals.__setitem__("x", 7), x)[1] for x in l]',
    "set y through f_locals": '[(frame().f_locals.__setitem__("y", 7), y)[1] for x in l]',
    "set new through f_locals": '[(frame().f_locals.__setitem__("z", 7), "z" in frame().f_locals)[1] for x in l]',
    "nested": '[[sorted(k for k in locals() if len(k) < 3) for w in l] for x in l]',
    "two": '[sorted(k for k in locals() if len(k) < 3) for x in l for w in l]',
    "shadow": '[locals()["y"] for y in l]',
    "shadow after": '([y for y in l], y, locals()["y"])',
    "set": '{tuple(sorted(k for k in locals() if len(k) < 3)) for x in l}',
    "dict": '{x: sorted(k for k in locals() if len(k) < 3) for x in l}',
    "genexp": 'list(sorted(k for k in locals() if len(k) < 3) for x in l)',
    "lambda inside": '[(lambda: sorted(k for k in locals() if len(k) < 3))() for x in l]',
    "lambda closes": '[(lambda: x)() for x in l]',
    "closes and locals": '[((lambda: x)(), sorted(k for k in locals() if len(k) < 3)) for x in l]',
    "walrus": '([sorted(k for k in locals() if len(k) < 3) for x in l if (q := x)], q)',
    "before the first": '[x for x in (sorted(k for k in locals() if len(k) < 3),)]',
    "condition": '[x for x in l if "x" in locals()]',
    "globals": '["x" in globals() for x in l]',
}

for label, body in bodies.items():
    print("=====", label)
    ns = lambda: {"frame": sys._getframe, "l": [1, 2], "y": 0}
    def module():
        g = ns(); exec("r = " + body, g); return g["r"]
    def module_locals():
        g = ns(); d = {}; exec("r = " + body, g, d); return d["r"]
    def in_class():
        g = ns(); exec("class C:\n    l = [1, 2]\n    y = 0\n    r = " + body, g); return g["C"].r
    def function():
        g = {"frame": sys._getframe}; exec("def f():\n    l = [1, 2]\n    y = 0\n    return " + body + "\nr = f()", g); return g["r"]
    def function_globals():
        g = ns(); exec("def f():\n    return " + body + "\nr = f()", g); return g["r"]
    def in_lambda():
        g = ns(); exec("r = (lambda: " + body + ")()", g); return g["r"]
    def evaluated():
        return eval(body, ns())
    def generator():
        g = {"frame": sys._getframe}; exec("def f():\n    l = [1, 2]\n    y = 0\n    yield " + body + "\nr = next(f())", g); return g["r"]
    for f in (module, module_locals, in_class, function, function_globals, in_lambda, evaluated, generator):
        attempt("  " + f.__name__, f)

print("===== when something is raised in one")
def short(d): return sorted(k for k in d if len(k) < 3)
def caught():
    y = 0
    try:
        [1 / x for x in (1, 0)]
    except ZeroDivisionError as e:
        return short(locals()), short(e.__traceback__.tb_frame.f_locals), y
attempt("caught in the function", caught)
def shadowed():
    x = "outer"
    try:
        [1 / x for x in (1, 0)]
    except ZeroDivisionError:
        return x, locals()["x"]
attempt("what it hid is back", shadowed)
def escapes():
    y = 0
    return [1 / x for x in (1, 0)]
def post_mortem():
    try:
        escapes()
    except ZeroDivisionError as e:
        return short(e.__traceback__.tb_next.tb_frame.f_locals)
attempt("afterwards, from the traceback", post_mortem)
def module_post_mortem():
    try:
        exec("y = 0\nr = [1 / x for x in (1, 0)]", {})
    except ZeroDivisionError as e:
        f = e.__traceback__.tb_next.tb_frame
        return short(f.f_locals), type(f.f_locals).__name__
attempt("of a module", module_post_mortem)
def seen_by_callee():
    def look(): return short(sys._getframe(1).f_locals)
    return [look() for x in (1,)], look()
attempt("from what it calls", seen_by_callee)
def deeper():
    def look(): return short(sys._getframe(2).f_locals)
    def via(): return look()
    return [[via() for w in (1,)] for x in (1,)], via()
attempt("from further down", deeper)

print("===== one that awaits")
class Wait:
    def __await__(self):
        yield "waiting"
        return 5
async def coro():
    y = 0
    r = [(await Wait()) + x for x in (1, 2)]
    return r, short(locals())
c = coro()
print(c.send(None), short(c.cr_frame.f_locals), c.cr_frame.f_locals.get("x"))
print(c.send(None), short(c.cr_frame.f_locals), c.cr_frame.f_locals.get("x"))
c.cr_frame.f_locals["x"] = 100
try:
    c.send(None)
except StopIteration as e:
    print(e.value)
async def agen():
    for i in (1, 2):
        yield i
async def coro2():
    return [(x, short(locals())) async for x in agen()]
c = coro2()
try:
    c.send(None)
except StopIteration as e:
    print(e.value)

print("===== what is told of each line")
seen = []
def tracer(frame, event, arg):
    if frame.f_code.co_name in ("traced", "<module>") and frame.f_code.co_filename in (__file__, "<traced>"):
        seen.append((event, frame.f_lineno - frame.f_code.co_firstlineno, short(frame.f_locals)))
    return tracer
def traced():
    a = 1
    r = [x + a
         for x in (1, 2)
         if x]
    return r
sys.settrace(tracer); traced(); sys.settrace(None)
for s in seen: print(" ", s)
seen.clear()
code = compile("a = 1\nr = [x + a\n     for x in (1, 2)\n     if x]\nb = 2\n", "<traced>", "exec")
sys.settrace(tracer); exec(code, {}); sys.settrace(None)
for s in seen: print(" ", s)

print("===== code that is run often")
def hot(n):
    t = 0
    for i in range(n):
        t += sum([locals()["x"] + x for x in (i, 1)])
    return t
print(hot(30000))
def hot2(n):
    return sum([eval("x") for x in range(n)])
print(hot2(20000))
def hot3(n):
    def poke(): sys._getframe(1).f_locals["x"] = 1
    return sum([(poke(), x)[1] for x in range(n)])
print(hot3(30000))
def hot4(n):
    x = "kept"
    for i in range(n):
        [x for x in (i,)]
    return x, locals()["x"]
print(hot4(30000))
def hot5(n):
    return [[(x, w) for w in (x,)][0] for x in range(n)][-1], short(locals())
print(hot5(30000))
