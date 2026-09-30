# What is at a place in a list, counted from the end or not there at all, in code that has been run often enough to be compiled for what it has been seen to do.
# A list is an array of JavaScript's, to which a place before the start or past the end is a property like another: got, it is undefined, and set, it is there from then on.


def getFromTheEnd(items, count):
    out = []
    for i in range(-count, 0):
        out.append(items[i])
    return out


def getBothWays(items, first, last):
    out = []
    for i in range(first):
        out.append(items[i])
    for i in range(-last, 0):
        out.append(items[i])
    return out


def setFromTheEnd(items, count, value):
    for i in range(-count, 0):
        items[i] = value
    return items


def getPastTheEnd(items, count):
    out = []
    try:
        for i in range(count):
            out.append(items[i])
    except IndexError as e:
        out.append(str(e))
    return out


def setPastTheEnd(items, count, value):
    try:
        for i in range(count):
            items[i] = value
    except IndexError as e:
        return items, str(e)
    return items


def getBeforeTheStart(items, count):
    out = []
    try:
        for i in range(-1, -count - 1, -1):
            out.append(items[i])
    except IndexError as e:
        out.append(str(e))
    return out


def setBeforeTheStart(items, count, value):
    try:
        for i in range(-1, -count - 1, -1):
            items[i] = value
    except IndexError as e:
        return items, str(e)
    return items


def getOne(items, i):
    return items[i]


def setOne(items, i, value):
    items[i] = value
    return items


def lastOf(items):
    return items[-1]


def swapEnds(items):
    items[0], items[-1] = items[-1], items[0]
    return items


def mayBeUnbound(items, which):
    if which:
        i = which
    try:
        return items[i]
    except (IndexError, UnboundLocalError) as e:
        return type(e).__name__


def attempt(f, *args):
    try:
        return f(*args)
    except IndexError as e:
        return "IndexError: " + str(e)


class Thing:
    def __repr__(self):
        return "thing"


thing = Thing()
kinds = {
    "ints": lambda: [1, 2, 3, 4, 5, 6],
    "strs": lambda: ["a", "b", "c", "d", "e", "f"],
    "floats": lambda: [1.5, 2.5, 3.5, 4.5, 5.5, 6.5],
    "mixed": lambda: [1, "b", 3.5, None, thing, (6,)],
    "tuple": lambda: (1, "b", 3.5, None, thing, (6,)),
}


def once(make, value, isList):
    result = [
        getFromTheEnd(make(), 2), getBothWays(make(), 3, 2), getPastTheEnd(make(), 7), getBeforeTheStart(make(), 7),
        attempt(getOne, make(), 5), attempt(getOne, make(), -6), attempt(getOne, make(), 6), attempt(getOne, make(), -7), lastOf(make()),
        mayBeUnbound(make(), -2), mayBeUnbound(make(), 9), mayBeUnbound(make(), 0),
    ]
    if isList:
        result += [
            setFromTheEnd(make(), 2, value), setPastTheEnd(make(), 7, value), setBeforeTheStart(make(), 7, value),
            attempt(setOne, make(), 5, value), attempt(setOne, make(), -6, value), attempt(setOne, make(), 6, value), attempt(setOne, make(), -7, value), swapEnds(make()),
        ]
    return repr(result)


for name, make in kinds.items():
    for value in (0, "x"):
        first = once(make, value, name != "tuple")
        print(name, repr(value), first)
        different = None
        for turn in range(6000):
            again = once(make, value, name != "tuple")
            if again != first:
                different = (turn, again)
                break
        print("    the same every time" if different is None else "    DIFFERENT at %d: %s" % different)

# And when it has only ever been seen to be within the list, until it is not.
for name, make in kinds.items():
    items = make()
    for turn in range(20000):
        getOne(items, turn % 6)
        if name != "tuple":
            setOne(items, turn % 6, items[turn % 6])
    print(name, [attempt(getOne, items, i) for i in (-1, -6, -7, 6, 100, -100)], [attempt(setOne, make(), i, 0) for i in (-1, -6, -7, 6, 100, -100)] if name != "tuple" else "")
    print("   ", len(items), items == make())
