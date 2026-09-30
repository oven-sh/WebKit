# A module can await, with nothing asked for. In CPython it is a SyntaxError unless compile() is given PyCF_ALLOW_TOP_LEVEL_AWAIT.
import ast
import inspect
import sys
import types

import js


def attempt(label, f):
    try:
        print("   ", label, "=>", f())
    except BaseException as e:
        print("   ", label, "=>", type(e).__name__, e)


print("---- the program")


class Ready:
    "It has nothing to wait for."
    def __await__(self):
        return iter(())


async def double(x):
    await Ready()
    return x * 2


value = await double(21)
print(value, await js.Promise.resolve(7), await double(await double(1)))


async def numbers():
    for i in range(3):
        await js.Promise.resolve(i)
        yield i


async for i in numbers():
    print("async for", i)


class Manager:
    async def __aenter__(self):
        print("enter")
        return 5

    async def __aexit__(self, *a):
        print("exit", a[0])
        return True


async with Manager() as m:
    print("inside", m)
    raise ValueError
print([x async for x in numbers()], {x: await double(x) async for x in numbers()}, [await double(x) for x in range(3)], {x async for x in numbers() if await double(x) > 1})
try:
    await js.Promise.reject(js.Error("rejected"))
except Exception as e:
    print("caught", type(e).__name__, e)
try:
    await double("a" if await double(0) else None)
except TypeError as e:
    print("caught", e)
finally:
    print("finally", await double(3))
if await double(1):
    while await double(value):
        value = 0
    else:
        print("else")
print(__name__, sorted(n for n in globals() if not n.startswith("__")), sys._getframe().f_code.co_name, bool(sys._getframe().f_code.co_flags & inspect.CO_COROUTINE))

print("---- what is imported")
import awaits_at_the_top
print(awaits_at_the_top.first, awaits_at_the_top.second, sys.modules["awaits_at_the_top"] is awaits_at_the_top)
import awaits_at_the_top as again
print(again is awaits_at_the_top)
del sys.modules["awaits_at_the_top"]
import imports_what_awaits
print(imports_what_awaits.both)


def imports():
    del sys.modules["awaits_at_the_top"]
    from awaits_at_the_top import second
    return second


attempt("in a function", imports)
attempt("it raises", lambda: __import__("awaits_and_raises"))
print("awaits_and_raises" in sys.modules)

print("---- compile(), exec() and eval()")
for source, mode in (("x = await f()", "exec"), ("x = 1", "exec"), ("await f()", "single"), ("await f()", "eval"), ("async for i in g(): pass", "exec"), ("async with m: pass", "exec"), ("[i async for i in g()]", "exec"), ("[i async for i in g()]", "eval"), ("[await f() for i in x]", "exec"),
                     ("def h():\n    await f()", "exec"), ("def h():\n    async with m: pass", "exec"), ("lambda: await f()", "exec"), ("class C:\n    await f()", "exec"), ("class C:\n    x = [i async for i in g()]", "exec"), ("yield", "exec"), ("return", "exec"), ("await", "exec"),
                     ("async def h():\n    await f()", "exec"), ("(await f() for i in x)", "exec"), ("await = 1", "exec")):
    for flags in (0, ast.PyCF_ALLOW_TOP_LEVEL_AWAIT):
        try:
            code = compile(source, "<test>", mode, flags)
            print("   ", repr(source), mode, flags, "coroutine" if code.co_flags & inspect.CO_COROUTINE else "not a coroutine", code.co_flags)
        except SyntaxError as e:
            print("   ", repr(source), mode, flags, "SyntaxError:", e.msg)
namespace = {"f": double, "js": js}
exec("x = await f(4)\ny = await js.Promise.resolve(x + 1)", namespace)
print(namespace["x"], namespace["y"])
local = {}
exec("z = await f(5)", namespace, local)
print(local, "z" in namespace)
code = compile("w = await f(6)", "<test>", "exec")
coroutine = eval(code, namespace)
print(type(coroutine).__name__, coroutine.__name__, coroutine.__qualname__, "w" in namespace, await coroutine, namespace["w"])
attempt("exec() of what raises", lambda: exec("await f(1)\n1 / 0", namespace))
attempt("of what is rejected", lambda: exec("await js.Promise.reject(js.Error('no'))", namespace))
attempt("of what awaits nothing that will come", lambda: exec("await js.Promise(lambda resolve, reject: None)", namespace))
attempt("exec() gives nothing", lambda: exec("await f(1)", namespace))
attempt("exec() in exec()", lambda: (exec("exec('v = await f(7)')\nu = await f(v)", namespace), namespace["u"])[1])
attempt("of a syntax tree", lambda: (exec(compile(ast.parse("t = await f(8)"), "<test>", "exec"), namespace), namespace["t"])[1])
attempt("single", lambda: exec(compile("await f(9)", "<test>", "single"), namespace))
function = types.FunctionType(compile("s = await f(10)", "<test>", "exec"), namespace)
made = function()
print(type(made).__name__, "s" in namespace, await made, namespace["s"])
attempt("a function made of what awaits nothing", lambda: (types.FunctionType(compile("r = 11", "<test>", "exec"), namespace)(), namespace["r"]))
attempt("of an expression", lambda: types.FunctionType(compile("r + 1", "<test>", "eval"), namespace)())
