# What is made of nothing but constants is worked out when the code is compiled. It has to come to what it would have come to if it had been run, or not be worked out. So each operator is tried between each two of a good
# many constants twice over: written out, and by way of variables, of which nothing can be worked out beforehand.
import _warnings

_warnings.filters.insert(0, ("ignore", None, Warning, None, 0))

VALUES = ["0", "1", "2", "3", "7", "10", "63", "64", "127", "128", "255", "256", "1000", "2147483647", "2147483648", "4294967296", "9007199254740992", "9007199254740993", "9223372036854775807", "9223372036854775808", "18446744073709551615",
          "18446744073709551616", "170141183460469231731687303715884105727", "170141183460469231731687303715884105728", "340282366920938463463374607431768211455", "340282366920938463463374607431768211456", "0xffffffffffffffffffffffffffffffffffffffff",
          "True", "False", "None", "...", "0.0", "1.0", "0.5", "1.5", "2.5", "1e16", "1e308", "5e-324", "1e-300", "0.1", "3.0", "1e400", "0j", "1j", "2.5j", "1e308j", "''", "'a'", "'ab'", "'\\xe9'", "'\\u4e2d'", "'\\U0001f600x'", "'\\ud83d'",
          "'abcdefghij'", "b''", "b'a'", "b'ab\\xff'", "()", "(1,)", "(1, 2)", "((1, 2), 'a')", "(1, (2, (3, (4,))))", "(b'a', 1)", "(None, ..., True)"]
NEGATED = ["-" + v for v in VALUES if v[0].isdigit()]
BINARY = ["+", "-", "*", "/", "//", "%", "**", "<<", ">>", "&", "|", "^", "@"]
UNARY = ["-", "+", "~", "not "]


def show(f):
    try:
        r = f()
        text = repr(r)
    except BaseException as e:
        return type(e).__name__ + ": " + str(e)
    return type(r).__name__ + ":" + (text if len(text) < 200 else "%s...%s (%d)" % (text[:60], text[-60:], len(text)))


def too_much(op, a, b):
    # What would take up all the room that there is, or all day.
    big = lambda v: v.lstrip("-")[0].isdigit() and "." not in v and "e" not in v and "j" not in v and len(v.lstrip("-")) > 4
    return (op in ("**", "<<") and big(b)) or (op == "*" and (big(a) or big(b)) and not (a.lstrip("-")[0].isdigit() and b.lstrip("-")[0].isdigit()))


total = 0
checksum = 0
wrong = []


def note(source, written, run):
    global total, checksum
    total += 1
    if written != run:
        wrong.append((source, written, run))
    for c in source + written:
        checksum = (checksum * 31 + ord(c)) % 1000000007


everything = VALUES + NEGATED
for op in BINARY:
    by_variables = eval("lambda a, b: a %s b" % op)
    for a in everything:
        for b in everything:
            if too_much(op, a, b):
                continue
            source = "(%s) %s (%s)" % (a, op, b)
            x, y = eval("[%s]" % a)[0], eval("[%s]" % b)[0]
            note(source, show(eval("lambda: " + source)), show(lambda: by_variables(x, y)))
    print(op, total, checksum, wrong[:3])
for op in UNARY:
    by_variables = eval("lambda a: %sa" % op)
    for a in everything + ["-(%s)" % v for v in NEGATED] + ["not " + v for v in VALUES]:
        x = eval("[%s]" % a)[0]
        note(op + a, show(eval("lambda: %s(%s)" % (op, a))), show(lambda: by_variables(x)))
print("unary", total, checksum, wrong[:3])
by_variables = lambda a, b: a[b]
for a in everything:
    for b in ["0", "1", "2", "3", "9", "10", "-1", "-2", "-3", "-11", "True", "False", "None", "0.0", "'a'", "()", "18446744073709551616", "-18446744073709551616"]:
        x, y = eval("[%s]" % a)[0], eval("[%s]" % b)[0]
        note("(%s)[%s]" % (a, b), show(eval("lambda: (%s)[%s]" % (a, b))), show(lambda: by_variables(x, y)))
print("subscript", total, checksum, wrong[:3])

# One thing in another.
for source in ("1 + 2 * 3 - 4", "(1 + 2) * (3 - 4) ** 2", "2 ** 10 ** 0", "-2 ** 2", "(-2) ** 2", "-(-(-1))", "~-1", "not not 1", "not not ()", "1 + 2j - 3 + 4.5", "-1j", "-0j", "0 - 0j", "-0.0 + 0j", "-0.0 - 0j", "1 - 1j - 1", "1e308 * 10 - 1e308 * 10",
               "'a' + 'b' * 3 + 'c'", "('a' 'b') * 2", "(1, 2) + (3,) * 2", "((1, 2) + (3, 4))[2]", "((1, 2), (3, 4))[1][0]", "'abc'[1] * 3", "b'abc'[1] + 1", "(1, 2)[True]", "60 * 60 * 24 * 365", "1 << 10 << 10", "1 << 127", "1 << 128", "2 ** 127", "2 ** 128",
               "-2 ** 127", "(2 ** 64) * (2 ** 63)", "(2 ** 64) * (2 ** 64)", "7 // 2", "-7 // 2", "7 // -2", "-7 // -2", "7 % 3", "-7 % 3", "7 % -3", "-7 % -3", "7 / 2", "1 / 3", "-1 / 3", "2 ** -1", "0 ** 0", "0 ** 5", "1 / 0", "1 // 0", "1 % 0", "1.0 / 0", "0 ** -1",
               "1 << -1", "1 >> -1", "-1 >> 200", "5 >> 200", "-5 >> 1", "True + True", "True & False", "True | 2", "True ^ True", "~True", "-True", "+True", "'a' * -1", "'' * 10 ** 9", "() * 10 ** 9", "'a' * 4096", "'a' * 4097", "(1,) * 256", "(1,) * 257",
               "((1, 2, 3, 4),) * 256", "'\\U0001f600a'[1]", "'\\U0001f600a'[-1]", "'%s' % 1", "'%d %d' % (1, 2)", "b'%d' % 1", "1 if 2 else 3", "1 and 2", "0 or 3", "1 < 2", "1 == 1.0", "1 is 1", "(1, 2) < (1, 3)",
               "1 in (1, 2)", "1 in [1, 2]", "1 in {1, 2}", "3 not in [1, 2]", "[1, 2, 3]", "{1, 2, 3} == {3, 2, 1}", "[-1, 2.5, 'a', (1, 2), None]", "[(1, 2), (1, 2), (1, 2)]", "[x for x in [1, 2, 3]]", "[x for x in {1}]", "sorted({1, 1.0, True, 2})", "[b'a', b'b', b'c']"):
    note(source, show(eval("lambda: " + source)), show(eval("lambda: " + source)))
    print(source, "=>", show(eval("lambda: " + source)))
print("in all", total, checksum, "that are otherwise when they are written out:", wrong)

# What has something to say when it is run is left to be run, so that it says it.
_warnings.filters[:] = [("error", None, Warning, None, 0)]
for source in ("~True", "~False", "-True", "~1"):
    print(source, "=>", show(eval("lambda: " + source)))
