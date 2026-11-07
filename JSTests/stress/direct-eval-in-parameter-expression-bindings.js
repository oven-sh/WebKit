//@ defaultRun; run("bytecode-optimizer", "--useBytecodeOptimizer=1", "--validateBytecodeOptimizerStaticScopes=1"); run("debugger-bytecode", "--forceDebuggerBytecodeGeneration=true"); run("profilers", "--useTypeProfiler=true", "--useControlFlowProfiler=true")

// A sloppy direct eval in a parameter expression declares its vars in the environment of the called function, outside
// the environment of the parameters (ECMA-262 FunctionDeclarationInstantiation, step 20). This file is about which
// binding each name is after that: who sees the var, and what the body and the code around the function keep.
// direct-eval-in-parameter-expression-var-scope.js has every kind of function and of eval, and
// direct-eval-in-parameter-expression-conflicts.js has the names that are in the way of such a var.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + "\n  expected: " + expected + "\n  actual:   " + actual);
}

// Compares what `run` returns, as a string. This file is global code and declares no `x`:
// one that the global object has afterwards came out of a function.
function check(run, expected) {
    let actual;
    try {
        actual = String(run());
    } catch (error) {
        actual = "throws " + error.name;
    }
    shouldBe(actual, expected, run);
    shouldBe(Object.hasOwn(globalThis, "x"), false, "a global x after " + run);
}

// Who sees the var: every parameter expression, also a closure made before the eval ran, and the body.
check(function (a = () => typeof x, b = eval("var x = 1")) { return a(); }, "number");
check(function (a = eval("var x = 1"), b = () => typeof x) { return b(); }, "number");
check(function (a = eval("var x = 1"), b = x) { return b; }, "1");
check(function (a = eval("var x = 1"), b = eval("x")) { return b; }, "1");
check(function (a = eval("var x = 1"), b = eval("var x; x")) { return b; }, "1");
check(function (a = eval("var x = 1"), b = eval("var x = 2"), c = () => x) { return [c(), x]; }, "2,2");
check(function (a = eval("var x = 1"), b = v => { x = v; }) { b(7); return x; }, "7");
check(function (a = eval("var x = 1"), b = () => x) { x = 5; return [b(), x]; }, "5,5");
check(function (a = eval("var x = 1; () => x")) { x = 3; return a(); }, "3");
check((a = () => typeof x, b = eval("var x = 1")) => a(), "number");
check((a = eval("var x = 1"), b = () => x) => { x = 5; return [b(), x]; }, "5,5");

// The body's own declaration of the name is another binding.
check(function (a = eval("var x = 1"), b = () => x) { var x = 2; return [b(), x]; }, "1,2");
check(function (a = eval("var x = 1"), b = () => x) { var before = [b(), typeof x]; var x = 2; return before; }, "1,undefined");
check(function (a = eval("var x = 1"), b = () => x) { let x = 2; return [b(), x]; }, "1,2");
check(function (a = eval("var x = 1"), b = () => x) { function x() {} return [b(), typeof x]; }, "1,function");
check(function (a = eval("var x = 1"), b = () => x) { eval("var x = 2"); return [b(), x]; }, "1,2");
check(function (a = eval("var x = 1"), b = () => x) { eval("var x = 2"); var seen = [x]; delete x; seen.push(x, b()); return seen; }, "2,1,1");
check((a = eval("var x = 1"), b = () => x) => { var x = 2; return [b(), x]; }, "1,2");

// Every call has its own var.
function freshInFunction(a = eval("var x"), b = () => typeof x) { var before = b(); x = 9; return before; }
var freshInArrow = (a = eval("var x"), b = () => typeof x) => { var before = b(); x = 9; return before; };
function recursive(n, a = eval("var x = n"), b = () => x) { return n ? [b()].concat(recursive(n - 1)) : [b()]; }
function sometimes(declare, a = declare && eval("var x = 1")) { return typeof x; }
check(() => [freshInFunction(), freshInFunction()], "undefined,undefined");
check(() => [freshInArrow(), freshInArrow()], "undefined,undefined");
check(() => recursive(2), "2,1,0");
check(() => [sometimes(true), sometimes(false)], "number,undefined");

// The code around the function keeps its own bindings and gets no new one.
check(function () { var x = "outer"; function f(a = eval("var x = 'inner'"), b = () => x) { return b(); } return [f(), x]; }, "inner,outer");
check(function () { var x = "outer"; var f = (a = eval("var x = 'inner'"), b = () => x) => b(); return [f(), x]; }, "inner,outer");
check(function () { var x = "outer"; function read() { return x; } function f(a = eval("var x = 'inner'"), b = () => x) { return [b(), read()]; } return f(); }, "inner,outer");
check(function () { var object = { x: "with" }; with (object) { var f = function (a = eval("var x = 'inner'"), b = () => x) { return b(); }; return [f(), object.x]; } }, "inner,with");
check(function () { try { throw "outer"; } catch (x) { var f = function (a = eval("var x = 'inner'"), b = () => x) { return b(); }; return [f(), x]; } }, "inner,outer");
check(function () { function f(a = eval("var x = 1")) {} f(); return typeof x; }, "undefined");
check(function () { eval(""); function f(a = eval("var x = 1")) {} f(); return typeof x; }, "undefined");
check(() => { var g = (a = eval("var x = 1")) => 0; g(); return typeof x; }, "undefined");

var globalVar = "global";
function readGlobalVar() { return globalVar; }
function overGlobalVar(a = eval("var globalVar = 'inner'"), b = () => globalVar) { return b(); }
check(() => [overGlobalVar(), globalVar, readGlobalVar()], "inner,global,global");

// A lexical declaration around the function is not in the way.
check(function () { let x = "outer"; function f(a = eval("var x = 'inner'"), b = () => x) { return b(); } return [f(), x]; }, "inner,outer");
check(function () { const x = "outer"; var f = (a = eval("var x = 'inner'"), b = () => x) => b(); return [f(), x]; }, "inner,outer");
check(function () { let x = "outer"; var read = () => x; function f(a = eval("var x = 'inner'")) { return read(); } return [f(), x]; }, "outer,outer");
check(function () { { let x = "outer"; var f = function (a = eval("var x = 'inner'"), b = () => x) { return b(); }; return [f(), x]; } }, "inner,outer");

let globalLexical = "global";
var overGlobalLexical = (a = eval("var globalLexical = 'inner'"), b = () => globalLexical) => b();
check(() => [overGlobalLexical(), globalLexical], "inner,global");

// A function between the called function and the global code gets no var, and the global binding keeps its value.
const globalConst = "global";
check(function () { function f(a = eval("var globalVar = 'inner'"), b = () => globalVar) { return b(); } return [f(), globalVar, readGlobalVar()]; }, "inner,global,global");
check(function () { var f = (a = eval("var globalLexical = 'inner'"), b = () => globalLexical) => b(); return [f(), globalLexical]; }, "inner,global");
check(function () { function f(a = eval("var globalConst = 'inner'"), b = () => globalConst) { return b(); } return [f(), globalConst, (() => globalConst)()]; }, "inner,global,global");
check(() => [globalVar, globalLexical, globalConst], "global,global,global");

// The name of the function is outside the function's environment: no conflict, and the var hides it.
function declared(a = eval("var declared = 1")) { return declared; }
check(function named(a = eval("var named = 1")) { return named; }, "1");
check(() => [declared(), typeof declared], "1,function");
