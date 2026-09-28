import builtins
globals().update({name: value for name, value in vars(builtins).items() if isinstance(value, type)})

class AppError(Exception):
    def __init__(self, code, detail):
        super().__init__(code, detail)
        self.code = code
    def __str__(self):
        return f"[{self.code}] {self.args[1]}"
class BadType(TypeError): pass
class Both(AppError, ValueError):
    def __init__(self): super().__init__(1, "both")

def raiser(kind, *args):
    raise kind(*args)
def inner():
    return {}["missing"]
def outer():
    return inner()
def chained():
    try:
        inner()
    except KeyError as e:
        raise RuntimeError("wrapped") from e
def missing_attribute():
    return None.nothing
def recurse():
    return recurse()
def call(f, *args):
    return f(*args)
def catch(f):
    try:
        f()
    except BaseException as e:
        return [type(e).__name__, str(e), [c.__name__ for c in type(e).__mro__][:3], isinstance(e, Exception)]
    return "nothing raised"
def catch_as(kind, f):
    try:
        f()
    except kind as e:
        return "caught " + type(e).__name__
def describe(e):
    return [type(e).__name__, repr(e), e.args, sorted(vars(e)), hasattr(e, "stack"), hasattr(e, "message"), e.__traceback__ is not None]
def reraise(e):
    raise e
def function_attributes(f):
    return [hasattr(f, "name"), hasattr(f, "length"), hasattr(f, "prototype"), f.__name__, vars(f)]
def sample(): pass
