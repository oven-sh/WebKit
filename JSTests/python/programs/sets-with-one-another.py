# What sets do with one another: which of two keys that are equal is kept, what is asked of the keys and how often, and what happens if answering changes the sets.
# The order that a set is in is not the same here as in CPython, so everything is sorted.


# Where there is a great deal, all that is printed is a number that stands for it. This is to see the rest.
EVERYTHING = False
total = [0, 0]


def t(label, f, brief=False):
    try:
        r = f()
    except BaseException as e:
        r = type(e).__name__ + ": " + str(e)[:120]
    if not brief or EVERYTHING:
        print(label, "=>", r)
    for c in label + repr(r):
        total[0] = (total[0] * 31 + ord(c)) % 1000000007
    total[1] += 1


def so_far(label):
    print(label, "=>", total[1], "that come to", total[0])
    total[:] = [0, 0]


def show(s):
    if not isinstance(s, (set, frozenset)):
        return s
    return type(s).__name__, sorted((float(x), type(x).__name__) for x in s)


class MySet(set):
    pass


class MyFrozen(frozenset):
    pass


print("---- which is kept")
lefts = [[1, 2, 3], [1.0, 2.0], [True], [], [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40]]
rights = [[1.0], [1.0, 2.0, 3.0, 4.0], [True, 2.0], [], [3.0, 3, 1.0, 1], [float(i) for i in range(40)], [5.0, 50.0]]
for make_left in (set, frozenset, MySet, MyFrozen):
    for make_right in (set, frozenset, MySet, list, tuple, dict.fromkeys, iter, lambda x: dict.fromkeys(x).keys()):
        for left in lefts:
            for right in rights:
                label = "%s%s %s%s" % (make_left.__name__, left[:4], make_right.__name__, right[:5])
                for name in ("union", "intersection", "difference", "symmetric_difference", "issubset", "issuperset", "isdisjoint"):
                    t(label + " " + name, lambda: show(getattr(make_left(left), name)(make_right(right))), True)
                for name in ("update", "intersection_update", "difference_update", "symmetric_difference_update"):
                    def in_place():
                        s = make_left(left)
                        r = getattr(s, name)(make_right(right))
                        return r, show(s)
                    t(label + " " + name, in_place, True)
                for name, f in (("|", lambda a, b: a | b), ("&", lambda a, b: a & b), ("-", lambda a, b: a - b), ("^", lambda a, b: a ^ b), ("<=", lambda a, b: a <= b), ("<", lambda a, b: a < b), ("==", lambda a, b: a == b)):
                    t(label + " " + name, lambda: show(f(make_left(left), make_right(right))), True)

                def augmented(op):
                    a = original = make_left(left)
                    b = make_right(right)
                    if op == "|": a |= b
                    if op == "&": a &= b
                    if op == "-": a -= b
                    if op == "^": a ^= b
                    return a is original, show(a), show(original)
                for op in "|&-^":
                    t(label + " " + op + "=", lambda: augmented(op), True)
        so_far("%s and %s" % (make_left.__name__, make_right.__name__))

print("---- with itself")
for make in (set, frozenset, MySet):
    for name in ("union", "intersection", "difference", "symmetric_difference", "issubset", "issuperset", "isdisjoint", "update", "intersection_update", "difference_update", "symmetric_difference_update"):
        def with_itself():
            s = make([1, 2, 3])
            r = getattr(s, name)(s)
            return show(r), r is s, show(s)
        t(make.__name__ + " " + name, with_itself)
    t(make.__name__ + " nothing, with itself", lambda: (lambda s: s.isdisjoint(s))(make()))

print("---- with several")
for name in ("union", "intersection", "difference", "update", "intersection_update", "difference_update", "symmetric_difference", "symmetric_difference_update", "issubset", "isdisjoint"):
    for others in ((), ([1.0, 5],), ([1.0, 2.0, 5], frozenset([2])), ([1.0], [2.0], [3.0]), ([1, 2], 5), (5, [1, 2]), ([1, 2], [[]]), ([[]], [1, 2]), ({1: 2}, "ab", range(3))):
        def several():
            s = {1, 2, 3, 4}
            try:
                r = getattr(s, name)(*others)
            except TypeError as e:
                r = "TypeError: " + str(e)
            return show(r), show(s) if all(isinstance(x, (int, float)) for x in s) else sorted(map(repr, s))
        t("%s%r" % (name, others), several)

