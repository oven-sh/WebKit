# A constant that is an object is one object, however often the code that has it is run, and it is the one that co_consts has.
import _warnings

_warnings.filters.insert(0, ("ignore", None, Warning, None, 0))


def t(label, f):
    try:
        r = f()
    except BaseException as e:
        r = type(e).__name__ + ": " + str(e)
    print(label, "=>", r)


def in_consts(f, value):
    return any(c is value for c in f.__code__.co_consts)


def tuples(): return (1000, 2000)
def nested(): return ((1000, 2000), (1000, 2000), 3000)
def two(): return (1000, 2000), (1000, 2000)
def worked_out(): return (1000,) + (2000,)
def numbers(): return 1j, 1 + 2j, -1j
def dots(): return ...
def slices(o): return o[1:2], o[1:2], o[::2], o[:], o[1000:]
def defaults(a=1000, b=(1, 2)): return a, b
def with_variable(x): return (1000, x)
def empty(): return ()
def lists(): return [1000, 2000, 3000]
def sets(): return {1000, 2000, 3000}
def lists_of_tuples(): return [(1, 2), (1, 2), (3, 4)]
def looked_in(x): return x in {1000, 2000}, x in [1000, 2000], x not in (1000, 2000)
def gone_through(): return [i for i in [1000, 2000, 3000]], [i for i in {1000}]
def generator():
    yield (1000, 2000)
    yield (1000, 2000)
async def coroutine(): return (1000, 2000)
lam = lambda: (1000, 2000)


class Taker:
    def __getitem__(self, key):
        return key


class InClass:
    constant = (1000, 2000)
    def method(self): return (1000, 2000)


t("a tuple", lambda: (tuples() is tuples(), in_consts(tuples, tuples()), tuples()))
t("in another", lambda: (nested() is nested(), nested()[0] is nested()[1], in_consts(nested, nested())))
t("twice", lambda: (two()[0] is two()[1], two() is two()))
t("worked out", lambda: (worked_out() is worked_out(), in_consts(worked_out, worked_out()), worked_out()))
t("complex", lambda: ([a is b for a, b in zip(numbers(), numbers())], numbers() is numbers(), numbers()))
t("Ellipsis", lambda: (dots() is dots(), dots() is Ellipsis))
t("slices", lambda: (lambda a, b: ([x is y for x, y in zip(a, b)], a[0] is a[1], a, in_consts(slices, a[0])))(slices(Taker()), slices(Taker())))
t("defaults", lambda: (defaults.__defaults__ is defaults.__defaults__, defaults()[1] is defaults()[1], defaults.__defaults__))
t("with a variable in it", lambda: (with_variable(1) is with_variable(1), with_variable(1)))
t("nothing", lambda: (empty() is empty(), empty() is tuple()))
t("a list is another each time", lambda: (lists() is lists(), lists() == lists(), lists(), (lambda l: (l.append(1), lists()))(lists())[1]))
t("and a set", lambda: (sets() is sets(), sets() == sets(), sorted(sets()), (lambda s: (s.add(1), sorted(sets())))(sets())[1], type(sets()).__name__))
t("what is in the list is not", lambda: (lists_of_tuples()[0] is lists_of_tuples()[0], lists_of_tuples()[0] is lists_of_tuples()[1], lists_of_tuples()))
t("looked in", lambda: (looked_in(1000), looked_in(3000), looked_in(1000.0), [type(c).__name__ for c in looked_in.__code__.co_consts]))
t("gone through", lambda: (gone_through(), [type(c).__name__ for c in gone_through.__code__.co_consts]))
t("a generator", lambda: (lambda a, b: (a[0] is a[1], a[0] is b[0]))(list(generator()), list(generator())))
t("a lambda", lambda: lam() is lam())
t("a class", lambda: (InClass().method() is InClass().method(), InClass.constant))


def run(c):
    try:
        c.send(None)
    except StopIteration as e:
        return e.value


t("a coroutine", lambda: run(coroutine()) is run(coroutine()))

print("---- what is made afresh has constants of its own")


def maker():
    def made(): return (1000, 2000)
    return made


t("the same code, made twice", lambda: (maker()() is maker()(), maker().__code__ is maker().__code__))
t("compiled twice", lambda: eval(compile("(1000, 2000)", "<s>", "eval")) is eval(compile("(1000, 2000)", "<s>", "eval")))
code = compile("(1000, 2000)", "<s>", "eval")
t("compiled once and run twice", lambda: (eval(code) is eval(code), eval(code) is code.co_consts[0], eval(code, {}) is eval(code, {"a": 1})))
code = compile("x = (1000, 2000)\ndef f(): return (1000, 2000), x", "<s>", "exec")
namespaces = [{}, {}]
for n in namespaces:
    exec(code, n)
