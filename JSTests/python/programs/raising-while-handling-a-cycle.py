# Something is raised while an exception is being handled whose __context__ leads back to itself, which a program can arrange. What is raised is put at the head of the chain, which is first gone along to see that it is not in
# it already, and that has to come to an end.
def chain(e, most=8):
    names = []
    while e is not None and len(names) < most:
        names.append(str(e))
        e = e.__context__
    return names


def ring(count, tail=0):
    "So many that go round, and so many that lead to them"
    ring = [ValueError("r%d" % i) for i in range(count)]
    for i, e in enumerate(ring):
        e.__context__ = ring[(i + 1) % count]
    head = ring[0]
    for i in range(tail):
        e = ValueError("t%d" % i)
        e.__context__ = head
        head = e
    return head, ring


for count in (1, 2, 3, 4, 5, 8):
    for tail in (0, 1, 2, 3, 5):
        head, members = ring(count, tail)
        try:
            raise head
        except ValueError:
            try:
                raise KeyError("new")
            except KeyError as e:
                by_statement = chain(e)
            try:
                1 / 0
            except ZeroDivisionError as e:
                by_the_engine = chain(e)
            try:
                {}["missing"]
            except KeyError as e:
                by_a_method = chain(e)
        print(count, tail, by_statement, by_the_engine[1:] == by_statement[1:], by_a_method[1:] == by_statement[1:])

print("---- one that is in it is raised again")
for count in (2, 3, 4):
    for which in range(count):
        head, members = ring(count, 1)
        try:
            raise head
        except ValueError:
            try:
                raise members[which]
            except ValueError as e:
                print(count, which, chain(e))

print("---- and shown")
import traceback
head, members = ring(2)
try:
    raise head
except ValueError as e:
    print(traceback.format_exception(e)[-1], len(traceback.format_exception(e)))
