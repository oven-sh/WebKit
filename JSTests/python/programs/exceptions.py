class AppError(Exception):
    def __init__(self, msg, code=1):
        super().__init__(msg)
        self.code = code
class NotFound(AppError): pass
def risky(n):
    if n == 0: raise ValueError("zero")
    if n == 1: raise NotFound("gone", 404)
    if n == 2: return 1 / 0
    if n == 3: return [][1]
    if n == 4: raise KeyError
    return "ok"
for i in range(6):
    try:
        r = risky(i)
    except (ValueError, ZeroDivisionError) as e: print(i, "value/zero", type(e).__name__, e, e.args)
    except AppError as e: print(i, "app", type(e).__name__, e, e.code, e.args, isinstance(e, Exception))
    except LookupError as e: print(i, "lookup", type(e).__name__, repr(e), str(e))
    else: print(i, "else", r)
    finally: print(i, "finally")
def order():
    try:
        print("try"); return "from try"
    finally:
        print("cleanup")
print(order())
# It is warned of, when it is compiled.
import _warnings
_warnings.filters.insert(0, ("ignore", None, SyntaxWarning, None, 0))
exec("def override():\n    try: return 1\n    finally: return 2")
del _warnings.filters[0]
print(override())
def nested():
    try:
        try: raise ValueError("inner")
        except ValueError as e:
            print("caught", e); raise TypeError("outer") from e
    except TypeError as e: print("then", e, repr(e.__cause__))
nested()
def reraise():
    try:
        try: raise KeyError("k")
        except KeyError: print("again"); raise
    except KeyError as e: print("outer got", e)
reraise()
for i in range(3):
    try:
        if i == 1: continue
        if i == 2: break
        print("body", i)
    finally: print("fin", i)
try: raise AppError("m")
except Exception as e: print(repr(e), str(e), e.code, type(e).__mro__[1].__name__)
try: raise Exception("a", 2)
except Exception as e: print(e, e.args, repr(e))
try: raise Exception
except Exception as e: print(repr(e), str(e) == "", e.args)
try: assert 1 == 2, "math"
except AssertionError as e: print("assert", e)
try: assert False
except AssertionError as e: print("assert", repr(e))
try: raise StopIteration(5)
except StopIteration as e: print(e.args)
print(issubclass(KeyError, LookupError), issubclass(ZeroDivisionError, ArithmeticError), issubclass(NotFound, Exception), issubclass(RecursionError, RuntimeError), issubclass(KeyboardInterrupt, Exception), Exception.__name__)
def cleanup_on_error(log):
    try:
        log.append("start"); raise RuntimeError("boom")
    except RuntimeError: log.append("handled"); return log
    finally: log.append("done")
print(cleanup_on_error([]))
e = "kept"
try: raise ValueError
except ValueError as err: pass
print(e)
try:
    try: raise ValueError("first")
    finally: print("finally runs")
except ValueError as x: print("after", x)
try: raise 42
except TypeError as x: print(x)
try: int("z")
except ValueError: print("builtin raised")
def gen_err():
    yield 1
    raise ValueError("in gen")
try:
    for v in gen_err(): print(v)
except ValueError as x: print("from gen", x)
