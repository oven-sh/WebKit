# Run by CPython. Goes through its own tests for pieces of source that will not compile.
import ast, doctest, os, sys, warnings
warnings.simplefilter("ignore")
root = sys.argv[1]
names = sys.argv[2:]
found = {}
def consider(source, where):
    if not isinstance(source, str) or not source.strip() or len(source) > 1500 or "\x00" in source: return
    if source in found: return
    try: compile(source, "<test>", "exec")
    except SyntaxError: found[source] = where
    except Exception: pass
def files(name):
    path = os.path.join(root, name)
    if os.path.isdir(path):
        for d, _, fs in os.walk(path):
            for f in sorted(fs):
                if f.endswith(".py"): yield os.path.join(d, f)
    else: yield path
parser = doctest.DocTestParser()
for name in names:
    for path in files(name):
        try: text = open(path, encoding="utf-8").read(); tree = ast.parse(text)
        except Exception as e: continue
        where = os.path.relpath(path, root)
        for node in ast.walk(tree):
            if isinstance(node, ast.Constant) and isinstance(node.value, str):
                s = node.value
                consider(s, where)
                # A docstring, or a string of them.
                if ">>> " in s:
                    try:
                        for example in parser.get_examples(s): consider(example.source, where)
                    except ValueError: pass
                import textwrap
                d = textwrap.dedent(s)
                if d != s: consider(d, where)
print(len(found), file=sys.stderr)
out = sorted(found)
print("# Pieces of source that CPython will not compile, out of its own tests: Lib/test/" + ", ".join(names))
print("SOURCES = [")
for s in out: print("    " + ascii(s) + ",")
print("]")
