class C:
    pass


class E(Exception):
    pass


class S:
    __slots__ = ("a", "__dict__")


def function():
    def f(): pass
    return f


def make(kind):
    return {"C": C, "E": E, "S": S, "function": function}[kind]()


def counts(o):
    "How many attributes it says that it has, and how many there turn out to be, each way of going through them."
    d = o.__dict__
    return [len(d), len(list(d)), len(list(d.items())), len(d.keys()), len(list(reversed(d))), len(vars(o)), sum(1 for k in d)]


def put(o, name, value):
    setattr(o, name, value)


def drop(o, name):
    delattr(o, name)


def by_dict(o, name, value):
    o.__dict__[name] = value


def drop_by_dict(o, name):
    del o.__dict__[name]


def changed_while_going_through(o, change):
    try:
        for k in o.__dict__:
            change(o)
        return "nothing said"
    except RuntimeError as e:
        return str(e)
