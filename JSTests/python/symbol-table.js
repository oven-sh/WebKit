//@ requireOptions("--useDollarVM=1")

// Python whose names have been meddled with, and what CPython 3.14 finds each name to refer to, or which rule it finds broken.
// resources/dump-symtable.py says what form that is in.
const cases = JSON.parse(readFile("./resources/symbol-table-cases.json", "caller relative"));
for (const [source, expected] of cases) {
    let actual;
    try {
        actual = $vm.pythonSymbolTable(source);
    } catch (e) {
        actual = String(e).replace(/^SyntaxError: /, "").replace(/ \((\d+):\d+\)$/, " ($1)");
    }
    if (actual !== expected)
        throw new Error(source + "\nshould be\n" + expected + "\nand is\n" + actual);
}
