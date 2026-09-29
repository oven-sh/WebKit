# _asyncio: Future and Task, and which loop and which task are running. The loop here is a list, so that nothing depends on when anything happens.
import _asyncio
import asyncio
import contextvars
import re
import warnings

Future, Task = _asyncio.Future, _asyncio.Task
# When something is found to have been dropped is not what this is about.
warnings.simplefilter("ignore", RuntimeWarning)


def clean(text):
    return re.sub(r"0x[0-9a-f]+", "0x", re.sub(r"(?:/[^ '\"<>]*/|<frozen |\b)([\w.-]+?)(?:\.py)?>?:(\d+)", r"\1:\2", text))


def t(label, f):
    try:
        result = repr(f())
    except BaseException as e:
        result = type(e).__name__ + ": " + str(e)
    print(label, "=>", clean(result))


class Loop:
    def __init__(self, debug=False):
        self.ready, self.debug, self.handled, self.running = [], debug, [], False

    def get_debug(self): return self.debug
    def is_running(self): return self.running
    def call_soon(self, callback, *args, context=None): self.ready.append((callback, args, context))
    def call_exception_handler(self, context): self.handled.append(sorted(context))
    def __repr__(self): return "<Loop>"

    def run(self):
        "Everything that is ready, and what that makes ready, in the context that it was given"
        count = 0
        _asyncio._set_running_loop(self)
        self.running = True
        try:
            while self.ready:
                callback, args, context = self.ready.pop(0)
                (context or contextvars.copy_context()).run(callback, *args)
                count += 1
        finally:
            self.running = False
            _asyncio._set_running_loop(None)
        return count


loop = Loop()
print("---- what there is")
t("the module", lambda: (_asyncio.__name__, _asyncio.__doc__, sorted(n for n in dir(_asyncio) if not n.startswith("__"))))
t("the classes", lambda: [(c.__name__, c.__qualname__, c.__module__, c.__mro__, c.__doc__[:40], c.__flags__ & (1 << 8 | 1 << 9 | 1 << 10)) for c in (Future, Task)])
t("asyncio has them", lambda: (asyncio.Future is Future, asyncio.Task is Task, asyncio.futures._CFuture is Future, asyncio.tasks._CTask is Task, asyncio.isfuture(Future(loop=loop))))
t("generic", lambda: (Future[int], Task[str]))

print("---- one that has not been initialized")
for cls in (Future, Task):
    bare = cls.__new__(cls)
    for name in ("result", "exception", "cancelled", "done", "get_loop", "cancel", "_make_cancelled_error", "__iter__", "__await__", "__repr__", "__del__"):
        t(cls.__name__ + "." + name, lambda: getattr(bare, name)())
    for name in ("set_result", "set_exception", "add_done_callback", "remove_done_callback"):
        t(cls.__name__ + "." + name, lambda: getattr(bare, name)(ValueError))
    for name in sorted(n for c in cls.__mro__ for n, v in vars(c).items() if type(v).__name__ == "getset_descriptor"):
        t(cls.__name__ + "." + name, lambda: getattr(bare, name))
    t(cls.__name__ + " set blocking", lambda: setattr(bare, "_asyncio_future_blocking", True))
# Not get_context(), which CPython crashes on.
for name in ("get_name", "get_coro", "cancelling", "uncancel", "get_stack"):
    t("Task." + name, lambda: getattr(Task.__new__(Task), name)())

print("---- how they are made")
t("Future()", lambda: [attempt for attempt in [Future(loop=loop)]])
for args, kwargs in (((loop,), {}), ((), {"loop": loop, "x": 1}), ((), {"loop": 5}), ((), {"loop": None})):
    t("Future%r%r" % (args, sorted(kwargs)), lambda: Future(*args, **kwargs))
for args, kwargs in (((), {}), ((5,), {"loop": loop}), ((None,), {"loop": loop}), ((lambda: 1,), {"loop": loop}), (((i for i in ()),), {"loop": loop}), ((1, 2), {})):
    t("Task%r" % (len(args),), lambda: Task(*args, **kwargs))

