def make(n, value):
    return bytearray([value]) * n


def grow(a, n, value):
    a += bytes([value]) * n


def shrink(a, n):
    del a[n:]


def drop(a, n):
    del a[:n]
