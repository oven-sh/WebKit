# What comes of everything that the other built-in objects have: what goes through things, views, generators and coroutines, functions and what is made of them, descriptors, code, frames, tracebacks, cells, modules,
# classes, exceptions and the rest. methods.py is for numbers, text and what holds things. Most of these have a state, so each is tried as it is when it is new, part of the way through, at its end, and with what it goes
# through changed under it, and some as they are only while they are running. Each attribute is got, is set to each of a dozen things and is deleted. If it can be called, it is called with nothing, with each of some
# thirty things, with each two of ten, and by keyword, both by way of getattr() and as it would be written. What is looked at is what comes of that and its class, or the exception and what it says, and what has become of the object.
#
# There is a line for each attribute of each, with how many things were tried and a number that stands for all that came of them. To see them all:
#
#     other-objects.py [--] <which> <attribute>
#     other-objects.py [--] any <attribute>
#     other-objects.py [--] everything [<which>]

import _warnings
import sys

_warnings.filters.insert(0, ("ignore", None, Warning, None, 0))


class Index:
    def __repr__(self):
        return "Index()"

    def __index__(self):
        return 1


class Sequence:
    def __init__(self, n=3):
        self.n = n

    def __repr__(self):
        return "Sequence(%d)" % self.n

    def __len__(self):
        return self.n

    def __getitem__(self, i):
        if not 0 <= i < self.n:
            raise IndexError(i)
        return i * 10


class Suspend:
    """Awaiting one gives what it has to whoever is running the coroutine, and comes to what that sends back."""

    def __init__(self, value):
        self.value = value

    def __await__(self):
        return (yield self.value)


def generator():
    try:
        x = yield 1
        y = yield (2, x)
        return (3, y)
    finally:
        pass


def stubborn():
    while True:
        try:
            yield "again"
        except GeneratorExit:
            yield "no"
        except BaseException as e:
            yield type(e).__name__


def delegating():
    return (yield from generator())


async def coroutine():
    x = await Suspend(1)
    y = await Suspend((2, x))
    return (3, y)


async def async_generator():
    x = yield 1
    await Suspend("between")
    y = yield (2, x)


class World:
    pass


def world():
    """Functions and classes that nothing else has, since what is tried can change them."""
    def function(a, b=2, /, c=3, *d, e, f=6, **g) -> int:
        "What it is for."
        local = a
        return lambda: (local, b)


    function.attribute = 1


    def generic[T, *Ts, **P](x: T) -> T:
        return x


    type Alias[T] = list[T]


    class Plain:
        "A class."
        attribute = 1
        __slots__ = ("slot", "empty", "__dict__")

        def __init__(self):
            self.slot = 5

        def __repr__(self):
            return "Plain()"

        def method(self, x=1):
            return x

        @classmethod
        def of_class(cls, x=1):
            return cls.__name__, x

        @staticmethod
        def static(x=1):
            return x

        @property
        def managed(self):
            "Managed."
            return self.slot

        @managed.setter
        def managed(self, value):
            self.slot = value


    class Derived(Plain):
        __slots__ = ()

        def method(self, x=2):
            return super().method(x)


    class Meta(type):
        def __repr__(cls):
            return "<Meta %s>" % cls.__name__


    class OfMeta(metaclass=Meta):
        pass

    w = World()
    w.function, w.generic, w.Alias, w.Plain, w.Derived, w.Meta, w.OfMeta = function, generic, Alias, Plain, Derived, Meta, OfMeta
    return w


# These are what things are given and set to, and are not themselves tried.
function, Plain = world().function, world().Plain


def world_generator():
    def generator():
        yield 1
    return generator


KEPT = []


def kept(value):
    """CPython is done with a generator as soon as nothing has it, and closes it, which its frame shows. When that is done is not what is being asked."""
    del KEPT[:-3]
    KEPT.append(value)
    return value


def counter():
    state = [0]

    def count():
        state[0] += 1
        return state[0]
    return count


