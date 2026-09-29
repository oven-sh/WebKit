# frame.f_lineno = n: what there is to it besides where it can go, which is in jumping-to-another-line.py.
import sys
import _warnings


def jump(function, jump_from, jump_to, arguments=(), again=None, then=None):
    first = function.__code__.co_firstlineno
    said = []

    def tracer(frame, what, argument):
        if frame.f_code is function.__code__ and what == "line" and frame.f_lineno - first == jump_from and not said:
            try:
                frame.f_lineno = first + jump_to
                said.append("jumped, and is on %d" % (frame.f_lineno - first))
                if again is not None:
                    frame.f_lineno = first + again
                    said.append("and again, and is on %d" % (frame.f_lineno - first))
            except BaseException as error:
                said.append(type(error).__name__ + ": " + str(error))
            if then:
                then()
        return tracer
    sys.settrace(tracer)
    try:
        result = function(*arguments)
        if hasattr(result, "__next__"):
            result = list(result)
    except BaseException as error:
        result = type(error).__name__ + ": " + str(error)
    finally:
        sys.settrace(None)
    return said, result


print("---- variables that have no value are given None, and it is said so")


def unbound(o):
    o.append(1)
    a = 2
    b = 3
    o.append((4, a, b))


_warnings.filters.insert(0, ("ignore", None, Warning, None, 0))
for jump_from, jump_to in ((1, 4), (1, 3), (1, 2), (3, 4), (4, 1)):
    output = []
    print(jump_from, jump_to, jump(unbound, jump_from, jump_to, (output,)), output)
_warnings.filters[0] = ("error", None, Warning, None, 0)
for jump_from, jump_to in ((1, 4), (3, 4), (4, 1)):
    output = []
    print("if that is an error, it stays where it is", jump_from, jump_to, jump(unbound, jump_from, jump_to, (output,)), output)
_warnings.filters[0] = ("ignore", None, Warning, None, 0)

print("---- twice")


def straight(o):
    o.append(1)
    o.append(2)
    o.append(3)
    o.append(4)
    o.append(5)


for target, again in ((4, 3), (4, 5), (5, 1), (3, 3), (4, 99), (4, -5)):
    output = []
    print(target, again, jump(straight, 2, target, (output,), again), output)


def loops(o):
    for i in range(2):
        o.append((2, i))
    o.append(3)
    for j in "ab":
        o.append((5, j))
    o.append(6)


for target, again in ((3, 5), (5, 2), (5, 3), (6, 2)):
    output = []
    print("out of a loop and", target, again, jump(loops, 2, target, (output,), again), output)

print("---- what is told raises, after")


def fail():
    raise KeyError("from what was told")


output = []
print(jump(straight, 2, 4, (output,), None, fail), output)

print("---- what is being handled")


def handling(o):
    try:
        raise ValueError("first")
    except ValueError:
        o.append((4, str(sys.exception())))
        try:
            raise KeyError("second")
        except KeyError:
            o.append((8, str(sys.exception())))
            o.append(9)
        o.append((10, str(sys.exception())))
    o.append((11, str(sys.exception())))


for jump_from, jump_to in ((8, 10), (8, 11), (8, 4), (9, 1), (4, 11), (10, 8), (4, 8), (8, 9)):
    output = []
    print(jump_from, jump_to, jump(handling, jump_from, jump_to, (output,)), output, sys.exception())


def raising_again(o):
    try:
        try:
            raise ValueError("first")
        except ValueError:
            o.append(5)
        o.append(6)
        try:
            raise KeyError("second")
        except KeyError:
            o.append(10)
            raise
    except Exception as e:
        o.append((13, type(e).__name__, str(e)))


for jump_from, jump_to in ((5, 10), (5, 11), (10, 5)):
    output = []
    print("raise, in another handler", jump_from, jump_to, jump(raising_again, jump_from, jump_to, (output,)), output)

print("---- variables that other functions use")


def cells(o):
    a = 1
    b = [lambda: (a, x) for x in range(2)
         if o.append(("if", x)) is None]
    o.append((4, [f() for f in b]))
    c = lambda: a
    o.append((6, c()))


for jump_from, jump_to in ((3, 4), (3, 5), (3, 6), (3, 1), (3, 2), (1, 3), (6, 2), (4, 2)):
    output = []
    print(jump_from, jump_to, jump(cells, jump_from, jump_to, (output,)), output)