print("---- a future")
f = Future(loop=loop)
t("new", lambda: (f, f._state, f._loop, f.get_loop(), f._callbacks, f._result, f._exception, f._log_traceback, f._source_traceback, f._cancel_message, f._asyncio_future_blocking, f._asyncio_awaited_by, f.done(), f.cancelled()))
t("not done", lambda: f.result())
t("not done", lambda: f.exception())
t("attributes", lambda: (setattr(f, "mine", 1), f.mine, hasattr(f, "__dict__"), __import__("weakref").ref(f)() is f))
t("set_result", lambda: (f.set_result(5), f, f._state, f.result(), f.exception(), f.done(), f._result))
t("twice", lambda: f.set_result(6))
t("twice", lambda: f.set_exception(ValueError()))
t("cancel when done", lambda: f.cancel())
t("how they are called", lambda: [attempt(x) for x in ()])
for name, args, kwargs in (("set_result", (), {}), ("set_result", (1, 2), {}), ("set_result", (), {"result": 1}), ("set_exception", (), {}), ("result", (1,), {}), ("exception", (1,), {}), ("exception", (), {"x": 1}), ("cancel", (1, 2), {}),
                           ("cancel", (), {"msg": "m"}), ("cancel", (), {"x": 1}), ("add_done_callback", (), {}), ("add_done_callback", (1, 2), {}), ("add_done_callback", (), {"fn": 1}), ("remove_done_callback", (), {}), ("get_loop", (1,), {}), ("done", (1,), {})):
    t("%s%r%r" % (name, args, kwargs), lambda: getattr(Future(loop=loop), name)(*args, **kwargs))

print("---- exceptions")
def with_exception(e):
    f = Future(loop=loop); f.set_exception(e); return f
t("an instance", lambda: (lambda f: (f, f._exception, f._log_traceback, f.exception(), f._log_traceback))(with_exception(ValueError("v"))))
t("a class", lambda: (lambda f: (f, f.exception()))(with_exception(KeyError)))
t("raised", lambda: with_exception(ValueError("v")).result())
t("not one", lambda: with_exception(5))
t("not one", lambda: with_exception(int))
t("StopIteration", lambda: (lambda e: (e, e.__cause__, e.__context__, e.__suppress_context__))(with_exception(StopIteration(1)).exception()))
t("StopIteration, the class", lambda: with_exception(StopIteration).exception())
class Awkward(Exception):
    def __init__(self): target.set_result("meanwhile")
target = Future(loop=loop)
t("a class that settles it as it is made", lambda: target.set_exception(Awkward))
def traceback_kept():
    try: raise ValueError("with a traceback")
    except ValueError as e: raised = e
    f = with_exception(raised); tb = raised.__traceback__; raised.__traceback__ = None
    try: f.result()
    except ValueError as e: return e is raised, e.__traceback__.tb_next is tb or e.__traceback__ is tb
t("its traceback is put back", traceback_kept)
t("_log_traceback", lambda: [(setattr(f, "_log_traceback", False), f._log_traceback) for f in [with_exception(ValueError())]])
t("_log_traceback = True", lambda: setattr(Future(loop=loop), "_log_traceback", True))
t("del _log_traceback", lambda: delattr(Future(loop=loop), "_log_traceback"))
def deleted(f):
    del loop.handled[:]; f.__del__(); first = list(loop.handled); f.__del__(); return first, loop.handled == first, f._log_traceback
t("__del__ with one that nobody looked at", lambda: deleted(with_exception(ValueError())))
t("__del__ otherwise", lambda: deleted(Future(loop=loop)))

print("---- cancelling")
def cancelled(*args, **kwargs):
    f = Future(loop=loop); return f, f.cancel(*args, **kwargs)