def raised(make):
    try:
        raise make()
    except BaseException as e:
        return e


def deep():
    def inner():
        raise ValueError("deep")
    try:
        inner()
    except ValueError as e:
        return e


# ---- What is tried. Each is a function that makes one and calls what it is given with it, since some are what they are only for as long as something is going on.

SUBJECTS = {}


def subject(name, make):
    SUBJECTS[name] = lambda action: action(make())


def step(it):
    try:
        return it.send(None) if hasattr(it, "send") else next(it)
    except BaseException:
        return None


def stages(name, make, most=8):
    """New, one on, and at its end."""
    def after(count):
        def made():
            it = make()
            for i in range(count):
                step(it)
            return it
        return made
    subject(name, make)
    subject(name + ", one on", after(1))
    subject(name + ", at its end", after(most))


for name, make in {
    "list_iterator": lambda: iter([1, 2, 3]), "list_reverseiterator": lambda: reversed([1, 2, 3]), "tuple_iterator": lambda: iter((1, 2, 3)), "str_ascii_iterator": lambda: iter("abc"), "str_iterator": lambda: iter("a中\U0001f600"),
    "bytes_iterator": lambda: iter(b"abc"), "bytearray_iterator": lambda: iter(bytearray(b"abc")), "range_iterator": lambda: iter(range(3)), "range_iterator by threes": lambda: iter(range(10, 0, -3)),
    "longrange_iterator": lambda: iter(range(2 ** 70, 2 ** 70 + 3)), "reversed range": lambda: reversed(range(3)), "reversed long range": lambda: reversed(range(2 ** 70, 2 ** 70 + 3)), "dict_keyiterator": lambda: iter({1: 2, 3: 4, 5: 6}),
    "dict_valueiterator": lambda: iter({1: 2, 3: 4, 5: 6}.values()), "dict_itemiterator": lambda: iter({1: 2, 3: 4, 5: 6}.items()), "dict_reversekeyiterator": lambda: reversed({1: 2, 3: 4, 5: 6}),
    "dict_reversevalueiterator": lambda: reversed({1: 2, 3: 4, 5: 6}.values()), "dict_reverseitemiterator": lambda: reversed({1: 2, 3: 4, 5: 6}.items()), "set_iterator": lambda: iter({1, 2, 3}), "frozenset iterator": lambda: iter(frozenset({1, 2, 3})),
    "memoryview iterator": lambda: iter(memoryview(b"abc")), "callable_iterator": lambda: iter(counter(), 4), "iterator": lambda: iter(Sequence()), "reversed": lambda: reversed(Sequence()), "enumerate": lambda: enumerate("abc"),
    "enumerate from far": lambda: enumerate("abc", 2 ** 70), "zip": lambda: zip("abc", [1, 2, 3]), "zip of nothing": lambda: zip(), "zip, strict": lambda: zip("abc", [1, 2], strict=True), "map": lambda: map(str, [1, 2, 3]),
    "map of two": lambda: map(pow, [1, 2, 3], [2, 2]), "map, strict": lambda: map(pow, [1, 2, 3], [2, 2], strict=True), "filter": lambda: filter(None, [0, 1, 2, 0, 3]), "filter by": lambda: filter(lambda x: x > 1, [0, 1, 2, 0, 3]),
    "generator": generator, "generator expression": lambda: (x * 2 for x in [1, 2, 3]), "generator that will not stop": stubborn, "generator that delegates": delegating, "coroutine": coroutine, "coroutine_wrapper": lambda: coroutine().__await__(),
    "async_generator_asend": lambda: async_generator().asend(None), "async_generator_athrow": lambda: async_generator().athrow(ValueError), "async_generator aclose": lambda: async_generator().aclose(),
    "anext_awaitable": lambda: anext(async_generator(), "default"),
}.items():
    stages(name, make)


def async_generator_after(count):
    def made():
        it = async_generator()
        for i in range(count):
            sent = it.asend(None)
            while True:
                try:
                    sent.send(None)
                except (StopIteration, StopAsyncIteration):
                    break
        return it
    return made