t("run in two places", lambda: (namespaces[0]["x"] is namespaces[1]["x"], namespaces[0]["f"]()[0] is namespaces[1]["f"]()[0]))

print("---- many")
source = "def big():\n    return [\n" + "".join("        (%d, %d.5, 'a%d', (%d, None)),\n" % (i, i, i, i % 7) for i in range(3000)) + "    ]\n"
n = {}
exec(source, n)
big = n["big"]
t("three thousand", lambda: (len(big()), big()[2999], big()[5] is big()[5], big()[0][3] is big()[7][3], len(big.__code__.co_consts)))
source = "def wide():\n    return (" + ", ".join(str(i + 1000) for i in range(5000)) + ")\n"
exec(source, n)
t("five thousand in one", lambda: (len(n["wide"]()), n["wide"]() is n["wide"](), sum(n["wide"]()), len(n["wide"].__code__.co_consts)))
source = "def deep():\n    return " + "(" * 60 + "1000" + ",)" * 60 + "\n"
exec(source, n)
t("sixty deep", lambda: (n["deep"]() is n["deep"](), repr(n["deep"]())[:70]))

print("---- bytes")
def some_bytes(): return b"abc", (b"abc", 1)
t("are equal, whether or not they are one", lambda: (some_bytes() == some_bytes(), some_bytes()))

print("---- a list that is written out has what is in it in common with the next, until it is written to")
MAKERS = {
    "ints": lambda: [1, 2, 3], "one": lambda: [1], "two": lambda: [1, 2], "floats": lambda: [1.5, 2.5, 3.5], "whole floats": lambda: [1.0, 2.0, 3.0], "ints and floats": lambda: [1, 1.0, 2, 2.0, -0.0, 0], "strings": lambda: ["a", "b", "c"],
    "of all kinds": lambda: [1, "b", 2.5, None, True, False, -1, -1.5], "worked out": lambda: [1 + 1, 2 * 3, "a" * 2, -(-1), not 1], "large and small": lambda: [2147483647, -2147483648, 0, 255, 256],
    "too large to be plain": lambda: [2147483648, 1, 2], "with a tuple": lambda: [(1, 2), 3, 4], "forty": lambda: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40],
    "in another": lambda: [[1, 2, 3], [1, 2, 3]],
}
CHANGES = {
    "append": lambda l: l.append(9), "extend": lambda l: l.extend([9, 8]), "insert": lambda l: l.insert(0, 9), "insert in the middle": lambda l: l.insert(1, 9), "pop": lambda l: l.pop(), "pop(0)": lambda l: l.pop(0), "remove": lambda l: l.remove(l[-1]),
    "clear": lambda l: l.clear(), "reverse": lambda l: l.reverse(), "sort": lambda l: l.sort(key=repr, reverse=True), "l[0] = 9": lambda l: l.__setitem__(0, 9), "l[-1] = 9": lambda l: l.__setitem__(-1, 9), "l[0] = 's'": lambda l: l.__setitem__(0, "s"),
    "l[0] = 1.5": lambda l: l.__setitem__(0, 1.5), "l[0] = []": lambda l: l.__setitem__(0, []), "del l[0]": lambda l: l.__delitem__(0), "del l[:2]": lambda l: l.__delitem__(slice(0, 2)), "l[:1] = [7, 8, 9]": lambda l: l.__setitem__(slice(0, 1), [7, 8, 9]),
    "l[:1] = [7]": lambda l: l.__setitem__(slice(0, 1), [7]), "del l[::2]": lambda l: l.__delitem__(slice(None, None, 2)), "+=": lambda l: l.__iadd__([9]), "*= 2": lambda l: l.__imul__(2), "*= 0": lambda l: l.__imul__(0), "__init__": lambda l: l.__init__([5]),
    "l[:] = l[::-1]": lambda l: l.__setitem__(slice(None), l[::-1]), "what is in it": lambda l: l[0].append(9) if isinstance(l[0], list) else None,
}
show_all = lambda l: [(type(x).__name__, x) for x in l]
for kind, make in MAKERS.items():
    first = repr(show_all(make()))
    changed = []
    results = 0
    for name, change in CHANGES.items():
        for again in range(3):
            l = make()
            other = make()
            change(l)
            for c in repr(l):
                results = (results * 31 + ord(c)) % 1000000007
            if repr(show_all(make())) != first or repr(show_all(other)) != first:
                changed.append(name)
    print(kind, "=>", first[:150], "| what came of changing them:", results, "| what changed another:", changed)
t("they are not one another", lambda: (MAKERS["ints"]() is MAKERS["ints"](), MAKERS["in another"]()[0] is MAKERS["in another"]()[1]))