t("cancel", lambda: (lambda f, r: (r, f, f._state, f.cancelled(), f.done(), f._cancel_message, f.cancel()))(*cancelled()))
t("with a message", lambda: (lambda f, r: (f._cancel_message, f._make_cancelled_error().args))(*cancelled("why")))
t("result", lambda: cancelled("why")[0].result())
t("exception", lambda: cancelled(msg="why")[0].exception())
t("set_result", lambda: cancelled()[0].set_result(1))
t("_cancel_message", lambda: [(setattr(f, "_cancel_message", "other"), f._cancel_message, f._make_cancelled_error().args) for f in [cancelled("why")[0]]])
t("del _cancel_message", lambda: delattr(Future(loop=loop), "_cancel_message"))
t("each is another", lambda: (lambda f: f._make_cancelled_error() is f._make_cancelled_error())(cancelled()[0]))

print("---- callbacks")
def a(f): seen.append(("a", var.get()))
def b(f): seen.append(("b", var.get()))
var = contextvars.ContextVar("var", default="unset")
seen = []
def callbacks():
    f = Future(loop=loop)
    var.set("when a was added"); f.add_done_callback(a)
    var.set("when b was added"); f.add_done_callback(b)
    other = contextvars.copy_context(); other.run(var.set, "given"); f.add_done_callback(a, context=other)
    var.set("later")
    return f
t("kept", lambda: [(c.__name__, type(x).__name__) for c, x in callbacks()._callbacks])
t("called, each in its context", lambda: (seen.clear(), callbacks().set_result(1), len(loop.ready), loop.run(), seen))
t("called when cancelled", lambda: (seen.clear(), callbacks().cancel(), loop.run(), seen))
t("gone when done", lambda: [(f.set_result(1), f._callbacks, loop.ready.clear()) for f in [callbacks()]])
t("added when done", lambda: [(f.set_result(1), f.add_done_callback(a), len(loop.ready), f._callbacks, loop.ready.clear()) for f in [Future(loop=loop)]])
t("removed", lambda: [(f.remove_done_callback(a), [c.__name__ for c, _ in f._callbacks], f.remove_done_callback(a), f.remove_done_callback(b), f._callbacks, f.remove_done_callback(b)) for f in [callbacks()]])
t("removed, of many", lambda: [([f.add_done_callback(x) for x in (a, b, a, b, b, a)], f.remove_done_callback(b), [c.__name__ for c, _ in f._callbacks], f.remove_done_callback(a), f._callbacks) for f in [Future(loop=loop)]])
class Equal:
    def __init__(self, f, what): self.f, self.what = f, what
    def __call__(self, f): pass
    def __eq__(self, other): return self.what(self.f)
t("what is compared raises", lambda: [(f.add_done_callback(Equal(f, lambda f: 1 / 0)), f.remove_done_callback(a)) for f in [Future(loop=loop)]])
t("what is compared cancels it", lambda: [([f.add_done_callback(Equal(f, lambda f: f.cancel() and False)) for _ in range(3)], f.remove_done_callback(a), f._callbacks, loop.ready.clear()) for f in [Future(loop=loop)]])
t("what is compared removes the rest", lambda: [([f.add_done_callback(Equal(f, lambda f: bool(f.remove_done_callback(b)))) for _ in range(2)], f.add_done_callback(b), f.add_done_callback(b), f.remove_done_callback(a), f._callbacks) for f in [Future(loop=loop)]])
class Refuses(Loop):
    def call_soon(self, *args, **kwargs): raise OSError("no room")
t("call_soon raises", lambda: [(f.add_done_callback(a), f.add_done_callback(b), attempt(f.set_result, 1), f._state, f._callbacks) for f in [Future(loop=Refuses())] for attempt in [lambda g, *x: t("    it says", lambda: g(*x))]])

print("---- awaiting one")
f = Future(loop=loop)
it = iter(f)
t("the iterator", lambda: (type(it).__name__, type(it).__module__, iter(it) is it, type(f.__await__()) is type(it), sorted(n for n in dir(it) if not n.startswith("__"))))
t("it cannot be made", lambda: type(it)())
t("first", lambda: (next(it) is f, f._asyncio_future_blocking))
t("again, with nobody having taken it up", lambda: next(it))
t("send", lambda: (setattr(f, "_asyncio_future_blocking", False), it.send("ignored") is f))
t("done", lambda: (f.set_result("the result"), next(it)))
def stopped(g):
    try: g()
    except StopIteration as e: return "StopIteration", e.value
