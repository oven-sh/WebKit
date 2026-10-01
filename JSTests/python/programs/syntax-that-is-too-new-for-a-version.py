# ast.parse(source, feature_version=(3, n)): what came into the language later than that is a syntax error
import ast
snippets = {
    "annotated name": "x: int = 1\n", "annotated name, no value": "x: int\n", "annotated attribute": "a.b: int = 1\n", "annotated subscript": "a[0]: int\n", "annotated in parentheses": "(x): int = 1\n",
    "@": "a @ b\n", "@=": "a @= b\n", "@ twice": "a @ b @ c\n", "@ in a call": "f(a @ b, 1)\n", "a decorator is no operator": "@d\ndef f(): pass\n",
    "async def": "async def f(): pass\n", "async def with more": "x = 1\nasync def f(a, b):\n    return a\ny = 2\n", "async for": "async def f():\n    async for x in y: pass\n", "async with": "async def f():\n    async with a: pass\n", "async with in parentheses": "async def f():\n    async with (a as b, c): pass\n",
    "await": "async def f():\n    await x\n", "await alone": "await x\n", "async comprehension": "async def f():\n    [x async for x in y]\n", "async comprehension alone": "[x async for x in y]\n", "async generator expression": "(x async for x in y)\n",
    "positional only": "def f(a, /, b): pass\n", "positional only, with defaults": "def f(a=1, /, b=2): pass\n", "positional only alone": "def f(a, /): pass\n", "in a lambda": "lambda a, /, b: 0\n", "in a lambda, with defaults": "lambda a=1, /: 0\n",
    "walrus": "(x := 1)\n", "walrus in an if": "if (n := 10) > 5: pass\n", "walrus in a call": "f(x := 1)\n", "walrus in a comprehension": "[y for x in z if (y := x)]\n",
    "with in parentheses": "with (a as b, c as d): pass\n", "with in parentheses, one": "with (a as b): pass\n", "with a tuple": "with (a, b): pass\n", "with in parentheses and a comma": "with (a, b,): pass\n",
    "match": "match x:\n    case 1: pass\n", "match with more": "match x, y:\n    case [a, b] if a: pass\n    case _: pass\n", "match is a name": "match = 1\nmatch(x)\n", "match without a colon": "match x\n    case 1: pass\n",
    "except*": "try: pass\nexcept* E: pass\n", "except* as": "try: pass\nexcept* E as e: pass\nelse: pass\n",
    "type": "type X = int\n", "type with parameters": "type X[T] = list[T]\n", "type is a name": "type = 1\ntype(x)\n", "def with parameters": "def f[T](a: T): pass\n", "class with parameters": "class C[T]: pass\n", "async def with parameters": "async def f[T](): pass\n",
    "a default": "def f[T = int](): pass\n", "a default of a star": "def f[*T = int](): pass\n", "a default of two stars": "def f[**T = int](): pass\n", "a default in a class": "class C[T: int = int]: pass\n", "a default in a type": "type X[T = int] = T\n",
    "except without parentheses": "try: pass\nexcept A, B: pass\n", "except* without parentheses": "try: pass\nexcept* A, B: pass\n", "except with parentheses": "try: pass\nexcept (A, B): pass\n",
    "t-string": "t'a{b}'\n", "t-string alone": "t''\n", "t-strings together": "t'a' t'b'\n", "f-string": "f'a{b}'\n", "f-string with =": "f'{a=}'\n", "nested quotes in an f-string": "f'{a['b']}'\n",
    "underscores": "1_000\n", "underscores in a float": "1_0.0_1\n", "underscores in hex": "0x_ff\n", "underscores in an exponent": "1e1_0\n", "underscores in a complex": "1_0j\n", "no underscores": "1000\n",
    "unpacking in a return": "def f():\n    return 1, *a\n", "unpacking in a subscript": "a[*b]\n", "a star in an index": "a[1, *b]\n", "decorator expression": "@a[0].b()\ndef f(): pass\n", "dict unpacking": "{**a, 'b': 1}\n", "star in a call": "f(*a, *b)\n", "keyword after star": "f(*a, b=1)\n", "trailing comma after star": "def f(*a,): pass\n",
    "two things": "x: int = 1\nasync def f(): pass\n", "the later first": "match x:\n    case 1:\n        y: int = 1\n", "inside the newer": "async def f():\n    x: int = (y := 1)\n", "a mistake besides": "x: int = \n", "a mistake before": "x = )\ny: int\n", "a mistake after": "y: int\nx = )\n",
    "many arguments": "f(" + ", ".join("a%d" % i for i in range(300)) + ")\n", "many parameters": "def f(" + ", ".join("a%d" % i for i in range(300)) + "): pass\n",
}
def result(source, **k):
    try:
        ast.parse(source, **k)
        return "ok"
    except SyntaxError as e:
        return (e.msg, e.lineno, e.offset, e.end_lineno, e.end_offset, (e.text or "")[:30])
    except Exception as e:
        return "%s: %s" % (type(e).__name__, e)
for label, source in snippets.items():
    rows = [(n, result(source, feature_version=(3, n))) for n in range(0, 16)]
    out = []
    for n, r in rows:
        if not out or out[-1][1] != r: out.append([n, r])
    print(label, "->", out)
print("===== how it is given")
for v in (None, (3, 4), (3, 14), (3, 99), (3, 0), (3, -1), (2, 7), (4, 0), (3,), (3, 4, 5), 4, 14, -1, -5, 0, "3.4", 3.4, [3, 4], (3, "4"), (3.0, 4), True):
    print(repr(v), "->", result("async def f(): pass\n", feature_version=v))
print("===== it is only for a tree")
for flags in (0, ast.PyCF_ONLY_AST, ast.PyCF_ONLY_AST | ast.PyCF_TYPE_COMMENTS, ast.PyCF_OPTIMIZED_AST):
    for v in (-1, 4, 14, 2**31, -2**31 - 1, "4", None):
        try: r = type(compile("async def f(): pass\n", "<s>", "exec", flags, _feature_version=v)).__name__
        except Exception as e: r = "%s: %s" % (type(e).__name__, e)
        print(flags, repr(v), "->", r)
for mode in ("exec", "eval", "single", "func_type"):
    print(mode, result({"exec": "a @ b\n", "eval": "a @ b", "single": "a @ b\n", "func_type": "(a @ b) -> c"}[mode], mode=mode, feature_version=(3, 4)))
print("done")
