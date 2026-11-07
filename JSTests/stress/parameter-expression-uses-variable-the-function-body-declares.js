// A parameter expression is evaluated outside of the environment of the function body's declarations. So a name in it
// that the body declares again is the variable from around the function:
//
//     function f() { let local = 10; return () => (b = local) => { var local = 7; return b; }; }
//     f()()() === 10
//
// The parser has one scope for the parameters and the body, so such a name is not a free variable of the function. The
// parser told only the direct parent scope that the function captures it. With one more function in between, the
// function that declares the variable did not hear of it, kept the variable in a register, and the parameter expression
// found no binding: it threw a ReferenceError, or it read or wrote a global or an import of that name. A function that
// the parser took from the source provider cache did pass the name on, so the result also depended on the cache.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${String(expected)} but got ${String(actual)}`);
}

// A parameter expression that finds no binding reads this.
globalThis.local = "global";

// Parameter lists that use `local`, which is 10. Each one gives `b` the value on the right.
const parameters = [
    ["b = local", 10],
    ["b = (() => local)()", 10],
    ["b = (function () { return local; })()", 10],
    ["b = typeof local", "number"],
    ["b = `${local}`", "10"],
    ["{ b = local } = {}", 10],
    ["[b = local] = []", 10],
    ["...[b = local]", 10],
    ["{ [local]: b } = { 10: 'computed' }", "computed"],
    ["g = () => M, M = local, b = g()", 10],
    ["b = class { static m() { return local; } }.m()", 10],
    ["b = { get v() { return local; } }.v", 10],
    ["b = ((c = local) => { var local = 6; return c; })()", 10],
];

// Declarations of `local` in the body of the function with the parameters.
const bodyDeclarations = [
    "var local = 7;",
    "let local = 7;",
    "const local = 7;",
    "function local() { }",
    "class local { }",
    "{ var local = 7; }",
    "for (var local of []) { }",
    "var { local } = { local: 7 };",
];

// The function with the parameters. Each text is a callable that returns `b`.
const functions = [
    (p, d) => `((${p}) => { ${d} return b; })`,
    (p, d) => `(function (${p}) { ${d} return b; })`,
    (p, d) => `(function named(${p}) { ${d} return b; })`,
    (p, d) => `({ m(${p}) { ${d} return b; } }).m`,
    (p, d) => `(class { static m(${p}) { ${d} return b; } }).m`,
    (p, d) => `(() => { function declared(${p}) { ${d} return b; } return declared; })()`,
    (p, d) => `(() => { let result; const o = { set s(v) { result = v; }, m(${p}) { ${d} this.s = b; return result; } }; return () => o.m(); })()`,
    (p, d) => `(() => new (class { constructor(${p}) { ${d} this.b = b; } })().b)`,
];

// What is between that function and the function that declares `local`. Each text gives back the value of `e`.
const between = [
    e => `(() => ${e})()`,
    e => `(() => { return ${e}; })()`,
    e => `(function () { return ${e}; })()`,
    e => `({ m() { return ${e}; } }).m()`,
    e => `({ get p() { return ${e}; } }).p`,
    e => `(function* () { yield ${e}; })().next().value`,
    e => `new (class { field = ${e}; })().field`,
    e => `(() => { let result; class C { static { result = ${e}; } } return result; })()`,
    e => `(() => () => () => ${e})()()()`,
    e => `(() => { { return ${e}; } })()`,
    e => `(() => { try { throw 1; } catch { return ${e}; } })()`,
    e => `((a = ${e}) => a)()`,
    e => `((a = ${e}) => { var local = 8; return a; })()`,
    e => `(function (a = ${e}) { let local = 8; return a; })()`,
    // No function in between: these always worked.
    e => `${e}`,
    e => `[${e}][0]`,
];

// The function that declares `local`. Each text gives back the value of `e`.
const declarations = [
    e => `(function () { let local = 10; return ${e}; })()`,
    e => `(function () { const local = 10; return ${e}; })()`,
    e => `(function () { var local = 10; return ${e}; })()`,
    e => `(function () { local = 10; return ${e}; var local; })()`,
    e => `(function (local) { return ${e}; })(10)`,
    e => `(function (local = 10) { return ${e}; })()`,
    e => `(function ({ local }) { return ${e}; })({ local: 10 })`,
    e => `((local) => ${e})(10)`,
    e => `(function () { try { throw 10; } catch (local) { return ${e}; } })()`,
    e => `(function () { try { throw { local: 10 }; } catch ({ local }) { return ${e}; } })()`,
    e => `(function () { for (let local = 10; ;) return ${e}; })()`,
    e => `(function () { for (const local of [10]) return ${e}; })()`,
    e => `(function () { { let local = 10; return ${e}; } })()`,
    e => `(function () { return (class { static local = 10; static m() { let local = this.local; return ${e}; } }).m(); })()`,
    e => `(function* () { let local = 10; yield ${e}; })().next().value`,
];

function check(source, expected) {
    let actual;
    try {
        actual = (0, eval)(source);
    } catch (e) {
        actual = String(e);
    }
    shouldBe(actual, expected, source);
}

function checkChoice(makeDeclaration, makeBetween, makeFunction, [parameter, expected], bodyDeclaration, alsoStrict) {
    const source = makeDeclaration(makeBetween(makeFunction(parameter, bodyDeclaration)) + "()");
    check(source, expected);
    if (alsoStrict)
        check(`"use strict"; ${source}`, expected);
}

