# What calling a class comes to is worked out once and gone by from then on: which __new__ it has, which __init__. So each of these makes instances until whatever does that has been compiled, then changes
# something that it depends on, and makes some more.
import abc
import sys


def outcome(f, *arguments):
    try:
        return f(*arguments)
    except Exception as e:
        return type(e).__name__, str(e)


def describe(x):
    return type(x).__name__, sorted(vars(x).items()) if hasattr(x, "__dict__") else None


def often(make, *arguments):
    "What comes of it the first time, and after a good many"
    def loop(n):
        for i in range(n):
            last = make(*arguments)
        return last
    first = outcome(lambda: describe(make(*arguments)))
    last = outcome(lambda: describe(loop(15000)))
    return first if first == last else ("AT FIRST", first, "AND THEN", last)


print("---- as it stands")


class Empty:
    pass


class Point:
    def __init__(self, x, y=2):
        self.x = x
        self.y = y


class Derived(Point):
    pass


class Further(Derived):
    def __init__(self, x):
        super().__init__(x, -1)
        self.z = x * 2


class Slotted:
    __slots__ = ("a", "b")

    def __init__(self, a):
        self.a = a


print("nothing to it =>", often(lambda: Empty()))
print("object =>", often(lambda: object()))
print("two things =>", often(lambda: Point(1, 5)))
print("one left out =>", often(lambda: Point(1)))
print("by name =>", often(lambda: Point(y=3, x=4)))
print("out of a list =>", often(lambda: Point(*[7, 8])))
print("what a base has =>", often(lambda: Derived(1, 5)))
print("that calls what its base has =>", often(lambda: Further(3)))
print("with places for them =>", often(lambda: Slotted(3)), Slotted(3).a, outcome(lambda: Slotted(3).b))
print("each is another =>", len({id(p) for p in [Point(i) for i in range(1000)]}), all(type(p) is Point and p.x == i for i, p in enumerate([Point(i) for i in range(1000)])))

print("---- given what it cannot be")
print("too many =>", often(lambda: Point(1, 2, 3)))
print("too few =>", often(lambda: Point()))
print("a name that there is not =>", often(lambda: Point(1, z=3)))
print("anything, that takes nothing =>", often(lambda: Empty(1)))
print("or by name =>", often(lambda: Empty(a=1)))
print("object, likewise =>", often(lambda: object(1)))

print("---- and then it is changed")


noted = []


def changed(label, make_class, change, *arguments):
    "Made often, then the change, then made often again, and by what has already been compiled as well as by what has not"
    C = make_class()
    # Written out afresh, so that this is the only class that it has ever been seen to call.
    namespace = {"C": C}
    exec("def loop(n):\n    for i in range(n):\n        last = C(%s)\n    return last" % ", ".join(map(repr, arguments)), namespace)
    loop = namespace["loop"]
    def many():
        for k in range(500):
            last = loop(30)
        return last
    before = outcome(lambda: describe(many()))
    change(C)
    after = outcome(lambda: describe(loop(3)))
    again = outcome(lambda: describe(many()))
    print(label, "=>", before, "THEN", after if after == again else ("AT FIRST", after, "AND THEN", again), sorted(set(noted)))
    del noted[:]


def plain():
    class Base:
        def __init__(self, x=0):
            self.x = x
    class C(Base):
        pass
    return C


def own():
    class C:
        def __init__(self, x=0):
            self.x = x
    return C


def other_init(self, x=0):
    self.other = x


def set_on(name, value, up=0):
    def change(C):
        setattr(C.__mro__[up], name, value)
    return change


def delete_from(name, up=0):
    def change(C):
        delattr(C.__mro__[up], name)
    return change


class Elsewhere:
    def __init__(self, x=0):
        self.elsewhere = x


class Meta(type):
    def __call__(cls, *arguments):
        return "what the class of the class says", arguments


class Quiet(type):
    pass


def with_quiet():
    class C(metaclass=Quiet):
        def __init__(self, x=0):
            self.x = x
    return C


class Callable:
    def __call__(self, *arguments):
        noted.append(("called with", len(arguments)))


class Descriptor:
    def __get__(self, instance, owner):
        return lambda *arguments: setattr(instance, "through", arguments)


had_one = own()
had_one.__new__ = lambda cls, *a: 1
del had_one.__new__

