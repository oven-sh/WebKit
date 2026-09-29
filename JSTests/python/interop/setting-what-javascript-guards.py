# Setting and deleting an attribute of an object of JavaScript's, each in the three ways that there are of saying it.
import js


def attempt(f):
    try:
        f()
        return "done"
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)


def by_statement(o, v):
    def f(): o.a = v
    return attempt(f)


def by_function(o, v):
    return attempt(lambda: setattr(o, "a", v))


def by_method(o, v):
    return attempt(lambda: object.__setattr__(o, "a", v))


def delete_by_statement(o):
    def f(): del o.a
    return attempt(f)


def delete_by_function(o):
    return attempt(lambda: delattr(o, "a"))


def delete_by_method(o):
    return attempt(lambda: object.__delattr__(o, "a"))


setters = [by_statement, by_function, by_method]
deleters = [delete_by_statement, delete_by_function, delete_by_method]


def said(f):
    try:
        return repr(f())
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)


def its_dict(o):
    "It is how the object is now, and there is no changing the object by it"
    def put(): o.__dict__["b"] = 2
    def drop(): del o.__dict__["a"]
    def replace(): o.__dict__ = {}
    def remove(): del o.__dict__
    return [said(lambda: o.__dict__), said(lambda: vars(o) == o.__dict__), said(lambda: vars(o) is vars(o)), attempt(put), attempt(drop), attempt(lambda: o.__dict__.clear()), attempt(replace), attempt(remove),
            said(lambda: sorted(vars(o).items()))]


def make_function():
    def f(): pass
    return f


def seen(o):
    "What Python sees of `a`, each way that there is of looking"
    return [said(lambda: o.a), said(lambda: getattr(o, "a", "nothing")), said(lambda: hasattr(o, "a")), said(lambda: "a" in vars(o)), said(lambda: vars(o).get("a", "nothing")), said(lambda: dict(vars(o))),
            said(lambda: (len(vars(o)), len(list(vars(o))), len(list(vars(o).items())))), said(lambda: "a" in dir(o))]


def by_dict(o, v):
    def f(): o.__dict__["a"] = v
    return attempt(f)


def delete_by_dict(o):
    def f(): del o.__dict__["a"]
    return attempt(f)


def others_by_dict(o):
    return [attempt(lambda: o.__dict__.update(a=3)), attempt(lambda: o.__dict__.setdefault("a", 4)), attempt(lambda: o.__dict__.pop("a")), attempt(lambda: o.__dict__.popitem()), attempt(lambda: o.__dict__.clear())]


def frozen_dataclass():
    "What Python itself says of the like"
    import dataclasses

    @dataclasses.dataclass(frozen=True)
    class D:
        a: int = 1
    return [by_statement(D(), 2), delete_by_statement(D()), issubclass(dataclasses.FrozenInstanceError, AttributeError)]


class Derived(js.Map):
    "One of Python's that is derived from one of JavaScript's keeps its own attributes as any of Python's does"


def derived():
    d = Derived()
    return [by_statement(d, 1), d.a, repr(vars(d)), delete_by_statement(d), repr(vars(d)), delete_by_statement(d)]


def the_global_object():
    return [attempt(lambda: setattr(js, "setFromPython", 5)), js.setFromPython, js.eval("setFromPython"), attempt(lambda: delattr(js, "setFromPython")), hasattr(js, "setFromPython"),
            attempt(lambda: setattr(js, "undefined", 1)), attempt(lambda: delattr(js, "undefined")), attempt(lambda: delattr(js, "nothingOfTheKind"))]
