# Some classes that CPython writes in C can be added to all the same, and most cannot.
import ast
import os
import time
import typing


def attempt(f, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)


for c in (ast.AST, typing.Generic, time.struct_time, os.stat_result, os.statvfs_result, os.terminal_size, os.times_result, os.uname_result, os.DirEntry, int, tuple, dict, type(os.scandir), type(iter([])), type(time.get_clock_info("time")), BaseException, type):
    print(c.__name__, bool(c.__flags__ & (1 << 8)), bool(c.__flags__ & (1 << 9)), attempt(setattr, c, "added", 1), attempt(getattr, c, "added"), attempt(delattr, c, "added"), attempt(getattr, c, "added"), attempt(type.__setattr__, c, "added", 2), attempt(type.__delattr__, c, "added"), attempt(delattr, c, "added"))
    print("   ", c.__module__, c.__name__, c.__qualname__, repr(c), attempt(setattr, c, "__name__", c.__name__), c.__module__, c.__name__, c.__qualname__, repr(c), "__module__" in vars(c))
    print("   ", attempt(setattr, c, "__doc__", c.__doc__), attempt(setattr, c, "__name__", c.__name__), attempt(setattr, c, "__qualname__", c.__qualname__), attempt(setattr, c, "__module__", c.__module__), attempt(delattr, c, "__name__"), attempt(setattr, c, "__type_params__", ()), attempt(setattr, c, "__annotations__", {}), attempt(delattr, c, "__annotations__"))

print(attempt(setattr, time.struct_time, "__name__", "renamed"), time.struct_time.__name__, time.struct_time.__module__, repr(time.struct_time), repr(time.struct_time(range(9)))[:20], attempt(setattr, time.struct_time, "__module__", "elsewhere"), repr(time.struct_time), attempt(time.struct_time, 5))
time.struct_time.__name__ = "struct_time"
time.struct_time.__module__ = "time"

# What is added is heard, whatever might have been done without asking
S = time.struct_time
x = S(range(9))
for name, function, use in (("__getitem__", lambda self, i: ("got", i), lambda: x[0]), ("__len__", lambda self: 99, lambda: len(x)), ("__iter__", lambda self: iter("ab"), lambda: list(x)), ("__contains__", lambda self, v: "yes", lambda: 500 in x), ("__eq__", lambda self, o: "equal", lambda: x == 5), ("__add__", lambda self, o: "added", lambda: x + (1,)), ("__repr__", lambda self: "shown", lambda: repr(x)), ("__hash__", lambda self: 7, lambda: hash(x)), ("__bool__", lambda self: False, lambda: bool(x))):
    before = attempt(use)
    had = name in vars(S)
    old = vars(S).get(name)
    setattr(S, name, function)
    during = attempt(use)
    if had:
        setattr(S, name, old)
    else:
        delattr(S, name)
    print(name, before, during, attempt(use))
S.extra = property(lambda self: self.tm_year + 1)
print(x.extra, attempt(setattr, x, "extra", 1))
del S.extra
print(attempt(getattr, x, "extra"))
