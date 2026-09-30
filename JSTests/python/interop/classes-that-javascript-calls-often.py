# JavaScript calls a class of Python's as Python does, and what comes of that is worked out once and gone by from then on. So each of these is called from JavaScript until what calls it has been compiled as far as it
# ever is, then something that it depends on is changed, and it is called some more.
import js

run = js.eval("""(source, C, n) => {
    // Written out afresh, so that this is the only class that it has ever been seen to call.
    const make = new Function("C", "n", "let last; for (let i = 0; i < n; ++i) last = " + source + "; return last;");
    return (n) => { try { return make(C, n); } catch (e) { return String(e); } };
}""")


def describe(x):
    return (type(x).__name__, sorted(vars(x).items())) if hasattr(x, "__dict__") else x


def changed(label, source, make_class, change):
    C = make_class()
    make = run(source, C, 0)
    for k in range(60):
        before = make(5000)
    change(C)
    after = make(3)
    for k in range(60):
        again = make(5000)
    print(label, "=>", describe(before), "THEN", describe(after) if describe(after) == describe(again) else ("AT FIRST", describe(after), "AND THEN", describe(again)))


def own():
    class C:
        def __init__(self, x=0):
            self.x = x
    return C


def derived():
    class Base:
        def __init__(self, x=0):
            self.x = x
    class C(Base):
        pass
    return C


def empty():
    class C:
        pass
    return C


def other(self, x=0):
    self.other = x


for source in ("C(5)", "new C(5)", "C.call(null, 5)", "Reflect.construct(C, [5])"):
    print("----", source)
    changed("as it stands", source, own, lambda C: None)
    changed("another __init__", source, own, lambda C: setattr(C, "__init__", other))
    changed("another in the base", source, derived, lambda C: setattr(C.__mro__[1], "__init__", other))
    changed("none", source, own, lambda C: delattr(C, "__init__"))
    changed("a __new__", source, own, lambda C: setattr(C, "__new__", lambda cls, *a: "made otherwise"))
    changed("one that returns something", source, own, lambda C: setattr(C, "__init__", lambda self, x: x))
    changed("one that raises", source, own, lambda C: setattr(C, "__init__", lambda self, x: 1 // 0))
print("---- with nothing to be given")
changed("as it stands", "C()", empty, lambda C: None)
changed("an __init__", "C()", empty, lambda C: setattr(C, "__init__", other))
changed("given something all the same", "C(1)", empty, lambda C: setattr(C, "__init__", other))
print("---- of which nothing is kept")
changed("nothing is made of it", "(C(5), i)", own, lambda C: setattr(C, "__init__", lambda self, x: 1 // 0))
changed("only what it has", "C(i).x", own, lambda C: setattr(C, "__init__", other))