subject("async_generator", async_generator)
subject("async_generator, one on", async_generator_after(1))
subject("async_generator, at its end", async_generator_after(4))


def closed(make):
    def made():
        it = make()
        it.close()
        return it
    return made


def started_and_closed(make):
    def made():
        it = make()
        step(it)
        it.close()
        return it
    return made


def thrown_into(make):
    def made():
        it = make()
        step(it)
        try:
            it.throw(KeyError("thrown"))
        except KeyError:
            pass
        return it
    return made


for name, make in (("generator", generator), ("coroutine", coroutine), ("generator that delegates", delegating)):
    subject(name + ", closed", closed(make))
    subject(name + ", begun and closed", started_and_closed(make))
    subject(name + ", thrown into", thrown_into(make))


def changed(make_container, get_iterator, change, first=1):
    def made():
        container = make_container()
        it = get_iterator(container)
        for i in range(first):
            next(it)
        change(container)
        return it
    return made


for name, make_container, changes in (
    ("list", lambda: [1, 2, 3], {"added to": lambda l: l.append(4), "emptied": lambda l: l.clear(), "cut short": lambda l: l.pop(), "put in front of": lambda l: l.insert(0, 0)}),
    ("bytearray", lambda: bytearray(b"abc"), {"added to": lambda l: l.append(100), "emptied": lambda l: l.clear()}),
    ("dict", lambda: {1: 2, 3: 4, 5: 6}, {"added to": lambda d: d.__setitem__(7, 8), "emptied": lambda d: d.clear(), "with another value": lambda d: d.__setitem__(3, 0), "with one for another": lambda d: (d.pop(5), d.__setitem__(7, 8))}),
    ("set", lambda: {1, 2, 3}, {"added to": lambda s: s.add(4), "emptied": lambda s: s.clear()}),
):
    for how, change in changes.items():
        subject("iterator of a %s that is %s" % (name, how), changed(make_container, iter, change))
        if name in ("list", "dict"):
            subject("reverse iterator of a %s that is %s" % (name, how), changed(make_container, reversed, change))
        if name == "dict":
            subject("iterator of the items of a dict that is " + how, changed(make_container, lambda d: iter(d.items()), change))
            subject("iterator of the values of a dict that is " + how, changed(make_container, lambda d: iter(d.values()), change))
        # And after it has come to its end.
        subject("finished iterator of a %s that is %s" % (name, how), changed(make_container, lambda c: (lambda it: (list(it), it)[1])(iter(c)), change, 0))


def while_running(make_body):
    """What a generator or a coroutine is to itself."""
    def use(action):
        box = []
        me = []
        me.append(make_body(lambda: box.append(action(me[0]))))
        step(me[0])
        return box[0]
    return use


def running_generator(act):
    act()
    yield 1


async def running_coroutine(act):
    act()
    await Suspend(1)


SUBJECTS["generator, running"] = while_running(running_generator)
SUBJECTS["coroutine, running"] = while_running(running_coroutine)


def running_async_generator(action):
    box = []
    me = []

    async def body():
        box.append(action(me[0]))
        yield 1
    me.append(body())
    step(me[0].asend(None))
    return box[0]


SUBJECTS["async_generator, running"] = running_async_generator

