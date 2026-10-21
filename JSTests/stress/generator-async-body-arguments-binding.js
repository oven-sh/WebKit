//@ defaultRun; run("debugger-bytecode", "--forceDebuggerBytecodeGeneration=true"); run("bytecode-optimizer", "--useBytecodeOptimizer=true"); run("profilers", "--useTypeProfiler=true", "--useControlFlowProfiler=true")
// A generator, or an async function that has an await, runs as two functions: a wrapper, which holds the parameters and
// `arguments`, and a body. A declaration in the body, or in a direct eval in the body, that has the name `arguments` or the
// name of a parameter has to see the binding of the wrapper as a binding of the same function.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

globalThis.describe = function (value) {
    if (typeof value === "function")
        return "function " + value.name;
    if (Object.prototype.toString.call(value) === "[object Arguments]")
        return "arguments(" + Array.prototype.join.call(value) + ")";
    return String(value);
};

// Each entry makes the source of a call that gives what the body returns, or a promise of it.
const generator = {
    "function*": (parameters, body, args) => `(function* (${parameters}) { ${body} })(${args}).next().value`,
};
const asyncKinds = {
    "async function*": (parameters, body, args) => `(async function* (${parameters}) { ${body} })(${args}).next().then(result => result.value)`,
    "async function": (parameters, body, args) => `(async function (${parameters}) { await 1; ${body} })(${args})`,
};
// The parser treats these like the three above.
const methodsAndConstructors = {
    "generator method": (parameters, body, args) => `({ *method(${parameters}) { ${body} } }).method(${args}).next().value`,
    "async method": (parameters, body, args) => `({ async method(${parameters}) { await 1; ${body} } }).method(${args})`,
    "GeneratorFunction": (parameters, body, args) => `new (function* () {}).constructor(${JSON.stringify(parameters)}, ${JSON.stringify(body)})(${args}).next().value`,
    "AsyncGeneratorFunction": (parameters, body, args) => `new (async function* () {}).constructor(${JSON.stringify(parameters)}, ${JSON.stringify(body)})(${args}).next().then(result => result.value)`,
    "AsyncFunction": (parameters, body, args) => `new (async function () {}).constructor(${JSON.stringify(parameters)}, ${JSON.stringify("await 1; " + body)})(${args})`,
};
// An async arrow function has parameters of its own, and the `arguments` of the function around it.
const asyncArrow = {
    "async arrow function": (parameters, body, args) => `(async (${parameters}) => { await 1; ${body} })(${args})`,
};

// The rows are [parameters, arguments, body, what the body returns]. V8 gives the same results.
// The eager JIT modes compile the builtins of async functions when they run often, so only a generator gets every row.

const inEveryKind = [
    // `var arguments` is the binding that holds the arguments object. A function in a block is hoisted to it, and it
    // gets the function when the block runs.
    ["", "1, 2", `var length = arguments.length; var arguments; var before = describe(arguments); { function arguments() {} } return length + "; " + before + "; " + describe(arguments);`, "2; arguments(1,2); function arguments"],
];

const inAsyncKinds = [
    // A direct eval reads `arguments` when the function does not name it. A direct eval that declares the name of a
    // parameter declares no new binding.
    ["a, b", "1, 2", `var before = describe(eval("arguments")); eval("var a; function b() {} function z() {}"); return before + "; " + describe(a) + "; " + describe(b) + "; " + describe(z);`, "arguments(1,2); 1; function b; function z"],
    // With parameter expressions, the var of the body is a binding of its own that starts with the arguments object, or
    // with the value of the parameter. A closure in the parameters keeps the first binding.
    ["a, f = () => a, g = () => arguments", "1", `var arguments = 5, a = 6; return describe(f()) + "; " + describe(g()) + "; " + describe(arguments) + "; " + describe(a);`, "1; arguments(1); 5; 6"],
];

const inAsyncArrow = [
    ["a, b", "1, 2", `eval("var a; function b() {} function z() {}"); return describe(a) + "; " + describe(b) + "; " + describe(z);`, "1; function b; function z"],
    ["a = 1", "", `eval("var a"); return describe(a);`, "undefined"],
    ["a = 1", "", `var a; eval("var a"); return describe(a);`, "1"],
];

