# Whatever is told of a frame, in a good deal of the library, f_lineno is the line that co_lines() has for f_lasti. CPython's tests of tracing look at every frame that they are told of.
import sys, warnings, json, textwrap, collections, re, argparse, string, fractions, decimal, difflib, pprint, dataclasses, enum, typing, ast, tokenize, io
bad = collections.Counter(); total = 0; examples = {}
def trace(frame, event, arg):
    global total
    total += 1
    last = "none"
    for s, e, l in frame.f_code.co_lines():
        if s <= frame.f_lasti < e: last = l
    if last != frame.f_lineno:
        key = (frame.f_code.co_filename.rsplit("/", 1)[-1], frame.f_lineno, event)
        bad[key] += 1
        examples.setdefault(key, (last, frame.f_lasti, frame.f_code.co_name))
    return trace
def work():
    with warnings.catch_warnings(): warnings.simplefilter("ignore")
    json.loads(json.dumps({"a": [1, 2, {"b": None}], "c": "d"}, indent=2))
    textwrap.fill("hello world " * 20, 30)
    re.compile(r"(a|b)*c+[d-f]{2,3}(?P<x>\w+)").match("abccdexyz")
    p = argparse.ArgumentParser(); p.add_argument("--x", type=int, default=1); p.parse_args(["--x", "3"])
    fractions.Fraction(3, 4) + fractions.Fraction("1/3")
    decimal.Decimal("1.5") * decimal.Decimal("2.25")
    list(difflib.unified_diff(["a", "b"], ["a", "c"]))
    pprint.pformat({"a": list(range(30)), "b": {"c": (1, 2)}})
    @dataclasses.dataclass
    class D: x: int = 1; y: str = "a"
    repr(D()); D() == D()
    class E(enum.Enum): A = 1; B = 2
    E(1); E["B"]
    ast.unparse(ast.parse("def f(a, *b, c=1):\n    return [x for x in a if x]\n"))
    list(tokenize.generate_tokens(io.StringIO("x = 1 + 2\n").readline))
    string.Template("$a b").substitute(a=1)
    typing.get_type_hints(D)
sys.settrace(trace); work(); sys.settrace(None)
print(total > 10000, sum(bad.values()), "where the line is not the one that co_lines() has;", len(bad), "places")
for key, n in bad.most_common(25): print(n, key, examples[key])