for name, make in {
    "dict_keys": lambda: {1: 2, "a": 3}.keys(), "dict_values": lambda: {1: 2, "a": 3}.values(), "dict_items": lambda: {1: 2, "a": 3}.items(), "dict_keys of nothing": lambda: {}.keys(), "dict_values of nothing": lambda: {}.values(),
    "dict_items of nothing": lambda: {}.items(), "dict_items with a list": lambda: {1: [2]}.items(), "mappingproxy": lambda: type(int.__dict__)({1: 2, "a": 3}), "mappingproxy of a class": lambda: type("C", (), {"a": 1}).__dict__,
    "function": lambda: world().function, "function made here": lambda: world().function(1, e=5), "lambda": lambda: (lambda x, y=2: x), "generic function": lambda: world().generic, "generator function": lambda: world_generator(), "builtin function": lambda: len,
    "builtin method": lambda: [1, 2].count, "builtin method of a class": lambda: dict.fromkeys, "method": lambda: world().Plain().method, "method of a class": lambda: world().Plain.of_class, "method of what is derived": lambda: world().Derived().method,
    "method of a builtin": lambda: type(world().Plain().method)(len, [1, 2]), "method_descriptor": lambda: str.upper, "method_descriptor that takes something": lambda: list.append, "wrapper_descriptor": lambda: int.__add__,
    "wrapper_descriptor of object": lambda: object.__init__, "method-wrapper": lambda: (5).__add__, "method-wrapper of a list": lambda: [1, 2].__len__, "classmethod_descriptor": lambda: dict.__dict__["fromkeys"],
    "getset_descriptor": lambda: type(world().function).__dict__["__code__"], "getset_descriptor that cannot be set": lambda: type.__dict__["__mro__"] if type(type.__dict__["__mro__"]).__name__ == "getset_descriptor" else int.__dict__["real"],
    "member_descriptor": lambda: world().Plain.__dict__["slot"], "member_descriptor of a builtin": lambda: slice.__dict__["start"], "property": lambda: world().Plain.__dict__["managed"], "property of nothing": lambda: property(),
    "property with a doc": lambda: property(len, None, None, "Doc."), "staticmethod": lambda: world().Plain.__dict__["static"], "classmethod": lambda: world().Plain.__dict__["of_class"], "staticmethod of a number": lambda: staticmethod(5),
    "classmethod of a builtin": lambda: classmethod(len), "super": lambda: (lambda w: super(w.Derived, w.Derived()))(world()), "super of a class": lambda: (lambda w: super(w.Derived, w.Derived))(world()), "super, unbound": lambda: super(world().Derived), "code": lambda: world().function.__code__,
    "code of a generator": lambda: generator.__code__, "code of a module": lambda: compile("x = 1", "<here>", "exec"), "code of an expression": lambda: compile("x + 1", "<here>", "eval"), "cell": lambda: world().function(1, e=5).__closure__[0],
    "cell with nothing in it": lambda: type(world().function(1, e=5).__closure__[0])(), "module": lambda: type(sys)("name", "Doc."), "module with something in it": lambda: (lambda m: (m.__dict__.update(a=1, __getattr__=lambda n: n.upper()), m)[1])(type(sys)("m")),
    "object": object, "instance": world().Plain, "instance of what is derived": world().Derived, "None": lambda: None, "NotImplemented": lambda: NotImplemented, "Ellipsis": lambda: ..., "class": lambda: type("C", (world().Plain,), {"a": 1}), "class with a metaclass": lambda: world().OfMeta,
    "metaclass": lambda: world().Meta, "type": lambda: type, "the class object": lambda: object, "the class int": lambda: int, "the class of functions": lambda: type(world().function), "the class of generators": lambda: type(generator()),
    "generic alias": lambda: list[int], "generic alias of two": lambda: dict[str, list[int]], "generic alias with a variable": lambda: list[world().generic.__type_params__[0]], "generic alias, unpacked": lambda: (*tuple[int, str],)[0],
    "union": lambda: int | str, "union with None": lambda: int | None, "union of aliases": lambda: list[int] | dict[str, int], "TypeVar": lambda: world().generic.__type_params__[0], "TypeVarTuple": lambda: world().generic.__type_params__[1],
    "ParamSpec": lambda: world().generic.__type_params__[2], "ParamSpecArgs": lambda: world().generic.__type_params__[2].args, "ParamSpecKwargs": lambda: world().generic.__type_params__[2].kwargs, "TypeAliasType": lambda: world().Alias, "TypeAliasType of something": lambda: world().Alias[int],
    "SimpleNamespace": lambda: type(sys.implementation)(a=1, b=[2]), "SimpleNamespace of nothing": lambda: type(sys.implementation)(), "frame that is done with": lambda: deep().__traceback__.tb_next.tb_frame,
    "frame of a generator": lambda: (lambda g: (next(g), g.gi_frame)[1])(kept(generator())), "frame of a new generator": lambda: kept(generator()).gi_frame, "traceback": lambda: deep().__traceback__, "traceback, the last": lambda: deep().__traceback__.tb_next,
    "BaseException": lambda: BaseException(), "Exception": lambda: Exception("a", 1), "raised": lambda: raised(lambda: ValueError("x")), "raised in handling another": lambda: raised(lambda: raised(lambda: KeyError("k")) and TypeError("t")),
    "KeyError": lambda: KeyError("k"), "KeyError of two": lambda: KeyError("k", 1), "StopIteration": lambda: StopIteration(5), "StopIteration of nothing": lambda: StopIteration(), "StopAsyncIteration": lambda: StopAsyncIteration(5),
    "SystemExit": lambda: SystemExit(3), "OSError": lambda: OSError(2, "No such"), "OSError of three": lambda: OSError(2, "No such", "file"), "OSError of five": lambda: OSError(2, "No such", "file", None, "other"), "OSError of one": lambda: OSError("x"),
    "FileNotFoundError": lambda: OSError(2, "x"), "BlockingIOError": lambda: BlockingIOError(11, "x", 5), "SyntaxError": lambda: SyntaxError("bad", ("file", 1, 2, "text", 1, 5)), "SyntaxError of one": lambda: SyntaxError("bad"),
    "SyntaxError, raised": lambda: compiled_wrongly(), "IndentationError": lambda: IndentationError("bad", ("file", 1, 2, "text")),
    "UnicodeDecodeError": lambda: UnicodeDecodeError("utf-8", b"a\xffb", 1, 2, "bad"), "UnicodeEncodeError": lambda: UnicodeEncodeError("ascii", "a\xe9b", 1, 2, "bad"), "UnicodeTranslateError": lambda: UnicodeTranslateError("a\xe9b", 1, 2, "bad"),
    "ImportError": lambda: ImportError("no", name="n", path="p"), "ModuleNotFoundError": lambda: ModuleNotFoundError("no", name="n"), "AttributeError": lambda: AttributeError("no", name="n", obj=5), "AttributeError, raised": lambda: raised_by(lambda: (5).nope),
    "NameError": lambda: NameError("no", name="n"), "NameError, raised": lambda: raised_by(lambda: nope), "ExceptionGroup": lambda: ExceptionGroup("g", [ValueError(1), TypeError(2)]),
    "ExceptionGroup within one": lambda: ExceptionGroup("g", [ValueError(1), ExceptionGroup("h", [KeyError(2), ValueError(3)])]), "BaseExceptionGroup": lambda: BaseExceptionGroup("g", [KeyboardInterrupt(), ValueError(1)]),
    "exception of a class of one's own": lambda: type("Mine", (Exception,), {"__init__": lambda self, a, b=2: setattr(self, "a", a)})(1), "exception with notes": lambda: (lambda e: (e.add_note("n"), e)[1])(ValueError("x")),
}.items():
    subject(name, make)