t("what it stops with", lambda: (stopped(lambda: next(it)), stopped(lambda: it.send(None)), stopped(lambda: next(iter(f)))))
t("a tuple", lambda: [(g.set_result((1, 2)), stopped(lambda: next(iter(g)))) for g in [Future(loop=loop)]])
t("an exception", lambda: next(iter(with_exception(KeyError("k")))))
t("cancelled", lambda: next(iter(cancelled("m")[0])))
t("set blocking", lambda: [(setattr(g, "_asyncio_future_blocking", v), g._asyncio_future_blocking) for g in [Future(loop=loop)] for v in (1, 0, "x", "", [])])
t("del blocking", lambda: delattr(Future(loop=loop), "_asyncio_future_blocking"))
with warnings.catch_warnings(record=True) as caught:
    warnings.simplefilter("always")
    for args in ((), (ValueError,), (ValueError("v"),), (ValueError, "v"), (ValueError, ValueError("w")), (ValueError, None, None), (ValueError("v"), "x"), (ValueError("v"), None), (5,), (int,), (ValueError, "v", 5), (1, 2, 3, 4)):
        t("throw%r" % (args,), lambda: iter(Future(loop=loop)).throw(*args))
    t("what was warned of", lambda: sorted({(w.category.__name__, str(w.message)) for w in caught}))
t("close", lambda: iter(Future(loop=loop)).close())
t("how they are called", lambda: None)
for name, args in (("send", ()), ("send", (1, 2)), ("close", (1,))):
    t("%s%r" % (name, args), lambda: getattr(iter(Future(loop=loop)), name)(*args))

print("---- a task")
async def returns(value="returned"):
    return value
async def raises(e):
    raise e
async def awaits(what):
    return ("got", await what)
def quiet(task):
    "It is not to complain of being dropped"
    task._log_destroy_pending = False
    _asyncio._unregister_task(task)
    if task._coro is not None: task._coro.close()
    loop.ready.clear()
    return task
task = Task(returns(), loop=loop)
t("new", lambda: (task, task._state, task.get_loop(), task.done(), task._must_cancel, task._fut_waiter, task._log_destroy_pending, task.cancelling(), type(task.get_context()).__name__, task.get_coro() is task._coro, task._coro.__name__))
t("what is ready", lambda: [(type(c).__name__, type(c).__module__, c.__self__ is task, a, x is task.get_context(), sorted(n for n in dir(c) if not n.startswith("__"))) for c, a, x in loop.ready])
step = loop.ready[0][0]
t("the wrapper is called with nothing", lambda: step(1))
t("the wrapper is called with nothing", lambda: step(x=1))
t("outside the loop", lambda: step())
t("run", lambda: (loop.run(), task, task.result(), task._coro, task.get_coro()))
t("stepped when done", lambda: (_asyncio._set_running_loop(loop), step()))
_asyncio._set_running_loop(None)
t("one can be made", lambda: type(step)().__self__)
t("by name", lambda: quiet(Task(loop=loop, coro=returns(), name="by name")).get_name())
t("names", lambda: [(quiet(x).get_name(), x.set_name(5), x.get_name(), x.set_name("s"), x.get_name()) for x in [Task(returns(), loop=loop, name="given"), Task(returns(), loop=loop, name=7)]])
t("names that are made", lambda: [int(b.get_name()[5:]) - int(a.get_name()[5:]) for a, b in [(quiet(Task(returns(), loop=loop)), quiet(Task(returns(), loop=loop)))]])
class Named(str): pass
t("a name of a class derived from str", lambda: [type(quiet(Task(returns(), loop=loop, name=Named("n"))).get_name()).__name__])
t("a context", lambda: [quiet(Task(returns(), loop=loop, context=c)).get_context() is c for c in [contextvars.copy_context()]])
t("what is no context is taken", lambda: quiet(Task(returns(), loop=loop, context=5)).get_context())
t("set_result", lambda: quiet(Task(returns(), loop=loop)).set_result(1))
t("set_exception", lambda: quiet(Task(returns(), loop=loop)).set_exception(ValueError()))
t("it raises", lambda: [(loop.run(), x, x.exception(), x._log_traceback) for x in [Task(raises(ValueError("v")), loop=loop)]])
t("it raises SystemExit", lambda: [(t("    which gets out", loop.run), x, loop.ready.clear()) for x in [Task(raises(SystemExit(3)), loop=loop)]])
t("it raises KeyboardInterrupt", lambda: [(t("    which gets out", loop.run), x.exception()) for x in [Task(raises(KeyboardInterrupt()), loop=loop)]])
t("it raises CancelledError", lambda: [(loop.run(), x, x.cancelled()) for x in [Task(raises(asyncio.CancelledError("m")), loop=loop)]])
t("it raises StopIteration", lambda: [(loop.run(), x.exception(), x.exception().__cause__) for x in [Task(raises(StopIteration(1)), loop=loop)]])
for name, args, kwargs in (("cancel", (1, 2), {}), ("cancel", (), {"msg": 1}), ("cancel", (), {"x": 1}), ("get_stack", (1,), {}), ("get_stack", (), {"limit": 1}), ("print_stack", (1,), {}), ("set_name", (), {}), ("get_name", (1,), {}), ("uncancel", (1,), {}), ("set_result", (), {})):
    t("%s%r%r" % (name, args, kwargs), lambda: getattr(quiet(Task(returns(), loop=loop)), name)(*args, **kwargs))

