# What is done with a list, a tuple, a deque, a dict or a set that runs something of the program's, when that changes the list: makes it shorter, makes it longer, empties it, or puts other things in it.
import collections
import itertools
import operator


def attempt(f, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)


def short(v):
    r = ascii(v)
    return r if len(r) < 150 else r[:150] + "... %d" % len(r)


ACTS = {
    "empties it": lambda l: l.clear(),
    "takes one off the end": lambda l: l and l.pop(),
    "takes one off the front": lambda l: l and l.pop(0),
    "adds one, up to a point": lambda l: len(l) < 12 and l.append(E(l, 100 + len(l))),
    "puts one in front, up to a point": lambda l: len(l) < 12 and l.insert(0, E(l, 100 + len(l))),
    "puts other things there": lambda l: l.__setitem__(slice(None), [None] * len(l)),
    "makes it very long": lambda l: len(l) < 5000 and l.extend([None] * 5000),
    "leaves one": lambda l: l.__delitem__(slice(1, None)),
}
act = None
log = []


class E:
    "An element that changes the list that it is in whenever it is asked anything"
    def __init__(self, victim, v):
        self.victim, self.v = victim, v

    def touch(self, what):
        log.append((what, self.v))
        if len(log) < 60:
            act(self.victim)

    def __eq__(self, other):
        self.touch("eq")
        return isinstance(other, E) and self.v == other.v

    def __lt__(self, other):
        self.touch("lt")
        return self.v < other.v

    def __gt__(self, other):
        self.touch("gt")
        return self.v > other.v

    def __hash__(self):
        self.touch("hash")
        return hash(self.v)

    def __repr__(self):
        self.touch("repr")
        return "E%d" % self.v

    def __str__(self):
        self.touch("str")
        return "e%d" % self.v

    def __bool__(self):
        self.touch("bool")
        return self.v % 2 == 1

    def __index__(self):
        self.touch("index")
        return self.v

    def __add__(self, other):
        self.touch("add")
        return self.v + (other.v if isinstance(other, E) else other)

    __radd__ = __add__

    def __iter__(self):
        self.touch("iter")
        return iter((self.v, self.v))

    def __len__(self):
        self.touch("len")
        return self.v

    def __call__(self, *a):
        self.touch("call")
        return self.v


def fresh(n=6):
    l = []
    l[:] = [E(l, i) for i in range(n)]
    return l


def plain(v):
    "Without asking anything of what is in it"
    if isinstance(v, E):
        return "E%d" % v.v
    if isinstance(v, (list, tuple, set, frozenset, collections.deque)):
        return [plain(x) for x in list.__iter__(v)] if isinstance(v, list) else [plain(x) for x in v]
    if isinstance(v, dict):
        return [(plain(k), plain(x)) for k, x in v.items()]
    return v


