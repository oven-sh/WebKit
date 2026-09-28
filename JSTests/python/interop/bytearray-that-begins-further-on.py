def make(n):
    return bytearray(i & 255 for i in range(n))


def drop(a, k):
    del a[:k]


def add(a, data):
    a += bytes(data)


def look(a):
    return [len(a), a[0], a[-1], sum(a) % 1000]


def attempt(f, *arguments):
    try:
        f(*arguments)
        return "done"
    except BufferError as e:
        return "BufferError: " + str(e)
