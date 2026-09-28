import sys
events = []
def describe(a):
    if a is None or isinstance(a, (int, str, tuple, list)): return a
    n = getattr(a, "__qualname__", None) or type(a).__name__
    return n
def profiler(frame, event, arg):
    if frame.f_code.co_filename != __file__: return
    events.append((frame.f_code.co_name, event, frame.f_lineno - frame.f_code.co_firstlineno, describe(arg)))
def profile(f, *a):
    events.clear(); sys.setprofile(profiler)
    try:
        try: f(*a)
        except Exception as e: pass
    finally: sys.setprofile(None)
    return [e for e in events if e[0] != "profile"]
def show(f, *a):
    print(f.__name__)
    for e in profile(f, *a): print("   ", *e)
def py(x=0): return x
def python_calls():
    py()
    return py(1)
def c_calls():
    len("ab")
    abs(-1)
    return max(1, 2)
def c_methods():
    l = []
    l.append(1)
    "a".upper()
    {}.get(1)
    return l.pop()
def unbound():
    list.append([], 1)
    str.upper("a")
    dict.get({}, 1)
def classes():
    int("1")
    list()
    dict(a=1)
    C()
    object()
class C:
    def __init__(self): pass
    def m(self): return 1
    def __call__(self): return 2
    def __len__(self): return 3
    def __add__(self, o): return 4
    @staticmethod
    def s(): return 5
    @classmethod
    def k(cls): return 6
    @property
    def p(self): return 7
def methods():
    c = C()
    c.m()
    C.m(c)
    c()
    c.s()
    c.k()
    C.k()
    b = c.m
    b()
def special():
    c = C()
    len(c)
    c + 1
    c.p
def c_raises():
    try: int("x")
    except ValueError: pass
    try: [].pop()
    except IndexError: pass
    try: len(1)
    except TypeError: pass
def c_raises_out():
    int("x")
def nested_c():
    return len(str(abs(-1)))
def c_calls_python():
    list(map(py, [1]))
    sorted([1], key=py)
    return max([1], key=py)
def keywords():
    py(x=1)
    int("1", base=10)
    sorted([1], reverse=True)
    dict(a=1, b=2)
def stars():
    a = (1,); k = {"x": 1}
    py(*a)
    py(**k)
    len(*["ab"])
    max(*[1, 2])
    print(*[], end="")
def no_args():
    dict()
    globals()
    locals()
def gen():
    yield 1
    yield 2
def generators():
    g = gen()
    next(g)
    list(g)
    for x in gen(): pass
def gen_methods():
    g = gen()
    g.send(None)
    g.close()
    g2 = gen()
    try: g2.throw(ValueError)
    except ValueError: pass
def comprehension():
    return [len("a") for _ in range(2)]
def lambdas():
    return (lambda: len("a"))()
def decorators():
    @staticmethod
    def f(): pass
    @py
    def g(): pass
def with_block():
    with M():
        pass
class M:
    def __enter__(self): return self
    def __exit__(self, *a): return False
def builds_class():
    class K: pass
    return K
def imports():
    import sys
def isinstance_etc():
    isinstance(1, int)
    getattr(C, "m")
    hasattr(C, "zz")
    repr(1)
    print("", end="")
def raising_python():
    def r(): raise KeyError
    try: r()
    except KeyError: pass
def in_expressions():
    x = len("a") + len("b")
    y = [len("c"), abs(1)]
    return x if len("d") else y
def chained_methods():
    return " a ".strip().upper().lower()
def super_calls():
    class A:
        def f(self): return 1
    class B(A):
        def f(self): return super().f()
    return B().f()
async def co(): return 1
def coroutines():
    c = co()
    try: c.send(None)
    except StopIteration: pass
for f in (python_calls, c_calls, c_methods, unbound, classes, methods, special, c_raises, c_raises_out, nested_c, c_calls_python, keywords, stars, no_args, generators, gen_methods, comprehension, lambdas, decorators, with_block, builds_class, imports, isinstance_etc, raising_python, in_expressions, chained_methods, super_calls, coroutines):
    show(f)
