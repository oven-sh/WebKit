# _thread, as far as a program goes that does not start a thread.
import sys
import time
import _thread


def nowhere(text):
    out, i = "", 0
    while True:
        j = text.find("0x", i)
        if j < 0:
            return out + text[i:]
        k = j + 2
        while k < len(text) and text[k] in "0123456789abcdef":
            k += 1
        out += text[i:j] + "0x"
        i = k


ME = str(_thread.get_ident())


def t(label, f):
    try:
        r = f()
    except BaseException as e:
        r = type(e).__name__ + ": " + str(e)
    print(label, "=>", nowhere(r if isinstance(r, str) else repr(r)).replace(ME, "ME"))


print("---- the module")
t("what is in it", lambda: sorted(k for k in vars(_thread) if not k.startswith("__")))
t("names", lambda: (_thread.__name__, _thread.error is RuntimeError, _thread.LockType is _thread.lock, _thread.TIMEOUT_MAX, _thread._NAME_MAXLEN > 0))
t("the classes", lambda: [(c.__name__, c.__module__, c.__qualname__, c.__bases__, sorted(k for k in vars(c) if k not in ("__doc__",))) for c in (_thread.lock, _thread.RLock, _thread._local, _thread._ThreadHandle)])
t("_ExceptHookArgs", lambda: (_thread._ExceptHookArgs.__name__, _thread._ExceptHookArgs.__module__, _thread._ExceptHookArgs.n_fields, _thread._ExceptHookArgs.n_sequence_fields, _thread._ExceptHookArgs.__match_args__))
t("what can be derived from", lambda: [(c.__name__, bool(c.__flags__ & (1 << 10))) for c in (_thread.lock, _thread.RLock, _thread._local, _thread._ThreadHandle, _thread._ExceptHookArgs)])
t("derived from lock", lambda: type("L", (_thread.lock,), {}))

print("---- lock")
lock = _thread.allocate_lock()
t("new", lambda: (type(lock) is _thread.lock, repr(lock), lock.locked(), type(_thread.allocate()) is _thread.lock, type(_thread.lock()) is _thread.lock))
t("lock(1)", lambda: _thread.lock(1))
t("lock(a=1)", lambda: _thread.lock(a=1))
t("acquire", lambda: (lock.acquire(), lock.locked(), repr(lock)))
t("again, without waiting", lambda: (lock.acquire(False), lock.acquire(0), lock.acquire(blocking=False), lock.acquire_lock(False), lock.locked_lock()))
t("release", lambda: (lock.release(), lock.locked()))
t("release again", lambda: lock.release())
t("release_lock", lambda: (lock.acquire_lock(), lock.release_lock(), lock.locked()))
t("release(1)", lambda: lock.release(1))


def with_lock():
    with lock as got:
        inside = (got, lock.locked())
    return inside, lock.locked()


t("with", with_lock)
t("__exit__", lambda: (lock.__enter__(), lock.__exit__(None, None, None), lock.locked()))
t("__exit__ with nothing", lambda: (lock.__enter__(), lock.__exit__()))
lock = _thread.allocate_lock()
for label, args, kwargs in (("(True, -1)", (True, -1), {}), ("(True, 0)", (True, 0), {}), ("(timeout=0.0)", (), {"timeout": 0.0}), ("(1, 5)", (1, 5), {}), ("([], -1)", ([], -1), {}), ("('a')", ("a",), {})):
    t("acquire" + label, lambda: (lock.acquire(*args, **kwargs), lock.locked(), lock.locked() and lock.release()))
for label, args, kwargs in (("(False, 1)", (False, 1), {}), ("(False, 0)", (False, 0), {}), ("(False, -1)", (False, -1), {}), ("(True, -2)", (True, -2), {}), ("(True, -0.5)", (True, -0.5), {}), ("(True, -1.0)", (True, -1.0), {}),
                            ("(True, 1e100)", (True, 1e100), {}), ("(True, TIMEOUT_MAX)", (True, _thread.TIMEOUT_MAX), {}), ("(True, TIMEOUT_MAX + 1)", (True, _thread.TIMEOUT_MAX + 1), {}),
                            ("(True, 10 ** 30)", (True, 10 ** 30), {}), ("(True, 9223372036)", (True, 9223372036), {}), ("(True, 9223372037)", (True, 9223372037), {}), ("(True, nan)", (True, float("nan")), {}),
                            ("(True, inf)", (True, float("inf")), {}), ("(True, 'a')", (True, "a"), {}), ("(True, None)", (True, None), {}), ("(True, 1j)", (True, 1j), {}), ("(1, 2, 3)", (1, 2, 3), {}), ("(a=1)", (), {"a": 1}),
                            ("(True, blocking=1)", (True,), {"blocking": 1})):
    t("acquire" + label, lambda: (lock.acquire(*args, **kwargs), lock.locked() and lock.release()))


