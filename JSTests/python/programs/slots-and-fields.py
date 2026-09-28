class A:
    __slots__ = ("x", "y")
class B(A):
    __slots__ = ("x", "z")
b = B()
b.x = "B's"
A.x.__set__(b, "A's")
print(b.x, A.x.__get__(b), B.x.__get__(b))
del b.x
print(A.x.__get__(b))
try:
    b.x
except AttributeError as e:
    print(e)
try:
    del b.x
except AttributeError as e:
    print(e)
try:
    b.w = 1
except AttributeError as e:
    print(e)
print(hasattr(b, "__dict__"), A.x, type(A.x).__name__, A.x.__name__, A.x.__objclass__ is A)
class C(A): pass
c = C(); c.x = 1; c.q = 2
print(c.__dict__, vars(c), c.x)
e = StopIteration(5); print(e.value, e.__dict__, vars(OSError(2, "m", "f")), OSError(2, "m", "f").filename)
e.value = 7; print(e.value, e.args)
s = SyntaxError("m", ("f", 1, 2, "t", 3, 4)); print(s.msg, s.filename, s.lineno, s.offset, s.text, s.end_lineno, s.end_offset, s.__dict__)
i = ImportError("m", name="n", path="p"); print(i.msg, i.name, i.path, i.name_from)
try:
    None.zzz
except AttributeError as ae:
    print(ae.name, ae.obj)