def compiled_wrongly():
    try:
        compile("x = (1 +\n", "<here>", "exec")
    except SyntaxError as e:
        return e


def raised_by(f):
    try:
        f()
    except BaseException as e:
        return e


def live_frame(action):
    def running(a, b=2):
        local = [a]
        return action(sys._getframe())
    return running(1)


def live_traceback(action):
    try:
        raise ValueError("live")
    except ValueError as e:
        return action(e.__traceback__)


SUBJECTS["frame, running"] = live_frame
SUBJECTS["traceback, being handled"] = live_traceback

try:
    import _contextvars
    variable = _contextvars.ContextVar("v", default=1)
    subject("ContextVar", lambda: _contextvars.ContextVar("v"))
    subject("ContextVar with a default", lambda: _contextvars.ContextVar("v", default=1))
    subject("Context", lambda: _contextvars.Context())
    subject("Context with something in it", lambda: (lambda c: (c.run(variable.set, 5), c)[1])(_contextvars.Context()))
    subject("Token", lambda: _contextvars.Context().run(variable.set, 5))
    SUBJECTS["Context, entered"] = lambda action: (lambda c: c.run(lambda: action(c)))(_contextvars.Context())
except ImportError:
    pass

# ---- What they are given. Each is made afresh each time, since what is tried can change it.