const inAGenerator = [
    // A function in a block.
    ["", "1, 2", `if (false) { function arguments() {} } return describe(arguments);`, "arguments(1,2)"],
    ["", "1, 2", `switch (1) { case 2: function arguments() {} } return arguments.length;`, 2],
    ["", "1, 2", `var read = () => describe(arguments); var before = read(); if (true) function arguments() {} return before + "; " + read();`, "arguments(1,2); function arguments"],
    ["", "1, 2", `with ({ arguments: 3 }) { { function arguments() {} } } return describe(arguments);`, "function arguments"],
    ["", "1, 2", `try { throw 1; } catch (arguments) { { function arguments() {} } } return describe(arguments);`, "function arguments"],
    ["", "1, 2", `{ function arguments() {} } return describe(delete arguments) + "; " + describe(arguments);`, "false; function arguments"],
    ["", "1, 2", `{ let arguments = 1; { function arguments() {} } } return describe(arguments);`, "arguments(1,2)"],

    // `var arguments`.
    ["", "1, 2", `var arguments; return describe(arguments);`, "arguments(1,2)"],
    ["", "1, 2", `var arguments = 5; return describe(arguments);`, "5"],
    ["", "1, 2", `var same = (() => arguments)() === arguments; var arguments; return same;`, true],
    ["a, b", "1, 2", `var arguments; a = 9; return describe(arguments);`, "arguments(9,2)"],
    ["a, b", "1, 2", `var arguments; eval("var a = 5"); return describe(arguments);`, "arguments(5,2)"],

    // A direct eval reads `arguments`.
    ["", "1, 2", `return describe(eval("arguments"));`, "arguments(1,2)"],
    ["", "1, 2", `"use strict"; return eval("arguments.length");`, 2],
    ["a", "1, 2", `return describe((() => eval("arguments"))());`, "arguments(1,2)"],
    ["", "1, 2", `eval("arguments = 5"); return describe(arguments);`, "5"],
    ["", "1, 2", `return describe(eval("delete arguments")) + "; " + describe(arguments);`, "false; arguments(1,2)"],

    // A direct eval declares `arguments`. There is one binding, and it keeps its value.
    ["", "1, 2", `eval("var arguments"); return describe(arguments);`, "arguments(1,2)"],
    ["", "1, 2", `var arguments = 3; eval("var arguments"); return describe(arguments);`, "3"],
    ["", "1, 2", `eval("if (false) { function arguments() {} }"); return describe(arguments);`, "arguments(1,2)"],
    ["", "1, 2", `eval("{ function arguments() {} }"); return describe(arguments);`, "function arguments"],
    ["", "1, 2", `eval("function arguments() {}"); return describe(arguments);`, "function arguments"],
    ["", "1, 2", `{ function arguments() {} } eval("var arguments"); return describe(arguments);`, "function arguments"],

    // These have a binding of their own in the body.
    ["", "1, 2", `function arguments() {} return describe(arguments);`, "function arguments"],
    ["", "1, 2", `let arguments = 1; return describe(eval("arguments"));`, "1"],
    ["", "1, 2", `return describe((function () { var arguments; return arguments; })(3)) + "; " + describe(arguments);`, "arguments(3); arguments(1,2)"],
    ["arguments", "1, 2", `var arguments; if (false) { function arguments() {} } return describe(eval("arguments"));`, "1"],

    // Parameter expressions.
    ["f = () => arguments", "", `{ function arguments() {} } return describe(f()) + "; " + describe(arguments);`, "arguments(); function arguments"],
    ["f = () => arguments, g = () => { arguments = 6; }", "", `var arguments; g(); return describe(f()) + "; " + describe(arguments);`, "6; arguments()"],
    ["f = () => arguments, g = () => { arguments = 6; }", "", `if (false) { function arguments() {} } g(); return describe(f()) + "; " + describe(arguments);`, "6; arguments()"],
    ["f = () => arguments, g = () => { arguments = 6; }", "", `{ let arguments = 1; { function arguments() {} } } g(); return describe(f()) + "; " + describe(arguments);`, "6; 6"],
    ["f = () => arguments, g = () => { arguments = 6; }", "", `g(); return describe(f()) + "; " + describe(arguments);`, "6; 6"],
    ["f = () => arguments", "", `eval("var arguments"); return describe(f()) + "; " + describe(arguments);`, "arguments(); undefined"],
    ["f = () => arguments", "", `var arguments; eval("var arguments = 4"); return describe(f()) + "; " + describe(arguments);`, "arguments(); 4"],
    ["a, f = () => a, g = () => { a = 6; }", "1", `var a; g(); return describe(f()) + "; " + describe(a);`, "6; 1"],
    ["a, f = () => a, g = () => { a = 6; }", "1", `a = 5; return describe(f()) + "; " + describe(a);`, "5; 5"],
    ["a, f = () => a, g = () => { a = 6; }", "1", `{ function a() {} } g(); return describe(f()) + "; " + describe(a);`, "6; 6"],
    ["a, f = () => a", "1", `eval("var a = 5"); return describe(f()) + "; " + describe(a);`, "1; 5"],
    ["a = 1", "", `eval("var a"); return describe(a);`, "undefined"],
    ["a = 1", "", `var a; eval("var a"); return describe(a);`, "1"],
    ["{ a }, ...rest", "{ a: 1 }, 2", `var a, rest; return describe(a) + "; " + describe(rest);`, "1; 2"],

    // A direct eval declares the name of a parameter: the parameter is the binding.
    ["a, b", "1, 2", `eval("var a = 5"); return describe(a) + "; " + describe(arguments);`, "5; arguments(5,2)"],
    ["a, b", "1, 2", `eval("var a"); return describe(delete a) + "; " + describe(a);`, "false; 1"],
    ["a, b", "1, 2", `eval("function z() {} function a() {}"); return describe(a) + "; " + describe(z);`, "function a; function z"],
    ["a, b", "1, 2", `eval("if (false) { function a() {} }"); return describe(a);`, "1"],
    ["a, b", "1, 2", `eval("{ function a() {} }"); return describe(a);`, "function a"],
    // A name that is new to the function is a binding of the body that can be deleted, as before.
    ["a, b", "1, 2", `eval("var z = 3"); return describe(z) + "; " + describe(delete z) + "; " + typeof z;`, "3; true; undefined"],
    // An eval in eval code declares for the generator too.
    ["a, b", "1, 2", `eval("eval('function z() {} function a() {}')"); return describe(a) + "; " + describe(z) + "; " + describe(delete a) + "; " + describe(delete z);`, "function a; function z; false; true"],
    // An eval in another function declares a var of that function.
    ["a, b", "1, 2", `(() => eval("var a = 7"))(); return describe(a);`, "1"],
    ["a, b", "1, 2", `(() => { eval("function z() {} function a() {}"); })(); return describe(a) + "; " + typeof z;`, "1; undefined"],
    ["a, b", "1, 2", `return (function () { eval("var a = 7"); return describe(a); })();`, "7"],
];

