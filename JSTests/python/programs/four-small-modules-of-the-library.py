# _stat, _statistics, _sysconfig and _types: four small modules that the library is written over.
import _stat, _statistics, _sysconfig, _types, stat, statistics, types, sys, random, math
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
    print(label, "->", ascii(r))
print("===== _stat")
same_everywhere = [n for n in sorted(vars(_stat)) if not n.startswith("__") and not n.startswith(("SF_", "UF_", "S_IFWHT"))]
for n in same_everywhere:
    v = getattr(_stat, n)
    print(n, v if isinstance(v, int) else type(v).__name__, getattr(stat, n) is v)
print(sorted(n for n in vars(_stat) if n.startswith(("UF_",))))
functions = [n for n in same_everywhere if callable(getattr(_stat, n))]
modes = [0, 0o644, 0o100644, 0o40755, 0o120777, 0o60660, 0o20620, 0o10600, 0o140755, 0o104755, 0o102755, 0o41777, 0o41776, 0o104644, 0o102644, 0o7777, 0o177777, 0o170000, 0o150000, 0o110000, 0o70000, 0o50000, 0o30000, 0o111, 0o222, 0o444, 0o001, 0o010, 0o100, True]
for m in modes:
    print(oct(m), [int(getattr(_stat, n)(m)) if n != "filemode" else _stat.filemode(m) for n in functions])
class Index:
    def __init__(self, v): self.v = v
    def __index__(self): return self.v
for bad in (-1, 2**64, 2**64 - 1, -2**70, 1.5, "1", None, Index(0o40755), Index(-1), Index("x"), b"1", [], 2**100):
    for n in ("S_ISDIR", "S_IMODE", "S_IFMT", "filemode"):
        attempt("%s(%s)" % (n, type(bad).__name__ if not isinstance(bad, int) else bad), lambda: getattr(_stat, n)(bad))
attempt("none", lambda: _stat.S_ISDIR())
attempt("two", lambda: _stat.S_ISDIR(1, 2))
attempt("keyword", lambda: _stat.S_ISDIR(mode=1))
print(_stat.filemode.__doc__, _stat.S_ISDIR.__text_signature__, _stat.__doc__[:40])
print("===== _statistics")
f = _statistics._normal_dist_inv_cdf
print(statistics._normal_dist_inv_cdf is f, f.__text_signature__)
r = random.Random(7)
def small(x):
    # By multiplying, which comes to the same everywhere. What ** comes to is up to the system.
    y = x
    for _ in range(19): y *= x
    return y
ps = [1e-300, 1e-100, 1e-20, 1e-10, 1e-5, 0.001, 0.01, 0.075, 0.0749999, 0.1, 0.25, 0.5, 0.75, 0.9, 0.925, 0.9250001, 0.99, 0.999, 1 - 1e-10, 1 - 1e-16, 5e-324, 0.5000000001, 0.4999999999] + [r.random() for _ in range(300)] + [small(r.random()) for _ in range(100)] + [1 - small(r.random()) for _ in range(100)]
for p in (p for p in ps if 0 < p < 1):
    print(repr(p), repr(f(p, 0.0, 1.0)), repr(f(p, 100.0, 15.0)), repr(f(p, -3.5, 0.001)))
for args in ((0.0, 0, 1), (1.0, 0, 1), (-0.5, 0, 1), (1.5, 0, 1), (math.nan, 0, 1), (math.inf, 0, 1), (0.5, math.nan, 1), (0.5, 0, math.inf), (0.3, math.inf, 1), (0.3, 0, -1), (0.3, 0, 0), ("0.5", 0, 1), (0.5, None, 1), (0.5, 0, []), (True, 0, 1), (Index(0), 0, 1), (0.5, Index(3), Index(2)), (0.5, 10**400, 1), (0.5,), (0.5, 0), (0.5, 0, 1, 2)):
    attempt(repr(args) if not any(isinstance(a, Index) for a in args) else "with an index", lambda: f(*args))
attempt("keyword", lambda: f(p=0.5, mu=0, sigma=1))
attempt("NormalDist", lambda: [statistics.NormalDist(100, 15).inv_cdf(p) for p in (0.1, 0.5, 0.975)])
attempt("quantiles", lambda: statistics.NormalDist(0, 1).quantiles(8))
print("===== _sysconfig")
attempt("config_vars", lambda: _sysconfig.config_vars())
attempt("a new one each time", lambda: _sysconfig.config_vars() is _sysconfig.config_vars())
attempt("an argument", lambda: _sysconfig.config_vars(1))
print(_sysconfig.__doc__, _sysconfig.config_vars.__doc__)
print("===== _types")
print(_types.__doc__)
for n in sorted(vars(_types)):
    if n.startswith("__"): continue
    t = getattr(_types, n)
    print(n, t, getattr(types, n) is t)
def g(): yield
async def c(): pass
async def ag(): yield
co = c(); co.close()
checks = {"AsyncGeneratorType": ag(), "BuiltinFunctionType": len, "BuiltinMethodType": [].append, "CellType": (lambda x: lambda: x)(1).__closure__[0], "ClassMethodDescriptorType": dict.__dict__["fromkeys"], "CodeType": g.__code__, "CoroutineType": co, "EllipsisType": ...,
          "FrameType": sys._getframe(), "FunctionType": g, "GeneratorType": g(), "GenericAlias": list[int], "GetSetDescriptorType": types.FunctionType.__code__, "LambdaType": lambda: 0, "MappingProxyType": type.__dict__, "MemberDescriptorType": types.FunctionType.__globals__,
          "MethodDescriptorType": str.join, "MethodType": Index(1).__index__, "MethodWrapperType": object().__str__, "ModuleType": sys, "NoneType": None, "NotImplementedType": NotImplemented, "SimpleNamespace": types.SimpleNamespace(), "UnionType": int | str, "WrapperDescriptorType": object.__init__}
for n, v in checks.items():
    print(n, type(v) is getattr(_types, n))
try: 1 / 0
except ZeroDivisionError as e: print("TracebackType", type(e.__traceback__) is _types.TracebackType)
print(sorted(set(types.__all__) - set(vars(_types))))
