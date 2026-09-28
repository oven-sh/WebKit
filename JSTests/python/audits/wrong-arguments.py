import builtins
samples = [1, 1.5, True, 1j, "s", b"b", bytearray(b"a"), [1], (1,), {1: 2}, {1}, frozenset({1}), range(3), slice(1), memoryview(b"m"), None, ..., NotImplemented, object(), iter([1]), {}.keys(), {}.values(), {}.items(), enumerate([]), zip(), map(len, []), filter(None, []), reversed([1]),
           property(), staticmethod(len), classmethod(len), ValueError("v"), len, [].append, str.join, type, int, (1).__add__, int.__add__, dict.__dict__["fromkeys"], iter(range(3)), iter("a"), iter({1: 1}), iter({1}), iter((1,)), super(int, 1)]
skip = {"__init_subclass__", "__subclasshook__", "__class__", "__reduce_ex__", "__reduce__", "__getstate__", "__sizeof__", "__format__", "__dir__", "__doc__", "__call__", "__class_getitem__", "__setstate__", "release", "clear", "__del__", "__exit__", "close"}
def attempt(label, f, *a, **k):
    try:
        f(*a, **k)
    except TypeError as e:
        print(label, "|", e)
    except BaseException as e:
        print(label, "|", type(e).__name__)
    else:
        print(label, "| fine")
for o in samples:
    t = type(o)
    for name in sorted(dir(t)):
        if name in skip:
            continue
        try:
            m = getattr(o, name)
        except Exception:
            continue
        if not callable(m) or isinstance(m, type):
            continue
        attempt(f"{t.__name__} {name} 0", m)
        attempt(f"{t.__name__} {name} 9", m, *[None] * 9)
        attempt(f"{t.__name__} {name} kw", m, zzz=1)
        u = getattr(t, name)
        attempt(f"{t.__name__} {name} unbound", u)
        attempt(f"{t.__name__} {name} wrong self", u, Ellipsis if t is not type(...) else 1)
for name in sorted(vars(builtins)):
    f = getattr(builtins, name)
    if name in ("exit", "quit", "input", "breakpoint", "help", "copyright", "credits", "license", "open", "print", "__import__", "__build_class__", "exec", "eval", "compile", "globals", "locals", "vars", "dir") or not callable(f) or (isinstance(f, type) and issubclass(f, BaseException)):
        continue
    attempt(f"builtins {name} 0", f)
    attempt(f"builtins {name} 9", f, *[None] * 9)
    attempt(f"builtins {name} kw", f, zzz=1)
    attempt(f"builtins {name} 1kw", f, None, zzz=1)
