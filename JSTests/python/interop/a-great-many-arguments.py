def rest(*a):
    return len(a), a[:2], a[-2:]


def two(x, y):
    return x, y


class Plain:
    def __init__(self, *a):
        self.count = len(a)

    def method(self, *a):
        return len(a)

    def __call__(self, *a):
        return len(a)


def outcome(f, *args):
    try:
        return f(*args)
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)[:70]


# Python gives them to what is JavaScript's.
def give(function, n):
    return outcome(lambda: function(*range(n)))


def give_by_name(function, n):
    return outcome(lambda: function(*range(n), last=1))


def make(constructor, n):
    return outcome(lambda: constructor(*range(n)).count)


def derive(base):
    class Derived(base):
        def __init__(self, *a):
            super().__init__(*a)
    return Derived
