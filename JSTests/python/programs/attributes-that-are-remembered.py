# Code that is run often remembers where it found an attribute. What it remembers is to be forgotten the moment that it is no longer so. So each of these is done many times over, then something is changed, and then it is
# done again.
import types, sys

def attempt(f, *a):
    try: return f(*a)
    except Exception as e: return "%s: %s" % (type(e).__name__, e)

def often(f, *a):
    for _ in range(300): r = attempt(f, *a)
    return r

def show(what, *results): print(what, "=>", *results)

def get_x(o): return o.x
def set_x(o, v): o.x = v; return o.x
def call_m(o): return o.m()
def get_m(o): return o.m
def del_x(o): del o.x

# ---- What the object itself has, and what the class comes to say about it

def fresh():
    class C:
        def __init__(self): self.x = 1
        def m(self): return "C.m"
    return C

def a_property_afterwards():
    C = fresh(); o = C()
    before = often(get_x, o), often(set_x, o, 2)
    C.x = property(lambda self: "the property", lambda self, v: self.__dict__.__setitem__("was_set", v))
    return before, get_x(o), set_x(o, 3), o.__dict__
show("a property afterwards", a_property_afterwards())

def a_property_with_no_setter():
    C = fresh(); o = C()
    before = often(set_x, o, 2)
    C.x = property(lambda self: "the property")
    return before, attempt(set_x, o, 3), attempt(del_x, o), o.__dict__
show("a property with no setter", a_property_with_no_setter())

def a_property_in_a_base():
    class B: pass
    class C(B):
        def __init__(self): self.x = 1
    o = C()
    before = often(get_x, o), often(set_x, o, 2)
    B.x = property(lambda self: "the base's")
    return before, get_x(o), attempt(set_x, o, 3)
show("a property in a base", a_property_in_a_base())

def what_is_no_data_descriptor():
    C = fresh(); o = C()
    before = often(get_x, o)
    class Getter:
        def __get__(self, instance, owner): return "got"
    C.x = Getter()
    after = get_x(o)
    del o.x
    return before, after, get_x(o)
show("what only has __get__ comes after", what_is_no_data_descriptor())

def the_property_taken_away():
    class C:
        x = property(lambda self: "the property")
    o = C(); o.__dict__["x"] = "its own"
    before = often(get_x, o)
    del C.x
    return before, get_x(o), often(get_x, o)
show("the property taken away", the_property_taken_away())

def getattribute_afterwards():
    C = fresh(); o = C()
    before = often(get_x, o), often(call_m, o)
    C.__getattribute__ = lambda self, name: (lambda: "called " + name) if name == "m" else "asked for " + name
    after = get_x(o), call_m(o)
    del C.__getattribute__
    return before, after, get_x(o), call_m(o)
show("__getattribute__ afterwards", getattribute_afterwards())

def setattr_afterwards():
    C = fresh(); o = C()
    before = often(set_x, o, 2)
    said = []
    C.__setattr__ = lambda self, name, v: said.append((name, v))
    after = set_x(o, 3)
    del C.__setattr__
    return before, after, said, set_x(o, 4)
show("__setattr__ afterwards", setattr_afterwards())

def getattr_for_what_is_missing():
    class C:
        def __getattr__(self, name): return "made up " + name
    o = C()
    before = often(get_x, o)
    o.x = 1
    mid = often(get_x, o)
    del o.x
    return before, mid, get_x(o)
show("__getattr__", getattr_for_what_is_missing())

# ---- What it has from its class

def a_method_replaced():
    C = fresh(); o = C()
    r = [often(call_m, o)]
    C.m = lambda self: "another"; r.append(call_m(o)); r.append(often(call_m, o))
    C.m = 5; r.append(attempt(call_m, o)); r.append(get_m(o))
    C.m = staticmethod(lambda: "static"); r.append(call_m(o)); r.append(often(call_m, o))
    C.m = classmethod(lambda cls: "class " + cls.__name__); r.append(call_m(o))
    C.m = property(lambda self: lambda: "by a property"); r.append(call_m(o))
    C.m = len; r.append(attempt(call_m, o))
    C.m = lambda self: "a function again"; r.append(call_m(o)); r.append(often(call_m, o))
    del C.m; r.append(attempt(call_m, o))
    return r
show("a method replaced", a_method_replaced())

def a_number_replaced():
    class C: x = 0
    o = C()
    r = [often(get_x, o)]
    for i in range(5):
        C.x += 1
        r.append(get_x(o))
    C.x = "a string"; r.append(get_x(o))
    C.x = lambda self: "a function"; r.append(type(get_x(o)).__name__)
    C.x = 7; r.append(often(get_x, o))
    return r
show("a number replaced", a_number_replaced())

def one_of_its_own_in_front():
    C = fresh(); o = C(); p = C()
    r = [often(call_m, o)]
    o.m = lambda: "its own"
    r += [call_m(o), call_m(p), often(call_m, o)]
    del o.m
    r += [call_m(o)]
    return r
