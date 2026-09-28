class Acc:
    def __init__(self): self.log = []
    def __repr__(self): return "Acc(" + " ".join(self.log) + ")"
def _make(name, symbol):
    def inplace(self, o): self.log.append("i" + symbol + str(o)); return self
    def plain(self, o):
        r = Acc(); r.log = self.log + [symbol + str(o)]; return r
    setattr(Acc, "__i" + name + "__", inplace); setattr(Acc, "__" + name + "__", plain)
for n, s in [("add", "+"), ("sub", "-"), ("mul", "*"), ("truediv", "/"), ("mod", "%"), ("pow", "**"), ("lshift", "<<"), ("rshift", ">>"), ("and", "&"), ("or", "|"), ("xor", "^")]:
    _make(n, s)
class OnlyPlain:
    def __init__(self, v=0): self.v = v
    def __add__(self, o): return OnlyPlain(self.v + o)
    def __radd__(self, o): return OnlyPlain(o + self.v)
    def __repr__(self): return f"OnlyPlain({self.v})"
class Declines:
    def __iadd__(self, o): return NotImplemented
    def __add__(self, o): return "fell back to __add__"
class Replaces:
    def __iadd__(self, o): return "something else"
def new_set(): return {1, 2}
def new_dict(): return {"a": 1}
set3 = {3}; set1 = {1}; dict_b = {"b": 2}
