//@ requireOptions("--useDollarVM=1")

// Python that has been damaged at random, and what CPython 3.14 makes of each piece: its syntax tree, or that it is not Python.
// resources/dump-ast.py says what form the tree is in.
const cases = JSON.parse(readFile("./resources/parser-cases.json", "caller relative"));
let sameMessage = 0, rejected = 0;
for (const [source, expected] of cases) {
    let actual;
    try {
        actual = $vm.pythonAST(source);
    } catch (e) {
        actual = String(e).replace(/^SyntaxError: /, "").replace(/ \((\d+):\d+\)$/, " ($1)");
    }
    if (expected.startsWith("{")) {
        if (actual !== expected)
            throw new Error(JSON.stringify(source) + " should be\n" + expected + "\nand is\n" + actual);
        continue;
    }
    if (actual.startsWith("{"))
        throw new Error(JSON.stringify(source) + " is not Python: " + expected);
    ++rejected;
    if (actual === expected)
        ++sameMessage;
}
// What is said about it is not always what CPython says, but it should not get worse.
if (sameMessage < rejected * 0.9)
    throw new Error("Only " + sameMessage + " of " + rejected + " are rejected in CPython's words");