ONE = lambda: [None, 0, 1, -1, 2, 3, 2 ** 70, True, 1.5, "", "a", "__name__", "attribute", b"a", (), (1,), (1, 2), [], [1], {}, {"a": 1}, {1}, int, str, len, Plain, Index(), StopIteration, StopIteration(1), ValueError, ValueError("x"), GeneratorExit, KeyboardInterrupt,
       slice(1), ..., NotImplemented, function, function.__code__]
FEW = lambda: [None, 0, 1, "a", (), tuple([1, 2]), {"a": 1}, int, ValueError, ValueError("x"), Plain]
THREE = lambda: ([None, ValueError, ValueError("x"), 1], [None, ValueError("y"), "a", 1], [None, 1, "a"])
KEYWORDS = [{"a": 1}, {"default": 1}, {"strict": True}, {"name": "n"}, {"co_name": "other"}, {"co_filename": 5}, {"x": 1}, {"self": 1}, {"instance": 1, "owner": int}]
SET_TO = lambda: [None, 0, 1, -1, "a", (), (1,), {}, {"a": 1}, [], int, len, function, function.__code__, generator.__code__, ValueError("x"), Plain(), True, 1.5, ("a", "b"), (int,), 2 ** 70]

# What depends on how things are kept, or on where they are, or is a great deal that is looked at by another audit.
NOT_TRIED = {
    "__sizeof__", "__hash__", "__doc__", "__reduce_ex__", "__getstate__", "__subclasses__", "__dir__", "__init_subclass__", "__subclasshook__",
    # For the traceback module to make suggestions from.
    "_metadata",
    "co_code", "_co_code_adaptive", "co_lnotab", "co_linetable", "co_exceptiontable", "co_stacksize", "co_consts", "co_positions", "co_lines", "co_branches", "_varname_from_oparg", "f_lasti", "tb_lasti",
}
# These are left to the typing module, which is not written yet.
NEEDS_TYPING = {
    ("TypeVar", "__or__"), ("TypeVar", "__ror__"), ("TypeVar", "__typing_subst__"), ("TypeVarTuple", "__iter__"), ("TypeVarTuple", "__typing_prepare_subst__"), ("ParamSpec", "__or__"), ("ParamSpec", "__ror__"),
    ("ParamSpec", "__typing_prepare_subst__"), ("ParamSpec", "__typing_subst__"), ("TypeAliasType", "__iter__"), ("TypeAliasType of something", "__iter__"), ("union", "__or__"), ("union", "__ror__"), ("union with None", "__or__"),
    ("union with None", "__ror__"), ("union of aliases", "__or__"), ("union of aliases", "__ror__"), ("union", "__class_getitem__"), ("union with None", "__class_getitem__"), ("union of aliases", "__class_getitem__"),
}
# There is one of it, which everything has, and what is done to it stays done.
IS_SHARED = {("builtin function", "__module__")}
# CPython 3.14.7 does not survive these.
CRASHES_CPYTHON = {
    # Setting it, when nothing is being traced.
    ("frame that is done with", "f_trace_opcodes"), ("frame of a generator", "f_trace_opcodes"), ("frame of a new generator", "f_trace_opcodes"), ("frame, running", "f_trace_opcodes"),
    # Giving a group other arguments, and then looking at it.
    ("ExceptionGroup", "__init__"), ("ExceptionGroup within one", "__init__"), ("BaseExceptionGroup", "__init__"), ("ExceptionGroup", "args"), ("ExceptionGroup within one", "args"), ("BaseExceptionGroup", "args"),
}
# All that is said of these is what kind of thing they are.
LARGE = {"__globals__", "f_globals", "f_builtins", "__builtins__", "__dict__", "f_back", "f_code", "gi_code", "cr_code", "ag_code", "__code__", "__base__", "__bases__", "__mro__", "__class__", "__objclass__", "__self__", "f_locals", "__wrapped__", "__func__"}