changed("another __init__", own, set_on("__init__", other_init), 5)
changed("another in the base", plain, set_on("__init__", other_init, 1), 5)
changed("one of its own, where it had the base's", plain, set_on("__init__", other_init), 5)
changed("none of its own any more", own, delete_from("__init__"))
changed("nor any to be given anything", own, delete_from("__init__"), 5)
changed("none in the base", plain, delete_from("__init__", 1), 5)
changed("a __new__", own, set_on("__new__", lambda cls, *a: "made otherwise"), 5)
changed("a __new__ in the base", plain, set_on("__new__", lambda cls, *a: "made otherwise", 1), 5)
changed("a __new__ that makes one all the same", own, set_on("__new__", lambda cls, *a: object.__new__(cls)), 5)
changed("a __new__, and then none", own, lambda C: (setattr(C, "__new__", lambda cls, *a: 1), delattr(C, "__new__")), 5)
changed("other bases", plain, lambda C: setattr(C, "__bases__", (Elsewhere,)), 5)
changed("a __new__ that is object's", own, set_on("__new__", object.__new__), 5)
changed("a __new__ in the base, and then none", plain, lambda C: (setattr(C.__mro__[1], "__new__", lambda cls, *a: 1), delattr(C.__mro__[1], "__new__")), 5)
changed("derived from what has had one", lambda: type("Later", (had_one,), {}), lambda C: None, 5)
changed("a class of another class", with_quiet, lambda C: setattr(C, "__class__", Meta), 5)
changed("one that returns something", own, set_on("__init__", lambda self, x=0: x), 5)
changed("one that returns None after all", own, set_on("__init__", lambda self, x=0: None), 5)
changed("one that raises", own, set_on("__init__", lambda self, x=0: 1 // 0), 5)
changed("one that yields", own, set_on("__init__", lambda self, x=0: (yield)), 5)
changed("one that is not a function", own, set_on("__init__", Callable()), 5)
changed("one that says what it is when it is got", own, set_on("__init__", Descriptor()), 5)
changed("a static one", own, set_on("__init__", staticmethod(lambda *a: noted.append(len(a)))), 5)
changed("one for the class", own, set_on("__init__", classmethod(lambda *a: noted.append((a[0].__name__, len(a))))), 5)
changed("one that is built in", own, set_on("__init__", object.__init__))
changed("one that is built in, and something for it", own, set_on("__init__", object.__init__), 5)
changed("one of some other class that is built in", own, set_on("__init__", list.__init__), 5)
changed("something that has nothing to do with it", own, set_on("unrelated", 1), 5)


def abstract():
    class C(abc.ABC):
        def __init__(self, x=0):
            self.x = x
        def method(self): pass
    return C


def make_abstract(C):
    C.method = abc.abstractmethod(C.method)
    abc.update_abstractmethods(C)


def make_concrete(C):
    C.method = lambda self: None
    abc.update_abstractmethods(C)


changed("something left for others to do", abstract, make_abstract, 5)
changed("and then done", abstract, lambda C: (make_abstract(C), make_concrete(C)), 5)


def with_meta():
    class C(metaclass=Meta):
        def __init__(self, x=0):
            self.x = x
    return C


changed("whose class says what calling it is, and then does not", with_meta, lambda C: delattr(Meta, "__call__"), 5)
Meta.__call__ = lambda cls, *arguments: ("said again", arguments)
changed("and says so again", with_meta, lambda C: None, 5)

print("---- from inside __init__")


class Looks:
    def __init__(self, n):
        frame = sys._getframe(1)
        self.caller = frame.f_code.co_name
        self.line = frame.f_lineno - frame.f_code.co_firstlineno
        self.seen = frame.f_locals.get("i")
        self.back = frame.f_back.f_code.co_name


def makes_one_that_looks(n):
    for i in range(n):
        last = Looks(
            n)
    return last


print("what called it =>", [describe(makes_one_that_looks(k)) for k in (3, 700)])


class Pokes:
    def __init__(self):
        sys._getframe(1).f_locals["poked"] = "by __init__"


def is_poked(n):
    poked = 0
    for i in range(n):
        if i == n - 1:
            Pokes()
    return poked


print("what it changes there =>", [is_poked(3)] + [is_poked(30) for k in range(400)][-1:])


class Fails:
    def __init__(self, x):
        local = x * 2
        self.value = 10 // x


def where_it_went(e):
    steps = []
    tb = e.__traceback__
    while tb:
        steps.append((tb.tb_frame.f_code.co_name, tb.tb_lineno - tb.tb_frame.f_code.co_firstlineno, sorted((k, v) for k, v in tb.tb_frame.f_locals.items() if isinstance(v, int))))
        tb = tb.tb_next
    return steps


def makes_one_that_fails(n):
    made = 0
    for i in range(n, -1, -1):
        try:
            Fails(i)
            made += 1
        except ZeroDivisionError as e:
            return made, where_it_went(e)


print("what it raises =>", [makes_one_that_fails(3)] + [makes_one_that_fails(30) for k in range(300)][-1:])


class Changes:
    def __init__(self):
        self.first = True
        if Changes.count == 650:
            Changes.__init__ = lambda self: setattr(self, "second", True)
        Changes.count += 1
    count = 0


print("which changes what it is =>", sorted({tuple(vars(Changes())) for i in range(1300)}), Changes.count)


class Nested:
    def __init__(self, depth):
        self.inner = Nested(depth - 1) if depth else None


def how_deep(x):
    n = 0
    while x:
        x, n = x.inner, n + 1
    return n


print("one in another =>", {how_deep(Nested(20)) for i in range(300)})
limit = sys.getrecursionlimit()
sys.setrecursionlimit(100)
print("too many in one another =>", {outcome(Nested, 200) for i in range(50)}, how_deep(Nested(50)))
sys.setrecursionlimit(limit)