print("---- what it waits for")
def waiting(what):
    x = Task(awaits(what), loop=loop); loop.run(); return x
f = Future(loop=loop)
task = waiting(f)
t("a future", lambda: (task._fut_waiter is f, f._asyncio_future_blocking, f._asyncio_awaited_by == frozenset({task}), [(repr(c), c.__self__ is task, c.__name__, type(c).__name__, x is task.get_context()) for c, x in f._callbacks]))
t("which is done", lambda: (f.set_result(1), loop.run(), task.result(), task._fut_waiter, f._asyncio_awaited_by))
t("which raises", lambda: [(g.set_exception(KeyError("k")), loop.run(), x.exception()) for g in [Future(loop=loop)] for x in [waiting(g)]])
t("which is cancelled", lambda: [(g.cancel("m"), loop.run(), x.cancelled(), x._make_cancelled_error().args) for g in [Future(loop=loop)] for x in [waiting(g)]])
t("two wait for one", lambda: [(len(g._asyncio_awaited_by), type(g._asyncio_awaited_by).__name__, g.set_result(1), loop.run(), g._asyncio_awaited_by, x.result(), y.result()) for g in [Future(loop=loop)] for x, y in [(waiting(g), waiting(g))]])
t("of another loop", lambda: waiting(Future(loop=Loop())).exception())
class Yields:
    def __init__(self, what): self.what = what
    def __await__(self): return (yield self.what)
t("nothing", lambda: [(x.done(), x.result()) for x in [waiting(Yields(None))]])
t("a future that was not awaited", lambda: waiting(Yields(Future(loop=loop))).exception())
t("a number", lambda: waiting(Yields(5)).exception())
t("a generator", lambda: waiting(Yields(i for i in ())).exception())
async def itself():
    return await holder[0]
holder = []
t("itself", lambda: (holder.append(Task(itself(), loop=loop)), loop.run(), holder[0].exception()))
class Like:
    "What will do for a future"
    _asyncio_future_blocking = True
    def __init__(self, loop): self._loop, self.calls = loop, []
    def add_done_callback(self, callback, *, context): self.calls.append(("add_done_callback", callback.__name__, type(context).__name__)); self.callback = callback
    def cancel(self, msg=None): self.calls.append(("cancel", msg)); return True
    def result(self): self.calls.append(("result",)); return "not looked at"
    def __await__(self): return (yield self)
