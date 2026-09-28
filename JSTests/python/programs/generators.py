def count(n):
    i = 0
    while i < n:
        yield i
        i += 1
print(list(count(3)), sum(count(5)), [x * 2 for x in count(3)], max(count(4)), tuple(count(2)), 2 in count(3), list(zip(count(3), "ab")), dict(enumerate(count(2))))
g = count(2); print(next(g), next(g), next(g, "done"), next(g, None))
try: next(count(0))
except StopIteration as e: print("StopIteration", e.args)
def echo():
    got = None
    while True:
        got = yield got
        if got == "stop": return "bye"
e = echo(); print(next(e), e.send(1), e.send("x"))
try: e.send("stop")
except StopIteration as s: print("returned", s.value if hasattr(s, "value") else s.args)
def inner():
    x = yield 1
    print("inner got", x)
    yield 2
    return "inner result"
def outer():
    r = yield from inner()
    print("outer got", r)
    yield from [10, 20]
    yield from "ab"
    yield from range(2)
o = outer(); print(next(o), o.send("sent"), list(o))
def fib():
    a, b = 0, 1
    while True:
        yield a
        a, b = b, a + b
def take(n, it):
    for _, v in zip(range(n), it): yield v
print(list(take(10, fib())), list(take(3, (x * x for x in fib()))), list(take(2, take(5, fib()))), list(take(90, fib()))[-1])
def cleanup():
    try:
        yield 1
        yield 2
    finally: print("closed")
c = cleanup(); print(next(c)); c.close(); print(list(c))
def catcher():
    while True:
        try: yield "ready"
        except ValueError as e: print("caught", e)
k = catcher(); print(next(k), k.throw(ValueError("thrown")))
def tree(t):
    if isinstance(t, list):
        for s in t: yield from tree(s)
    else: yield t
print(list(tree([1, [2, [3, 4]], [[5]], 6])))
lazy = (print("evaluated", i) or i for i in range(2)); print("made"); print(list(lazy))
ge = (x for x in range(3)); print(list(ge), list(ge), sum(x for x in range(4)), ",".join(str(x) for x in range(3)), any(x > 1 for x in range(3)), sorted(x % 3 for x in range(5)))
def pipeline(data):
    evens = (x for x in data if x % 2 == 0)
    squares = (x * x for x in evens)
    return sum(squares)
print(pipeline(range(10)), type(count(1)).__name__, iter(g) is g)
def early():
    yield 1
    return
    yield 2
print(list(early()))
def args_gen(*a, **k):
    yield from a
    yield from sorted(k)
print(list(args_gen(1, 2, z=1, y=2)))
class Tree:
    def __init__(self, v, kids=()): self.v, self.kids = v, kids
    def __iter__(self):
        yield self.v
        for k in self.kids: yield from k
print(list(Tree(1, [Tree(2, [Tree(3)]), Tree(4)])), sum(Tree(1, [Tree(2)])))
