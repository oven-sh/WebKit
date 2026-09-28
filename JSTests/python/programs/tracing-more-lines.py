import sys
events = []
def short(a):
    return a if isinstance(a, (int, str, list, tuple, type(None))) else type(a).__name__
def tracer(frame, event, arg):
    if frame.f_code.co_filename != __file__: return None
    a = short(arg) if event == "return" else (arg[0].__name__ if event == "exception" else None)
    # What CPython does at the end of `except*` is on no line at all, and its f_lineno is None.
    if frame.f_code.co_name == "except_star" and event == "return": return tracer
    events.append((frame.f_code.co_name, event, frame.f_lineno - frame.f_code.co_firstlineno, a))
    return tracer
def trace(f, *a):
    events.clear(); sys.settrace(tracer)
    try:
        try: f(*a)
        except Exception as e: pass
    finally: sys.settrace(None)
    return [e for e in events if e[0] not in ("trace",)]
def show(f, *a):
    print(f.__name__)
    for e in trace(f, *a): print("   ", *e)
def return_in_try():
    try:
        return 1
    finally:
        x = 2
def return_in_with():
    with cm():
        return 1
def return_in_except():
    try:
        raise ValueError
    except ValueError as e:
        return 2
def return_in_nested():
    try:
        try:
            return 1
        finally:
            a = 1
    finally:
        b = 2
def break_in_try():
    for i in range(3):
        try:
            break
        finally:
            x = 1
    return x
def continue_in_try():
    for i in range(2):
        try:
            continue
        finally:
            x = 1
    return x
def raise_in_finally():
    try:
        try:
            raise ValueError("a")
        finally:
            x = 1
    except ValueError:
        y = 2
def with_raises():
    with cm():
        raise KeyError("k")
def with_suppresses():
    with cm(True):
        raise KeyError("k")
    return 1
def with_two():
    with cm() as a, cm() as b:
        x = 1
    return x
def with_multi():
    with (cm() as a,
          cm() as b):
        x = 1
class cm:
    def __init__(self, s=False): self.s = s
    def __enter__(self): return self
    def __exit__(self, *a): return self.s
def bare_raise():
    try:
        raise ValueError("v")
    except ValueError:
        raise
def raise_from():
    try:
        raise ValueError("v")
    except ValueError as e:
        raise KeyError("k") from e
def except_star():
    try:
        raise ExceptionGroup("g", [ValueError(1), KeyError(2)])
    except* ValueError:
        a = 1
    except* KeyError:
        b = 2
def try_else():
    try:
        x = 1
    except ValueError:
        x = 2
    else:
        x = 3
    return x
def gen_throw():
    def g():
        try:
            yield 1
        except ValueError:
            yield 2
    it = g()
    next(it)
    it.throw(ValueError("t"))
    it.close()
def gen_close():
    def g():
        try:
            yield 1
        finally:
            x = 1
    it = g()
    next(it)
    it.close()
def gen_unstarted():
    def g():
        yield 1
    it = g()
    it.close()
    it2 = g()
    try: it2.throw(ValueError("u"))
    except ValueError: pass
def yield_from():
    def inner():
        yield 1
        return "r"
    def outer():
        x = yield from inner()
        return x
    return list(outer())
def genexpr():
    return list(x
                for x in range(2))
def coroutine():
    async def inner(): return 1
    async def outer():
        a = await inner()
        return a
    c = outer()
    try: c.send(None)
    except StopIteration as e: return e.value
class Aw:
    def __await__(self):
        yield "s"
        return "done"
def suspended():
    async def co():
        return await Aw()
    c = co()
    c.send(None)
    try: c.send(None)
    except StopIteration as e: return e.value
def async_gen():
    async def ag():
        yield 1
        yield 2
    async def use():
        return [x async for x in ag()]
    c = use()
    try: c.send(None)
    except StopIteration as e: return e.value
def async_with_for():
    class A:
        async def __aenter__(s): return s
        async def __aexit__(s, *a): return False
        def __aiter__(s): return s
        async def __anext__(s): raise StopAsyncIteration
    async def use():
        async with A() as a:
            async for x in a:
                pass
        return 1
    c = use()
    try: c.send(None)
    except StopIteration as e: return e.value