t("what will do for one", lambda: [(x._fut_waiter is g, g._asyncio_future_blocking, g.calls, g.callback(g), x.result(), g.calls[1:]) for g in [Like(loop)] for x in [waiting(g)]])
t("with get_loop()", lambda: [(setattr(type(g), "get_loop", lambda self: loop), waiting(g).done(), delattr(type(g), "get_loop"), loop.ready.clear()) for g in [Like(None)]])
t("of another loop", lambda: waiting(Like(Loop())).exception())
class NotBlocking(Like): _asyncio_future_blocking = False
t("that was not awaited", lambda: waiting(NotBlocking(loop)).exception())
class NoneBlocking(Like): _asyncio_future_blocking = None
t("that says None", lambda: waiting(NoneBlocking(loop)).exception())
class Derived(Future): pass
t("of a class derived from Future", lambda: [(x._fut_waiter is g, g._asyncio_awaited_by == frozenset({x}), g.set_result(2), loop.run(), x.result(), g._asyncio_awaited_by) for g in [Derived(loop=loop)] for x in [waiting(g)]])
class Coroutine:
    "Not a coroutine, but what asyncio takes for one"
    def __init__(self): self.calls = []
    def send(self, value): self.calls.append(("send", value)); raise StopIteration("sent")
    def throw(self, *args): self.calls.append(("throw", args)); raise StopIteration("thrown")
    def close(self): self.calls.append(("close",))
    def __await__(self): return self
__import__("collections.abc").abc.Coroutine.register(Coroutine)
t("what is taken for a coroutine", lambda: [(loop.run(), x.result(), c.calls) for c in [Coroutine()] for x in [Task(c, loop=loop)]])
t("and cancelled", lambda: [(x.cancel("m"), loop.run(), x.cancelled(), [(n, [type(a).__name__ for a in args[0]]) for n, *args in c.calls]) for c in [Coroutine()] for x in [Task(c, loop=loop)]])

print("---- cancelling a task")
t("before it has begun", lambda: [(x.cancel("early"), x._must_cancel, x.cancelling(), x._cancel_message, loop.run(), x.cancelled(), x._must_cancel, x._make_cancelled_error().args) for x in [Task(returns(), loop=loop)]])
t("while it waits", lambda: [(x.cancel("why"), x._must_cancel, g.cancelled(), g._cancel_message, loop.run(), x.cancelled(), x.cancelling()) for g in [Future(loop=loop)] for x in [waiting(g)]])
t("when it is done", lambda: [(loop.run(), x.cancel(), x.cancelling()) for x in [Task(returns(), loop=loop)]])
t("more than once", lambda: [(x.cancel(), x.cancel(), x.cancelling(), x.uncancel(), x._must_cancel, x.uncancel(), x._must_cancel, x.uncancel(), loop.run(), x.result()) for x in [Task(returns(), loop=loop)]])
async def refuses(g):
    try: await g
    except asyncio.CancelledError: return "refused"
t("it will not have it", lambda: [(loop.run(), x.cancel(), loop.run(), x.result(), x.cancelled(), x.cancelling()) for g in [Future(loop=loop)] for x in [Task(refuses(g), loop=loop)]])
class WillNot(Like):
    def cancel(self, msg=None): self.calls.append(("cancel", msg)); return False
t("what it waits for will not", lambda: [(x.cancel("m"), x._must_cancel, g.calls[1:], g.callback(g), x.cancelled()) for g in [WillNot(loop)] for x in [waiting(g)]])
t("__del__ while pending", lambda: [(loop.handled.clear(), x.__del__(), list(loop.handled), quiet(x).__del__(), len(loop.handled)) for x in [Task(returns(), loop=loop)]])
t("_log_destroy_pending", lambda: [(setattr(x, "_log_destroy_pending", v), x._log_destroy_pending) for x in [quiet(Task(returns(), loop=loop))] for v in (1, 0, "x")])
t("del _log_destroy_pending", lambda: delattr(quiet(Task(returns(), loop=loop)), "_log_destroy_pending"))
t("_must_cancel cannot be set", lambda: setattr(quiet(Task(returns(), loop=loop)), "_must_cancel", True))

