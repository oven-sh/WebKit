# JSError is the class that an Error of JavaScript's has in Python, if it is of no kind that Python has a class for. Calling it makes one, and it is JavaScript that makes it, as it is for any class of JavaScript's.
import js


def attempt(f, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)


is_error = js.Function("x", "return x instanceof Error && Object.getPrototypeOf(x) === Error.prototype")
for label, make in (("JSError()", lambda: JSError()), ("JSError('boom')", lambda: JSError("boom")), ("JSError.__new__(JSError, 'boom')", lambda: JSError.__new__(JSError, "boom")), ("js.Error('boom')", lambda: js.Error("boom")), ("JSError(5)", lambda: JSError(5)), ("JSError(None)", lambda: JSError(None)), ("JSError('a', 'b')", lambda: JSError("a", "b"))):
    e = make()
    print(label, "=>", type(e).__name__, repr(e), str(e), e.args, repr(e.message), is_error(e), isinstance(e, Exception), type(e.stack).__name__)

try:
    raise JSError("raised")
except JSError as e:
    print("caught", repr(e), e.__traceback__ is not None)
print(js.Function("f", "try { f() } catch (e) { return [e instanceof Error, e.message].join() }")(lambda: (_ for _ in ()).throw(JSError("thrown"))))

# It is made as nothing else is, so nothing else's __new__() will do for it.
print(attempt(BaseException.__new__, JSError))
print(attempt(Exception.__new__, JSError, "x"))
print(attempt(object.__new__, JSError))
print(attempt(JSError.__new__, Exception))
print(attempt(JSError.__new__))
print(attempt(JSError.__new__, 5))
print(attempt(JSError, message="x"))
print(attempt(type, "P", (JSError,), {}))
print(sorted(vars(JSError)), JSError.__mro__)
