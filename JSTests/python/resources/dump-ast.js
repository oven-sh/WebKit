// The other half of dump-ast.py: jsc --useDollarVM=1 dump-ast.js -- <index file> <directory to write to>
// For each "<n>\t<path>" of the index, writes what JavaScriptCore makes of the file to <directory>/<n>.json, or why it makes nothing of it.
const [index, directory] = arguments;
for (const line of readFile(index).split("\n")) {
    if (!line)
        continue;
    const [n, path] = line.split("\t");
    let out;
    try {
        out = $vm.pythonAST(readFile(path));
    } catch (e) {
        out = String(e);
    }
    writeFile(directory + "/" + n + ".json", out);
}
