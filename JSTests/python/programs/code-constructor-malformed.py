# Bytes that are not what co_code gives, or not quite. Whatever comes of them, it is an exception or a code object that can be looked at and run.
import sys
Function = type(lambda: 0); Code = type((lambda: 0).__code__)
FIELDS = ("co_argcount", "co_posonlyargcount", "co_kwonlyargcount", "co_nlocals", "co_stacksize", "co_flags", "co_code", "co_consts", "co_names", "co_varnames", "co_filename", "co_name", "co_qualname", "co_firstlineno", "co_linetable", "co_exceptiontable", "co_freevars", "co_cellvars")
SOURCE = '''
def plain(a, b=2, /, c=3, *d, e=5, **g):
    "doc"
    return a
def closure(v):
    def inner(): return v
    return inner
def gen(n):
    yield n
async def co(): return 1
lam = lambda q, *r: q
genexp = (i for i in ())
class K:
    a: int = 1
    def m(self): return __class__
def annotated(p: int) -> bool: return True
def generic[T: int, *Ts, **P = 1](t: T) -> T: return t
class G[T = int]: pass
type Alias[T] = list[T]
'''
def walk(code):
    yield code
    for c in code.co_consts:
        if isinstance(c, Code): yield from walk(c)
codes = list(walk(compile(SOURCE, "m", "exec")))
state = 12345
def rand(n):
    global state
    state = (state * 1103515245 + 12345) & 0x7fffffff
    return (state >> 8) % n
made = failed = ran = 0
kinds = {}
def attempt(code, data):
    global made, failed, ran
    p = [getattr(code, n) for n in FIELDS]; p[6] = bytes(data)
    try:
        c = Code(*p)
    except (ValueError, TypeError, SystemError, SyntaxError, OverflowError, MemoryError) as e:
        failed += 1; kinds[type(e).__name__] = kinds.get(type(e).__name__, 0) + 1
        return
    made += 1
    for n in FIELDS: getattr(c, n)
    list(c.co_lines()); list(c.co_positions()); repr(c); hash(c); c == code
    try:
        cells = tuple(type((lambda x: lambda: x)(1).__closure__[0])(0) for _ in c.co_freevars)
        r = Function(c, {"__name__": "m"}, None, None, cells or None)(*([{}] * c.co_argcount))
        if hasattr(r, "close"): r.close()
        ran += 1
    except BaseException: pass
for code in codes:
    good = code.co_code
    for i in range(min(len(good), 90)):           # Every byte of what comes before the source, every way that is likely to matter
        for v in (0, 1, 2, 3, 5, 8, 9, 11, 0x7f, 0x80, 0xff, good[i] ^ 1, (good[i] + 1) & 0xff):
            d = bytearray(good); d[i] = v; attempt(code, d)
    for n in range(0, len(good), 3):              # Cut short
        attempt(code, good[:n] + (b"" if n % 2 == 0 else b"\0"))
    for _ in range(300):                           # At random
        d = bytearray(good)
        for _ in range(1 + rand(4)): d[rand(len(d))] = rand(256)
        attempt(code, d)
    for other in codes:                            # The head of one and the source of another
        for cut in (8, 9, 12, 16, 20, 21, 40):
            d = good[:cut] + other.co_code[cut:]
            attempt(code, d if len(d) % 2 == 0 else d + b"\0")
print("tried", made + failed, "made", made > 0, "failed", failed > 0, "ran", ran > 0, sorted(kinds))
