# frame.f_lineno = n, in a trace function, which is how a debugger goes on from somewhere else. Each of these is run once for each two of its lines, with a trace function that jumps from the one to the other the first
# time that it comes to it. What is looked at is what is said if that is refused, and otherwise which lines were run after it and how it ended.
import sys
import _warnings

_warnings.filters.insert(0, ("ignore", None, Warning, None, 0))


class Manager:
    def __init__(self, output, n, swallows=False):
        self.output, self.n, self.swallows = output, n, swallows

    def __enter__(self):
        self.output.append("enter %d" % self.n)
        return self

    def __exit__(self, kind, value, traceback):
        self.output.append("exit %d %s" % (self.n, kind and kind.__name__))
        return self.swallows


class AsyncManager(Manager):
    async def __aenter__(self): return self.__enter__()
    async def __aexit__(self, *a): return self.__exit__(*a)


class AsyncRange:
    def __init__(self, n): self.i, self.n = 0, n
    def __aiter__(self): return self
    async def __anext__(self):
        if self.i >= self.n:
            raise StopAsyncIteration
        self.i += 1
        return self.i - 1


BODIES = {
    "one after another": """
o.append(1)
o.append(2)
o.append(3)
o.append(4)
""", "variables": """
a = 1
b = a + 1
o.append((3, a, b))
del a
o.append(5)
c = 6
o.append((7, c))
""", "if": """
o.append(1)
if len(o) == 1:
    o.append(3)
    o.append(4)
else:
    o.append(6)
    o.append(7)
o.append(8)
""", "for": """
o.append(1)
for i in range(2):
    o.append((3, i))
    o.append(4)
o.append(5)
""", "for and else": """
o.append(1)
for i in range(2):
    o.append((3, i))
else:
    o.append(5)
o.append(6)
""", "for in for": """
o.append(1)
for i in range(2):
    o.append((3, i))
    for j in "ab":
        o.append((5, i, j))
    o.append(6)
o.append(7)
""", "one for and then another": """
for i in range(2):
    o.append((2, i))
o.append(3)
for j in "ab":
    o.append((5, j))
o.append(6)
""", "break and continue": """
for i in range(4):
    o.append((2, i))
    if i == 1:
        continue
    if i == 2:
        break
    o.append(7)
o.append(8)
""", "while": """
n = 0
while n < 2:
    o.append((3, n))
    n += 1
else:
    o.append(6)
o.append(7)
""", "try and except": """
o.append(1)
try:
    o.append(3)
    raise ValueError
    o.append(5)
except ValueError:
    o.append(7)
    o.append(8)
o.append(9)
""", "except as": """
o.append(1)
try:
    o.append(3)
    raise ValueError("v")
except ValueError as e:
    o.append((6, str(e)))
    o.append(7)
o.append(8)
""", "nothing raised": """
o.append(1)
try:
    o.append(3)
    o.append(4)
except ValueError:
    o.append(6)
else:
    o.append(8)
o.append(9)
""", "two handlers": """
try:
    o.append(2)
    raise KeyError
except ValueError:
    o.append(5)
except KeyError:
    o.append(7)
o.append(8)
""", "try and finally": """
o.append(1)
try:
    o.append(3)
    o.append(4)
finally:
    o.append(6)
    o.append(7)
o.append(8)
""", "finally, with something raised": """
try:
    try:
        o.append(3)
        raise ValueError
    finally:
        o.append(6)
        o.append(7)
    o.append(8)
except ValueError:
    o.append(10)
o.append(11)
""", "all four": """
try:
    o.append(2)
except ValueError:
    o.append(4)
else:
    o.append(6)
finally:
    o.append(8)
o.append(9)
""", "a handler in a handler": """
try:
    raise ValueError
except ValueError:
    o.append(4)
    try:
        raise KeyError
    except KeyError:
        o.append(8)
    o.append(9)
o.append(10)
""", "one handler and then another": """
try:
    raise ValueError("first")
except ValueError:
    o.append((4, str(sys.exception())))
o.append(5)
try:
    raise KeyError("second")
except KeyError:
    o.append((9, str(sys.exception())))
o.append((10, sys.exception()))
""", "raise again": """
try:
    try:
        raise ValueError("v")
    except ValueError:
        o.append(5)
        raise
    o.append(7)
except ValueError as e:
    o.append((9, str(e)))
o.append(10)
""", "with": """
o.append(1)
with Manager(o, 2):
    o.append(3)
    o.append(4)
o.append(5)
""", "with as": """
o.append(1)
with Manager(o, 2) as m:
    o.append((3, m.n))
o.append(4)
""", "with in with": """
with Manager(o, 1):
    o.append(2)
    with Manager(o, 3):
        o.append(4)
    o.append(5)
o.append(6)
""", "one with and then another": """
with Manager(o, 1):
    o.append(2)
o.append(3)
with Manager(o, 4):
    o.append(5)
o.append(6)
""", "two in one with": """
o.append(1)
with Manager(o, 2), Manager(o, 20):
    o.append(3)
o.append(4)
""", "for in with in try": """
try:
    with Manager(o, 2):
        for i in range(2):
            o.append((4, i))
        o.append(5)
    o.append(6)
finally:
    o.append(8)
o.append(9)
""", "with in for": """
for i in range(2):
    with Manager(o, 2):
        o.append((3, i))
    o.append(4)
o.append(5)
""", "try in for": """
for i in range(2):
    try:
        o.append((3, i))
        raise ValueError
    except ValueError:
        o.append(6)
    o.append(7)
o.append(8)
""", "over several lines": """
o.append(1)
o.append((
    2,
    len(o),
    4))
x = [
    len(o),
    7]
o.append(x)
""", "return": """
o.append(1)
if len(o) > 5:
    return 3
o.append(4)
return 5
o.append(6)
""", "return in try": """
try:
    o.append(2)
    return 3
finally:
    o.append(5)
o.append(6)
""", "def and class": """
o.append(1)
def f():
    o.append(3)
f()
class C:
    o.append(6)
o.append(7)
""", "comprehension": """
o.append(1)
x = [i
     for i in range(2)
     if i >= 0]
o.append((5, x))
""", "match": """
o.append(1)
match len(o):
    case 0:
        o.append(4)
    case 1:
        o.append(6)
    case _:
        o.append(8)
o.append(9)
""", "except star": """
try:
    o.append(2)
    raise ExceptionGroup("g", [ValueError(1), KeyError(2)])
except* ValueError:
    o.append(5)
except* KeyError:
    o.append(7)
o.append(8)
""", "nothing there": """
o.append(1)

# nothing
o.append(4)
pass
o.append(6)
global g
o.append(8)
""",
}
GENERATORS = {
    "generator": """
o.append(1)
x = yield 2
o.append((3, x))
yield 4
o.append(5)
""", "yield in for": """
for i in range(2):
    o.append((2, i))
    yield i
    o.append(4)
o.append(5)
""", "yield in try": """
try:
    o.append(2)
    yield 3
    o.append(4)
finally:
    o.append(6)
o.append(7)
""", "yield from": """
o.append(1)
yield from "ab"
o.append(3)
yield from "cd"
o.append(5)
""",
}
COROUTINES = {
    "async for": """
o.append(1)
async for i in AsyncRange(2):
    o.append((3, i))
    o.append(4)
o.append(5)
""", "async with": """
o.append(1)
async with AsyncManager(o, 2):
    o.append(3)
    o.append(4)
o.append(5)
""", "async for and for": """
async for i in AsyncRange(2):
    o.append((2, i))
o.append(3)
for j in "ab":
    o.append((5, j))
o.append(6)
""", "async with and with": """
async with AsyncManager(o, 1):
    o.append(2)
o.append(3)
with Manager(o, 4):
    o.append(5)
o.append(6)
""",
}


