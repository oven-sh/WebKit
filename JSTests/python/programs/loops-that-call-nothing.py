# Going round a loop through a range, a list or a tuple runs no code but what is in the loop. Once it has been compiled that is gone by: what its variables have is kept wherever is quickest, and what has been found out about
# anything is not found out again each time round. So this is about what changes all the same, and about what looks.
import os
import signal
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


print("---- what is gone through changes on the way")


def grows(n):
    l = [1, 2, 3]
    seen = []
    for x in l:
        seen.append(x)
        if len(l) < n:
            l.append(x * 10)
    return seen


def grows_a_great_deal(n):
    l = [0]
    total = 0
    for x in l:
        total += x
        if x < n:
            l.append(x + 1)
    return total, len(l)


def shrinks():
    l = [1, 2, 3, 4, 5, 6]
    seen = []
    for x in l:
        seen.append(x)
        l.pop()
    return seen, l


def is_emptied():
    l = [1, 2, 3, 4]
    seen = []
    for x in l:
        seen.append(x)
        if x == 2:
            del l[:]
    return seen, l


def is_emptied_and_filled_again():
    l = [1, 2, 3]
    seen = []
    for x in l:
        seen.append(x)
        if x == 3:
            l.clear()
    l.extend([7, 8])
    return seen, l


def has_another_kind_put_in_it():
    l = [1, 2, 3, 4, 5]
    seen = []
    for i, x in enumerate(l):
        pass
    k = 0
    for x in l:
        seen.append(x)
        if k == 0:
            l[2] = 2.5
        if k == 1:
            l[3] = "s"
        if k == 2:
            l[4] = None
        k += 1
    return seen


def has_what_is_next_changed():
    l = [1, 2, 3, 4]
    seen = []
    k = 0
    for x in l:
        seen.append(x)
        k += 1
        if k < len(l):
            l[k] = x * 2
    return seen


def is_put_in_order():
    l = [3, 1, 2]
    seen = []
    for x in l:
        seen.append(x)
        l.sort()
    return seen, l


def has_one_put_in_front():
    l = [1, 2]
    seen = []
    for x in l:
        seen.append(x)
        if len(l) < 5:
            l.insert(0, 0)
    return seen, l


show("it grows", grows, 6)
show("a great deal", grows_a_great_deal, 300)
show("it shrinks", shrinks)
show("it is emptied", is_emptied)
show("and filled again afterwards", is_emptied_and_filled_again)
show("something of another kind is put in it", has_another_kind_put_in_it)
show("what is next is changed", has_what_is_next_changed)
show("it is put in order", is_put_in_order)
show("one is put in front", has_one_put_in_front)

print("---- what goes through it is used elsewhere too")


def taken_from_inside(n):
    it = iter(range(n))
    seen = []
    for x in it:
        seen.append((x, next(it, "no more")))
    return seen


def two_loops(n):
    it = iter(list(range(n)))
    first = []
    for x in it:
        first.append(x)
        if x == 2:
            break
    return first, [x for x in it], [x for x in it]


def has_run_out():
    l = [1, 2]
    it = iter(l)
    first = [x for x in it]
    l.append(3)
    return first, [x for x in it], l


def one_inside_another(n):
    it = iter(tuple(range(n)))
    seen = []
    for x in it:
        for y in it:
            seen.append((x, y))
            if y % 3 == 0:
                break
    return seen


def how_far_it_has_got(n):
    it = iter(list(range(n)))
    hints = []
    for x in it:
        hints.append(it.__length_hint__())
    return hints, it.__length_hint__()


show("taken from inside the loop", taken_from_inside, 7)
show("two loops", two_loops, 6)
show("one that has run out", has_run_out)
show("one loop inside another", one_inside_another, 10)
show("how far it has got", how_far_it_has_got, 4)

print("---- numbers that get big")


def past_what_fits(start, n):
    seen = []
    for i in range(start, start + n):
        seen.append(i)
    return seen, [type(i).__name__ for i in seen]


def by_steps(start, stop, step):
    return [i for i in range(start, stop, step)]


