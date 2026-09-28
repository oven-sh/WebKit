import builtins
globals().update({name: value for name, value in vars(builtins).items() if not name.startswith("_")})
def names(c): return [k.__name__ for k in c.__mro__]
def type_name(o): return type(o).__name__
def catch(f, *classes):
    try:
        f()
        return "nothing raised"
    except classes as e:
        return ["caught", type(e).__name__, str(e), names(type(e))]
def raise_it(C, *args): raise C(*args)
def match_it(o, C):
    match o:
        case C(): return "matched"
        case _: return "no"
class AppError(Exception):
    def __init__(self, message, code=0):
        super().__init__(message)
        self.code = code
    def describe(self): return f"{type(self).__name__}[{self.code}]: {self}"
class Shape:
    __slots__ = ("w",)
    def __init__(self, w): self.w = w
