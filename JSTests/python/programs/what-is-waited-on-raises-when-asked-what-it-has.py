# yield from something whose __getattr__() raises: what comes of asking it for send, throw and close
import sys
class Broken:
    def __iter__(self): return self
    def __next__(self): return 1
    def __getattr__(self, attr): 1/0
    def __repr__(self): return "<Broken>"
def g(): yield from Broken()
def attempt(label, f):
    try: r = f()
    except BaseException as e: r = "%s: %s" % (type(e).__name__, e)
    print(label, "->", r)
seen = []
sys.unraisablehook = lambda u: seen.append((u.exc_type.__name__, u.err_msg, u.object))
for n, how in {"send": lambda x: x.send(1), "throw": lambda x: x.throw(AttributeError), "close": lambda x: x.close(), "throw GeneratorExit": lambda x: x.throw(GeneratorExit)}.items():
    x = g(); next(x); seen.clear()
    attempt(n, lambda: how(x)); print("  ", seen, x.gi_frame is not None, x.gi_running, x.gi_yieldfrom is not None)
print("done")
