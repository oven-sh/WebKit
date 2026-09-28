def tb(e):
    out = []
    t = e.__traceback__
    while t:
        out.append((t.tb_frame.f_code.co_name, t.tb_lineno))
        t = t.tb_next
    return out

def boom():
    raise KeyError("k")

def helper_reraise():
    raise

def via_helper():
    try:
        boom()
    except KeyError:
        helper_reraise()

def gen():
    try:
        yield 1
    except KeyError:
        raise
    yield 2

def throw_saved(saved):
    g = gen()
    next(g)
    g.throw(saved)

class CM:
    def __enter__(self): return self
    def __exit__(self, *a): return False

def with_block():
    with CM():
        boom()

def star():
    try:
        raise ExceptionGroup("g", [KeyError(1), ValueError(2)])
    except* ValueError:
        pass

def star_reraise():
    try:
        raise ExceptionGroup("g", [KeyError(1)])
    except* KeyError:
        raise

def rec(n):
    if n == 0:
        boom()
    try:
        rec(n - 1)
    finally:
        pass

def nested_finally():
    try:
        try:
            boom()
        finally:
            x = 1
    finally:
        y = 2

def unmatched():
    try:
        boom()
    except ValueError:
        pass
    except TypeError:
        pass

def raise_from_handler():
    try:
        boom()
    except KeyError as e:
        raise e

def twice():
    try:
        raise_from_handler()
    except KeyError as e:
        raise e

def with_tb_none():
    try:
        boom()
    except KeyError as e:
        raise e.with_traceback(None)

def loop_reraise():
    saved = None
    for i in range(3):
        try:
            if saved is None:
                boom()
            raise saved
        except KeyError as e:
            saved = e
    raise saved

def comp():
    return [boom() for _ in range(1)]

def lam():
    return (lambda: boom())()

def key_in_sorted():
    return sorted([1, 2], key=lambda v: boom())

def in_class_body():
    class C:
        boom()

saved = None
for f in (via_helper, with_block, star, star_reraise, lambda: rec(2), nested_finally, unmatched, raise_from_handler, twice,
          with_tb_none, loop_reraise, comp, lam, key_in_sorted, in_class_body):
    try:
        f()
    except BaseException as e:
        print(f.__name__, tb(e))
        if saved is None:
            saved = e
try:
    throw_saved(saved)
except KeyError as e:
    print("throw_saved", tb(e))

async def co():
    boom()
async def outer():
    await co()
try:
    outer().send(None)
except KeyError as e:
    print("await", tb(e))
def delegating():
    yield from gen2()
def gen2():
    yield 1
    boom()
try:
    list(delegating())
except KeyError as e:
    print("yield from", tb(e))
