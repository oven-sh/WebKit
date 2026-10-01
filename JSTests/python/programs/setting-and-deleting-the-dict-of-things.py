# Setting and deleting the __dict__ of things.
import types, functools, collections, io
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
    print(label, "->", r)
class Base: pass
class E1(Exception, Base): pass
class E2(Base, Exception): pass
class E3(Exception): pass
class M1(types.ModuleType, Base): pass
class M2(Base, types.ModuleType): pass
class P1(functools.partial): pass
class D1(dict): pass
class S1(io.StringIO): pass
class Slots: __slots__ = ("__dict__",)
class T(type): pass
def f(): pass
things = {"object subclass": Base, "Exception": Exception, "ValueError": ValueError, "E1": E1, "E2": E2, "E3": E3, "module": lambda: types.ModuleType("m"), "M1": lambda: M1("m"), "M2": lambda: M2("m"),
          "function": lambda: (lambda: 0), "partial": lambda: functools.partial(f), "P1": lambda: P1(f), "dict subclass": D1, "StringIO": io.StringIO, "S1": S1,
          "slots": Slots, "class": lambda: type("C", (), {}), "metaclass instance": lambda: T("C", (), {}), "SimpleNamespace": types.SimpleNamespace, "deque subclass": lambda: type("Q", (collections.deque,), {})(),
          "int subclass": lambda: type("I", (int,), {})(), "method": lambda: Base().__init__, "generator": lambda: (i for i in ()), "property subclass": lambda: type("P", (property,), {})()}
for label, make in things.items():
    print("=====", label)
    attempt("get", lambda: type(make().__dict__).__name__)
    attempt("set", lambda: (lambda o: (setattr(o, "__dict__", {"a": 1}), o.a)[1])(make()))
    attempt("set to a dict subclass", lambda: (lambda o: (setattr(o, "__dict__", D1(a=1)), type(o.__dict__).__name__, o.a)[1:])(make()))
    attempt("set to a proxy", lambda: setattr(make(), "__dict__", types.MappingProxyType({})))
    attempt("set to None", lambda: setattr(make(), "__dict__", None))
    attempt("set to 5", lambda: setattr(make(), "__dict__", 5))
    attempt("delete", lambda: (lambda o: (delattr(o, "__dict__"), o.__dict__)[1])(make()))
    attempt("delete twice", lambda: (lambda o: (delattr(o, "__dict__"), delattr(o, "__dict__")))(make()))
    attempt("delete, having set something", lambda: (lambda o: (setattr(o, "z", 1), delattr(o, "__dict__"), hasattr(o, "z"))[2])(make()))
    attempt("where it is defined", lambda: [c.__name__ for c in type(make()).__mro__ if "__dict__" in vars(c)])