print("---- a generator")


def generator(o):
    o.append(len("1"))
    yield 2
    o.append(len("333"))
    yield 4
    o.append(5)


for jump_from, jump_to in ((3, 1), (5, 1), (5, 3), (1, 5), (3, 5), (1, 3)):
    output = []
    print(jump_from, jump_to, jump(generator, jump_from, jump_to, (output,)), output)

print("---- with sys.monitoring")
events = sys.monitoring.events


def monitored(event, jump_from, jump_to):
    told = []
    first = loops.__code__.co_firstlineno

    def line(code, number):
        if code is loops.__code__:
            told.append(number - first)
            if number - first == jump_from and "jumped" not in told:
                try:
                    sys._getframe(1).f_lineno = first + jump_to
                    told.append("jumped")
                except ValueError as error:
                    told.append(str(error))

    def other(code, *rest):
        if code is loops.__code__ and not any(isinstance(t, str) for t in told):
            try:
                sys._getframe(1).f_lineno = first + jump_to
                told.append("jumped")
            except BaseException as error:
                told.append(type(error).__name__ + ": " + str(error))
    sys.monitoring.use_tool_id(1, "test")
    sys.monitoring.register_callback(1, event, line if event == events.LINE else other)
    sys.monitoring.set_events(1, event)
    output = []
    try:
        loops(output)
    finally:
        sys.monitoring.set_events(1, 0)
        sys.monitoring.register_callback(1, event, None)
        sys.monitoring.free_tool_id(1)
    return told, output


print("LINE", monitored(events.LINE, 2, 5), monitored(events.LINE, 3, 2), monitored(events.LINE, 5, 6))
for name in ("PY_START", "PY_RETURN", "CALL", "INSTRUCTION"):
    print(name, monitored(getattr(events, name), 0, 3))

print("---- when nothing is being told")
try:
    sys._getframe().f_lineno = 1
except ValueError as error:
    print(error)
for value in ("1", 1.0, None, True):
    try:
        sys._getframe().f_lineno = value
    except ValueError as error:
        print(repr(value), error)
try:
    del sys._getframe().f_lineno
except AttributeError as error:
    print(error)

print("---- what is in no function")


def run_source(source, name, jump_from, jump_to, namespace=None):
    code = compile(source, "<source>", "exec")
    said = []

    def tracer(frame, what, argument):
        if frame.f_code.co_name == name and frame.f_code.co_filename == "<source>" and what == "line" and frame.f_lineno == jump_from and not said:
            try:
                frame.f_lineno = jump_to
                said.append("jumped")
            except ValueError as error:
                said.append(str(error))
        return tracer
    namespace = {"o": []} if namespace is None else namespace
    sys.settrace(tracer)
    try:
        exec(code, namespace)
        result = None
    except BaseException as error:
        result = type(error).__name__ + ": " + str(error)
    finally:
        sys.settrace(None)
    return said, namespace["o"], result, sorted(k for k in namespace if len(k) == 1)


MODULE = """o.append(1)
a = 2
for i in range(2):
    o.append((4, i))
try:
    o.append(6)
finally:
    o.append(8)
b = 9
"""
for jump_from, jump_to in ((1, 9), (1, 4), (4, 9), (4, 1), (2, 6), (2, 8), (8, 2), (9, 3), (1, 0), (1, 10)):
    print("a module", jump_from, jump_to, run_source(MODULE, "<module>", jump_from, jump_to))
CLASS = """o.append(1)
class C:
    o.append(3)
    a = 4
    for i in range(2):
        o.append((6, i))
    b = 7
    o.append((8, sorted(k for k in locals() if len(k) == 1)))
o.append(9)
"""
for jump_from, jump_to in ((3, 7), (3, 8), (3, 6), (6, 8), (6, 3), (8, 3), (3, 2), (3, 1), (3, 9), (4, 4)):
    print("a class", jump_from, jump_to, run_source(CLASS, "C", jump_from, jump_to))
for jump_from, jump_to in ((1, 9), (1, 3), (2, 9), (9, 2)):
    print("what a class is in", jump_from, jump_to, run_source(CLASS, "<module>", jump_from, jump_to))
