# What is said of source that cannot be made sense of because of the characters in it: where in a literal an escape is wrong, and half of a surrogate pair.
#
# CPython compiles UTF-8. Before it hands a literal to the codec that sees to the escapes, it writes each character that is not ASCII as \\UXXXXXXXX, which is ten bytes, and it is in those bytes that the codec says where it got to.


def attempt(f, *a):
    try:
        return f(*a)
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)


def t(label, f):
    r = attempt(f)
    print(label, "=>", r if isinstance(r, str) else ascii(r))


t("where in a literal it is said to be", lambda: [attempt(eval, s) for s in ("'\xe9\\N{NOPE}'", "'\xe9\xe9\\N{N\xe9}'", "'\U0001f600\\N{\U0001f600}'", "'a\\N{\u4e00b}c'", "'\xe9\\x4'", "'\xe9\\N{'", "'\xe9\\N'", "'\\N{a\xe9'", "'\xe9\\u12'", "f'\xe9\\N{NOPE}'", "'''\n\xe9\\N{NOPE}'''", "'''\r\n\\N{NOPE}'''", "'''\r\r\n\n\xe9\\x4'''", "b'''\r\n\\x4'''", "'\xe9\\U00110000'", "'''\\N{\r\n}'''")])
t("half of a surrogate pair in source", lambda: [attempt(f, s, *a) for s in ("'\ud800'", "x = '\udfff'", "\ud800", "'\ud800\0'", "'\0\ud800'", "'\udc00\ud800'", "# \ud800") for f, a in ((eval, ()), (exec, ()), (compile, ("<s>", "exec")))] + [eval("'\U0001f600'")])
