a = [1]; b = a; a += [2]; a += (3,); a += "x"; print(a, b, a is b); a *= 2; print(b, a is b)
t = (1,); u = t; t += (2,); print(t, u, t is u)
s = {1, 2}; r = s; s |= {3}; s &= {2, 3}; s -= {3}; s ^= {9}; print(sorted(s), sorted(r), s is r)
d = {"a": 1}; e = d; d |= {"b": 2}; print(d, e, d is e)
n = 1; n += 1; n *= 3; n -= 1; n //= 2; n **= 3; n %= 5; n <<= 4; n |= 1; n &= 0xff; n ^= 2; n >>= 1; print(n)
f = 1.0; f += 1; f *= 2.5; f /= 2; f -= 0.5; print(f); w = "a"; w += "b"; w *= 2; print(w)
class Acc:
    def __init__(self): self.items = []
    def __iadd__(self, x): self.items.append(x); return self
class Plain:
    def __init__(self, v): self.v = v
    def __add__(self, o): return Plain(self.v + o)
x = Acc(); y = x; x += 1; x += 2; print(x.items, x is y); p = Plain(1); q = p; p += 5; print(p.v, q.v, p is q)
class H:
    def __init__(self): self.l = [0]; self.n = 0
h = H(); k = h.l; h.l += [1]; h.n += 1; print(h.l, k, h.l is k, h.n); m = [[0], 1]; z = m[0]; m[0] += [5]; m[1] += 1; print(m, z, m[0] is z)
def local(l):
    l += [9]
    return l
o = [1]; print(local(o), o)
try:
    c = [1]; c += 5
except TypeError as err: print(err)
