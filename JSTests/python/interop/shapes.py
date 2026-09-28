class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y
    def norm2(self):
        return self.x * self.x + self.y * self.y
    def __repr__(self):
        return f"Point({self.x}, {self.y})"

def add(a, b=10, *, scale=1):
    return (a + b) * scale

def squares(n):
    for i in range(n):
        yield i * i

def describe(value):
    return f"{type(value).__name__}: {value!r}"

numbers = [1, 2, 3]
table = {"a": 1}
data = b"abc"
def boom():
    raise ValueError("from python")
def call_it(f, *args):
    return f(*args)