OPERATIONS = {
    "index of what is not there": lambda l: l.index(None),
    "index of the last": lambda l: l.index(E(l, 5)),
    "index of one that is added": lambda l: l.index(E(l, 108)),
    "index from 2": lambda l: l.index(None, 2),
    "index from 2 to 4": lambda l: l.index(None, 2, 4),
    "index from -2": lambda l: l.index(None, -2),
    "count": lambda l: l.count(None),
    "count of one": lambda l: l.count(E(l, 3)),
    "remove what is not there": lambda l: l.remove(None),
    "remove the last": lambda l: l.remove(E(l, 5)),
    "in": lambda l: None in l,
    "in, of the last": lambda l: E(l, 5) in l,
    "operator.contains": lambda l: operator.contains(l, None),
    "operator.countOf": lambda l: operator.countOf(l, None),
    "operator.indexOf": lambda l: operator.indexOf(l, None),
    "== a copy": lambda l: l == list(l),
    "a copy ==": lambda l: list(l) == l,
    "!= a copy": lambda l: l != list(l),
    "< a copy": lambda l: l < list(l),
    "== itself": lambda l: l == l,
    "== others as good": lambda l: l == [E(l, i) for i in range(6)],
    "< others as good": lambda l: l < [E(l, i) for i in range(6)],
    ">= others, of which the last is more": lambda l: l >= [E(l, i) for i in range(5)] + [E(l, 9)],
    "repr": lambda l: repr(l),
    "str": lambda l: str(l),
    "format": lambda l: "%s|%r|{}".format(l) % (l, l),
    "an f-string": lambda l: f"{l}{l!r}",
    "sort": lambda l: l.sort(),
    "sorted": lambda l: sorted(l),
    "min": lambda l: min(l),
    "max": lambda l: max(l),
    "min by a key": lambda l: min(l, key=lambda e: e()),
    "sum": lambda l: sum(l),
    "any": lambda l: any(l),
    "all": lambda l: all(l),
    "filter": lambda l: list(filter(None, l)),
    "map": lambda l: list(map(bool, l)),
    "set": lambda l: len(set(l)),
    "frozenset": lambda l: len(frozenset(l)),
    "dict.fromkeys": lambda l: len(dict.fromkeys(l)),
    "dict": lambda l: dict(l),
    "a set comprehension": lambda l: len({e for e in l}),
    "bytes": lambda l: bytes(l),
    "bytearray": lambda l: bytearray(l),
    "bytearray.extend": lambda l: (lambda b: (b.extend(l), b)[1])(bytearray()),
    "join, which wants strs": lambda l: "".join(l),
    "join of their strs": lambda l: "".join(map(str, l)),
    "a loop": lambda l: [bool(e) for e in l],
    "a loop backwards": lambda l: [bool(e) for e in reversed(l)],
    "enumerate": lambda l: [(i, bool(e)) for i, e in enumerate(l)],
    "zip with itself": lambda l: [(bool(a), plain(b)) for a, b in zip(l, l)],
    "unpacking": lambda l: (lambda a, *b: (bool(a), plain(b)))(*l),
    "a call with *": lambda l: (lambda *a: len(a))(*(e for e in l if e or True)),
    "extend by a generator over itself, up to a point": lambda l: l.extend(itertools.islice((e for e in l if bool(e) or True), 20)),
    "slice assignment from a generator over itself": lambda l: l.__setitem__(slice(1, 3), (e for e in l if bool(e) or True)),
    "an index that changes it": lambda l: plain(l[l[2]]),
    "a slice whose ends change it": lambda l: plain(l[l[1]:l[4]]),
    "a slice with a step that changes it": lambda l: plain(l[::l[2]]),
    "setting at an index that changes it": lambda l: l.__setitem__(l[2], "set"),
    "setting a slice whose ends change it": lambda l: l.__setitem__(slice(l[1], l[4]), ["a", "b"]),
    "setting a stepped slice whose step changes it": lambda l: l.__setitem__(slice(None, None, l[2]), ["a", "b", "c"]),
    "deleting at an index that changes it": lambda l: l.__delitem__(l[2]),
    "deleting a slice whose ends change it": lambda l: l.__delitem__(slice(l[1], l[4])),
    "deleting a stepped slice": lambda l: l.__delitem__(slice(None, None, l[2])),
    "pop at an index that changes it": lambda l: plain(l.pop(l[2])),
    "insert at an index that changes it": lambda l: l.insert(l[2], "in"),
    "* by what changes it": lambda l: plain(l * l[2]),
    "*= by what changes it": lambda l: plain(l.__imul__(l[2])),
    "tuple ==": lambda l: tuple(l) == tuple(l),
    "tuple <": lambda l: tuple(l) < tuple(E(l, i) for i in range(6)),
    "tuple.index": lambda l: tuple(l).index(None),
    "tuple.count": lambda l: tuple(l).count(None),
    "in a tuple": lambda l: None in tuple(l),
    "hash of a tuple": lambda l: hash(tuple(l)) == hash(tuple(range(6))),
    "repr of a tuple": lambda l: repr(tuple(l)),
    "deque.index": lambda l: collections.deque(l).index(None),
    "deque.count": lambda l: collections.deque(l).count(None),
    "deque ==": lambda l: collections.deque(l) == collections.deque(l),
    "list == in a list": lambda l: [l] == [list(l)],
    "in a list of lists": lambda l: list(l) in [l],
    "a match statement": lambda l: matched(l),
}


def matched(l):
    match l:
        case [a, b, *rest] if bool(a) or bool(b):
            return plain(rest)
        case [a, *rest]:
            return "second", plain(rest)
        case _:
            return "neither"


for act_name, act in ACTS.items():
    print("----", act_name)
    for name, operation in OPERATIONS.items():
        l = fresh()
        log.clear()
        result = attempt(operation, l)
        asked = log[:]
        act, kept = (lambda victim: None), act
        print(name, "=>", short(plain(result)), "|", short(plain(l)), "|", len(asked), short(asked[:14]))
        act = kept