show("upward", past_what_fits, 2 ** 31 - 3, 6)
show("downward", by_steps, -2 ** 31 + 2, -2 ** 31 - 4, -1)
show("in big steps", by_steps, 0, 2 ** 33, 2 ** 31 - 1)
show("nothing at all", by_steps, 5, 5, 1)
show("the wrong way", by_steps, 5, 0, 1)

print("---- given something else to go through")


class Looks:
    "What goes through it is looked at each time"
    def __init__(self, n):
        self.n = n
        self.seen = []

    def __iter__(self):
        return self

    def __next__(self):
        variables = sys._getframe(1).f_locals
        self.seen.append((variables.get("total"), variables.get("x")))
        if not self.n:
            raise StopIteration
        self.n -= 1
        return self.n


class Changes(Looks):
    def __next__(self):
        sys._getframe(1).f_locals["total"] = 1000
        return Looks.__next__(self)


def adds_up(things):
    total = 0
    for x in things:
        total += x
    return total


def generates(n):
    for i in range(n):
        yield i


def surprised():
    results = []
    for again in range(2):
        for i in range(700):
            adds_up(range(5))
        for make in (lambda: [1, 2, 3], lambda: (4, 5), lambda: {6: 0, 7: 0}, lambda: {8, 9}, lambda: generates(4), lambda: zip(), lambda: map(abs, [-1, -2]), lambda: iter([1, 2]), lambda: reversed([1, 2]),
                     lambda: bytes([1, 2]), lambda: {1: 2}.values(), lambda: [0.5, 1.5], lambda: [2 ** 40], lambda: range(2 ** 31 - 1, 2 ** 31 + 1)):
            results.append(adds_up(make()))
        looks = Looks(3)
        results.append((adds_up(looks), looks.seen))
        changes = Changes(2)
        results.append((adds_up(changes), changes.seen))
        for bad in (5, None, "ab"):
            try:
                results.append(adds_up(bad))
            except TypeError as e:
                results.append(str(e))
    return results


print("after nothing but ranges =>", surprised())

print("---- a signal")
handled = []


def notes(number, frame):
    handled.append((frame.f_code.co_name, frame.f_locals.get("i"), frame.f_locals.get("total")))


def changes_it(number, frame):
    frame.f_locals["total"] = -1000


def raises(number, frame):
    raise KeyError("from the handler")


def counts(n, when):
    total = 0
    for i in range(n):
        if i in when:
            signal.raise_signal(signal.SIGUSR1)
        total += 1
    return total


def run_counts(n, when):
    del handled[:]
    return counts(n, when), [(name, i) for name, i, total in handled], all(total in (i, i + 1) for name, i, total in handled)


class E:
    pass


def looks_at_what_need_not_be_made(number, frame):
    e = frame.f_locals.get("e")
    handled.append((type(e).__name__, e.v if e is not None else None))


def makes_one_each_time(n, when):
    total = 0
    for i in range(n):
        e = E()
        e.v = i
        if i == when:
            signal.raise_signal(signal.SIGUSR1)
        total += e.v
    return total


def run_makes(n, when):
    del handled[:]
    return makes_one_each_time(n, when), handled[:]


previous = signal.signal(signal.SIGUSR1, notes)
show("once", run_counts, 30, (27,))
show("time and again", run_counts, 60, tuple(range(5, 60, 5)))
signal.signal(signal.SIGUSR1, changes_it)
show("whose handler changes a variable", lambda: counts(30, (10,)) < 0)
signal.signal(signal.SIGUSR1, raises)
show("whose handler raises", counts, 30, (10,))
signal.signal(signal.SIGUSR1, looks_at_what_need_not_be_made)
show("whose handler looks at what need not have been made", run_makes, 30, 12)
signal.signal(signal.SIGUSR1, notes)
del handled[:]
# Long enough for it to be compiled for the last time while it is going round.
print("in a long loop =>", counts(400000, (399990,)), [(name, i) for name, i, total in handled])

# raise_signal() sees to the signal itself before it comes back. What comes from outside is only put off, and it is for the loop to find that there is something.


def counts_and_is_sent_one(n, when, me):
    total = 0
    for i in range(n):
        if i in when:
            os.kill(me, signal.SIGUSR1)
        total += 1
    return total


