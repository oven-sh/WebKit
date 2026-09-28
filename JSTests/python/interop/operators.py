class Vec:
    def __init__(self, x, y): self.x = x; self.y = y
    def __repr__(self): return f"Vec({self.x}, {self.y})"
    def __add__(self, o): return Vec(self.x + o.x, self.y + o.y) if isinstance(o, Vec) else Vec(self.x + o, self.y + o)
    def __radd__(self, o): return Vec(o + self.x, o + self.y)
    def __sub__(self, o): return Vec(self.x - o.x, self.y - o.y) if isinstance(o, Vec) else Vec(self.x - o, self.y - o)
    def __rsub__(self, o): return Vec(o - self.x, o - self.y)
    def __mul__(self, k): return Vec(self.x * k, self.y * k)
    __rmul__ = __mul__
    def __truediv__(self, k): return Vec(self.x / k, self.y / k)
    def __mod__(self, k): return Vec(self.x % k, self.y % k)
    def __pow__(self, k): return Vec(self.x ** k, self.y ** k)
    def __neg__(self): return Vec(-self.x, -self.y)
    def __invert__(self): return Vec(self.y, self.x)
    def __lshift__(self, k): return Vec(self.x << k, self.y << k)
    def __rshift__(self, k): return Vec(self.x >> k, self.y >> k)
    def __and__(self, o): return "and"
    def __or__(self, o): return "or"
    def __xor__(self, o): return "xor"
    def __eq__(self, o): return isinstance(o, Vec) and (self.x, self.y) == (o.x, o.y)
    def __lt__(self, o): return (self.x, self.y) < (o.x, o.y)
    def __le__(self, o): return (self.x, self.y) <= (o.x, o.y)
    __hash__ = None
class Plain: pass
class InPlace:
    def __init__(self): self.log = []
    def __iadd__(self, o): self.log.append(o); return self
    def __add__(self, o): return "added"
class Path:
    def __init__(self, p): self.p = p
    def __truediv__(self, o): return Path(self.p + "/" + str(o))
    def __rtruediv__(self, o): return Path(str(o) + "/" + self.p)
    def __str__(self): return self.p
class VList(list):
    def __add__(self, o): return "VList add"
    def __eq__(self, o): return "always"
    __hash__ = None
class VBytes(bytes):
    def __add__(self, o): return "VBytes add"
class Odd:
    def __lt__(self, o): return []
    def __gt__(self, o): return [0]
    def __eq__(self, o): raise ValueError("no comparing")
    __hash__ = None
a_set = {1, 2, 3}; b_set = {2, 3, 4}
a_tuple = (1, 2); b_tuple = (1, 2); c_tuple = (1, 3)
a_dict = {"a": 1}; b_dict = {"a": 1}; c_dict = {"b": 2}
z = 1 + 2j
r = range(3); range3 = range(0, 3, 1)