print("---- what is asked of the keys")
log = []


class Key:
    def __init__(self, value, tag):
        self.value, self.tag = value, tag

    def __hash__(self):
        log.append("hash " + self.tag)
        return hash(self.value)

    def __eq__(self, other):
        log.append("eq " + self.tag + " " + other.tag)
        return self.value == other.value

    def __repr__(self):
        return self.tag


def keys(letter, values):
    return [Key(v, letter + str(v)) for v in values]


def asked(f, left, right, make_right):
    a = set(keys("a", left))
    b = make_right(keys("b", right))
    del log[:]
    r = f(a, b)
    said = sorted(log)
    del log[:]
    # What stops as soon as it knows has asked more or less, as the order that they are in has it.
    if r is False:
        said = None
    return sorted(map(repr, r)) if isinstance(r, (set, frozenset)) else r, sorted(map(repr, a)), said


operations = (("|", lambda a, b: a.union(b)), ("&", lambda a, b: a.intersection(b)), ("-", lambda a, b: a.difference(b)), ("^", lambda a, b: a.symmetric_difference(b)), ("|=", lambda a, b: a.update(b)), ("&=", lambda a, b: a.intersection_update(b)),
              ("-=", lambda a, b: a.difference_update(b)), ("^=", lambda a, b: a.symmetric_difference_update(b)), ("<=", lambda a, b: a.issubset(b)), (">=", lambda a, b: a.issuperset(b)), ("disjoint", lambda a, b: a.isdisjoint(b)))
for make_right in (set, frozenset, list, dict.fromkeys):
    for left, right in (((1, 2, 3), (2, 3, 4)), ((1, 2, 3), (2,)), ((2,), (1, 2, 3)), ((1, 2), (3, 4)), ((), (1, 2)), ((1, 2), ()), (tuple(range(1, 41)), (5, 50)), ((5, 50), tuple(range(1, 41))), ((1, 2, 3), (2, 2, 3, 3))):
        for name, f in operations:
            t("%s %s %s %s" % (left[:4], name, make_right.__name__, right[:4]), lambda: asked(f, left, right, make_right), len(left) + len(right) > 8)
    so_far("asked, with " + make_right.__name__)

print("---- when answering changes them")


class Meddler:
    def __init__(self, value, what):
        self.value, self.what = value, what

    def __hash__(self):
        return hash(self.value)

    def __eq__(self, other):
        self.what()
        return self.value == getattr(other, "value", other)


for name, f in operations:
    for which in ("left", "right", "both"):
        for how in ("clear", "add", "discard"):
            def meddled():
                a, b = set(), set()
                count = [100]

                def what():
                    for s in ((a,) if which == "left" else (b,) if which == "right" else (a, b)):
                        if how == "clear":
                            s.clear()
                        elif how == "add":
                            count[0] += 1
                            s.add(count[0])
                        else:
                            s.discard(3)
                a.update(Meddler(i, what) for i in range(8))
                b.update(Meddler(i, lambda: None) for i in range(4, 12))
                try:
                    f(a, b)
                except RuntimeError as e:
                    return "RuntimeError"
                return "done"
            # What is left depends on the order. It is that it ends, and how, that is looked at.
            t("%s %s %s" % (name, which, how), meddled)

print("---- what cannot be hashed")
for name, f in operations:
    t(name + " a list of lists", lambda: f({1, 2}, [[1]]))
    t(name + " a list of sets", lambda: show(f({1, frozenset([2])}, [{2}])))
    t(name + " a number", lambda: f({1, 2}, 5))

print("---- after a good many have been taken out")
s = set(range(100000))
s.difference_update(range(99990))
print(sorted(s), len(s))
s -= set(range(99995))
print(sorted(s), len(s))
s |= {1, 2}
print(sorted(s), len(s), 1 in s, 99999 in s, 5 in s)
s = set(range(100000))
s &= {5, 6, 100001}
print(sorted(s))
s ^= set(range(10))
print(sorted(s))
