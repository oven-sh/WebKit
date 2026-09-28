def f(x): return x + 1
def g(x): return x + 100
def h(x): return x + 10000
def swap(target, source): target.__code__ = source.__code__
class C:
    def m(self, x): return x + 1
c = C()
def other_method(self, x): return x + 1000
def make(n):
    def closure(x): return x + n
    return closure
Function = type(f)
Cell = type(make(0).__closure__[0])
def build(code, *contents): return Function(code, {}, None, None, tuple(Cell(v) for v in contents))
closure_code = make(0).__code__
