def look(x):
    out = []
    for label, f in [("len", lambda: len(x)), ("x[0]", lambda: x[0]), ("x[-1]", lambda: x[-1]), ("list", lambda: list(x)), ("iter", lambda: [v for v in x]), ("repr", lambda: repr(x)), ("tuple", lambda: tuple(x)), ("slice", lambda: x[1:]), ("in", lambda: 2 in x), ("index", lambda: x.index(2)), ("count", lambda: x.count(2)), ("copy", lambda: x.copy()), ("+", lambda: x + [9]), ("*", lambda: x * 2), ("==", lambda: x == [1, 2, 3]), ("reversed", lambda: list(reversed(x))), ("sorted", lambda: sorted(x)), ("unpack", lambda: (lambda a, b, c: (a, b, c))(*x)), ("sum", lambda: sum(x)), ("append", lambda: x.append(4)), ("setitem", lambda: x.__setitem__(0, 9)), ("pop", lambda: x.pop()), ("reverse", lambda: x.reverse()), ("sort", lambda: x.sort()), ("del", lambda: x.__delitem__(0)), ("clear", lambda: x.clear()), ("after", lambda: list(x))]:
        try: out.append(f"{label}={f()!r}")
        except Exception as e: out.append(f"{label}!!{type(e).__name__}: {e}")
    return "; ".join(out)
def first(x):
    return x[0]
def total(x):
    return sum(x)
