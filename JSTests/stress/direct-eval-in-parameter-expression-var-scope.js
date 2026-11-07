//@ defaultRun; run("bytecode-optimizer", "--useBytecodeOptimizer=1", "--validateBytecodeOptimizerStaticScopes=1"); run("debugger-bytecode", "--forceDebuggerBytecodeGeneration=true"); run("profilers", "--useTypeProfiler=true", "--useControlFlowProfiler=true")

// A sloppy direct eval in a parameter expression declares its vars and functions in the environment of the called
// function, outside the environment of the parameters (ECMA-262 FunctionDeclarationInstantiation, step 20). The code
// around the function does not get them. This file has every kind of function, every place for the eval and everything
// it can declare. direct-eval-in-parameter-expression-bindings.js has which binding each name is after that,
// direct-eval-in-parameter-expression-conflicts.js has the names that are in the way, and
// direct-eval-in-parameter-expression-tiers.js has the same in code that the JIT tiers compile.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + "\n  expected: " + expected + "\n  actual:   " + actual);
}

// Compares what `run` returns, as a string. This file is global code and declares no `x`, `y` or `z`:
// one that the global object has afterwards came out of a function.
function check(run, expected) {
    shouldBe(String(run()), expected, run);
    for (const name of ["x", "y", "z"])
        shouldBe(Object.hasOwn(globalThis, name), false, "a global " + name + " after " + run);
}

// Every kind of function. The body reads the var, and so do a closure and an eval of the body.
function declaration(p = eval("var x = 1")) { return [typeof x, (() => typeof x)(), eval("typeof x")]; }
var expression = function (p = eval("var x = 1")) { return [typeof x, (() => typeof x)(), eval("typeof x")]; };
var namedExpression = function named(p = eval("var x = 1")) { return [typeof x, (() => typeof x)(), eval("typeof x")]; };
var arrow = (p = eval("var x = 1")) => { return [typeof x, (() => typeof x)(), eval("typeof x")]; };
var arrowWithExpressionBody = (p = eval("var x = 1")) => [typeof x, (() => typeof x)(), eval("typeof x")];
var fromFunctionConstructor = new Function(`p = eval("var x = 1")`, `return [typeof x, (() => typeof x)(), eval("typeof x")];`);
function Constructor(p = eval("var x = 1")) { this.seen = [typeof x, (() => typeof x)(), eval("typeof x")]; }
function* generator(p = eval("var x = 1")) { yield [typeof x, (() => typeof x)(), eval("typeof x")]; }
var seenByAsync;
async function asyncFunction(p = eval("var x = 1")) { seenByAsync = [typeof x, (() => typeof x)(), eval("typeof x")]; }
async function asyncFunctionWithAwait(p = eval("var x = 1")) { seenByAsync = [typeof x, (() => typeof x)(), eval("typeof x")]; await 0; }
var asyncArrow = async (p = eval("var x = 1")) => { seenByAsync = [typeof x, (() => typeof x)(), eval("typeof x")]; };
var asyncArrowWithAwait = async (p = eval("var x = 1")) => { seenByAsync = [typeof x, (() => typeof x)(), eval("typeof x")]; await 0; };
async function* asyncGenerator(p = eval("var x = 1")) { seenByAsync = [typeof x, (() => typeof x)(), eval("typeof x")]; yield 0; }
var object = {
    method(p = eval("var x = 1")) { return [typeof x, (() => typeof x)(), eval("typeof x")]; },
    set setter(p = eval("var x = 1")) { this.seen = [typeof x, (() => typeof x)(), eval("typeof x")]; },
    *generatorMethod(p = eval("var x = 1")) { yield [typeof x, (() => typeof x)(), eval("typeof x")]; },
    async asyncMethod(p = eval("var x = 1")) { seenByAsync = [typeof x, (() => typeof x)(), eval("typeof x")]; await 0; },
    async *asyncGeneratorMethod(p = eval("var x = 1")) { seenByAsync = [typeof x, (() => typeof x)(), eval("typeof x")]; yield 0; },
};

// An async function runs up to its first await in the call. What it saw is there when the call returns.
function afterCall(start) {
    seenByAsync = "not run";
    start();
    return seenByAsync;
}

