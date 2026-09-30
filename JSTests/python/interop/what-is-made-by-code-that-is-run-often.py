# A function of Python's inherits from the class `function`, and a generator from the class `generator`, which is how JavaScript finds what they have. That is so however often what makes them has been run, and so
# whichever compiler has had it.
import js

prototype_of = js.Object.getPrototypeOf


def make_function():
    def inner(a, *, b=1): return a
    return inner


def make_lambda():
    return lambda: None


def make_method():
    class C:
        def method(self, *, k=None): pass
    return C.method


def make_generator():
    def g():
        yield 1
    return g()


def make_expression():
    return (x for x in ())


async def coroutine():
    pass


def make_coroutine():
    c = coroutine()
    c.close()
    return c


for make in (make_function, make_lambda, make_method, make_generator, make_expression, make_coroutine):
    first = prototype_of(make())
    kinds = {id(prototype_of(make())) for i in range(4000)}
    print(make.__name__, "=>", first, first is type(make()), len(kinds), kinds == {id(first)})

# What that is for.
has = js.eval("(x, name) => typeof x[name]")
for i in range(3):
    print("to JavaScript", has(make_function(), "__name__"), has(make_function(), "call"), has(make_generator(), "send"), has(make_generator(), "next"), has(make_generator(), "return"))
