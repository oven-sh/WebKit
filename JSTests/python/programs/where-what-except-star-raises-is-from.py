# Where a traceback says that what comes out of an except* block was raised
import traceback, sys
def show(f):
    try: f()
    except BaseException as e:
        for fs in traceback.extract_tb(e.__traceback__): print("  ", fs.name, fs.lineno, fs.end_lineno, fs.colno, fs.end_colno, repr(fs.line))
def a():
    try:
        raise Exception(42)
    except* Exception as e:
        raise
def b():
    try:
        raise ExceptionGroup("g", [ValueError(1), TypeError(2)])
    except* ValueError as e:
        raise
def c():
    try:
        raise ExceptionGroup("g", [ValueError(1), TypeError(2)])
    except* ValueError:
        pass
def d():
    try:
        raise ExceptionGroup("g", [ValueError(1)])
    except* ValueError:
        raise KeyError(5)
def e():
    try:
        raise ValueError(1)
    except ValueError:
        raise
def f():
    try:
        raise ExceptionGroup("g", [ValueError(1), TypeError(2)])
    except* ValueError:
        raise
    except* TypeError:
        raise
for g in (a, b, c, d, e, f):
    print(g.__name__); show(g)