print("---- in debug mode")
debugging = Loop(debug=True)
t("where it was made", lambda: [(type(g._source_traceback).__name__, g._source_traceback[-1].name, "created at" in repr(g)) for g in [Future(loop=debugging)]])
t("__del__", lambda: [(g.set_exception(ValueError()), g.__del__(), debugging.handled) for g in [Future(loop=debugging)]])

print("---- which is running")
# Anything will do for a task here. Which it is goes by which object it is.
a_task, outer, swapped_in = "a task", "outer", "swapped in"
t("no loop", lambda: (_asyncio._get_running_loop(), asyncio.get_running_loop is _asyncio.get_running_loop))
t("no loop", lambda: _asyncio.get_running_loop())
t("no loop", lambda: _asyncio.current_task())
t("no loop", lambda: _asyncio.all_tasks())
t("set", lambda: (_asyncio._set_running_loop(loop), _asyncio._get_running_loop(), _asyncio.get_running_loop(), _asyncio.get_event_loop(), _asyncio.current_task(), _asyncio.current_task(loop), _asyncio.current_task(Loop())))
t("a future finds it", lambda: Future().get_loop())
t("enter", lambda: (_asyncio._enter_task(loop, a_task), _asyncio.current_task(), _asyncio.current_task(loop=loop)))
t("enter another", lambda: _asyncio._enter_task(loop, "another"))
t("enter for another loop", lambda: _asyncio._enter_task(Loop(), "another"))
t("leave another", lambda: _asyncio._leave_task(loop, "another"))
t("leave for another loop", lambda: _asyncio._leave_task(Loop(), a_task))
t("swap", lambda: (_asyncio._swap_current_task(loop, swapped_in), _asyncio.current_task(), _asyncio._swap_current_task(loop, None), _asyncio.current_task(), _asyncio._swap_current_task(loop, a_task)))
t("swap for another loop", lambda: _asyncio._swap_current_task(Loop(), None))
t("leave", lambda: (_asyncio._leave_task(loop, a_task), _asyncio.current_task()))
t("leave when there is none", lambda: _asyncio._leave_task(loop, a_task))
async def who():
    return _asyncio.current_task()
t("as it runs", lambda: [(loop.run(), x.result() is x) for x in [Task(who(), loop=loop)]])
_asyncio._set_running_loop(loop)
class Other:
    "A task of somebody else's"
    def __init__(self, loop, done=False): self._loop, self._done = loop, done
    def done(self): return self._done
    def __repr__(self): return "<Other>"
mine, done, elsewhere = Other(loop), Other(loop, True), Other(Loop())
# Some have been left waiting for what will never come, with nothing holding on to them. Whether they are still there is not what this is about.
for left in _asyncio.all_tasks():
    _asyncio._unregister_task(left)
pending = Task(returns(), loop=loop, name="pending")
def names_of(tasks): return sorted(x.get_name() for x in tasks)
t("all_tasks", lambda: sorted(x.get_name() for x in _asyncio.all_tasks()))
t("those of others", lambda: ([_asyncio._register_task(x) for x in (mine, done, elsewhere)], sorted(map(repr, _asyncio.all_tasks())), type(_asyncio.all_tasks()).__name__, _asyncio.all_tasks(elsewhere._loop), [_asyncio._unregister_task(x) for x in (mine, done, elsewhere, mine)], len(_asyncio.all_tasks())))
t("those that are eager", lambda: (_asyncio._register_eager_task(mine), len(_asyncio.all_tasks()), _asyncio._unregister_eager_task(mine), _asyncio._unregister_eager_task(mine), len(_asyncio.all_tasks())))
t("its own, taken off and put back", lambda: (_asyncio._unregister_task(pending), len(_asyncio.all_tasks()), _asyncio._register_task(pending), _asyncio._register_task(pending), len(_asyncio.all_tasks()), _asyncio._unregister_eager_task(pending), len(_asyncio.all_tasks()), _asyncio._register_eager_task(pending), len(_asyncio.all_tasks())))
t("one that is done is not among them", lambda: (loop.run(), _asyncio._set_running_loop(loop), names_of(_asyncio.all_tasks())))
t("awaited by", lambda: [(_asyncio.future_add_to_awaited_by(g, h), g._asyncio_awaited_by == {h}, _asyncio.future_add_to_awaited_by(g, k), g._asyncio_awaited_by == {h, k}, _asyncio.future_discard_from_awaited_by(g, h), g._asyncio_awaited_by == {k},
                          _asyncio.future_discard_from_awaited_by(g, h), _asyncio.future_add_to_awaited_by(g, 5), _asyncio.future_add_to_awaited_by(5, g), _asyncio.future_discard_from_awaited_by(5, 5), len(g._asyncio_awaited_by)) for g, h, k in [[Future() for _ in range(3)]]])
