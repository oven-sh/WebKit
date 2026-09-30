# What is made and goes nowhere need not be made at all, and in code that has been compiled for the last time it is not. Whatever looks for it is to find it all the same, and to find the one thing however often it looks.
import sys


def show(label, f, *arguments):
    "The first time, before anything has been compiled, and every time after that: what goes wrong may do so only on the run after it was compiled"
    def outcome():
        try:
            return f(*arguments)
        except Exception as e:
            return type(e).__name__, str(e)
    first = outcome()
    for i in range(700):
        now = outcome()
        if now != first:
            print(label, "=>", ("AT FIRST", first, "AND THEN", now))
            return
    print(label, "=>", first)


class E:
    pass


class F:
    def __repr__(self):
        return "F(%r)" % sorted(vars(self).items())


class WithInit:
    def __init__(self, x, y):
        self.x = x
        self.y = y


def peek():
    return sys._getframe(1).f_locals


print("---- that goes nowhere")


def goes_nowhere(n):
    e = E()
    e.x = n
    e.y = n + 1
    return e.x + e.y


def one_in_another(n):
    a = E()
    b = E()
    a.other = b
    b.x = n
    return a.other.x


def that_has_itself(n):
    a = E()
    a.me = a
    a.x = n
    return a.me.me is a, a.me.me.x


def by_two_names(n):
    e = E()
    f = e
    f.x = n
    return e is f, id(e) == id(f), e.x, e == f, e != f


def more_than_there_is_room_for(n):
    e = E()
    e.a = n; e.b = n + 1; e.c = n + 2; e.d = n + 3; e.e = n + 4; e.f = n + 5; e.g = n + 6; e.h = n + 7; e.i = n + 8; e.j = n + 9
    return e.a + e.b + e.c + e.d + e.e + e.f + e.g + e.h + e.i + e.j


def in_a_loop(n):
    total = 0
    for i in range(n):
        e = E()
        e.v = i
        e.w = 0.5
        total += e.v + e.w
    return total


def with_an_initializer(n):
    p = WithInit(n, n + 1)
    return p.x + p.y


show("attributes and no more", goes_nowhere, 3)
show("one in another", one_in_another, 3)
show("one that has itself", that_has_itself, 3)
show("by two names", by_two_names, 3)
show("more than there is room for", more_than_there_is_room_for, 3)
show("in a loop", in_a_loop, 40)
show("with __init__", with_an_initializer, 3)
# Long enough for it to be compiled for the last time while it is going round.
print("in a long loop =>", in_a_loop(400000))

print("---- that is looked for")


def by_what_is_called(n):
    e = E()
    e.x = n
    seen = peek()
    return seen["e"].x, seen["e"] is e, type(seen["e"]).__name__, seen["e"] is peek()["e"]


def changed_by_what_is_called(n):
    def change():
        sys._getframe(1).f_locals["e"].x = "changed"
    e = E()
    e.x = n
    change()
    return e.x


def put_in_its_place(n):
    def replace():
        other = F()
        other.x = "another"
        sys._getframe(1).f_locals["e"] = other
    e = E()
    e.x = n
    replace()
    return e.x, type(e).__name__


def in_a_traceback(n):
    e = E()
    e.x = n
    try:
        1 // (n - n)
    except ZeroDivisionError as error:
        found = error.__traceback__.tb_frame.f_locals["e"]
        return found.x, found is e


def in_the_traceback_of_what_called(n):
    def fails():
        e = E()
        e.x = n
        e.y = [n]
        return e.x // (n - n)
    try:
        fails()
    except ZeroDivisionError as error:
        found = error.__traceback__.tb_next.tb_frame.f_locals["e"]
        return found.x, found.y, type(found).__name__


show("by what is called", by_what_is_called, 3)
show("and changed", changed_by_what_is_called, 3)
show("and something else put in its place", put_in_its_place, 3)
show("in a traceback", in_a_traceback, 3)
show("in the traceback of what was called", in_the_traceback_of_what_called, 3)

print("---- that goes somewhere only now and then")
kept = []


def now_and_then(n, when):
    e = E()
    e.x = n
    e.y = n * 2
    if when:
        kept.append(e)
    return e.x + e.y


def run_now_and_then():
    del kept[:]
    total = 0
    for i in range(1500):
        total += now_and_then(i, i % 500 == 499)
    return total, [(e.x, e.y, type(e).__name__) for e in kept]


def either_way(n, which):
    e = E()
    e.x = n
    if which:
        e.y = 1
    else:
        e.z = 2
    return sorted(vars(e).items())


print("kept =>", run_now_and_then())
show("with one attribute", either_way, 3, True)
show("or another", either_way, 3, False)

print("---- when something is not what it has always been")


def added_to(n, d):
    e = E()
    e.x = n
    e.y = e.x + d
    return e.y, sorted(vars(e).items())


def and_goes_nowhere(n, d):
    e = E()
    e.x = n
    e.y = n * 2
    inner = E()
    inner.z = d
    e.inner = inner
    r = e.x + d
    return r, e.x, e.y, e.inner.z


def surprised(f, usual, surprises):
    for i in range(700):
        f(3, usual)
    results = []
    for again in range(2):
        for d in surprises:
            try:
                results.append(f(3, d))
            except Exception as error:
                results.append((type(error).__name__, str(error)))
        for i in range(700):
            f(3, usual)
    return results


print("something else to add =>", surprised(added_to, 1, [0.5, 2 ** 40, True, "s", None, 2 ** 31 - 3]))
print("to what goes nowhere =>", surprised(and_goes_nowhere, 1, [0.5, 2 ** 40, True, "s", None, 2 ** 31 - 3]))

print("---- what else can be done to it")


def as_a_dictionary(n):
    e = E()
    e.x = n
    d = e.__dict__
    d["y"] = n + 1
    return e.y, sorted(d.items()), vars(e) is d


def of_another_class(n):
    e = E()
    e.x = n
    e.__class__ = F
    return repr(e), type(e).__name__


def with_one_taken_away(n):
    e = E()
    e.x = n
    e.y = n
    del e.x
    return sorted(vars(e)), hasattr(e, "x"), getattr(e, "x", "gone")


def with_one_that_is_not_there(n):
    e = E()
    e.x = n
    return e.y


def by_name(n):
    e = E()
    setattr(e, "x", n)
    return getattr(e, "x"), e.x


show("as a dictionary", as_a_dictionary, 3)
show("of another class", of_another_class, 3)
show("with an attribute taken away", with_one_taken_away, 3)
show("with one that is not there", with_one_that_is_not_there, 3)
show("by name", by_name, 3)
