# A code object that marshal has written out and read back says of itself what it said before. That is what is in a .pyc, so what cannot be read back cannot be imported a second time.
import marshal, os, types, warnings
warnings.simplefilter("ignore")
attributes = ("co_argcount", "co_posonlyargcount", "co_kwonlyargcount", "co_nlocals", "co_flags", "co_names", "co_varnames", "co_filename", "co_name", "co_qualname", "co_firstlineno", "co_freevars", "co_cellvars")
def shown(x):
    "What is in a frozenset is in no particular order."
    return sorted(map(shown, x), key=repr) if isinstance(x, frozenset) else tuple(map(shown, x)) if isinstance(x, tuple) else repr(x)
def difference(a, b, path=""):
    for n in attributes:
        if getattr(a, n) != getattr(b, n): return "%s: %s %r != %r" % (path, n, getattr(a, n), getattr(b, n))
    if len(a.co_consts) != len(b.co_consts): return "%s: %d constants, %d" % (path, len(a.co_consts), len(b.co_consts))
    if list(a.co_lines()) != list(b.co_lines()): return "%s: co_lines()" % path
    if list(a.co_positions()) != list(b.co_positions()): return "%s: co_positions()" % path
    for x, y in zip(a.co_consts, b.co_consts):
        if isinstance(x, types.CodeType):
            r = difference(x, y, path + "/" + x.co_name)
            if r: return r
        elif type(x) is not type(y) or shown(x) != shown(y): return "%s: constant %r != %r" % (path, x, y)
def back(code):
    try: return difference(code, marshal.loads(marshal.dumps(code)))
    except Exception as e: return "%s: %s" % (type(e).__name__, e)

print("===== which variables are cells, and in what order")
def parameters_first(cls, segments, style=None, post_style=None):
    result = segments
    if style:
        apply = style.__add__
        result = (cls(t, apply(s)) for t, s in result)
    if post_style:
        result = (cls(t, s + post_style) for t, s in result)
    return result
def one_of_each(z, a):
    b = y = 1
    return lambda: (a, b, y, z)
def only_parameters(c, b, a): return lambda: (a, b, c)
def only_the_rest(x):
    c = b = a = 1
    return lambda: (a, b, c)
def star(*zs, **ks):
    a = 1
    return lambda: (a, zs, ks)
def keyword_only(p, *, z, m=1):
    a = n = 1
    return lambda: (a, m, n, p, z)
def positional_only(z, y, /, x):
    a = 1
    return lambda: (a, x, y, z)
def not_all(z, y, x):
    a = w = 1
    return lambda: (a, y)
def deeper(z):
    a = 1
    def middle(y):
        b = 1
        return lambda: (a, b, y, z)
    return middle
def set_again(z):
    a = 1
    z = 2
    def inner():
        nonlocal z, a
        z = a = 3
    return inner
class Methods:
    def uses_super(self, zed):
        able = 1
        return lambda: (super().__init__, zed, able, self)
    def names_the_class(zed):
        able = 1
        return lambda: (__class__, zed, able)
def comprehensions(zed, items):
    able = 1
    return [lambda: (zed, able, i) for i in items], {k: (lambda: able) for k in items}, (zed for _ in items)
async def coroutine(zed):
    able = 1
    return lambda: (able, zed)
def generator(zed):
    able = 1
    yield lambda: (able, zed)
def generic[T, A](zed: T, able: A):
    first = 1
    return lambda: (zed, able, first, T, A)
def all_code(c):
    yield c
    for k in c.co_consts:
        if isinstance(k, types.CodeType): yield from all_code(k)
for f in (parameters_first, one_of_each, only_parameters, only_the_rest, star, keyword_only, positional_only, not_all, deeper, set_again, Methods.uses_super, Methods.names_the_class, comprehensions, coroutine, generator, generic):
    for c in all_code(f.__code__):
        print(c.co_qualname, c.co_varnames, c.co_cellvars, c.co_freevars, c.co_nlocals, back(c))
print("===== the closure is in the order of co_freevars")
for f in (one_of_each(1, 2), keyword_only(1, z=2), deeper(1)(2), Methods().uses_super(1)):
    print(f.__code__.co_freevars, [type(c.cell_contents).__name__ for c in f.__closure__])
print("===== the library")
library = os.path.dirname(os.__file__)
count, bad = 0, []
for name in sorted(os.listdir(library)):
    if not name.endswith(".py") or name.startswith("_sysconfig"): continue
    with open(os.path.join(library, name), "rb") as file: source = file.read()
    count += 1
    r = back(compile(source, name, "exec"))
    if r: bad.append((name, r))
print(count > 150, bad)
