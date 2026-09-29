import inspect
import sys
import types
import weakref


def said(f):
    try:
        return repr(f())
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)


def run(source, **names):
    "What it defines, less what it was given"
    given = dict(names)
    exec(source, names)
    return sorted((k, repr(v)) for k, v in names.items() if k != "__builtins__" and k not in given)


def what_it_is(m):
    return [said(lambda: type(m)), said(lambda: type(m) is types.ModuleType), said(lambda: isinstance(m, types.ModuleType)), said(lambda: inspect.ismodule(m)), said(lambda: m.__class__), said(lambda: repr(m)), said(lambda: str(m)),
            said(lambda: callable(m)), said(lambda: bool(m)), said(lambda: m == m), said(lambda: hash(m) == hash(m)), said(lambda: weakref.ref(m)() is m), said(lambda: {m: 1}[m])]


def what_it_has(m):
    return [said(lambda: dir(m)), said(lambda: m.count), said(lambda: (m.bump(), m.count, m.bump(), m.count)), said(lambda: m.default("you")), said(lambda: (m.Thing, m.Thing(5).x, type(m.Thing(5)) is m.Thing)), said(lambda: m._private),
            said(lambda: m.whatThisIs()), said(lambda: (lambda f: f())(m.whatThisIs)), said(lambda: m.again is m), said(lambda: getattr(m, "count")), said(lambda: hasattr(m, "count")), said(lambda: types.ModuleType.__getattribute__(m, "count")),
            said(lambda: object.__getattribute__(m, "count")), said(lambda: [name for name, _ in inspect.getmembers(m)])]


def what_it_has_not(m):
    return [said(lambda: m.nothing), said(lambda: hasattr(m, "nothing")), said(lambda: getattr(m, "nothing", "instead")), said(lambda: m.__name__), said(lambda: m.__file__), said(lambda: m.__spec__), said(lambda: m.__doc__), said(lambda: m.toString),
            said(lambda: m.constructor), said(lambda: len(m)), said(lambda: m["count"]), said(lambda: "count" in m), said(lambda: list(m)), said(lambda: m())]


def set_and_delete(m):
    def put(name, value):
        def f(): setattr(m, name, value)
        return f
    def statement(): m.count = 5
    def delete_statement(): del m.count
    class Derived(types.ModuleType): pass
    def become(): m.__class__ = Derived
    return [said(statement), said(put("count", 5)), said(lambda: types.ModuleType.__setattr__(m, "count", 5)), said(lambda: object.__setattr__(m, "count", 5)), said(delete_statement), said(lambda: delattr(m, "count")), said(lambda: m.count),
            said(put("extra", [1])), said(lambda: m.extra), said(lambda: "extra" in dir(m)), said(lambda: vars(m)), said(lambda: m.__dict__ is vars(m)), said(put("__name__", "named")), said(lambda: repr(m)), said(lambda: m.nothing),
            said(lambda: delattr(m, "extra")), said(lambda: m.extra), said(lambda: delattr(m, "extra")), said(lambda: vars(m).update(by_dict=1)), said(lambda: m.by_dict), said(lambda: vars(m).update(count="in the way")), said(lambda: m.count),
            said(lambda: vars(m).pop("count")), said(become), said(lambda: setattr(m, "__getattr__", lambda name: "made up " + name)), said(lambda: m.anything), said(lambda: delattr(m, "__getattr__")),
            said(lambda: setattr(m, "__dir__", lambda: ["as", "it", "says"])), said(lambda: dir(m)), said(lambda: delattr(m, "__dir__")), said(lambda: sorted(vars(m)))]


def imported(m):
    sys.modules["put_there"] = m
    try:
        return [said(lambda: run("import put_there")), said(lambda: run("import put_there\nsame = put_there is m", m=m)), said(lambda: run("from put_there import count, Thing as T, default")), said(lambda: run("from put_there import *")),
                said(lambda: run("from put_there import nothing")), said(lambda: setattr(m, "__all__", ["bump", "default", "_private"])), said(lambda: run("from put_there import *")), said(lambda: delattr(m, "__all__")),
                said(lambda: run("import put_there.below"))]
    finally:
        del sys.modules["put_there"]


def give_back(m):
    return m


def takes_keywords(*args, **kwargs):
    return args, kwargs


def called(function, a_class, not_callable, no_default, of_python):
    "Calling a module calls what it exports by default, Python having no other way to say that that is what it wants of it"
    import functools
    import operator
    def often(m, *args):
        results = set()
        for _ in range(300):
            results.add(repr(m(*args)))
        return sorted(results)
    return [said(lambda: function("you")), said(lambda: function()), said(lambda: function(*["a", "b"])), said(lambda: often(function, "again")), said(lambda: callable(function)),
            said(lambda: (type(a_class(1, 2)) is a_class.default, a_class(1, 2).given, a_class().given)), said(lambda: callable(a_class)), said(lambda: len({type(a_class(i)) for i in range(300)})),
            said(lambda: not_callable()), said(lambda: callable(not_callable)), said(lambda: no_default()), said(lambda: no_default(1, x=2)), said(lambda: callable(no_default)),
            said(lambda: of_python(1, 2, x=3)), said(lambda: of_python(*[1], **{"y": 2})), said(lambda: of_python()), said(lambda: function("you", loudly=True)),
            said(lambda: list(map(function, ["a", "b"]))), said(lambda: functools.partial(function, "partly")()), said(lambda: operator.call(function, "by operator.call")), said(lambda: sorted(["b", "a"], key=function)),
            said(lambda: function.__call__), said(lambda: type(function).__call__)]


def in_the_middle(m):
    sys.modules["half_done"] = m
    try:
        return [said(lambda: dir(m)), said(lambda: m.before), said(lambda: m.after), said(lambda: hasattr(m, "after")), said(lambda: getattr(m, "after", "not yet")), said(lambda: m.later), said(lambda: m.hoisted()), said(lambda: m.seen),
                said(lambda: run("from half_done import before")), said(lambda: run("from half_done import after")), said(lambda: run("from half_done import *")), said(lambda: setattr(m, "after", 3))]
    finally:
        del sys.modules["half_done"]


def afterwards(m):
    return [said(lambda: (m.before, m.after, m.later)), said(lambda: hasattr(m, "after"))]