// One eval for each kind gives the results of all its rows, so that the test stays short in the eager JIT modes.
let failure = null;
let pending = 0;
function checkAll(kinds, rows) {
    for (const [kind, makeSource] of Object.entries(kinds)) {
        const sources = rows.map(([parameters, args, body]) => `(() => { try { return ${makeSource(parameters, body, args)}; } catch (error) { return error; } })()`);
        const results = (0, eval)(`[${sources.join(",\n")}]`);
        rows.forEach(([parameters, args, body, expected], index) => {
            const message = `${kind} (${parameters}) { ${body} }`;
            const result = results[index];
            if (!(result instanceof Promise)) {
                shouldBe(result, expected, message);
                return;
            }
            pending++;
            result.then(value => {
                pending--;
                shouldBe(value, expected, message);
            }).catch(error => {
                failure ??= error;
            });
        });
    }
}

checkAll({ ...generator, ...asyncKinds, ...methodsAndConstructors }, inEveryKind);
checkAll({ ...generator, ...asyncKinds }, inAsyncKinds);
checkAll(asyncArrow, inAsyncArrow);
checkAll(generator, inAGenerator);

drainMicrotasks();
if (failure)
    throw failure;
shouldBe(pending, 0, "checks that did not finish");

// Two calls do not share the binding, and it lives across a yield.
{
    function* counter() {
        var arguments;
        yield arguments[0];
        { function arguments() { return "function"; } }
        yield arguments();
    }
    const first = counter(1);
    const second = counter(2);
    shouldBe([first.next().value, second.next().value, first.next().value, second.next().value].join(), "1,2,function,function", "two generators");
}

// The wrapper function makes the var of the body when the function is called, not when the body first runs.
{
    let leak;
    function* late(f = (leak = () => { arguments = 7; })) {
        var arguments;
        return describe(arguments);
    }
    const lateGenerator = late();
    leak();
    shouldBe(lateGenerator.next().value, "arguments()", "a write before the body runs");
    shouldBe(late().return(5).value, 5, "return() before the body runs");
}

// In a parameter expression, a direct eval cannot declare `arguments`.
{
    let error;
    try {
        (function* (a = eval("var arguments")) { var arguments; })().next();
    } catch (e) {
        error = e;
    }
    shouldBe(error instanceof SyntaxError, true, "eval('var arguments') in a parameter expression");
}

// Functions of this file, which the bytecode cache holds.
{
    function* evaluated(a) {
        "use strict";
        return eval("arguments")[0] + a;
    }
    function* redeclared(a, b) {
        eval("var a, arguments; function b() { return a; }");
        return a + arguments.length + b();
    }
    async function awaited(a, f = () => a) {
        await 1;
        var a, arguments;
        a++;
        return describe(f()) + "; " + describe(a) + "; " + describe(arguments);
    }
    shouldBe(evaluated(3).next().value, 6, "evaluated");
    shouldBe(redeclared(3, 0).next().value, 8, "redeclared");
    let result;
    awaited(3).then(value => { result = value; });
    drainMicrotasks();
    shouldBe(result, "3; 4; arguments(3)", "awaited");
}

// All the tiers.
{
    function* declared(a) {
        var arguments;
        if (a & 1) {
            function arguments() { return a; }
        }
        return typeof arguments === "function" ? arguments() + 1 : arguments[0];
    }
    for (let i = 0; i < testLoopCount; i++)
        shouldBe(declared(i).next().value, i + (i & 1), "declared");
}
