# What CPython finds every name in Python source to refer to, in the form that $vm.pythonSymbolTable() gives what JavaScriptCore finds.
#
#     python3.14 dump-symtable.py <directory to write to> < "<n>\t<path>" lines, as dump-ast.py prints
import _symtable, json, sys, warnings

warnings.simplefilter("ignore")
sys.setrecursionlimit(100000)

TYPES = {
    _symtable.TYPE_FUNCTION: "function", _symtable.TYPE_CLASS: "class", _symtable.TYPE_MODULE: "module",
    _symtable.TYPE_ANNOTATION: "annotation", _symtable.TYPE_TYPE_ALIAS: "type alias",
    _symtable.TYPE_TYPE_PARAMETERS: "type parameters", _symtable.TYPE_TYPE_VARIABLE: "type variable",
}

def dump(block):
    return {
        "type": TYPES[block.type], "name": block.name, "line": block.lineno, "nested": int(block.nested),
        "symbols": dict(sorted(block.symbols.items())), "varnames": block.varnames,
        "children": [dump(child) for child in block.children],
    }

def text(source, path):
    try:
        return json.dumps(dump(_symtable.symtable(source, path, "exec")), separators=(",", ":"))
    except SyntaxError as e:
        return f"{type(e).__name__}: {e.msg} ({e.lineno})"

if __name__ == "__main__":
    for line in sys.stdin:
        n, path = line.rstrip("\n").split("\t")
        with open(path, encoding="utf-8") as f:
            source = f.read()
        with open(f"{sys.argv[1]}/{n}.json", "w") as f:
            f.write(text(source, path))