class Index:
    def __index__(self): return 0


class Bad:
    def __bool__(self): raise ValueError("no truth")


t("a timeout with __index__", lambda: (lock.acquire(True, Index()), lock.release()))
t("blocking that raises", lambda: lock.acquire(Bad()))


def waits(acquire, seconds):
    began = time.monotonic()
    got = acquire(True, seconds)
    took = time.monotonic() - began
    return got, took >= seconds, took < seconds + 2


lock.acquire()
t("waits as long as it is told", lambda: (waits(lock.acquire, 0.05), waits(lock.acquire, 0.001), waits(lock.acquire, 1e-12)))
lock.release()
t("_at_fork_reinit", lambda: (lock.acquire(), lock._at_fork_reinit(), lock.locked()))
t("weakly referred to", lambda: __import__("_weakref").ref(lock)() is lock)
t("no attributes", lambda: setattr(lock, "a", 1))
t("hash and ==", lambda: (lock == lock, lock == _thread.allocate_lock(), hash(lock) == hash(lock)))

print("---- RLock")
r = _thread.RLock()
t("new", lambda: (repr(r), r.locked(), r._is_owned(), r._recursion_count()))
t("RLock(1, a=2)", lambda: type(_thread.RLock(1, a=2)).__name__)
t("acquire", lambda: (r.acquire(), repr(r), r.locked(), r._is_owned(), r._recursion_count()))
t("again", lambda: (r.acquire(), r.acquire(False), r.acquire(True, 1), repr(r), r._recursion_count()))
t("release", lambda: (r.release(), r._recursion_count(), r.release(), r.release(), r._recursion_count(), r.locked(), r.release(), r.locked(), repr(r)))
t("release too often", lambda: r.release())
t("_release_save of one not held", lambda: r._release_save())
t("_release_save", lambda: (r.acquire(), r.acquire(), r._release_save(), r.locked(), r._recursion_count(), repr(r)))
t("_acquire_restore", lambda: (r._acquire_restore((3, _thread.get_ident())), r._recursion_count(), repr(r), r.release(), r.release(), r.release(), r.locked()))
t("as another thread's", lambda: (r._acquire_restore((2, 12345)), repr(r), r.locked(), r._is_owned(), r._recursion_count(), r.acquire(False), waits(r.acquire, 0.01)))
t("which cannot be released", lambda: r.release())
t("_at_fork_reinit", lambda: (r._at_fork_reinit(), repr(r)))
for label, arg in (("()", ()), ("(1,)", (1,)), ("(1, 2, 3)", (1, 2, 3)), ("[1, ME]", [1, _thread.get_ident()]), ("None", None), ("5", 5), ("'ab'", "ab"), ("('a', 1)", ("a", 1)), ("(1, 'a')", (1, "a")), ("(1, -1)", (1, -1)),
                   ("(1, 2 ** 64 + 5)", (1, 2 ** 64 + 5)), ("(2 ** 70, 1)", (2 ** 70, 1)), ("(1.5, 1)", (1.5, 1))):
    fresh = _thread.RLock()
    t("_acquire_restore(%s)" % label, lambda: (fresh._acquire_restore(arg), repr(fresh)))
t("_acquire_restore()", lambda: r._acquire_restore())


def with_rlock():
    with r as got:
        with r:
            inside = (got, r._recursion_count())
    return inside, r.locked()


t("with", with_rlock)


class MyRLock(_thread.RLock):
    def __init__(self, name): self.name = name


t("derived from", lambda: (MyRLock("n").name, repr(MyRLock("n")), MyRLock("n").acquire(), isinstance(MyRLock("n"), _thread.RLock)))
for label, args in (("(False, 1)", (False, 1)), ("(True, -2)", (True, -2)), ("(True, 1e100)", (True, 1e100)), ("(True, 'a')", (True, "a"))):
    t("acquire" + label, lambda: r.acquire(*args))

