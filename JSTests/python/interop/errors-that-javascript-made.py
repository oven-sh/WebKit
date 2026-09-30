# An Error that JavaScript made is an instance of a class of Python's that has never been through its __new__() or its __init__(). Everything that an exception has is tried on each kind of them. What comes of it is
# not looked at: it is that there is something to look at, and that the engine is still there afterwards.
import js, sys, builtins, copy, io, pickle, traceback
MAKERS = {"Error": "new Error('m')", "TypeError": "new TypeError('m')", "RangeError": "new RangeError('m')", "SyntaxError": "new SyntaxError('m')", "ReferenceError": "new ReferenceError('m')", "EvalError": "new EvalError('m')", "URIError": "new URIError('m')",
          "AggregateError": "new AggregateError([new Error('a'), 5], 'm')", "extends Error": "new (class E extends Error {})('m')", "extends TypeError": "new (class E extends TypeError {})('m')", "no message": "new Error()", "with a cause": "new Error('m', { cause: new Error('c') })",
          "thrown by the engine": "(() => { try { null.x; } catch (e) { return e; } })()", "too deep": "(() => { const f = () => f(); try { f(); } catch (e) { return e; } })()", "a syntax error": "(() => { try { eval('('); } catch (e) { return e; } })()", "frozen": "Object.freeze(new Error('m'))",
          "message not a string": "Object.assign(new Error('m'), { message: 5 })", "with args": "Object.assign(new Error('m'), { args: 5 })"}
ARGS = [(), (None,), (1,), ("a",), ({},), ({"a": 1},), ((),), ([],), (None, None), (4,), (Exception,), (lambda e: True,)]
count = 0
for label, source in MAKERS.items():
    x = js.eval(source)
    T = type(x)
    before = count
    names = sorted(set(dir(T)) | set(n for C in vars(builtins).values() if isinstance(C, type) and issubclass(C, BaseException) and isinstance(x, C) for n in dir(C)))
    for name in names:
        if name in ("__class__", "__init_subclass__", "__subclasshook__"): continue
        try: a = getattr(x, name)
        except BaseException as e: a = None
        try: repr(a)
        except BaseException: pass
        if callable(a):
            for args in ARGS:
                y = js.eval(source)
                try: r = getattr(y, name)(*args)
                except BaseException as e: r = e
                try: repr(r); str(r)
                except BaseException: pass
                count += 1
        else:
            for value in (None, 1, "a", (), (1, 2), [], {}, x, ValueError("v")):
                y = js.eval(source)
                try: setattr(y, name, value); repr(getattr(y, name))
                except BaseException: pass
                count += 1
            try: delattr(js.eval(source), name)
            except BaseException: pass
    for f in (copy.copy, copy.deepcopy, pickle.dumps, lambda e: traceback.format_exception(e), lambda e: traceback.format_exception_only(e), str, repr, hash, lambda e: e == e, vars, dir, lambda e: BaseException.__str__(e), lambda e: BaseException.__repr__(e), lambda e: BaseException.__reduce__(e), lambda e: OSError.__reduce__(e), lambda e: OSError.__str__(e),
              lambda e: AttributeError.__reduce__(e), lambda e: ImportError.__reduce__(e), lambda e: SyntaxError.__str__(e), lambda e: KeyError.__str__(e), lambda e: UnicodeDecodeError.__str__(e), lambda e: BaseExceptionGroup.__str__(e), lambda e: BaseExceptionGroup.__repr__(e), lambda e: BaseExceptionGroup.split(e, Exception), lambda e: BaseExceptionGroup.derive(e, []), lambda e: StopIteration.value.__get__(e), lambda e: SystemExit.code.__get__(e)):
        try: repr(f(js.eval(source)))
        except BaseException: pass
        count += 1
    print(label, "=>", T.__name__, count - before, "things tried")
print(count, "in all, and it is still here")
