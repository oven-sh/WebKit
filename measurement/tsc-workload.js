// App-shaped workload: the TypeScript compiler type-checks and emits a set of TS files, all in memory.
// Runs in the jsc shell (load + read) and in bun/node (require + fs).
// Usage: jsc tsc-workload.js -- [rounds]      |   bun tsc-workload.js [rounds]
var isShell = typeof load === "function" && typeof read === "function" && typeof require === "undefined";
var ROOT = "/workspace/wkbuild/workloads";
var readText, ts, log, argv, now;
if (isShell) {
    readText = function (p) { return read(p); };
    load(ROOT + "/node_modules/typescript/lib/typescript.js");
    ts = globalThis.ts;
    log = print;
    argv = (typeof arguments !== "undefined") ? arguments : [];
    now = function () { return preciseTime() * 1000; };
} else {
    var fs = require("fs");
    readText = function (p) { return fs.readFileSync(p, "utf8"); };
    ts = require(ROOT + "/node_modules/typescript/lib/typescript.js");
    log = console.log;
    argv = process.argv.slice(2);
    now = function () { return performance.now(); };
}
var rounds = parseInt(argv[0] || "1", 10);
var manifest = JSON.parse(readText(ROOT + "/tsc-inputs.json"));
var files = new Map();
for (var i = 0; i < manifest.files.length; i++)
    files.set(manifest.files[i].name, readText(manifest.files[i].path));
var libDir = ROOT + "/node_modules/typescript/lib/";
var libCache = new Map();
function getText(fileName) {
    if (files.has(fileName)) return files.get(fileName);
    if (fileName.indexOf("/lib/lib.") === 0) {
        var base = fileName.slice(5);
        if (!libCache.has(base)) {
            try { libCache.set(base, readText(libDir + base)); } catch (e) { libCache.set(base, undefined); }
        }
        return libCache.get(base);
    }
    return undefined;
}
var options = { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.ESNext, strict: true, noResolve: true, skipLibCheck: false, types: [], lib: ["lib.es2020.d.ts"], noEmitOnError: false };
var host = {
    getSourceFile: function (fileName, languageVersion) { var t = getText(fileName); return t === undefined ? undefined : ts.createSourceFile(fileName, t, languageVersion, true); },
    getDefaultLibFileName: function () { return "/lib/lib.es2020.d.ts"; },
    getDefaultLibLocation: function () { return "/lib"; },
    writeFile: function (name, text) { emitted += text.length; },
    getCurrentDirectory: function () { return "/"; },
    getCanonicalFileName: function (f) { return f; },
    useCaseSensitiveFileNames: function () { return true; },
    getNewLine: function () { return "\n"; },
    fileExists: function (f) { return getText(f) !== undefined; },
    readFile: function (f) { return getText(f); },
    directoryExists: function () { return true; },
    getDirectories: function () { return []; },
};
var emitted = 0;
var total = 0;
for (var r = 0; r < rounds; r++) {
    var t0 = now();
    emitted = 0;
    var program = ts.createProgram(Array.from(files.keys()), options, host);
    var diags = program.getSyntacticDiagnostics().length + program.getSemanticDiagnostics().length;
    program.emit();
    var t1 = now();
    total += t1 - t0;
    log("round " + r + ": " + (t1 - t0).toFixed(0) + " ms, " + diags + " diagnostics, " + emitted + " bytes emitted");
}
log("total " + total.toFixed(0) + " ms");