def run_sent(n, when):
    del handled[:]
    return counts_and_is_sent_one(n, when, os.getpid()), [(name, i) for name, i, total in handled], all(total in (i, i + 1) for name, i, total in handled)


show("sent from outside", run_sent, 30, (27,))
show("time and again", run_sent, 60, tuple(range(5, 60, 5)))
signal.signal(signal.SIGUSR1, changes_it)
show("whose handler changes a variable", lambda: counts_and_is_sent_one(30, (10,), os.getpid()) < 0)
signal.signal(signal.SIGUSR1, raises)
show("whose handler raises", counts_and_is_sent_one, 30, (10,), os.getpid())
signal.signal(signal.SIGUSR1, previous)

# And here nothing is called at all. Nothing but the loop can find it, and it has nothing to go by but what the handler changes.
rung = []


def rings(number, frame):
    rung.append((frame.f_code.co_name, frame.f_locals["i"] == frame.f_locals["last"] or frame.f_locals["i"] == frame.f_locals["last"] + 1))


def waits_for_it():
    last = -1
    for i in range(2 ** 31 - 1):
        if rung:
            return True
        last = i
    return False


def waits_with_while():
    i = last = 0
    while not rung:
        last = i
        i += 1
    return True


previous = signal.signal(signal.SIGALRM, rings)
for waits in (waits_for_it, waits_with_while):
    outcomes = set()
    for again in range(40):
        del rung[:]
        signal.setitimer(signal.ITIMER_REAL, 0.002)
        outcomes.add((waits(), tuple(rung)))
    print("a loop that calls nothing is interrupted =>", waits.__name__, sorted(outcomes))

# There is no way out of these but for the handler to raise. So what is in the loop is sure to be come to, which is when a compiler may think of doing it once and for all beforehand.


def stops_it(number, frame):
    raise KeyError(frame.f_code.co_name)


def goes_round_for_ever():
    while True:
        pass


def counts_for_ever():
    n = 0
    while True:
        n += 1


def goes_round_inside_for_ever():
    while True:
        for i in range(3):
            pass


signal.signal(signal.SIGALRM, stops_it)
for spins in (goes_round_for_ever, counts_for_ever, goes_round_inside_for_ever):
    outcomes = set()
    for again in range(40):
        signal.setitimer(signal.ITIMER_REAL, 0.002)
        try:
            spins()
        except KeyError as e:
            outcomes.add(str(e))
    print("a loop that there is no way out of is interrupted =>", sorted(outcomes))
signal.setitimer(signal.ITIMER_REAL, 0)
signal.signal(signal.SIGALRM, previous)

print("---- what has been found out is still so")


class P:
    def __init__(self, x):
        self.x = x


def attribute_changed_in_the_loop(n):
    p = P(1)
    total = 0
    for i in range(n):
        total += p.x
        if i == n // 2:
            p.x = 100
    return total


def attribute_of_another_kind(n):
    p = P(1)
    seen = []
    for i in range(n):
        seen.append(p.x)
        if i == 1:
            p.x = 0.5
        if i == 2:
            p.x = "s"
    return seen


def another_attribute_in_the_loop(n):
    p = P(1)
    total = 0
    for i in range(n):
        total += p.x
        if i == n - 3:
            p.y = 5
        if i == n - 2:
            total += p.y
    return total, sorted(vars(p))


def another_object_in_the_loop(n):
    p = P(1)
    q = P(10)
    q.extra = 0
    total = 0
    for i in range(n):
        total += p.x
        if i == n // 2:
            p = q
    return total


def list_changed_in_the_loop(n):
    l = [1, 2, 3]
    total = 0
    for i in range(n):
        total += l[0] + len(l)
        if i == n // 2:
            l.insert(0, 50)
        if i == n - 2:
            l[0] = 0.25
    return total


show("an attribute is changed", attribute_changed_in_the_loop, 20)
show("to another kind", attribute_of_another_kind, 5)
show("there is another attribute", another_attribute_in_the_loop, 20)
show("it is another object", another_object_in_the_loop, 20)
show("a list is changed", list_changed_in_the_loop, 20)