# Where CPython goes by what it has worked out so far of something that it is in the middle of, which is on its stack. It will go from the middle of one thing to the middle of another that has as much, and make what
# it can of what is there: `x = [len(o), 7]` comes to [<method 'append' of 'list' objects>, 7]. Here that is refused. What is left out is from one to another of these lines.
IN_THE_MIDDLE = {
    "over several lines": {3, 4, 5, 6, 8},
    "comprehension": {2, 3, 4},
    # From one clause of an `except*` to another, between which there is what is left of the group.
    "except star": {4, 5, 6, 7},
}
# Going round a `for` with what an `async for` was going round is a TypeError, and says something else.
OTHERWISE = {("async for and for", 2, 5)}


def make(head, body):
    namespace = dict(globals())
    exec(head + "".join("    " + line + "\n" for line in body.strip("\n").split("\n")), namespace)
    return namespace["f"]


def attempt(function, drive, jump_from, jump_to, event="line"):
    output = []
    said = []
    told = []
    code = function.__code__
    steps = [0]

    def tracer(frame, what, argument):
        # A jump can be to where it will never end.
        steps[0] += 1
        if steps[0] > 400:
            raise TimeoutError("it goes on and on")
        if frame.f_code is code:
            told.append(what[0] + str(frame.f_lineno - 1))
        if frame.f_code is code and what == event and not said and frame.f_lineno == jump_from + 1:
            try:
                frame.f_lineno = jump_to + 1
                said.append("jumped")
                told.append("to" + str(frame.f_lineno - 1))
            except ValueError as error:
                said.append(str(error))
        return tracer
    sys.settrace(tracer)
    try:
        result = drive(function, output)
    except BaseException as error:
        result = type(error).__name__ + ": " + str(error)
    finally:
        sys.settrace(None)
    return said[0] if said else "not come to", output[:40], result, " ".join(told[:60])


