//@ requireOptions("--useDollarVM=1")
// (defaultRun without its bytecode cache mode: what $vm.evaluateInModuleLoaderScope() evaluates is a string, which
// has no cache, and that mode requires every program to come from one.)
//@ runDefault
//@ runDefault("--useRunOnceCodeRelease=0")
//@ runDefault("--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=100")
//@ runMiniMode
//@ runNoLLInt
//@ runNoCJITValidatePhases
//@ runDFGEager
//@ runNoFTL
//@ runFTLEager
//@ runFTLNoCJITValidate

// A program evaluated in a scope of its own (JSC::evaluateInScope: here a module loader's module scope, which holds
// the loader's bindings) resolves that scope's variables as closure variables, in every tier, where the same source
// evaluated in the global scope resolves globals. The baseline code cached on unlinked code assumes one of the two, so
// the scoped program's code is its own; programs of the same source in scopes with the same symbol tables share it, and
// each run's functions close over the scope of their own run.

function assert(condition, message) {
    if (!condition)
        throw new Error("assertion failed: " + message);
}

globalThis.who = "global";
globalThis.onlyGlobal = "only global";
let lexical = "global lexical";

// What a CommonJS loader evaluates: a function expression, called later with the module's values.
const wrapper = `(function (exports, tag) {
    exports.who = function () { return who; };
    exports.setWho = function (value) { who = value; };
    exports.both = function () { return who + "/" + onlyGlobal + "/" + lexical + "/" + tag; };
    exports.typeofMissing = function () { return typeof missingEverywhere; };
    exports.nested = function () { return (() => [1].map(() => who)[0])(); };
})`;
const url = "program-in-module-loader-scope/wrapper.js";

function instantiate(holder, tag, from = url) {
    const exports = {};
    const fn = $vm.evaluateInModuleLoaderScope(holder, wrapper, from);
    fn(exports, tag);
    return exports;
}

const n = 2 * testLoopCount;
function warmUp(f) {
    let last;
    for (let i = 0; i < n; ++i)
        last = f();
    return last;
}

const first = $vm.createModuleLoader({ who: "first" });
const second = $vm.createModuleLoader({ who: "second" }, first); // the same symbol table as first's
const other = $vm.createModuleLoader({ who: "other", extra: 1 }); // another shape
const plain = $vm.createModuleLoader(); // no module scope of its own: its programs run in the global scope

// The global-scope program first, tiered up, so that code for the global resolution exists (in the code cache, with
// baseline code on it) before any scoped run of the same source.
const host = instantiate(plain, "host");
assert(warmUp(host.who) === "global", "host reads the global");
assert(warmUp(host.both) === "global/only global/global lexical/host", "host reads globals");

const a = instantiate(first, "a");
assert(warmUp(a.who) === "first", "a reads its scope's binding");
assert(warmUp(a.both) === "first/only global/global lexical/a", "a reads its scope, then the global scope");
assert(warmUp(a.nested) === "first", "closures inside a's functions resolve through the scope too");
assert(a.typeofMissing() === "undefined", "an unresolvable name is still unresolvable");

// The same source in a scope with the same symbol table: shared code, its own scope.
const b = instantiate(second, "b");
assert(b.who() === "second", "b reads its own scope on its first call");
assert(warmUp(b.who) === "second", "b reads its own scope once optimized");
assert(warmUp(b.both) === "second/only global/global lexical/b", "b reads its scope, then the global scope");
assert(warmUp(a.who) === "first", "a still reads its own after b ran the shared code");

// Writes go to the scope, not to the global object, and not to the other scope.
a.setWho("first, changed");
assert(warmUp(a.who) === "first, changed" && warmUp(b.who) === "second" && globalThis.who === "global", "a write stays in its scope");

// Another shape.
const c = instantiate(other, "c");
assert(warmUp(c.who) === "other", "c reads its scope");

// The host is untouched by all of it, and a later global-scope program still resolves globals.
assert(warmUp(host.who) === "global", "host still reads the global");
const hostAgain = instantiate(plain, "host again");
assert(warmUp(hostAgain.both) === "global/only global/global lexical/host again", "a later global-scope program reads globals");

// The other order, which is the one that matters: baseline code made for the closure resolution does not check what
// it runs under, so a global-scope program must not get the code a scoped program of the same source ran first.
{
    const scopedFirst = instantiate(first, "scoped first", "program-in-module-loader-scope/scoped-first.js");
    assert(warmUp(scopedFirst.both) === "first, changed/only global/global lexical/scoped first", "the scoped program, optimized");
    const globalSecond = instantiate(plain, "global second", "program-in-module-loader-scope/scoped-first.js");
    assert(globalSecond.who() === "global", "the global-scope program reads the global on its first call");
    assert(warmUp(globalSecond.both) === "global/only global/global lexical/global second", "the global-scope program, optimized");
    globalSecond.setWho("global, changed");
    assert(globalThis.who === "global, changed" && scopedFirst.who() === "first, changed", "its write goes to the global object");
    globalThis.who = "global";
}

// After all code is deleted and collected, the next scoped run has its code generated again.
$vm.deleteAllCodeWhenIdle();
fullGC();
const e = instantiate($vm.createModuleLoader({ who: "third" }, first), "e");
assert(warmUp(e.who) === "third" && warmUp(a.who) === "first, changed", "scoped programs run after their code was deleted");

// Declarations at the top level of a scoped program are the global object's, as for any program.
$vm.evaluateInModuleLoaderScope(first, "var declaredByScopedProgram = who; function declaredFunction() { return who; }", "program-in-module-loader-scope/declarations.js");
assert(globalThis.declaredByScopedProgram === "first, changed", "a scoped program's var is a global");
assert(globalThis.declaredFunction() === "first, changed", "a scoped program's function declaration is a global that closes over the scope");

// A syntax error and a thrown exception come out as exceptions of the evaluation.
let threw = 0;
try { $vm.evaluateInModuleLoaderScope(first, "(function () { return who +; })", "program-in-module-loader-scope/syntax.js"); } catch (error) { threw += error instanceof SyntaxError; }
try { $vm.evaluateInModuleLoaderScope(first, "throw new RangeError(who)", "program-in-module-loader-scope/throws.js"); } catch (error) { threw += error instanceof RangeError && error.message === "first, changed"; }
assert(threw === 2, "errors are thrown to the caller");