DIRECTORY = function.__code__.co_filename[:function.__code__.co_filename.rfind("/") + 1]


def nowhere(text):
    """Without where things are in memory, or where this file is."""
    if DIRECTORY:
        text = text.replace(DIRECTORY, "")
    parts = text.split(" at 0x")
    return parts[0] + "".join(" at" + part.lstrip("0123456789abcdefABCDEF") for part in parts[1:])


def show(value, depth=0):
    kind = type(value)
    name = kind.__name__
    try:
        if isinstance(value, (set, frozenset)):
            return name + "{" + ", ".join(sorted(show(item, depth + 1) for item in value)) + "}"
        if depth < 4 and kind in (list, tuple):
            return name + "(" + ", ".join(show(item, depth + 1) for item in value) + ")"
        if depth < 4 and (kind is dict or name == "mappingproxy"):
            return name + "{" + ", ".join(show(key, depth + 1) + ": " + show(item, depth + 1) for key, item in list(value.items())[:12] if not (isinstance(key, str) and key.startswith("__"))) + "}"
        if isinstance(value, BaseException):
            return name + ":" + nowhere(repr(value))[:200] + "/" + show(value.args, depth + 1)[:200]
        if depth < 3 and (hasattr(kind, "__next__") or name in ("dict_keys", "dict_values", "dict_items")):
            items = []
            try:
                for item in value:
                    items.append(show(item, depth + 1))
                    if len(items) >= 8:
                        break
            except BaseException as error:
                items.append("raises " + type(error).__name__ + ": " + nowhere(str(error))[:100])
            return name + "<" + ", ".join(items) + ">"
        text = nowhere(repr(value))
    except BaseException as error:
        return name + " that cannot be shown: " + type(error).__name__ + ": " + nowhere(str(error))[:100]
    if len(text) > 300:
        text = text[:150] + "..." + text[-150:] + " (%d)" % len(text)
    return name + ":" + text


def state(instance):
    """What has become of it. This uses it up, if it is something that can be."""
    said = show(instance)
    for name in ("gi_running", "gi_suspended", "cr_running", "cr_suspended", "ag_running", "ag_suspended", "gi_yieldfrom", "cr_await", "ag_await", "args", "__notes__", "__cause__", "__context__", "__suppress_context__", "cell_contents", "__name__",
                 "__qualname__", "__defaults__", "__kwdefaults__", "__annotations__", "__module__", "f_lineno", "tb_lineno", "tb_next", "f_trace", "f_trace_lines", "f_trace_opcodes", "fget", "fset", "fdel", "__isabstractmethod__"):
        try:
            said += " " + name + "=" + show(getattr(instance, name), 2)
        except AttributeError:
            pass
        except BaseException as error:
            said += " " + name + " raises " + type(error).__name__
    return said


def outcome(f):
    try:
        return show(f())
    except RecursionError:
        return "RecursionError"
    except BaseException as error:
        return type(error).__name__ + ": " + nowhere(str(error))[:300]


WRITTEN = {}


def written(name, count, keys):
    if (name, count, keys) not in WRITTEN:
        WRITTEN[name, count, keys] = eval("lambda instance, a, k: instance.%s(%s)" % (name, ", ".join(["a[%d]" % i for i in range(count)] + ["%s=k[%r]" % (key, key) for key in keys])))
    return WRITTEN[name, count, keys]


def can_be_called(instance, name):
    try:
        return callable(getattr(instance, name))
    except BaseException:
        return False


