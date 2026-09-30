# JavaScript remembers where it found a property, and where it put one. An attribute of something of Python's is a property that its class may come to have something to say about. So each of these is done often enough
# for every kind of compiled code that there is, then something is changed, and then it is done again.
import js

get = js.eval("(o, n) => { let r; for (let i = 0; i < n; i++) r = o.x; return r; }")
put = js.eval("(o, n, v) => { for (let i = 0; i < n; i++) o.x = v; return o.x; }")
add = js.eval("(make, n) => { let o; for (let i = 0; i < n; i++) { o = make(); o.a = i; o.b = i + 1; o.c = 'c'; } return o; }")
both = js.eval("(o, n) => { let s = 0; for (let i = 0; i < n; i++) { o.x = i; s += o.x; } return s; }")
attempt = js.eval("(f, ...a) => { try { return f(...a); } catch (e) { return 'threw ' + String(e); } }")
MANY = 200000

def show(what, *results): print(what, "=>", *results)

def fresh():
    class C:
        def __init__(self): self.x = 1
    return C

def a_property_afterwards():
    C = fresh(); o = C()
    before = get(o, MANY), put(o, MANY, 2), int(both(o, MANY))
    said = []
    C.x = property(lambda self: "the property", lambda self, v: said.append(v))
    return before, get(o, 1), put(o, 1, 3), said, o.__dict__, get(o, MANY), put(o, 3, 4), said
show("a property afterwards", a_property_afterwards())

def a_property_with_no_setter():
    C = fresh(); o = C()
    before = put(o, MANY, 2)
    C.x = property(lambda self: "the property")
    return before, attempt(put, o, 1, 3), o.__dict__
show("a property with no setter", a_property_with_no_setter())

def a_property_in_a_base():
    class B: pass
    class C(B):
        def __init__(self): self.x = 1
    o = C()
    before = get(o, MANY), put(o, MANY, 2)
    B.x = property(lambda self: "the base's")
    return before, get(o, 1), attempt(put, o, 1, 3)
show("a property in a base", a_property_in_a_base())

def setattr_afterwards():
    C = fresh(); o = C()
    before = put(o, MANY, 2)
    said = []
    C.__setattr__ = lambda self, name, v: said.append((name, v))
    after = put(o, 2, 3)
    del C.__setattr__
    return before, after, said, put(o, 1, 4), put(o, MANY, 5)
show("__setattr__ afterwards", setattr_afterwards())

def getattribute_afterwards():
    C = fresh(); o = C()
    before = get(o, MANY)
    C.__getattribute__ = lambda self, name: "asked for " + name
    after = get(o, 1)
    del C.__getattribute__
    return before, after, get(o, 1)
show("__getattribute__ afterwards", getattribute_afterwards())

def what_is_added():
    class C: pass
    o = add(C, MANY)
    before = o.__dict__
    said = []
    C.b = property(lambda self: "the property", lambda self, v: said.append(v))
    p = add(C, 2)
    return before, p.__dict__, p.b, said
show("what is added", what_is_added())

def with_slots():
    class S:
        __slots__ = ("x",)
    o = S()
    return put(o, MANY, 1), get(o, MANY), attempt(js.eval("o => { 'use strict'; o.y = 1; return o.y; }"), o)
show("with slots", with_slots())

def another_dict():
    C = fresh(); o = C(); p = C()
    before = put(o, MANY, 2), get(o, MANY)
    shared = {"x": "shared"}
    o.__dict__ = shared; p.__dict__ = shared
    return before, get(o, 1), put(o, 1, "set by one"), get(p, 1), shared
show("another dict", another_dict())

def another_class():
    class A:
        def __init__(self): self.x = 1
    class B:
        x = property(lambda self: "B's property", lambda self, v: None)
    o = A()
    before = put(o, MANY, 2), get(o, MANY)
    o.__class__ = B
    return before, get(o, 1), put(o, 1, 3), o.__dict__
show("another class", another_class())

def what_kind_of_number():
    C = fresh(); o = C()
    put(o, MANY, 2)
    return [(type(o.x).__name__, o.x) for v in (1, 1.0, 1.5, 2**31, 2**40, -0.0, True, None, "s") if put(o, 1, v) is not Ellipsis]
show("what kind of number", what_kind_of_number())

def by_python_and_by_javascript():
    C = fresh(); o = C()
    def python_puts(o, v): o.x = v; return o.x
    r = []
    for i in range(300): python_puts(o, i)
    r.append((put(o, MANY, "js"), o.x, python_puts(o, "py"), get(o, MANY)))
    C.x = property(lambda self: "the property", lambda self, v: None)
    r.append((python_puts(o, 1), put(o, 1, 2), get(o, 1), o.__dict__))
    return r
show("by both", by_python_and_by_javascript())
