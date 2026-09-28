import sys
def show(label, f):
    try:
        print(label, "=>", f())
    except BaseException as e:
        print(label, "!!", type(e).__name__, e)

B = 10**30
M = sys.maxsize
class Idx:
    def __init__(self, v): self.v = v
    def __index__(self): return self.v
class MyInt(int): pass
class Eq5:
    def __eq__(self, o): return o == 5
    __hash__ = None

ranges = {
    "small": range(2, 20, 3), "empty": range(5, 5), "back": range(20, 2, -3), "one": range(7, 8), "big start": range(B, B + 10, 3), "big len": range(0, B), "big step": range(0, B * 10, B),
    "big back": range(B, -B, -B // 3), "big empty": range(B, 0), "edge": range(-M - 1, M), "edge2": range(M - 2, M + 3), "edge back": range(M, -M - 2, -M), "min step": range(M, -M - 1, -M - 1), "neg": range(-B, -B + 4),
}
for name, r in ranges.items():
    show(name, lambda: (repr(r), r.start, r.stop, r.step, bool(r)))
    show(name + " len", lambda: len(r))
    show(name + " ends", lambda: (r[0], r[-1]))
    show(name + " first", lambda: [x for _, x in zip(range(4), r)])
    show(name + " reversed", lambda: [x for _, x in zip(range(4), reversed(r))])
    show(name + " types", lambda: (type(iter(r)).__name__, type(reversed(r)).__name__))
    show(name + " hint", lambda: (iter(r).__length_hint__(), reversed(r).__length_hint__()))
    show(name + " slices", lambda: (r[1:], r[:-1], r[::2], r[::-1], r[1:3], r[-2:], r[5:2], r[B:], r[:-B], r[::B], r[::-B]))
    show(name + " hash", lambda: hash(r) == hash(range(r.start, r.stop, r.step)))
    show(name + " reduce", lambda: r.__reduce__())
    show(name + " out", lambda: r[len(r) if len(r) < M else M])
show("big index", lambda: (range(0, B)[B - 1], range(0, B)[-B], range(0, B, 7)[10**20]))
show("big index out", lambda: range(0, B)[B])
show("big index out neg", lambda: range(0, B)[-B - 1])
show("index types", lambda: (range(10)[Idx(3)], range(10)[True], range(10)[MyInt(4)], range(0, B)[Idx(B - 2)]))
show("index float", lambda: range(10)[1.0])
show("index str", lambda: range(10)["1"])
show("contains", lambda: (5 in range(2, 20, 3), 6 in range(2, 20, 3), B + 3 in range(B, B + 10, 3), B + 4 in range(B, B + 10, 3), B in range(10), -B in range(-B, 0), B * 3 in range(0, B * 10, B), True in range(3), 5.0 in range(10), 5.5 in range(10), MyInt(5) in range(10), Eq5() in range(10), "a" in range(3), None in range(3)))
show("contains edge", lambda: (M - 1 in range(-M - 1, M), -M - 1 in range(-M - 1, M), M in range(-M - 1, M), 0 in range(-M - 1, M, 2), -1 in range(-M - 1, M, 2), 0 in range(M, -M - 2, -M), 1 in range(M, -M - 2, -M)))
show("index()", lambda: (range(2, 20, 3).index(8), range(B, B + 10, 3).index(B + 6), range(0, B).index(B - 1), range(20, 2, -3).index(14), range(10).index(True), range(10).index(5.0), range(10).index(Eq5()), range(2**40, 2**41).index(2**40 + 5)))
show("index() missing", lambda: range(10).index(10))
show("index() missing big", lambda: range(10).index(B))
show("index() missing float", lambda: range(10).index(5.5))
show("index() no arguments", lambda: range(10).index())
show("count()", lambda: (range(10).count(3), range(10).count(30), range(0, B).count(B - 1), range(10).count(3.0), range(10).count("x"), range(10).count(Eq5()), range(10).count(True)))
show("equal", lambda: (range(0) == range(5, 5), range(1, 2) == range(1, 5, 10), range(0, 10, 2) == range(0, 9, 2), range(0, 10, 2) == range(0, 11, 2), range(B, B + 1) == range(B, B + 5, 9), range(0, B) == range(0, B), range(0, B) != range(0, B + 1), range(3) == [0, 1, 2], range(0, B * 3, B) == range(0, B * 3 - 1, B)))
show("ordering", lambda: range(3) < range(4))
show("hash equal", lambda: (hash(range(0)) == hash(range(5, 5)), hash(range(1, 2)) == hash(range(1, 5, 10)), hash(range(0, 10, 2)) == hash(range(0, 9, 2)), hash(range(3)), hash(range(0)), hash(range(1, 2))))
show("arguments", lambda: (range(True, 5), range(Idx(2), Idx(B)), range(MyInt(1), MyInt(3)), type(range(MyInt(1), 3).start).__name__, type(range(True).stop).__name__))
show("float argument", lambda: range(1.0))
show("zero step", lambda: range(1, 2, 0))
show("no arguments", lambda: range())
show("too many", lambda: range(1, 2, 3, 4))
show("keywords", lambda: range(stop=3))
show("read only", lambda: setattr(range(3), "start", 1))
show("sum and list", lambda: (sum(range(B, B + 5)), list(range(B, B - 3, -1)), max(range(0, B * 4, B)), sorted(range(B + 2, B, -1)), tuple(range(-B, -B + 2))))
show("unpack", lambda: (lambda a, b, c: (a, b, c))(*range(B, B + 3)))
show("iterator ends", lambda: (it := iter(range(B, B + 2)), next(it), next(it), next(it, "done"), next(it, "done"), it.__length_hint__())[1:])
show("small iterator ends", lambda: (it := iter(range(2)), next(it), next(it), next(it, "done"), next(it, "done"), it.__length_hint__())[1:])
show("edge iteration", lambda: (list(range(M - 2, M + 1)), list(range(-M + 1, -M - 2, -1)), list(range(M - 1, M)), list(range(M, M - 3, -1)), list(range(M - 5, M, 3)), list(range(-M - 1, -M + 5, 4))))
show("len of huge", lambda: len(range(-M - 1, M)))
show("len at most", lambda: (len(range(M)), len(range(-M, 0)), len(range(0, M * 2, 2))))
show("enumerate", lambda: list(enumerate(range(B, B + 2), B)))
for s, n in [(slice(None), 10), (slice(1, None, 2), 10), (slice(None, None, -1), 10), (slice(-3, None), 10), (slice(B, None), 10), (slice(None, -B), 10), (slice(None, None, B), 10), (slice(None, None, -B), 10), (slice(None), B), (slice(-5, None), B),
             (slice(B // 2, None, B // 4), B), (slice(None, None, -1), B), (slice(Idx(1), Idx(5), Idx(2)), 10), (slice(True, 5), 10), (slice(None), 0), (slice(5, 2), 10), (slice(2, 5, -1), 10), (slice(None), True), (slice(None), Idx(4))]:
    show(f"indices {[getattr(x, "v", x) for x in (s.start, s.stop, s.step, n)]}", lambda: s.indices(n))
show("indices negative", lambda: slice(None).indices(-1))
show("indices zero step", lambda: slice(None, None, 0).indices(5))
show("indices float", lambda: slice(1.0).indices(5))
show("indices float length", lambda: slice(1).indices(5.0))
show("indices arguments", lambda: slice(1).indices())
show("for loop", lambda: [i for i in range(3)] + [i for i in range(B, B + 2)])
class H:
    def __init__(self, v): self.v = v
    def __hash__(self): return self.v
show("__hash__ as given", lambda: [hash(H(v)) for v in (0, 1, -1, -2, 2**61 - 1, 2**61, 2**62, -2**62, 2**63 - 1, -2**63, 2**63, -2**63 - 1, 2**64, 10**30, True)])
show("enumerate over the edge", lambda: list(enumerate("abc", M - 1)))
show("enumerate negative", lambda: list(enumerate("ab", -B)))
show("enumerate float", lambda: enumerate("ab", 1.0))