// Each entry of each list with the first entry of the other lists. The functions and the body declarations also as
// strict mode code.
const lists = [declarations, between, functions, parameters, bodyDeclarations];
for (const list of lists) {
    for (const entry of list) {
        const choice = lists.map(other => other === list ? entry : other[0]);
        checkChoice(...choice, list === functions || list === bodyDeclarations);
    }
}

// Each kind of code in between with an arrow function, a function expression, a method and a constructor.
for (const makeBetween of between) {
    for (const makeFunction of [functions[0], functions[1], functions[3], functions[7]])
        checkChoice(declarations[0], makeBetween, makeFunction, parameters[0], bodyDeclarations[0], false);
}

// The text of this file is one source: the parser takes most of the functions below from the source provider cache when
// it compiles the ones around them. The programs above are each a source of their own.

// An assignment in a parameter expression.
{
    function assigns() {
        let local = 1;
        const result = (() => (b = (local = 10)) => { var local = 7; return b; })()();
        return [local, result];
    }
    const [local, result] = assigns();
    shouldBe(local, 10, "the assignment writes the variable of the enclosing function");
    shouldBe(result, 10, "the assignment is the default value");
    shouldBe(globalThis.local, "global", "the assignment does not write the global");

    function updates() {
        let local = 1;
        (() => (b = local += 9, c = local++) => { var local = 7; })()();
        return local;
    }
    shouldBe(updates(), 11, "compound assignment and update");
}

// The variable is read when the function is called, not when it is made.
{
    function later() {
        let local = 1;
        const g = (() => (b = local) => { var local = 7; return b; })();
        local = 10;
        return g();
    }
    shouldBe(later(), 10, "read at the call");
}

// The temporal dead zone of the variable.
{
    function tdz() {
        const g = (() => (b = local) => { var local = 7; return b; })();
        let error;
        try {
            g();
        } catch (e) {
            error = e;
        }
        let local = 10;
        return [error, g()];
    }
    const [error, value] = tdz();
    shouldBe(error instanceof ReferenceError, true, "a call in the temporal dead zone throws a ReferenceError");
    shouldBe(value, 10, "a call after the temporal dead zone");
}

// A catch parameter that only a parameter expression uses. Without the source provider cache the parser took it for an
// unused one.
{
    function caught() {
        try {
            throw 10;
        } catch (local) {
            return ((b = local) => { var local = 7; return b; })();
        }
    }
    shouldBe(caught(), 10, "catch parameter");
}

// Each closure has its own variable.
{
    function loop() {
        const functions = [];
        for (let local = 10; local < 13; local++)
            functions.push((() => (b = local) => { var local = 7; return b; })());
        return functions.map(g => g()).join();
    }
    shouldBe(loop(), "10,11,12", "loop variable");
}

// A with statement in between.
{
    function withStatement() {
        let local = 10;
        return (() => { with ({ }) { return (b = local) => { var local = 7; return b; }; } })()();
    }
    shouldBe(withStatement(), 10, "with statement");

    function withStatementThatHasTheName() {
        let local = 10;
        return (() => { with ({ local: 9 }) { return (b = local) => { var local = 7; return b; }; } })()();
    }
    shouldBe(withStatementThatHasTheName(), 9, "with statement whose object has the name");
}

// Two names, and only one of them is declared again.
{
    function two() {
        let local = 10;
        let other = 5;
        return (() => (b = local + other) => { var local = 7; return b + other; })()();
    }
    shouldBe(two(), 20, "two names");
}

// "arguments". A function that is not an arrow function has its own. An arrow function has the one from around it.
{
    function ownArguments() {
        return () => function (a = arguments.length) { var arguments = 7; return a; };
    }
    shouldBe(ownArguments(1, 2, 3)()(undefined, 2), 2, "the function's own arguments");

    function outerArguments() {
        return () => (a = arguments.length) => { var arguments = 7; return a; };
    }
    shouldBe(outerArguments(1, 2, 3)()(), 3, "the arguments of the enclosing function");
}

// The name of a function expression, and of a function declaration, in the function's own parameter expressions.
{
    function calleeName() {
        let named = 10;
        const g = (() => function named(b = named) { var named = 7; return b; })();
        return g() === g;
    }
    shouldBe(calleeName(), true, "the name of a function expression is the function");

    function declarationName() {
        let result;
        (() => {
            function named(b = named) { var named = 7; return b; }
            result = named() === named;
        })();
        return result;
    }
    shouldBe(declarationName(), true, "the name of a function declaration is the function");
}

// Code from eval and from the Function constructor.
shouldBe((0, eval)("let evalLocal = 10; (() => (b = evalLocal) => { var evalLocal = 7; return b; })()()"), 10, "indirect eval");
shouldBe((function () { let local = 10; return eval("(() => (b = local) => { var local = 7; return b; })()()"); })(), 10, "direct eval");
shouldBe(new Function("let local = 10; return (() => (b = local) => { var local = 7; return b; })()();")(), 10, "Function constructor");

// Generators and async functions keep the declarations of the body in a scope of their own: these always worked.
{
    function generator() {
        let local = 10;
        return (() => function* (b = local) { var local = 7; yield b; })()().next().value;
    }
    shouldBe(generator(), 10, "generator");

    function asyncArrow() {
        let local = 10;
        let result;
        (() => async (b = local) => { var local = 7; result = b; })()();
        return result;
    }
    shouldBe(asyncArrow(), 10, "async arrow function");
}

// All tiers.
{
    function hot() {
        let local = 10;
        const make = () => (b = local) => { var local = 7; return b; };
        let sum = 0;
        for (let i = 0; i < testLoopCount; i++)
            sum += make()();
        return sum;
    }
    shouldBe(hot(), 10 * testLoopCount, "hot loop");
}