print("---- _local")
l = _thread._local()
t("new", lambda: (type(l).__name__, l.__dict__, vars(l)))
t("attributes", lambda: (setattr(l, "a", 1), l.a, l.__dict__, delattr(l, "a"), l.__dict__, hasattr(l, "a")))
t("none such", lambda: l.missing)
t("del none such", lambda: delattr(l, "missing"))
t("__dict__ is the one dict", lambda: (l.__dict__ is l.__dict__, l.__dict__.__setitem__("b", 2), l.b, setattr(l, "c", 3), sorted(l.__dict__)))
t("__dict__ cannot be set", lambda: setattr(l, "__dict__", {}))
t("or deleted", lambda: delattr(l, "__dict__"))
t("arguments", lambda: _thread._local(1))
t("by name", lambda: _thread._local(a=1))
t("what it has comes before what the class has", lambda: (setattr(l, "__class__x", 1), l.__dict__.__setitem__("__doc__", "mine"), l.__doc__, l.__dict__.__setitem__("__class__", 5), l.__class__, type(l).__name__))
t("a name that is no str", lambda: getattr(l, 1))
t("__class__", lambda: setattr(_thread._local(), "__class__", int))
t("dir", lambda: [k for k in dir(_thread._local()) if not k.startswith("__")])


class Local(_thread._local):
    shared = "class"
    calls = []

    def __init__(self, *args, **kwargs):
        self.calls.append((args, kwargs))
        self.given = args

    @property
    def prop(self): return "property"


m = Local(1, 2, k=3)
t("derived from", lambda: (m.given, m.shared, m.prop, Local.calls, m.__dict__, sorted(k for k in vars(Local) if not k.startswith("__") or k in ("__dict__", "__weakref__"))))
t("a property comes first", lambda: (m.__dict__.__setitem__("prop", "instance"), m.prop))
t("and cannot be set", lambda: setattr(m, "prop", 1))
t("__dict__ cannot be set", lambda: setattr(m, "__dict__", {}))


class SlotsLocal(_thread._local):
    __slots__ = ("s",)


s = SlotsLocal()
t("with __slots__", lambda: (setattr(s, "s", 1), s.s, setattr(s, "other", 2), s.other, s.__dict__))
t("weakly referred to", lambda: __import__("_weakref").ref(l)() is l)

print("---- _ThreadHandle")
h = _thread._ThreadHandle()
t("new", lambda: (repr(h), h.ident, h.is_done()))
t("_ThreadHandle(1, a=2)", lambda: repr(_thread._ThreadHandle(1, a=2)))
t("join before it is started", lambda: h.join())
t("_set_done before it is started", lambda: h._set_done())
t("ident cannot be set", lambda: setattr(h, "ident", 1))
mine = _thread._make_thread_handle(_thread.get_ident())
t("of this thread", lambda: (repr(mine), mine.ident == _thread.get_ident(), mine.is_done()))
t("join", lambda: mine.join())
t("join(0)", lambda: mine.join(0))
t("_set_done", lambda: (mine._set_done(), mine.is_done(), mine.join(), mine.join(1), mine._set_done()))
other = _thread._make_thread_handle(12345)
t("of some other", lambda: (repr(other), other.is_done(), other.join(0), other.join(0.01), other.is_done()))
for label, arg in (("'a'", "a"), ("nan", float("nan")), ("1e100", 1e100), ("[]", [])):
    t("join(%s)" % label, lambda: other.join(arg))
t("join(1, 2)", lambda: other.join(1, 2))
t("join(timeout=1)", lambda: other.join(timeout=1))
for label, arg in (("'a'", "a"), ("1.5", 1.5), ("-1", -1), ("2 ** 64", 2 ** 64), ("2 ** 64 - 1", 2 ** 64 - 1), ("True", True), ("0", 0)):
    t("_make_thread_handle(%s)" % label, lambda: repr(_thread._make_thread_handle(arg)))

print("---- functions")
t("get_ident", lambda: (type(_thread.get_ident()).__name__, _thread.get_ident() == _thread.get_ident(), _thread.get_ident() > 0, _thread._get_main_thread_ident() == _thread.get_ident()))
t("get_native_id", lambda: (type(_thread.get_native_id()).__name__, _thread.get_native_id() > 0))
t("_count and the like", lambda: (_thread._count(), _thread._is_main_interpreter(), _thread.daemon_threads_allowed(), _thread._shutdown()))
t("exit", lambda: _thread.exit())
t("exit_thread", lambda: _thread.exit_thread())
t("exit(1)", lambda: _thread.exit(1))
for a in (1, 4096, 53247, 53248, 65536, 100000, 1 << 20, -1, 0):
    t("stack_size(%d)" % a, lambda: _thread.stack_size(a))
