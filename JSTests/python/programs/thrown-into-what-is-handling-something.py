# What is thrown into a generator or a coroutine is raised while it is handling whatever it was handling when it yielded, and has that for its __context__: _PyErr_ChainStackItem()
def show(label, e): print(label, repr(e), "context:", repr(e.__context__), "cause:", repr(e.__cause__))
def g():
    try:
        raise KeyError(3)
    except Exception:
        try:
            yield 1
        except Exception as e:
            show("thrown while handling", e)
    try:
        yield 2
    except Exception as e:
        show("thrown while handling nothing", e)
    yield 3
it = g(); next(it)
it.throw(ValueError("a"))
it.throw(ValueError("b"))
# One that has a context already keeps it.
it = g(); next(it)
already = ValueError("c"); already.__context__ = OSError("mine")
it.throw(already)
# From where something else is being handled.
it = g(); next(it)
try:
    raise IndexError("outside")
except IndexError:
    it.throw(ValueError("d"))
# The one that is being handled, thrown back in.
def h():
    try:
        raise KeyError(4)
    except Exception as k:
        try:
            yield k
        except Exception as e:
            show("itself", e)
    yield
it = h(); k = next(it); it.throw(k)
# By class.
it = g(); next(it); it.throw(ValueError)
async def c():
    try:
        raise KeyError(5)
    except Exception:
        try:
            await A()
        except Exception as e:
            show("coroutine", e)
class A:
    def __await__(self): yield
co = c(); co.send(None)
try: co.throw(ValueError("e"))
except StopIteration: pass
