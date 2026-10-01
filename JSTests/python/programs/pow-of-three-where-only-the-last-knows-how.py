# pow(a, b, c) asks the class of each of the three, in turn, and a class that is written in C can be the third alone
import weakref, re
address = re.compile("0x[0-9a-f]+")
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
    print(label, "->", address.sub("0x", repr(r)))
class I(float): pass # There are no weak references to an int.
class P:
    def __init__(self, v): self.v = v
    def __pow__(self, o, m=None): return ("pow", self.v, o, m)
    def __rpow__(self, o, m=None): return ("rpow", self.v, o, m)
class N:
    def __pow__(self, o, m=None): return NotImplemented
    def __rpow__(self, o, m=None): return NotImplemented
five, three, two, p, n = I(5), I(3), I(2), P(7), N()
proxies = {"5": weakref.proxy(five), "3": weakref.proxy(three), "2": weakref.proxy(two), "P": weakref.proxy(p), "N": weakref.proxy(n)}
plain = {"5": 5, "3": 3, "2": 2, "P": p, "N": n, "1.5": 1.5, "1j": 1j, "x": "x", "None": None}
for a in ("2", "P", "N", "1.5", "x"):
    for b in ("3", "P", "N", "1j"):
        for c in ("5", "P", "N", "None", "1.5"):
            for which in range(8):
                pick = [(proxies if which >> i & 1 and k in proxies else plain)[k] for i, k in enumerate((a, b, c))]
                if sum(which >> i & 1 and k not in proxies for i, k in enumerate((a, b, c))): continue
                attempt("pow(%s, %s, %s) with proxies %d" % (a, b, c, which), lambda: pow(*pick))
attempt("the special methods", lambda: (proxies["2"].__pow__(3, 5), proxies["3"].__rpow__(2, 5), proxies["2"].__pow__(3), type(proxies["2"]).__pow__(proxies["2"], proxies["3"], proxies["5"])))
attempt("of something else", lambda: type(proxies["2"]).__pow__(2, 3, proxies["5"]))
attempt("**=", lambda: [x for x in [proxies["2"]] for x in [x ** 3]])
print("done")