KEPT = []


def call(function, output):
    return function(output)


def go_through(function, output):
    # It is kept, since CPython closes it as soon as nothing has it, which would be traced.
    KEPT.append(function(output))
    return [item for item, again in zip(KEPT[-1], range(30))]


def run(function, output):
    coroutine = function(output)
    KEPT.append(coroutine)
    try:
        while True:
            coroutine.send(None)
    except StopIteration as stop:
        return stop.value


# jumping-to-another-line.py [--] <which> [<from> <to>], to try one of them by itself.
only = sys.argv[1:]
for bodies, head, drive in ((BODIES, "def f(o):\n", call), (GENERATORS, "def f(o):\n", go_through), (COROUTINES, "async def f(o):\n", run)):
    for name, body in bodies.items():
        if only and only[0] != name:
            continue
        function = make(head, body)
        count = len(body.strip("\n").split("\n"))
        print("----", name)
        for jump_from in range(1, count + 1):
            for jump_to in range(-1, count + 3):
                if only[1:] and (jump_from, jump_to) != (int(only[1]), int(only[2])):
                    continue
                # The line that a generator or a coroutine begins on is where CPython makes it, and to go back there makes another.
                if not jump_to and drive is not call:
                    continue
                if jump_from != jump_to and {jump_from, jump_to} <= IN_THE_MIDDLE.get(name, set()) or (name, jump_from, jump_to) in OTHERWISE:
                    continue
                print(jump_from, "to", jump_to, "=>", end=" ", flush=True)
                print(*attempt(function, drive, jump_from, jump_to), flush=True)

if only:
    sys.exit()
print("---- from what is not a line")
function = make("def f(o):\n", BODIES["try and except"])
for event in ("call", "return", "exception", "opcode"):
    for jump_from in (0, 4, 9):
        print(event, jump_from, "=>", end=" ", flush=True)
        print(*attempt(function, call, jump_from, 3, event), flush=True)
print("---- what it is set to", flush=True)
function = make("def f(o):\n", BODIES["one after another"])
for value in (None, "3", 3.0, True, 2 ** 31, -2 ** 31 - 1, 2 ** 70, -1, 0):
    said = []

    def tracer(frame, what, argument):
        if frame.f_code is function.__code__ and what == "line" and not said:
            try:
                frame.f_lineno = value
                said.append("jumped")
            except BaseException as error:
                said.append(type(error).__name__ + ": " + str(error))
        return tracer
    output = []
    sys.settrace(tracer)
    function(output)
    sys.settrace(None)
    print(repr(value), "=>", said, output)
