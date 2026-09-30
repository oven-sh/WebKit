# instance.method(...) makes no bound method in CPython if the class finds attributes as object does. One that has __getattr__() or __getattribute__() does not. It can be told of a method that is written in C, which says what it
# is called by the class that it was got by way of.
import collections


def say(label, f):
    try:
        print("%-44s %r" % (label, f()))
    except TypeError as e:
        print("%-44s %s" % (label, e))


asked = []


class Plain(collections.deque):
    pass


class WithGetattr(collections.deque):
    def __getattr__(self, name):
        asked.append(name)
        raise AttributeError(name)

    def mine(self, a):
        return a


class Further(WithGetattr):
    pass


class WithGetattribute(collections.deque):
    def __getattribute__(self, name):
        return super().__getattribute__(name)


for c in (collections.deque, Plain, WithGetattr, Further, WithGetattribute):
    x = c()
    say(c.__name__ + ": called at once", lambda: x.rotate(n=1))
    f = x.rotate
    say(c.__name__ + ": got first", lambda: f(n=1))
    say(c.__name__ + ": too many", lambda: x.rotate(1, 2, 3))
    say(c.__name__ + ": none wanted", lambda: x.clear(1))
    say(c.__name__ + ": one wanted", lambda: x.append())
    say(c.__name__ + ": of the class", lambda: c.rotate(x, n=1))
    say(c.__name__ + ": as it should be", lambda: (x.append(1), x.appendleft(2), x.rotate(1), list(x), x.count(1)))
say("one of its own", lambda: WithGetattr().mine())
say("one of its own, further on", lambda: Further().mine(1, 2))
print(asked)

# Over and over, so that what is found is remembered.
x = WithGetattr()
total = 0
for i in range(20000):
    x.append(i)
    total += x.mine(i) + x.pop() + len(x)
print(total, asked)
for i in range(3):
    say("after that", lambda: x.append())
    say("and of its own", lambda: x.mine())

# It comes to have one, and to have none.
y = Plain()
for i in range(20000):
    y.append(i)
    y.pop()
say("before", lambda: y.append())
Plain.__getattr__ = lambda self, name: asked.append(name) or 5
say("with", lambda: y.append())
say("what there is not", lambda: y.nothing)
for i in range(20000):
    y.append(i)
    y.pop()
say("with, still", lambda: y.append())
del Plain.__getattr__
say("without", lambda: y.append())
print(asked)

# What the instance has by the name comes first, as ever.
z = WithGetattr()
z.append = lambda *a: "the instance's"
say("the instance's own", lambda: z.append())
del z.append
say("and without it", lambda: z.append())
