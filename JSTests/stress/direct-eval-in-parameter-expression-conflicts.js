//@ defaultRun; run("bytecode-optimizer", "--useBytecodeOptimizer=1", "--validateBytecodeOptimizerStaticScopes=1"); run("debugger-bytecode", "--forceDebuggerBytecodeGeneration=true"); run("profilers", "--useTypeProfiler=true", "--useControlFlowProfiler=true")

// A sloppy direct eval in a parameter expression declares its vars in the environment of the called function, outside
// the environment of the parameters (ECMA-262 FunctionDeclarationInstantiation, step 20). This file is about the names
// that are in the way of such a var, about what the eval itself sees, and about the code that keeps its behavior:
// strict code, nested functions and classes, and an eval in the body.
// direct-eval-in-parameter-expression-var-scope.js has every kind of function and of eval.

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

// A parameter is in the way: the parameters are between the eval and the function's environment.
check(function (a = eval("var a = 1")) { return a; }, "throws SyntaxError");
check(function (a, b = eval("var a = 1")) { return a; }, "throws SyntaxError");
check(function (a = eval("var b = 1"), b) { return b; }, "throws SyntaxError");
check(function (a = eval("var c = 1"), { c } = {}) { return c; }, "throws SyntaxError");
check(function (a = eval("var rest = 1"), ...rest) { return rest; }, "throws SyntaxError");
check(function (a = eval("function b() {}"), b) { return typeof b; }, "throws SyntaxError");
check((a = eval("var b = 1"), b) => b, "throws SyntaxError");
check(function (a = eval("{ function b() {} }"), b) { return typeof b; }, "undefined");
check(function (a = eval("let b = 1; b"), b) { return [a, typeof b]; }, "1,undefined");

// So is `arguments`, in a function that has its own.
check(function (a = eval("var arguments = 1")) { return typeof arguments; }, "throws SyntaxError");
check(() => ({ method(a = eval("var arguments = 1")) { return typeof arguments; } }).method(), "throws SyntaxError");
check(() => (function* (a = eval("var arguments = 1")) { yield typeof arguments; })().next().value, "throws SyntaxError");
check((a = eval("var arguments = 1"), arguments) => 0, "throws SyntaxError");

// An arrow function has no `arguments`. The var is the arrow function's, not the `arguments` of the function around it.
check(function () { return [((a = eval("var arguments = 'param'")) => arguments)(), arguments.length]; }, "param,0");
check(function () { return ((a = eval("var arguments = 'param'"), b = () => arguments) => { var arguments = "local"; return [arguments, b()]; })(); }, "local,param");
check(function () { return ((a = eval("var arguments = 'param'"), b = () => arguments) => { function arguments() {} return [typeof arguments, b()]; })(); }, "function,param");
check(function () { return ((a = eval("var arguments = 'param'"), b = () => arguments) => { let arguments = "local"; return [arguments, b()]; })(); }, "local,param");

// The `arguments` of a function stays where it is.
check(function () { return [eval("delete arguments"), typeof arguments]; }, "false,object");
check(function (a = 1) { return [eval("delete arguments"), typeof arguments]; }, "false,object");
check(function () { with ({}) { return [delete arguments, typeof arguments]; } }, "false,object");
check(function () { return (() => [eval("delete arguments"), typeof arguments])(); }, "false,object");

// The eval still sees what the parameters see.
var object = {
    __proto__: { inherited: 5 },
    method(a = eval("var x = 1; this"), b = () => this) { return a === object && b() === object; },
    arrowInMethod() { return ((a = eval("var x = 1; this")) => a === object)(); },
    superInMethod(a = eval("var x = 1; super.inherited"), b = () => super.inherited) { return [a, b()]; },
};
function Constructor(a = eval("var x = 1; new.target"), b = () => new.target) { this.value = a === Constructor && b() === Constructor; }
check(() => object.method(), "true");
check(() => object.arrowInMethod(), "true");
check(() => object.superInMethod(), "5,5");
check(() => new Constructor().value, "true");
check(() => (function (a = eval("var x = 1; arguments.length"), b = () => arguments.length) { return [a, b()]; })(undefined, undefined, 2), "3,3");
check(function named(a = eval("var x = 1; typeof named")) { return a; }, "function");
check(function (a = eval("var x = 1; b"), b) {}, "throws ReferenceError");

// An eval or a later parameter that throws leaves nothing behind.
var rejects = async function (a = eval("var x = 1; throw new RangeError")) {};
function settle(promise) {
    var outcome = "pending";
    promise.then(() => { outcome = "fulfilled"; }, error => { outcome = "rejects " + error.name; });
    drainMicrotasks();
    return outcome;
}
check(function (a = eval("var x = 1; throw new RangeError")) {}, "throws RangeError");
check(function (a = eval("var x = 1"), b = (() => { throw new RangeError; })()) {}, "throws RangeError");
check(() => settle(rejects()), "rejects RangeError");

// Strict code never made a binding.
check(function () { "use strict"; return (function (a = eval("var x = 1")) { return typeof x; })(); }, "undefined");
check(() => new (class { method(a = eval("var x = 1")) { return typeof x; } })().method(), "undefined");

// An eval in a function nested in the parameters declares in that function.
check(function (a = (() => eval("var x = 1; typeof x"))(), b = typeof x) { return [a, b, typeof x]; }, "number,undefined,undefined");
check(function (a = (function () { return eval("var x = 1; typeof x"); })(), b = typeof x) { return [a, b, typeof x]; }, "number,undefined,undefined");
check(function (a = ((c = eval("var x = 1")) => typeof x)(), b = typeof x) { return [a, b, typeof x]; }, "number,undefined,undefined");
check((a = ((c = eval("var x = 1")) => typeof x)(), b = typeof x) => [a, b, typeof x], "number,undefined,undefined");

// An eval in a field or a static block of a class in the parameters is strict code of the class.
check(function (a = class { static field = eval("var x = 1; typeof x"); }.field, b = typeof x) { return [a, b, typeof x]; }, "number,undefined,undefined");
check((a = class { static value; static { this.value = eval("var x = 1; typeof x"); } }.value, b = typeof x) => [a, b, typeof x], "number,undefined,undefined");

// An eval in the heritage or in a computed key of such a class runs in the function. The code around it gets nothing.
check(function () { var f = function (a = class { [eval("var x = 1; 'key'")]() {} }) { return typeof a; }; return [f(), typeof x]; }, "function,undefined");
check(function () { var f = (a = class extends eval("var x = 1; Object") {}) => typeof a; return [f(), typeof x]; }, "function,undefined");

// An eval in the body declares in the body, as before.
check(function (a = 1) { eval("var x = 1"); return [typeof x, delete x, typeof x]; }, "number,true,undefined");
check(function (a) { eval("var x = 1"); return [typeof x, delete x, typeof x]; }, "number,true,undefined");
check((a = 1) => { eval("var x = 1"); return [typeof x, delete x, typeof x]; }, "number,true,undefined");
