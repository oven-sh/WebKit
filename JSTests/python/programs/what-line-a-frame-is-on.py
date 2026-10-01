# What line a frame is said to be on each time that something is told of it, and whether that is the line that co_lines() has for f_lasti.
import contextlib
import sys


# What is to be told when something has gone is told whenever that is found out, which may be in the middle of this. It is no part of what is looked at here.
def is_told_of_what_has_gone(frame):
    return frame.f_code.co_filename.endswith(("weakref.py", "_weakrefset.py", "importlib._bootstrap>", "_bootstrap.py"))


def part0():
    seen = []
    def trace(frame, event, arg):
        if is_told_of_what_has_gone(frame): return None
        seen.append((frame.f_code.co_name, event, frame.f_lineno))
        return trace



    def f(a, b):
        # a comment
        p = '_%s__' % (a, )
        q = len(p)
        if (
                len(b) > q
                and b.startswith(p)
                and (b[-1] != '_' or b[-2] != '_')
            ):
            return True
        else:
            return False
    def g(a):
        x = '%s-%r-%a' % (a, a,
                          a)
        y = ('%s'
             % (a,))
        return x, y
    code = compile("\n\nx = 1\ny = 2\n", "<m>", "exec")
    sys.settrace(trace); f("A", "_A__x"); g(1); exec(code); exec("z = 1"); sys.settrace(None)
    for s in seen: print(s)
    print(list(f.__code__.co_lines())[:3] and [l for s, e, l in f.__code__.co_lines()], [l for s, e, l in g.__code__.co_lines()], [l for s, e, l in code.co_lines()], code.co_firstlineno)


print("----", 'what is made of something else, and a module')
part0()

def part1():
    seen = []
    def trace(frame, event, arg):
        if is_told_of_what_has_gone(frame): return None
        last = "none"
        for s, e, l in frame.f_code.co_lines():
            if s <= frame.f_lasti < e: last = l
        seen.append((frame.f_code.co_name, event, frame.f_lineno, "same" if last == frame.f_lineno else "co_lines() says %s" % last))
        return trace

    def decorate(f): return f

    @decorate
    @decorate
    def plain():
        return 1

    @decorate
    def gen():
        yield 1
        yield 2

    @decorate
    async def coro():
        return 1

    @decorate
    class C:
        x = 1

    def tryReturn(a):
        try:
            if a:
                return 1
        finally:
            a = 2
        return 3

    def withReturn(m):
        with m:
            return 1

    def loopReturn(items):
        for i in items:
            if i:
                return i
        else:
            return None
    empty = compile("", "<empty>", "exec")
    comment = compile("# nothing\n\n", "<comment>", "exec")
    doc = compile('"doc"\n', "<doc>", "exec")
    one = compile("\n\n\nx = 1", "<one>", "exec")
    expr = compile("1 + 1", "<expr>", "eval")
    sys.settrace(trace)
    plain(); list(gen())
    try: coro().send(None)
    except StopIteration: pass
    tryReturn(1); tryReturn(0); withReturn(contextlib.nullcontext()); loopReturn([0, 1]); loopReturn([])
    exec(empty); exec(comment); exec(doc); exec(one); eval(expr)
    @decorate
    class D:
        y = 2
    sys.settrace(None)
    for s in seen:
        if s[0] not in ("__init__", "__enter__", "__exit__"): print(s)


print("----", 'where what has decorators begins, what leaves by way of something, and a module with nothing in it')
part1()

def part2():
    seen = []
    def trace(frame, event, arg):
        if is_told_of_what_has_gone(frame): return None
        last = "none"
        for s, e, l in frame.f_code.co_lines():
            if s <= frame.f_lasti < e: last = l
        seen.append((frame.f_code.co_name, event, frame.f_lineno, "same" if last == frame.f_lineno else "co_lines() says %s" % last))
        return trace
    class M:
        def __enter__(self): pass
        def __exit__(self, *a): pass
    def other(x): return x
    def a(m, x):
        with m:
            y = x
            if y:
                return other(y)
        return 0
    def b(m, x):
        with m:
            return other(x)
    def c(m, x):
        with m:
            with m:
                if x:
                    return other(
                        x)
    def d(x):
        try:
            return other(x)
        finally:
            x = 0
    def e(items):
        for i in items:
            with M():
                if i:
                    return other(i)
                continue
    def f(m, x):
        with m:
            raise ValueError(x)
    sys.settrace(trace)
    a(M(), 1); a(M(), 0); b(M(), 1); c(M(), 1); d(1); e([0, 1])
    try: f(M(), 1)
    except ValueError: pass
    sys.settrace(None)
    for s in seen:
        if s[0] in "abcdef": print(s)


print("----", 'leaving by way of with and finally')
part2()

def part3():
    seen = []
    def trace(frame, event, arg):
        if is_told_of_what_has_gone(frame): return None
        seen.append((frame.f_code.co_name, event, frame.f_lineno - frame.f_code.co_firstlineno))
        return trace
    def a():
        try:
            1/0
        except:
            pass
    def b():
        try:
            try:
                1/0
            finally:
                x = 5
        except:
            pass
    def c():
        try:
            try:
                1/0
            finally:
                x = 5
        except ZeroDivisionError:
            pass
    def d():
        try:
            try:
                1/0
            except IndexError:
                x = 5
        except:
            pass
    def e():
        try:
            try:
                1/0
            except IndexError:
                x = 5
            finally:
                x = 7
        except:
            pass
    def f():
        try:
            with open(__file__):
                1/0
        except:
            pass
    def g():
        try:
            try:
                1/0
            finally:
                x = 5
        except:
            pass
        finally:
            x = 9
    def h():
        for i in range(2):
            try:
                try:
                    1/0
                finally:
                    if i:
                        x = 6
            except:
                x = 8
    sys.settrace(trace)
    for fn in (a, b, c, d, e, f, g, h): fn()
    sys.settrace(None)
    cur = None
    for name, event, line in seen:
        if name not in "abcdefgh": continue
        if name != cur: print(); print(name, end=": "); cur = name
        print(event[0] + str(line), end=" ")
    print()


print("----", 'except, with and without a class')
part3()