def is_not_to_be_tried(which, name, arguments):
    # It would go on for ever, or take up all the room that there is.
    if name in ("__mul__", "__rmul__", "__imul__", "__pow__", "__lshift__") and any(isinstance(a, int) and abs(a) > 1000 for a in arguments):
        return True
    # A generator that will not stop, gone through.
    return which.startswith("generator that will not stop") and name in ("__iter__",)


everything = sys.argv[1:2] == ["everything"]
wanted = not everything and sys.argv[1:] and tuple(sys.argv[1:3])

for which, use in SUBJECTS.items():
    if everything and sys.argv[2:] and which not in sys.argv[2:]:
        continue
    if wanted and wanted[0] not in (which, "any"):
        continue
    names = use(lambda instance: sorted(dir(instance)))
    print(which, "dir", "|", len(names), int.from_bytes(" ".join(names).encode(), "big") % 1000000007)
    if wanted and wanted[1:] == ("dir",) or everything:
        print(which, " ".join(names))
    print(which, "as it is", "|", 1, int.from_bytes((use(lambda instance: type(instance).__name__ + " " + type(instance).__module__ + " " + type(instance).__qualname__) + use(state)).encode("utf-8", "backslashreplace"), "big") % 1000000007)
    if wanted and wanted[1:] == ("as",) or everything:
        print(which, use(state))
    for name in names:
        if name in NOT_TRIED or (which, name) in CRASHES_CPYTHON or (which, name) in NEEDS_TYPING or (which, name) in IS_SHARED or (wanted and wanted[1:] and wanted[1] != name):
            continue
        # That of object is in a module that is not written yet.
        if name == "__reduce__" and use(lambda instance: all("__reduce__" not in kind.__dict__ for kind in type(instance).__mro__[:-1])):
            continue
        results = []

        def note(said, act):
            def action(instance):
                result = outcome(lambda: act(instance))
                return result + " -> " + state(instance)
            results.append(said + ": " + use(action))

        if name in LARGE:
            note("get", lambda instance: type(getattr(instance, name)).__name__)
        else:
            note("get", lambda instance: getattr(instance, name))
        note("delete", lambda instance: delattr(instance, name))
        for i in range(len(SET_TO())):
            note("set " + show(SET_TO()[i]), lambda instance: setattr(instance, name, SET_TO()[i]))
        if use(lambda instance: can_be_called(instance, name)) and name not in ("__class__", "__objclass__", "__self__", "__func__", "__wrapped__", "__base__", "fget", "fset", "fdel", "__origin__", "__thisclass__", "__self_class__"):
            def call(make=lambda: (), **keywords):
                if is_not_to_be_tried(which, name, make()):
                    return
                said = ", ".join([show(argument) for argument in make()] + ["%s=%s" % (key, show(value)) for key, value in keywords.items()])
                note("(" + said + ")", lambda instance: getattr(instance, name)(*make(), **keywords))
                # And as it would be written, `instance.name(a, k=v)`, which CPython does not go about in the same way: what is said of the wrong arguments can be otherwise.
                if len(make()) < 2:
                    note("written (" + said + ")", lambda instance: written(name, len(make()), tuple(keywords))(instance, make(), keywords))
            call()
            for i in range(len(ONE())):
                call(lambda: (ONE()[i],))
            for i in range(len(FEW())):
                for j in range(len(FEW())):
                    call(lambda: (FEW()[i], FEW()[j]))
            for i in range(4):
                for j in range(4):
                    for k in range(3):
                        call(lambda: (THREE()[0][i], THREE()[1][j], THREE()[2][k]))
            for keywords in KEYWORDS:
                call(**keywords)
                call(lambda: (1,), **keywords)
        what = "%s %s" % (which, name)
        if everything:
            print("\n".join(what + " " + result for result in results))
            continue
        if wanted:
            print("\n".join(which + " " + result if wanted[0] == "any" else result for result in results))
        # At once, so that if it goes no further it can be seen how far it went.
        print(what, "|", len(results), int.from_bytes("\n".join(results).encode("utf-8", "backslashreplace"), "big") % 1000000007, flush=True)
