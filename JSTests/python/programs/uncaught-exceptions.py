import sys
# What is printed says where this file is, which is not the same everywhere.
class WithoutDirectory:
    directory = __file__[:__file__.rfind("/") + 1]
    def write(self, text): sys.stdout.write(text.replace(self.directory, "") if self.directory else text)
    def flush(self): pass
sys.stderr = WithoutDirectory()
def show(f, *a):
    print("=====", f.__name__)
    try: f(*a)
    except BaseException:
        t, v, tb = sys.exc_info()
        sys.__excepthook__(t, v, tb.tb_next)
def positions(f, *a):
    try: f(*a)
    except BaseException as e:
        tb = e.__traceback__.tb_next; out = []
        while tb:
            c = tb.tb_frame.f_code; p = list(c.co_positions())[tb.tb_lasti // 2]
            out.append((c.co_name, p[0] - c.co_firstlineno, p[1] - c.co_firstlineno, p[2], p[3])); tb = tb.tb_next
        print("  positions", out)
x = None; d = {}; l = [1]; z = 0
def name(): return undefined_name
def attribute(): return x.missing
def chain_attr(): return d.keys().nothing.more
def subscript(): return d["k"]
def nested_subscript(): return l[0][1]
def binop(): return 1 + x
def binop_long(): return (1 + 2) * x + 3
def division(): return 1 / z
def call(): return x()
def call_args(): return len(1, 2)
def method_call(): return l.append(1, 2)
def inner(): raise ValueError("inner")
def calls_inner(): return inner()
def in_expression(): return 1 + inner() + 2
def multi_line():
    return (1 +
            x +
            2)
def multi_line_call():
    return len(
        1,
        2,
    )
def unary(): return -x
def compare(): return 1 < x
def contains(): return 1 in x
def unpack(): a, b = l
def star_call(): return len(*x)
def assign_sub(): x[0] = 1
def assign_attr(): x.a = 1
def del_sub(): del d["k"]
def aug(): 
    y = None
    y += 1
def raise_stmt(): raise KeyError("k")
def raise_from():
    try: 1 / z
    except ZeroDivisionError as e: raise ValueError("second") from e
def raise_during():
    try: 1 / z
    except ZeroDivisionError: raise ValueError("second")
def raise_from_none():
    try: 1 / z
    except ZeroDivisionError: raise ValueError("second") from None
def notes():
    e = ValueError("noted"); e.add_note("one"); e.add_note("two\nlines"); raise e
def recursion(n=0): return recursion(n + 1)
def few_repeats(n=5): return few_repeats(n - 1) if n else 1 / z
def group(): raise ExceptionGroup("g", [ValueError(1), TypeError(2)])
def nested_group():
    try: 1 / z
    except ZeroDivisionError as e: inner_error = e
    raise ExceptionGroup("outer", [ExceptionGroup("inner", [inner_error, KeyError("k")]), ValueError("v")])
def group_with_cause():
    try: raise ExceptionGroup("first", [ValueError(1)])
    except ExceptionGroup as e: raise TypeError("second") from e
def unicode_line(): return "é" + "日本語" + x
def tabs():
	return	1 +	x
def assertion(): assert z, "message"
def assertion_bare(): assert z == 1
def fstring(): return f"{x.missing}"
def comprehension(): return [1 / i for i in (1, 0)]
def lambda_(): return (lambda: 1 / z)()
def with_stmt():
    with x: pass
def for_stmt():
    for i in x: pass
def import_stmt(): import nothing_by_this_name
def import_from(): from sys import nothing_by_this_name
def no_message(): raise ValueError
def multi_arg(): raise ValueError(1, 2)
def custom():
    class MyError(Exception): pass
    raise MyError("mine")
def bad_str():
    class Bad(Exception):
        def __str__(self): raise RuntimeError("no")
    raise Bad()
def system_exit(): raise SystemExit(3)
def keyboard(): raise KeyboardInterrupt
def syntax(): compile("a b", "<s>", "exec")
def exec_error(): exec("1 / 0")
def semicolons(): a = 1; b = a / z; return b
def long_line(): return                                                                       1 / z
def call_of_call(): return (lambda: None)()()
def await_like(): return l[0](1)[2]
def slice_(): return x[1:2]
def kw_call(): return len(a=1)
cases = [name, attribute, chain_attr, subscript, nested_subscript, binop, binop_long, division, call, call_args, method_call, calls_inner, in_expression, multi_line, multi_line_call, unary, compare, contains, unpack, star_call, assign_sub, assign_attr, del_sub, aug, raise_stmt, raise_from, raise_during, raise_from_none, notes, recursion, few_repeats, group, nested_group, group_with_cause, unicode_line, tabs, assertion, assertion_bare, fstring, comprehension, lambda_, with_stmt, for_stmt, import_stmt, import_from, no_message, multi_arg, custom, bad_str, system_exit, keyboard, syntax, exec_error, semicolons, long_line, call_of_call, await_like, slice_, kw_call]
for f in cases:
    show(f)
    if f is not recursion: positions(f)
