# The globals of a function, or its builtins, can be a dict of a class that a program has derived, and then what the class has to say about looking things up is heard.
import types


def show(e):
    return type(e).__name__ + ": " + str(e)


def attempt(f, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return show(e)


def t(label, f):
    r = attempt(f)
    print(label, "=>", r if isinstance(r, str) else ascii(r))


log = []


class Missing(dict):
    def __missing__(self, key):
        log.append(("missing", key))
        return "made up " + key


class Refuses(dict):
    def __missing__(self, key):
        log.append(("missing", key))
        raise KeyError(key)


class Fails(dict):
    def __missing__(self, key):
        raise ValueError("no " + key)


class Gets(dict):
    def __getitem__(self, key):
        log.append(("getitem", key))
        return "got " + key


class GetsSome(dict):
    def __getitem__(self, key):
        log.append(("getitem", key))
        return super().__getitem__(key)


class Plain(dict):
    pass


class Sets(dict):
    def __setitem__(self, key, value):
        log.append(("setitem", key))
        super().__setitem__(key, value)

    def __delitem__(self, key):
        log.append(("delitem", key))
        super().__delitem__(key)


def f():
    return (a, b, len)


def with_globals(function, globals, **k):
    return types.FunctionType(function.__code__, globals, **k)


def logged(thunk):
    log.clear()
    return (attempt(thunk), log[:])


for make in (Missing, Refuses, Fails, Gets, GetsSome, Plain, dict):
    t(make.__name__, lambda: logged(with_globals(f, make(a=1))))
    t(make.__name__ + " with everything there", lambda: logged(with_globals(f, make(a=1, b=2, len=3))))
    t(make.__name__ + " with nothing", lambda: logged(with_globals(f, make())))
t("over and over", lambda: [g() for g in [with_globals(f, Missing(a=1))] for i in range(300)][-1])
t("and it changes meanwhile", lambda: [(g(), d.__setitem__("b", 5), g(), d.__delitem__("a"), g(), d.clear(), g()) for d in [Missing(a=1)] for g in [with_globals(f, d)]])
t("what it says are its globals", lambda: [(g.__globals__ is d, type(g.__globals__).__name__, sorted(k for k in d if k != "__builtins__")) for d in [Missing(a=1)] for g in [with_globals(f, d)]])


def stores():
    global s
    s = 1
    r = s
    del s
    return r


t("what is stored and deleted does not ask", lambda: [logged(with_globals(stores, d)) + (sorted(k for k in d if k != "__builtins__"),) for d in [Sets()]])


def inner_uses():
    def inner():
        return (a, b)
    return inner()


t("what is defined inside", lambda: logged(with_globals(inner_uses, Missing(a=1))))
t("a comprehension, a lambda, a class", lambda: logged(with_globals(lambda: ([a for i in range(2)], (lambda: b)(), type("C", (), {"x": c}).x), Missing(a=1))))

print("---- one piece of code, and now one kind of globals and now another")
code = compile("def g(): return (a, b)\nr = g()", "<s>", "exec")
for i in range(3):
    for make in (dict, Missing, Plain, Gets, dict):
        d = make(a=1, b=2)
        print(make.__name__, logged(lambda: exec(code, d)), ascii(d.get("r") if make is not Gets else dict.get(d, "r")))
for i in range(3):
    for make in (dict, Missing, dict, Refuses):
        d = make(a=1)
        print(make.__name__, logged(lambda: exec(code, d)))
h = with_globals(f, {"a": 1, "b": 2})
t("hot with a dict, and then not", lambda: ([h() for i in range(300)][-1], with_globals(f, Missing(a=1))(), h(), with_globals(f, Gets(a=1, b=2))()))

print("---- under exec and eval")
for make in (Missing, Refuses, Fails, Gets, Plain):
    t("exec " + make.__name__, lambda: logged(lambda: exec("r = (a, b, len)", d) or dict.get(d, "r")) if (d := make(a=1)) is not None else None)
    t("eval " + make.__name__, lambda: logged(lambda: eval("(a, b, len)", make(a=1))))
    t("eval with other locals " + make.__name__, lambda: logged(lambda: eval("(a, b, len)", make(a=1), {})))
    t("eval with those for locals " + make.__name__, lambda: logged(lambda: eval("(a, b, len)", {"a": 0}, make(a=1))))
    t("a class body " + make.__name__, lambda: logged(lambda: eval("type('C', (), {})", make()) and exec("class C:\n    x = (a, b)\nr = C.x", d) or dict.get(d, "r")) if (d := make(a=1)) is not None else None)

print("---- builtins")
for make in (Missing, Refuses, Fails, Gets, Plain):
    t("builtins " + make.__name__, lambda: logged(with_globals(f, {"a": 1, "__builtins__": make(len=3)})))
    t("builtins and globals " + make.__name__, lambda: logged(with_globals(f, make(a=1, __builtins__=make(len=3)))))
    t("under eval " + make.__name__, lambda: logged(lambda: eval("(a, b, len)", {"a": 1, "__builtins__": make(len=3)})))

print("---- what it is for")
import annotationlib


class C:
    x: NotThere[int]
    y: int
    z: "a string"
    w: list[AlsoNot.attribute]


def function(a: Nowhere, b: int = 1) -> Nothing | None: pass


for format in (annotationlib.Format.FORWARDREF, annotationlib.Format.STRING, annotationlib.Format.VALUE):
    t("of a class, " + format.name, lambda: annotationlib.get_annotations(C, format=format))
    t("of a function, " + format.name, lambda: ascii(annotationlib.get_annotations(function, format=format)).replace(hex(id(function)), "ADDRESS"))
import dataclasses


@dataclasses.dataclass
class D:
    a: Undefined
    b: int = 1
    c: "ClassVar[int]" = 2


t("a dataclass", lambda: (D(1), [(x.name, x.type) for x in dataclasses.fields(D)], D.__init__.__annotations__ if False else None))
