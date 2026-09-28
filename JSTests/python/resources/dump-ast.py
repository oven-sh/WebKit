# What CPython makes of Python source, in the form that $vm.pythonAST() gives what JavaScriptCore makes of it.
#
#     python3.14 dump-ast.py <directory to write to> < list of files
#
# For each file that is Python, and is UTF-8, writes <directory>/<n>.json and prints "<n>\t<path>".
import ast, json, struct, sys, warnings

warnings.simplefilter("ignore")
sys.setrecursionlimit(100000)

def constant(value):
    if value is None or value is True or value is False or value is Ellipsis:
        return {"c": repr(value)}
    if isinstance(value, int):
        return {"c": "int", "v": format(value, "x")}
    if isinstance(value, float):
        return {"c": "float", "v": str(struct.unpack("<Q", struct.pack("<d", value))[0])}
    if isinstance(value, complex):
        return {"c": "complex", "v": str(struct.unpack("<Q", struct.pack("<d", value.imag))[0])}
    if isinstance(value, str):
        return {"c": "str", "v": value}
    if isinstance(value, bytes):
        return {"c": "bytes", "v": value.decode("latin-1")}
    raise TypeError(type(value))

def dump(node):
    if isinstance(node, ast.AST):
        out = {"_": type(node).__name__}
        for name in node._fields:
            value = getattr(node, name, None)
            if name == "value" and isinstance(node, (ast.Constant, ast.MatchSingleton)):
                out[name] = constant(value)
            else:
                out[name] = dump(value)
        if hasattr(node, "lineno"):
            out["@"] = [node.lineno, node.col_offset, node.end_lineno, node.end_col_offset]
        return out
    if isinstance(node, list):
        return [dump(item) for item in node]
    return node

def main():
    directory = sys.argv[1]
    n = 0
    for line in sys.stdin:
        path = line.rstrip("\n")
        try:
            with open(path, "rb") as f:
                data = f.read()
            source = data.decode("utf-8")
            # What is declared to be in some other encoding is a test of that, and not of the parser.
            tree = ast.parse(data, path)
            if ast.dump(ast.parse(source, path)) != ast.dump(tree):
                continue
        except (SyntaxError, UnicodeDecodeError, ValueError, RecursionError, MemoryError):
            continue
        with open(f"{directory}/{n}.json", "w") as f:
            json.dump(dump(tree), f, separators=(",", ":"))
        print(f"{n}\t{path}")
        n += 1

main()
