def some_bytes():
    return b"abc"


def bytes_in_a_tuple():
    return (b"abc", 1)


def a_tuple():
    return (1000, 2000)


def a_list():
    return [1000, 2000, 3000]


def a_frozenset(x):
    return x in {1000, 2000}


def a_slice(o):
    return o[1:2]


def a_complex():
    return 1 + 2j


def constants():
    return a_tuple.__code__.co_consts
