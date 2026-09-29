# frame.f_lineno = n, where what is done here is not what CPython 3.14 does, and why. The rest is in programs/jumping-to-another-line.py.
import sys
import _warnings

_warnings.filters.insert(0, ("ignore", None, Warning, None, 0))


def trace(code, decide):
    said = []

    def tracer(frame, what, argument):
        target = None if said else decide(frame, what)
        if target:
            try:
                target[0].f_lineno = target[1]
                said.append("jumped")
            except ValueError as error:
                said.append(str(error))
        return tracer
    return tracer, said


def attempt(function, decide, drive=lambda f, o: f(o)):
    output = []
    tracer, said = trace(function.__code__, decide)
    sys.settrace(tracer)
    try:
        result = drive(function, output)
    except BaseException as error:
        result = type(error).__name__ + ": " + str(error)
    finally:
        sys.settrace(None)
    return said, output, result


print("---- a frame further out")
# It is in the middle of calling something. CPython takes it, if it is in a trace function that it is done, and does not survive. There is nowhere for such a frame to go on from but where the call returns to.


def outer(o):
    o.append(1)
    inner(o)
    o.append(3)
    o.append(4)


def inner(o):
    o.append("inner")


print(attempt(outer, lambda frame, what: (frame.f_back, outer.__code__.co_firstlineno + 4) if frame.f_code is inner.__code__ and what == "line" else None))

print("---- a frame that is over, and one that has not begun")
kept = []


def keeps(o):
    kept.append(sys._getframe())


keeps([])
suspended = (lambda: (yield))()
print(attempt(outer, lambda frame, what: (kept[0], keeps.__code__.co_firstlineno + 1) if frame.f_code is outer.__code__ and what == "line" else None))
print(attempt(outer, lambda frame, what: (suspended.gi_frame, suspended.gi_code.co_firstlineno) if frame.f_code is outer.__code__ and what == "line" else None))

print("---- as a generator yields, and as it is resumed")
# CPython can do it then. It is at a line that a debugger stops.


def generator(o):
    o.append(1)
    yield 2
    o.append(3)
    yield 4
    o.append(5)


first = generator.__code__.co_firstlineno
go_through = lambda f, o: list(f(o))
print(attempt(generator, lambda frame, what: (frame, first + 5) if frame.f_code is generator.__code__ and what == "return" and frame.f_lineno == first + 2 else None, go_through))
print(attempt(generator, lambda frame, what: (frame, first + 5) if frame.f_code is generator.__code__ and what == "call" and frame.f_lineno == first + 2 else None, go_through))

print("---- what a generator did not keep")
# When it yields, it keeps what could be made use of afterwards, as the code is written. After this `yield` it returns, so what the loop is going round is not kept, and there is no going from there to where the loop
# goes on. It goes by how the code is written and not by what has been run, so it is the same on the line of the `yield`, before that has been done. From before the `if` it can.


def leaves(o):
    for i in range(3):
        o.append((2, i))
        if i == 1:
            yield i
            o.append(5)
            return
        o.append(7)
    o.append(8)


first_of_leaves = leaves.__code__.co_firstlineno
for jump_from, jump_to in ((5, 7), (5, 2), (6, 7), (5, 8), (5, 1), (5, 6), (4, 7), (3, 7), (7, 5)):
    print(jump_from, jump_to, attempt(leaves, lambda frame, what: (frame, first_of_leaves + jump_to) if frame.f_code is leaves.__code__ and what == "line" and frame.f_lineno == first_of_leaves + jump_from else None, go_through))

print("---- one kind of thing for another")
# CPython goes by whether a thing is an iterator, something that was being handled, or anything else. So it will take two iterators for the __exit__ of a `with` and what that is a method of, and call one.


class Manager:
    def __init__(self, o): self.o = o
    def __enter__(self): self.o.append("enter")
    def __exit__(self, *a): self.o.append("exit")


def kinds(o):
    for i in range(2):
        for j in range(2):
            o.append((3, i, j))
    with Manager(o):
        o.append(5)
    o.append(6)


first_of_kinds = kinds.__code__.co_firstlineno
print(attempt(kinds, lambda frame, what: (frame, first_of_kinds + 5) if frame.f_code is kinds.__code__ and what == "line" and frame.f_lineno == first_of_kinds + 3 else None))

print("---- the middle of something")
# What has been worked out so far of something that goes over several lines is on CPython's stack, and it can go from the middle of one such thing to the middle of another.


def several(o):
    o.append((
        2,
        len(o)))
    x = [
        len(o),
        6]
    o.append(x)


first_of_several = several.__code__.co_firstlineno
for jump_from, jump_to in ((3, 2), (3, 6), (3, 3), (3, 7), (3, 1), (1, 3)):
    print(jump_from, jump_to, attempt(several, lambda frame, what: (frame, first_of_several + jump_to) if frame.f_code is several.__code__ and what == "line" and frame.f_lineno == first_of_several + jump_from else None))