for name, args, kwargs in (("current_task", (1, 2), {}), ("current_task", (), {"x": 1}), ("all_tasks", (1, 2), {}), ("_enter_task", (1,), {}), ("_enter_task", (), {"loop": loop, "task": 1}), ("_leave_task", (), {"task": 1, "loop": loop}), ("_set_running_loop", (), {}),
                           ("_set_running_loop", (), {"loop": 1}), ("_register_task", (), {}), ("_get_running_loop", (1,), {}), ("future_add_to_awaited_by", (1,), {}), ("future_add_to_awaited_by", (), {"fut": 1, "waiter": 2}), ("_swap_current_task", (1,), {})):
    t("%s%r%r" % (name, args, sorted(kwargs)), lambda: getattr(_asyncio, name)(*args, **kwargs))

print("---- begun at once")
loop.running = True
order = []
async def eager(g=None):
    order.append(("running", _asyncio.current_task().get_name(), var.get()))
    if g is not None: await g
    return "done"
t("to its end", lambda: [(order.clear(), var.set("outside")) and None or (x.done(), x.result(), order, loop.ready, x._coro, _asyncio.current_task(), names_of(_asyncio.all_tasks())) for x in [Task(eager(), loop=loop, name="eager", eager_start=True)]])
t("until it waits", lambda: [(x.done(), x._fut_waiter is g, len(_asyncio.all_tasks()), g.set_result(1), loop.run(), x.result()) for g in [Future(loop=loop)] for x in [Task(eager(g), loop=loop, name="eager", eager_start=True)]])
_asyncio._set_running_loop(loop); loop.running = True
t("it raises", lambda: [(x.done(), x.exception()) for x in [Task(raises(ValueError("v")), loop=loop, eager_start=True)]])
t("inside another", lambda: (_asyncio._enter_task(loop, outer), Task(eager(), loop=loop, name="inner", eager_start=True).result(), _asyncio.current_task(), _asyncio._leave_task(loop, outer)))
t("in what is no context", lambda: Task(returns(), loop=loop, context=5, eager_start=True))
loop.running = False
t("with the loop not running", lambda: [(x.done(), len(loop.ready), quiet(x) and None) for x in [Task(returns(), loop=loop, eager_start=True)]])
t("eager_start is true or false", lambda: [quiet(Task(returns(), loop=loop, eager_start=v)).done() for v in (0, "", [], None)])
_asyncio._set_running_loop(None)

print("---- derived classes")
class MyTask(Task):
    def __init__(self, *args, **kwargs): super().__init__(*args, **kwargs); self.extra = "mine"
    def cancel(self, msg=None): self.asked = msg; return super().cancel(msg)
t(a_task, lambda: [(x.extra, x, isinstance(x, Future), loop.run(), x.result(), x.cancel("late"), x.asked) for x in [MyTask(returns(), loop=loop, name="my")]])
class MyFuture(Future):
    def __repr__(self): return "<MyFuture>"
    def add_done_callback(self, callback, *, context=None): calls.append("add_done_callback"); return super().add_done_callback(callback, context=context)
    def result(self): calls.append("result"); return super().result()
calls = []
t("a future, whose methods are used", lambda: [(calls[:], g.set_result(1), loop.run(), x.result(), calls) for g in [MyFuture(loop=loop)] for x in [waiting(g)]])
t("initialized twice", lambda: [(g.set_result(1), g.__init__(loop=loop), g._state, g._result) for g in [Future(loop=loop)]])