t("stack_size()", lambda: (_thread.stack_size(), _thread.stack_size(1 << 20), _thread.stack_size(), _thread.stack_size(0), _thread.stack_size()))
t("stack_size('a')", lambda: _thread.stack_size("a"))
t("stack_size(2 ** 70)", lambda: _thread.stack_size(2 ** 70))
t("names", lambda: (_thread.set_name("a name"), _thread._get_name(), _thread.set_name("x" * 100), len(_thread._get_name()) == _thread._NAME_MAXLEN, _thread.set_name("é\udc80"), _thread._get_name(), _thread.set_name(""), _thread._get_name()))
t("set_name(1)", lambda: _thread.set_name(1))
t("set_name(name='n')", lambda: (_thread.set_name(name="n"), _thread._get_name(), _thread.set_name("")))

print("---- interrupt_main")


def interrupted():
    try:
        _thread.interrupt_main()
    except KeyboardInterrupt as e:
        return "caught", e.args, e.__traceback__.tb_lineno - interrupted.__code__.co_firstlineno
    return "not raised"


t("is raised there and then", interrupted)
t("by number", lambda: _thread.interrupt_main(2))
t("some other signal", lambda: _thread.interrupt_main(30))
for a in (0, -1, 31, 32, 1000, 2 ** 40, -2 ** 40, 2 ** 70):
    t("interrupt_main(%d)" % a, lambda: _thread.interrupt_main(a))
t("interrupt_main('a')", lambda: _thread.interrupt_main("a"))

print("---- _excepthook")


class File:
    def __init__(self): self.written = []
    def write(self, text): self.written.append(text)
    def flush(self): self.written.append("<flush>")


class Thread:
    name = "the name"


def hooked(thread, error=ValueError("said"), kind=None):
    file, sys.stderr = sys.stderr, File()
    try:
        _thread._excepthook(_thread._ExceptHookArgs((kind or type(error), error, None, thread)))
        return "".join(sys.stderr.written)
    finally:
        sys.stderr = file


t("with a thread", lambda: hooked(Thread()))
t("with none", lambda: hooked(None))
t("with one that has no name", lambda: hooked(object()))
t("SystemExit", lambda: hooked(Thread(), SystemExit(1)))
t("not what it wants", lambda: _thread._excepthook((1, 2, 3, 4)))
t("_ExceptHookArgs", lambda: (_thread._ExceptHookArgs((1, 2, 3, 4)), _thread._ExceptHookArgs((1, 2, 3, 4)).thread, _thread._ExceptHookArgs((1, 2, 3, 4))[0]))
t("of three", lambda: _thread._ExceptHookArgs((1, 2, 3)))


def without_stderr(thread):
    file, sys.stderr = sys.stderr, None
    try:
        return _thread._excepthook(_thread._ExceptHookArgs((ValueError, ValueError("said"), None, thread)))
    finally:
        sys.stderr = file


t("no sys.stderr and no thread", lambda: without_stderr(None))
t("nor the thread's", lambda: without_stderr(Thread()))

print("---- what is asked before a thread is started")
for label, args, kwargs in (("()", (), {}), ("(f)", (len,), {}), ("(1, ())", (1, ()), {}), ("(f, [])", (len, []), {}), ("(f, (), [])", (len, (), []), {}), ("(f, (), {}, 1)", (len, (), {}, 1), {}), ("(f, (), a=1)", (len, ()), {"a": 1})):
    t("start_new_thread" + label, lambda: _thread.start_new_thread(*args, **kwargs))
for label, args, kwargs in (("()", (), {}), ("(1)", (1,), {}), ("(f, 1)", (len, 1), {}), ("(f, None, Bad())", (len, None, Bad()), {}), ("(f, a=1)", (len,), {"a": 1}), ("(f, None, 1, 2)", (len, None, 1, 2), {})):
    t("start_joinable_thread" + label, lambda: _thread.start_joinable_thread(*args, **kwargs))
done = _thread._make_thread_handle(1)
t("with a handle that is in use", lambda: _thread.start_joinable_thread(len, done))