show("one of its own in front", one_of_its_own_in_front())

def in_a_base():
    class A:
        def m(self): return "A.m"
    class B(A): pass
    class C(B): pass
    o = C()
    r = [often(call_m, o)]
    B.m = lambda self: "B.m"; r.append(call_m(o)); r.append(often(call_m, o))
    C.m = lambda self: "C.m"; r.append(call_m(o)); r.append(often(call_m, o))
    del C.m; r.append(call_m(o))
    del B.m; r.append(call_m(o))
    del A.m; r.append(attempt(call_m, o))
    return r
show("in a base", in_a_base())

def with_two_bases():
    class A:
        def m(self): return "A.m"
    class B:
        def m(self): return "B.m"
        x = "B.x"
    class L(A): pass
    class R(B): pass
    class C(L, R): pass
    o = C()
    r = [often(call_m, o), often(get_x, o)]
    del A.m; r.append(call_m(o)); r.append(often(call_m, o))
    L.x = "L.x"; r.append(get_x(o))
    A.m = lambda self: "A.m again"; r.append(call_m(o))
    R.m = lambda self: "R.m"; r.append(call_m(o))
    del A.m; r.append(call_m(o))
    return r
show("with two bases", with_two_bases())

def other_bases():
    class A:
        def m(self): return "A.m"
        x = "A.x"
    class B:
        def m(self): return "B.m"
        x = "B.x"
    class C(A): pass
    o = C()
    r = [often(call_m, o), often(get_x, o)]
    C.__bases__ = (B,)
    r += [call_m(o), get_x(o), often(call_m, o)]
    C.__bases__ = (A, B)
    r += [call_m(o), get_x(o)]
    return r
show("other bases", other_bases())

def another_class():
    class A:
        def m(self): return "A.m"
        x = "A.x"
    class B:
        def m(self): return "B.m"
        x = "B.x"
    o = A()
    r = [often(call_m, o), often(get_x, o)]
    o.__class__ = B
    r += [call_m(o), get_x(o), often(call_m, o)]
    o.x = "its own"
    o.__class__ = A
    r += [call_m(o), get_x(o)]
    return r
show("another class", another_class())

# ---- Where it keeps what it has

def another_dict():
    C = fresh(); o = C(); p = C()
    r = [often(get_x, o), often(set_x, o, 2)]
    o.__dict__ = {"x": "from another dict"}
    r += [get_x(o), set_x(o, 3), o.__dict__, get_x(p)]
    shared = {"x": "shared"}
    o.__dict__ = shared; p.__dict__ = shared
    r += [get_x(o), get_x(p), set_x(o, "set by one"), get_x(p), often(get_x, p)]
    return r
show("another dict", another_dict())

def by_way_of_the_dict():
    C = fresh(); o = C()
    r = [often(get_x, o)]
    d = o.__dict__
    d["x"] = "by way of the dict"; r.append(get_x(o))
    del d["x"]; r.append(attempt(get_x, o))
    d["x"] = "back"; r.append(get_x(o))
    d.clear(); r.append(attempt(get_x, o))
    d.update(x="updated", y=2); r.append(get_x(o))
    vars(o)["m"] = lambda: "in the dict"; r.append(call_m(o))
    d.pop("m"); r.append(call_m(o))
    d[1] = "not a name"; r.append((get_x(o), sorted(map(str, d))))
    return r
show("by way of the dict", by_way_of_the_dict())

def taken_away_and_put_back():
    C = fresh(); o = C()
    r = []
    for i in range(6):
        r.append(often(get_x, o))
        del o.x
        r.append(attempt(get_x, o)[:14])
        o.x = i
    return r
show("taken away and put back", taken_away_and_put_back())

def a_great_many():
    class C: pass
    def fill(o, n):
        o.a = n; o.b = n + 1; o.c = n + 2; o.d = n + 3; o.e = n + 4; o.f = n + 5; o.g = n + 6; o.h = n + 7; o.i = n + 8; o.j = n + 9; o.k = n + 10; o.l = n + 11
        o.n = n + 12; o.o = n + 13; o.p = n + 14; o.q = n + 15; o.r = n + 16; o.s = n + 17; o.t = n + 18; o.u = n + 19; o.v = n + 20; o.w = n + 21; o.x = n + 22
        return o
    made = [fill(C(), i) for i in range(400)]
    return sum(o.a + o.l + o.x for o in made), made[399].__dict__ == {k: 399 + i for i, k in enumerate("abcdefghijklnopqrstuvwx")}, list(made[7].__dict__) == list("abcdefghijklnopqrstuvwx")
show("a great many", a_great_many())

def in_another_order():
    class C: pass
    def one_way(o): o.a = 1; o.b = 2; return o
    def the_other(o): o.b = 3; o.a = 4; return o
    def both(o): return o.a, o.b
    r = []
    for i in range(400):
        o = (one_way if i % 3 else the_other)(C())
        r.append(both(o) + tuple(o.__dict__))
    return sorted(set(r))
show("in another order", in_another_order())

