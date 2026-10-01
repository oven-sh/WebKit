# What the mro() of a metaclass can get up to while it is being asked.
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
    print(label, "->", r)
class Named(type):
    def __repr__(cls): return cls.__name__
log = []
def case(label, body):
    log.clear()
    attempt(label, body)
    print("   ", log)

def first():
    class M(Named):
        def mro(cls):
            log.append((cls.__name__, cls.__mro__, cls.__bases__))
            return type.mro(cls)
    class A(metaclass=M): pass
    class B(A): pass
    return A.__mro__, B.__mro__
case("it has no order while it is first asked", first)

def extend():
    class M(Named):
        def mro(cls):
            if cls.__mro__ is None and cls.__name__ != "X":
                try:
                    class X(cls): pass
                except TypeError as e:
                    log.append(str(e))
            return type.mro(cls)
    class A(metaclass=M): pass
    return A.__mro__
case("nothing can be derived from it yet", extend)

def sup():
    class M(Named):
        def mro(cls):
            if cls.__mro__ is None:
                try:
                    super(cls, cls).xxx
                except AttributeError as e:
                    log.append(str(e))
                log.append(isinstance(cls, M))
                log.append(issubclass(cls, object))
                log.append(cls.__subclasses__())
            return type.mro(cls)
    class A(metaclass=M): pass
    return A.__mro__
case("super() of it", sup)

def bases_on_self():
    steps = [1]
    class M(Named):
        def mro(cls):
            if steps:
                steps.pop()
                log.append(cls.__mro__)
                cls.__bases__ += ()
            return type.mro(cls)
    class A(metaclass=M): pass
    return A.__mro__, A.__bases__
case("its bases are set meanwhile", bases_on_self)

def error_path():
    class E(Exception): pass
    state = {"ready": False}
    class M(Named):
        def mro(cls):
            if state["ready"] and cls.__name__ == "C":
                if C.__bases__ == (B2,):
                    state["ready"] = False
                else:
                    C.__bases__ = (B2,)
                    raise E
            return type.mro(cls)
    class A(metaclass=M): pass
    class B1(A): pass
    class B2(A): pass
    class C(A): pass
    state["ready"] = True
    try:
        C.__bases__ = (B1,)
    except E:
        log.append("E")
    r1 = C.__bases__, C.__mro__
    B1.__bases__ = (C,)
    return r1, C.__bases__, C.__mro__, B1.__mro__
case("what was set meanwhile is not undone", error_path)

def plain_error():
    class E(Exception): pass
    state = {"fail": False}
    class M(Named):
        def mro(cls):
            if state["fail"] and cls.__name__ == "D": raise E
            return type.mro(cls)
    class A(metaclass=M): pass
    class B(metaclass=M): pass
    class C(A): pass
    class D(C): pass
    state["fail"] = True
    try:
        C.__bases__ = (B,)
    except E:
        log.append("E")
    return C.__bases__, C.__mro__, D.__mro__, A.__subclasses__(), B.__subclasses__()
case("what fails further down undoes it all", plain_error)

for label, result in (("empty", lambda cls: ()), ("no class", lambda cls: (cls, 1)), ("not iterable", lambda cls: 5), ("without itself", lambda cls: (object,)), ("unsuitable", lambda cls: (cls, int, object)), ("a list", lambda cls: [cls, object]), ("twice", lambda cls: (cls, cls, object))):
    def odd():
        class M(Named):
            def mro(cls): return result(cls)
        class A(metaclass=M): pass
        return A.__mro__
    case(label, odd)
def gone():
    class B: pass
    class M(Named):
        def mro(cls):
            del M.mro
            return (B,)
    class A(metaclass=M): pass
case("it takes itself away", gone)
