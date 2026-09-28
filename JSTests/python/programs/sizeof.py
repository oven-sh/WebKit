def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)
show("int", lambda: [(n).__sizeof__() for n in (0, 1, -1, 2**30 - 1, 2**30, -2**30, 2**60 - 1, 2**60, 2**90, -2**300, 10**100, True, False)])
show("str", lambda: [s.__sizeof__() for s in ("", "a", "abc", "x" * 1000, "a" + chr(0xe9), chr(0x20ac), "a" + chr(0x20ac), chr(0x1F600), "ab" + chr(0x1F600))])
show("list", lambda: [l.__sizeof__() for l in ([], [1], list(range(10)), [0] * 100, [None] * 3)])
show("tuple and bytes", lambda: [x.__sizeof__() for x in ((), (1,), (1, 2, 3), b"", b"a", b"abc")])
show("bytearray", lambda: [b.__sizeof__() for b in (bytearray(), bytearray(b"a"), bytearray(b"abc"), bytearray(100))])
def grown(make, add, n):
    c = make(); sizes = []
    for i in range(n): add(c, i); sizes.append(c.__sizeof__())
    return sizes
show("set", lambda: grown(set, lambda s, i: s.add(i), 400))
show("set at once", lambda: [s.__sizeof__() for s in (set(), frozenset(), {1}, frozenset({1, 2, 3, 4}))])
show("dict of str", lambda: grown(dict, lambda d, i: d.__setitem__(str(i), i), 400))
show("dict of int", lambda: grown(dict, lambda d, i: d.__setitem__(i, i), 400))
show("dict big", lambda: [grown(dict, lambda d, i: d.__setitem__(i, i), n)[-1] for n in (1000, 50000, 70000)])
show("dict empty", lambda: ({}.__sizeof__(), dict().__sizeof__()))
class C: pass
class D:
    __slots__ = ("a",)
class L(list): pass
class I(int): pass
class S(str): pass
class M(type): pass
show("type", lambda: [type.__sizeof__(t) for t in (int, object, type, str, C, D, BaseException, L)])
show("instances", lambda: [x.__sizeof__() for x in (C(), D(), object(), None, 1.5, 1j, range(3), slice(1), ...)])
show("derived", lambda: [x.__sizeof__() for x in (L(), L([1, 2]), I(5), I(2**70), S("abc"))])
show("who has", lambda: [t.__name__ for t in (object, int, bool, float, complex, str, bytes, bytearray, list, tuple, dict, set, frozenset, type, range, slice, memoryview, type(None), type(lambda: 0), type(iter([])), type({}.keys()), BaseException) if "__sizeof__" in vars(t)])
show("wrong", lambda: (1).__sizeof__(1))
show("wrong 2", lambda: int.__sizeof__("a"))
show("wrong 3", lambda: list.__sizeof__())
show("kind", lambda: (type(vars(int)["__sizeof__"]).__name__, type([].__sizeof__).__name__, int.__sizeof__.__doc__, list.__sizeof__.__text_signature__))
