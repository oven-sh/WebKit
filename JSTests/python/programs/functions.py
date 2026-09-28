def defaults(a, b=1, c=None): return (a, b, c)
print(defaults(0), defaults(0, 2), defaults(0, None, 3), defaults(0, c=5), defaults(a=1), defaults(*[1, 2]), defaults(**{"a": 9, "c": 8}), defaults(1, *(2,), **{"c": 3}))
def mutable(x, acc=[]):
    acc.append(x); return acc
print(mutable(1), mutable(2), mutable(3, []))
def varargs(*a, **k): return a, sorted(k.items())
print(varargs(), varargs(1, 2), varargs(x=1), varargs(*"ab", **{"y": 2}), varargs(*range(3)))
def kwonly(a, *, b, c=3): return a, b, c
print(kwonly(1, b=2), kwonly(1, c=4, b=0))
def counter():
    n = 0
    def inc(by=1):
        nonlocal n
        n += by
        return n
    return inc
c1, c2 = counter(), counter(); print(c1(), c1(5), c2(), c1())
total = 0
def bump():
    global total
    total += 1
bump(); bump(); print(total)
fs = [lambda x, i=i: x + i for i in range(3)]; print([f(10) for f in fs], [(lambda: i)() for i in range(3)])
late = [lambda: i for i in range(3)]; print([f() for f in late])
def deco(f):
    def wrapper(*a, **k):
        return ("wrapped", f(*a, **k))
    return wrapper
@deco
def add(a, b): return a + b
print(add(1, 2), add(a=1, b=2), add.__name__)
def twice(n):
    def d(f): return lambda *a: [f(*a)] * n
    return d
@twice(2)
def hi(x): return "hi " + x
print(hi("a"))
def rec(n): return 1 if n == 0 else n * rec(n - 1)
print(rec(20), rec(30), (lambda f: f(f, 5))(lambda f, n: 1 if n < 2 else n * f(f, n - 1)))
def noreturn(): pass
print(noreturn(), noreturn() is None, callable(noreturn), callable(1), noreturn.__name__, (lambda: 0).__name__)
print(sorted([3, 1, 2], key=lambda v: -v), list(map(defaults, [1, 2])), max([1, 5, 3], key=lambda v: v % 5), (lambda *a: a)(1, 2), (lambda **k: k)(z=1))
def walrus(data):
    if (n := len(data)) > 2: return n
    return -n
print(walrus([1, 2, 3]), walrus([1]), [y for x in range(5) if (y := x * 2) > 4])
def shadow(list): return list
print(shadow(3), list((1, 2)))
x = 10
def reads_global(): return x
def local_x():
    x = 5
    return x
print(reads_global(), local_x(), x)
for bad in (lambda: defaults(), lambda: defaults(1, 2, 3, 4), lambda: defaults(1, a=2), lambda: defaults(1, z=2), lambda: kwonly(1), lambda: kwonly(1, 2), lambda: noreturn(1), lambda: (1)(), lambda: undefined_name, lambda: None.x, lambda: rec("a")):
    try: bad()
    except Exception as e: print(type(e).__name__, e)
def deep(n): return deep(n + 1)
try: deep(0)
except RecursionError as e: print("RecursionError")