def recursion(n=2):
    if n:
        return recursion(n - 1)
    return 0
def default_args():
    def f(a=1,
          b=2):
        return a
    return f()
def call_multi():
    return max(
        1,
        min(2,
            3),
    )
def chained():
    a = 1
    return (a <
            2 <
            3)
def boolops():
    a = 0
    b = (a or
         a or
         1)
    c = (b and
         a and
         1)
    return b
def subscripts():
    d = {}
    d[
        1
    ] = 2
    return d[
        1]
def attributes():
    class O: pass
    o = O()
    o.a = (
        1)
    return (o
            .a)
def fstrings():
    a = 1
    return f"{a}" f"""{
        a}"""
def unpacking():
    a, b = (1,
            2)
    (c,
     d) = a, b
    return c
def aug():
    x = 1
    x += (
        1)
    l = [0]
    l[0] += 1
    return x
def star_calls():
    a = (1, 2)
    k = {}
    return max(*a,
               **k)
def walrus():
    if (n :=
            1):
        return n
def nested_loops():
    for i in range(2):
        for j in range(2):
            if j: break
        else:
            x = 1
    return i
def while_true():
    n = 0
    while True:
        n += 1
        if n == 2:
            break
    return n
def while_else_break():
    n = 0
    while n < 1:
        n += 1
    else:
        n = 5
    return n
def if_chain(v):
    if v == 1:
        return "a"
    elif v == 2:
        return "b"
    elif v == 3: return "c"
    else:
        return "d"
def if_no_else(v):
    if v:
        x = 1
    if not v: x = 2
    return x
def asserts():
    assert 1, "m"
    assert (1 and
            2)
    assert 0, "fails"
def dels():
    a = b = 1
    del (a,
         b)
def lambdas_multi():
    f = (lambda:
         1)
    return f()
def class_multi():
    class K(
            object):
        "doc"
        x = 1
        def m(self):
            return 1
        y = 2
    return K().m()
def type_stuff():
    type A = int
    def g[T](x: T) -> T: return x
    return g(1)
def imports():
    import sys
    from sys import (path,
                     argv)
def global_nonlocal():
    x = 1
    def f():
        nonlocal x
        global GG
        x = 2
        GG = 3
    f()
def try_in_loop():
    for i in range(2):
        try:
            if i: raise ValueError
        except ValueError:
            continue
        finally:
            z = 1
    return z
def empty_return():
    return
def implicit():
    x = 1
def only_doc():
    "doc"
def ellipsis_body(): ...
for f, a in [(return_in_try, ()), (return_in_with, ()), (return_in_except, ()), (return_in_nested, ()), (break_in_try, ()), (continue_in_try, ()), (raise_in_finally, ()), (with_raises, ()), (with_suppresses, ()), (with_two, ()), (with_multi, ()), (bare_raise, ()), (raise_from, ()), (except_star, ()), (try_else, ()),
        (gen_throw, ()), (gen_close, ()), (gen_unstarted, ()), (yield_from, ()), (genexpr, ()), (coroutine, ()), (suspended, ()), (async_gen, ()), (async_with_for, ()), (recursion, ()), (default_args, ()), (call_multi, ()), (chained, ()), (boolops, ()), (subscripts, ()), (attributes, ()), (fstrings, ()), (unpacking, ()), (aug, ()), (star_calls, ()),
        (walrus, ()), (nested_loops, ()), (while_true, ()), (while_else_break, ()), (if_chain, (1,)), (if_chain, (2,)), (if_chain, (3,)), (if_chain, (4,)), (if_no_else, (1,)), (if_no_else, (0,)), (asserts, ()), (dels, ()), (lambdas_multi, ()), (class_multi, ()), (type_stuff, ()), (imports, ()), (global_nonlocal, ()), (try_in_loop, ()), (empty_return, ()), (implicit, ()), (only_doc, ()), (ellipsis_body, ())]:
    show(f, *a)
