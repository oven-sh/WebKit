log = []
class Path:
    """Has something to say to a string, on either side."""
    def __init__(self, p): self.p = p
    def __repr__(self): return f"Path({self.p!r})"
    def __add__(self, o): log.append(f"{self!r}.__add__({o!r})"); return Path(self.p + o) if isinstance(o, str) else NotImplemented
    def __radd__(self, o): log.append(f"{self!r}.__radd__({o!r})"); return Path(o + self.p) if isinstance(o, str) else NotImplemented
class Text:
    """What it says is a string."""
    def __init__(self, t): self.t = t
    def __add__(self, o): log.append(f"Text.__add__({o!r})"); return self.t + o
    def __radd__(self, o): log.append(f"Text.__radd__({o!r})"); return o + self.t
class Plain:
    """Has nothing to say to anything."""
    def __str__(self): log.append("Plain.__str__"); return "plain"
class Declines:
    def __add__(self, o): log.append(f"Declines.__add__({o!r})"); return NotImplemented
    def __radd__(self, o): log.append(f"Declines.__radd__({o!r})"); return NotImplemented
    def __str__(self): return "declines"
class Counts:
    """What it says is a number."""
    def __add__(self, o): return len(o)
    def __radd__(self, o): return len(o)
class Raises:
    def __add__(self, o): raise ValueError("no")
    __radd__ = __add__
class InPlace:
    def __init__(self): self.parts = []
    def __iadd__(self, o): self.parts.append(o); return self
    def __repr__(self): return f"InPlace({self.parts})"
class Str(str):
    pass
class Int(int):
    pass
def took():
    r = "; ".join(log); log.clear(); return r
values = {"tuple": (1, 2), "dict": {"a": 1}, "set": {1}, "range": range(2), "complex": 1j, "bytes": b"ab", "none": None, "class": Path, "function": took, "big": 1 << 70}