check(declaration, "number,number,number");
check(expression, "number,number,number");
check(namedExpression, "number,number,number");
check(arrow, "number,number,number");
check(arrowWithExpressionBody, "number,number,number");
check(fromFunctionConstructor, "number,number,number");
check(() => new Constructor().seen, "number,number,number");
check(() => generator().next().value, "number,number,number");
check(() => afterCall(() => asyncFunction()), "number,number,number");
check(() => afterCall(() => asyncFunctionWithAwait()), "number,number,number");
check(() => afterCall(() => asyncArrow()), "number,number,number");
check(() => afterCall(() => asyncArrowWithAwait()), "number,number,number");
check(() => afterCall(() => asyncGenerator().next()), "number,number,number");
check(() => object.method(), "number,number,number");
check(() => (object.setter = undefined, object.seen), "number,number,number");
check(() => object.generatorMethod().next().value, "number,number,number");
check(() => afterCall(() => object.asyncMethod()), "number,number,number");
check(() => afterCall(() => object.asyncGeneratorMethod().next()), "number,number,number");

// Every place for the eval in the parameter list. A function has a scope of its own above its parameters, for `this`.
// An arrow function has none, and an arrow function whose parameters bind no name has no scope for them either.
check(function (o, p = eval("var x = 1")) { return typeof x; }, "number");
check(function ({ a = eval("var x = 1") } = {}) { return typeof x; }, "number");
check(function ([a = eval("var x = 1")] = []) { return typeof x; }, "number");
check(function ({ [eval("var x = 1; 'key'")]: a } = {}) { return typeof x; }, "number");
check(function (...[a = eval("var x = 1")]) { return typeof x; }, "number");
check(function ({} = eval("var x = 1; ({})")) { return typeof x; }, "number");
check(function (p = eval("eval('var x = 1')")) { return typeof x; }, "number");
check((o, p = eval("var x = 1")) => typeof x, "number");
check(({ a = eval("var x = 1") } = {}) => typeof x, "number");
check(([a = eval("var x = 1")] = []) => typeof x, "number");
check(({ [eval("var x = 1; 'key'")]: a } = {}) => typeof x, "number");
check((...[a = eval("var x = 1")]) => typeof x, "number");
check(({} = eval("var x = 1; ({})")) => typeof x, "number");
check((p = eval("eval('var x = 1')")) => typeof x, "number");

// Everything the eval can declare. The var can be deleted, like any other var of eval code.
check(function (p = eval("var x")) { return [typeof x, "x" in globalThis]; }, "undefined,false");
check(function (p = eval("var x = 1")) { return [delete x, typeof x]; }, "true,undefined");
check(function (p = eval("function x() {}")) { return [typeof x, delete x, typeof x]; }, "function,true,undefined");
check(function (p = eval("{ function x() {} }")) { return typeof x; }, "function");
check(function (p = eval("if (true) function x() {}")) { return typeof x; }, "function");
check(function (p = eval("for (var x = 0; x < 3; x++);")) { return x; }, "3");
check(function (p = eval("var x = 1, y = 2; function z() {}")) { return [x, y, typeof z]; }, "1,2,function");
check((p = eval("var x")) => [typeof x, "x" in globalThis], "undefined,false");
check((p = eval("var x = 1")) => [delete x, typeof x], "true,undefined");
check((p = eval("function x() {}")) => [typeof x, delete x, typeof x], "function,true,undefined");
check((p = eval("{ function x() {} }")) => typeof x, "function");
check((p = eval("if (true) function x() {}")) => typeof x, "function");
check((p = eval("for (var x = 0; x < 3; x++);")) => x, "3");
check((p = eval("var x = 1, y = 2; function z() {}")) => [x, y, typeof z], "1,2,function");

// What is not a var of the function: a let, a var of strict eval code, a call of another function named eval.
check(function (p = eval("let x = 1; x")) { return [p, typeof x]; }, "1,undefined");
check(function (p = eval("'use strict'; var x = 1; x")) { return [p, typeof x]; }, "1,undefined");
check(function (eval = source => source.length, p = eval("var x = 1")) { return [p, typeof x]; }, "9,undefined");
check((p = eval("let x = 1; x")) => [p, typeof x], "1,undefined");
check((p = eval("'use strict'; var x = 1; x")) => [p, typeof x], "1,undefined");
check((eval = source => source.length, p = eval("var x = 1")) => [p, typeof x], "9,undefined");

// An indirect eval declares a global, as before.
shouldBe(String((function (p = (0, eval)("var indirect = 1")) { return [typeof indirect, Object.hasOwn(globalThis, "indirect")]; })()), "number,true", "indirect eval");