def with_slots():
    class S:
        __slots__ = ("x", "y")
        def m(self): return "S.m"
    o = S()
    r = [attempt(get_x, o)[:14], often(set_x, o, 1), often(get_x, o), often(call_m, o)]
    del o.x
    r += [attempt(get_x, o)[:14], attempt(lambda: setattr(o, "z", 1))]
    def set_z(o): o.z = 1
    r.append(often(set_z, o))
    return r
show("with slots", with_slots())

# ---- Many kinds at one place

def many_kinds(n):
    classes = []
    for i in range(n):
        class C:
            k = i
            def __init__(self): self.x = self.k * 10
            def m(self, i=i): return "m of %d" % i
        classes.append(C)
    objects = [C() for C in classes]
    for _ in range(100):
        r = [(get_x(o), call_m(o)) for o in objects]
    classes[n // 2].m = lambda self: "replaced"
    classes[0].x = property(lambda self: "a property")
    return r == [(i * 10, "m of %d" % i) for i in range(n)], [(get_x(o), call_m(o)) for o in objects][:n // 2 + 1:max(1, n // 2)]
for n in (1, 2, 3, 8, 9, 40): show("kinds: %d" % n, many_kinds(n))

def all_sorts():
    class C:
        x = "C's"
        def m(self): return "C.m"
    class D(dict):
        x = "D's"
        def m(self): return "D.m"
    class L(list):
        def m(self): return "L.m"
    class T(tuple):
        x = "T's"
    class E(Exception):
        def m(self): return "E.m"
    module = types.ModuleType("made"); module.x = "the module's"; module.m = lambda: "the module's m"
    things = [C(), D(), L(), T(), E(), module, C, 5, "s", None, [], {}, (), 1.5, len, get_x, types.SimpleNamespace(x="a namespace's"), object()]
    for _ in range(60):
        r = [(attempt(get_x, o), attempt(call_m, o)) for o in things]
    return r
for line in all_sorts(): show("of all sorts", line)

# ---- What is built in

def upper(s): return s.upper()
def append(l, v): l.append(v); return l
def get(d, k): return d.get(k, "nothing")
def count(t, v): return t.count(v)
def bit_length(n): return n.bit_length()
class Loud(str):
    def upper(self): return "LOUD"
class Deaf(list):
    def append(self, v): list.append(self, "not " + str(v))
class Sure(dict):
    def get(self, k, d=None): return "sure"
show("built in", often(upper, "abc"), upper(Loud("abc")), often(upper, Loud("x")), upper("def"), attempt(upper, 5), attempt(upper, b"bytes"))
show("built in", often(append, [], 1)[:2], append(Deaf(), 1), append([1.5], 2), append(["a"], None), attempt(append, (), 1), append([1, 2, 3], 4))
show("built in", often(get, {1: 2}, 1), get(Sure(), 1), get({}, 1), attempt(get, [], 1))
show("built in", often(count, (1, 1, 2), 1), count([1, 1, 1], 1), count("aXa", "a"), often(bit_length, 255), bit_length(True), bit_length(2**70), attempt(bit_length, 1.5))

# ---- A module

made = types.ModuleType("made")
made.value = 1
made.function = lambda: "the first"
def module_value(): return made.value
def module_call(): return made.function()
r = [often(module_value), often(module_call)]
made.value = 2; made.function = lambda: "the second"
r += [module_value(), module_call()]
del made.value
r.append(attempt(module_value))
made.__getattr__ = lambda name: "made up " + name
r += [module_value(), often(module_value)]
made.value = 3
r += [module_value()]
made.__dict__["function"] = lambda: "by way of the dict"
r += [module_call()]
class Loudly(types.ModuleType):
    @property
    def value(self): return "a property of the module's class"
made.__class__ = Loudly
r += [module_value(), often(module_value)]
show("a module", r)

# ---- What is said when there is none, and where

def where(f, *a):
    try: f(*a)
    except Exception as e:
        t = e.__traceback__
        while t.tb_next: t = t.tb_next
        return type(e).__name__, str(e), t.tb_frame.f_code.co_name, t.tb_lineno - t.tb_frame.f_code.co_firstlineno, getattr(e, "name", None), type(getattr(e, "obj", None)).__name__
def several(o):
    a = o.x
    b = o.m()
    c = o.nothing
    return a, b, c
def sets_several(o):
    o.x = 1
    o.y = 2
    o.z = 3
C = fresh()
class Two:
    __slots__ = ("x", "y")
for _ in range(300): r = where(several, C()), where(several, 5), where(sets_several, Two()), where(sets_several, 5), where(call_m, types.SimpleNamespace(m=5))
for line in r: show("what is said", line)

# ---- Classes that come and go

def come_and_go():
    total = 0
    for i in range(3000):
        class C:
            def __init__(self, v): self.x = v
            def m(self): return self.x + 1
        o = C(i)
        total += get_x(o) + call_m(o) + set_x(o, 1)
    return total
show("classes that come and go", come_and_go())
