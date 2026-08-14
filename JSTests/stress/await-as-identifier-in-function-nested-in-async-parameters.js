// The parameter list of an async function is parsed with `await` reserved. A non-async function in a
// parameter default is not covered by that: its body is a fresh FunctionBody in which `await` is an
// ordinary identifier (only the function's own async-ness, module code and class static blocks reserve
// it there). The parser used to keep the enclosing parameter list's restriction active while parsing
// the nested body, so every "accept" case below was a SyntaxError. The check happens while the
// enclosing code is parsed, so each case goes through eval to exercise that parse.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: ${String(actual)}, expected: ${String(expected)}`);
}

function shouldThrowSyntaxError(code) {
    let threw = false;
    try {
        (0, eval)(code);
    } catch (e) {
        threw = true;
        if (!(e instanceof SyntaxError))
            throw new Error(`Expected SyntaxError but got ${e.constructor.name}: ${e.message}: ${code}`);
    }
    if (!threw)
        throw new Error(`Expected SyntaxError for: ${code}`);
}

function shouldNotThrowSyntaxError(code) {
    try {
        (0, eval)(code);
    } catch (e) {
        if (e instanceof SyntaxError)
            throw new Error(`Unexpected SyntaxError: ${e.message}: ${code}`);
        throw e;
    }
}

// Ways for the body of a non-async function to use `await` as an identifier. All of them are valid in
// strict mode too (the class bodies below are strict code): `await` is not a strict mode reserved word.
const awaitAsIdentifier = [
    `var await;`,
    `var await = 1;`,
    `let await = 1;`,
    `const await = 1;`,
    `var { await } = { await: 1 };`,
    `var { a: await } = { a: 1 };`,
    `var [await] = [1];`,
    `let [...await] = [];`,
    `function await() {}`,
    `class await {}`,
    `await: 1;`,
    `await: for (;;) break await;`,
    `try {} catch (await) {}`,
    `for (var await in {}) {}`,
    `for (var await of []) {}`,
    `for (let await of []) {}`,
    `({ await });`,
    `({ await } = {});`,
    `[await] = [];`,
    `await => 1;`,
    `(await) => 1;`,
    `(a = await) => a;`,
    `function f(await) {}`,
    `await;`,
    `await = 1;`,
    `\\u0061wait;`,
    `var \\u0061wait;`,
    `\\u0061wait: 1;`,
];

// Every kind of non-async function body, as (part of) a parameter default.
const nestedFunctions = [
    body => `function () { ${body} }`,
    body => `function named() { ${body} }`,
    body => `function* () { ${body} }`,
    body => `() => { ${body} }`,
    body => `{ m() { ${body} } }`,
    body => `{ get g() { ${body} } }`,
    body => `{ set s(v) { ${body} } }`,
    body => `{ *g() { ${body} } }`,
    body => `class { m() { ${body} } }`,
    body => `class { static m() { ${body} } }`,
    body => `class { constructor() { ${body} } }`,
    body => `class { #m() { ${body} } }`,
    body => `class { *g() { ${body} } }`,
    body => `class { static { (function () { ${body} })(); } }`,
    body => `class { f = function () { ${body} }; }`,
    body => `class { f = () => { ${body} }; }`,
    body => `class { static f = function () { ${body} }; }`,
    body => `function () { function inner() { ${body} } }`,
    body => `function () { return () => { ${body} }; }`,
];

// Every kind of parameter list that is parsed with `await` reserved. The last one is not async itself:
// the parameters of an arrow function inherit the restriction from the enclosing async function.
const reservedAwaitParameterLists = [
    nested => `async function f(x = ${nested}) {}`,
    nested => `(async function (x = ${nested}) {});`,
    nested => `async function* f(x = ${nested}) {}`,
    nested => `(async function* (x = ${nested}) {});`,
    nested => `({ async m(x = ${nested}) {} });`,
    nested => `({ async *m(x = ${nested}) {} });`,
    nested => `(class { async m(x = ${nested}) {} });`,
    nested => `(class { static async m(x = ${nested}) {} });`,
    nested => `(class { async *m(x = ${nested}) {} });`,
    nested => `async function f({ x = ${nested} }) {}`,
    nested => `async function f([x = ${nested}]) {}`,
    nested => `async function f(x = ${nested}, y = ${nested}) {}`,
    nested => `"use strict"; async function f(x = ${nested}) {}`,
    nested => `function outer() { async function f(x = ${nested}) {} }`,
    nested => `async function outer() { async function f(x = ${nested}) {} }`,
    nested => `async (x = ${nested}) => {};`,
    nested => `async function outer() { (x = ${nested}) => {}; }`,
];

for (const body of awaitAsIdentifier) {
    for (const nest of nestedFunctions)
        shouldNotThrowSyntaxError(`async function f(x = ${nest(body)}) {}`);
}

for (const body of [`var await;`, `await: 1;`]) {
    for (const parameterList of reservedAwaitParameterLists) {
        for (const nest of nestedFunctions)
            shouldNotThrowSyntaxError(parameterList(nest(body)));
    }
}

// Arrow function parameters are parsed more than once (the first pass finds out that they are arrow
// function parameters, without the restriction), and a later pass skips the body of a nested function
// that an earlier pass parsed and cached. Functions this short are not cached, so with these the arrow
// parameter lists above parse the nested body under their restriction for real.
for (const parameterList of reservedAwaitParameterLists) {
    shouldNotThrowSyntaxError(parameterList(`function(){var await}`));
    shouldNotThrowSyntaxError(parameterList(`function(){await:0}`));
}

// Expression-bodied arrows.
shouldNotThrowSyntaxError(`async function f(x = () => await) {}`);
shouldNotThrowSyntaxError(`async function f(x = () => ({ await })) {}`);
shouldNotThrowSyntaxError(`async function f(x = () => \\u0061wait) {}`);
shouldNotThrowSyntaxError(`async function f(x = () => await => await) {}`);
shouldNotThrowSyntaxError(`async function f(x = () => function () { var await; }) {}`);

// A nested function that is itself async still has `await` as an operator in its body.
shouldNotThrowSyntaxError(`async function f(x = async function () { await 1; }) {}`);
shouldNotThrowSyntaxError(`async function f(x = async () => { await 1; }) {}`);
shouldNotThrowSyntaxError(`async function f(x = async () => await 1) {}`);
shouldNotThrowSyntaxError(`async function f(x = { async m() { await 1; } }) {}`);
shouldNotThrowSyntaxError(`async function f(x = function () { return async () => { await 1; }; }) {}`);

// Still reserved in the parameter list itself, also after a nested function has been parsed, and in the
// async function's body.
shouldThrowSyntaxError(`async function f(x = await 1) {}`);
shouldThrowSyntaxError(`async function f(x = await) {}`);
shouldThrowSyntaxError(`async function f(await) {}`);
shouldThrowSyntaxError(`async function f({ await }) {}`);
shouldThrowSyntaxError(`async function f(x = function () { var await; }, y = await) {}`);
shouldThrowSyntaxError(`async function f(x = function () { var await; }, await) {}`);
shouldThrowSyntaxError(`async function f(x = function () { var await; }, { await }) {}`);
shouldThrowSyntaxError(`async function f(x = function () { var await; }, y = (await) => 1) {}`);
shouldThrowSyntaxError(`async function f(x = function () { var await; }, y = await => 1) {}`);
shouldThrowSyntaxError(`async function f(x = () => { var await; }, y = await) {}`);
shouldThrowSyntaxError(`async function f(x = class { m() { var await; } }, await) {}`);
shouldThrowSyntaxError(`async function f(x = function () { var await; }) { var await; }`);
shouldThrowSyntaxError(`async function f(x = function () { await: 1; }) { await: 1; }`);
shouldThrowSyntaxError(`async function f(x = function () { function await() {} }) { function await() {} }`);
shouldThrowSyntaxError(`async function f(x = function () { var await; }) { (await) => 1; }`);
shouldThrowSyntaxError(`(async function (x = function () { var await; }, await) {});`);
shouldThrowSyntaxError(`async function* f(x = function () { var await; }, await) {}`);
shouldThrowSyntaxError(`({ async m(x = function () { var await; }, await) {} });`);
shouldThrowSyntaxError(`(class { async m(x = function () { var await; }, await) {} });`);
shouldThrowSyntaxError(`(class { static async m(x = function () { var await; }, await) {} });`);
shouldThrowSyntaxError(`async (await) => {};`);
shouldThrowSyntaxError(`async await => {};`);
shouldThrowSyntaxError(`async (x = function(){var await}, await) => {};`);
shouldThrowSyntaxError(`async function outer() { (await) => {}; }`);
shouldThrowSyntaxError(`async function outer() { (x = function(){var await}, await) => {}; }`);
shouldThrowSyntaxError(`async function outer() { (x = function(){var await}, y = await) => {}; }`);

// Still reserved in a nested function that is itself async, in its parameters, and in a static block.
shouldThrowSyntaxError(`async function f(x = async function () { var await; }) {}`);
shouldThrowSyntaxError(`async function f(x = async function () { await: 1; }) {}`);
shouldThrowSyntaxError(`async function f(x = async function* () { var await; }) {}`);
shouldThrowSyntaxError(`async function f(x = async () => { var await; }) {}`);
shouldThrowSyntaxError(`async function f(x = { async m() { var await; } }) {}`);
shouldThrowSyntaxError(`async function f(x = class { async m() { var await; } }) {}`);
shouldThrowSyntaxError(`async function f(x = class { static async m() { var await; } }) {}`);
shouldThrowSyntaxError(`async function f(x = async function (await) {}) {}`);
shouldThrowSyntaxError(`async function f(x = async function (y = await) {}) {}`);
shouldThrowSyntaxError(`async function f(x = function () { async function g() { var await; } }) {}`);
shouldThrowSyntaxError(`async function f(x = function () { async function g(await) {} }) {}`);
shouldThrowSyntaxError(`async function f(x = function () { async function g(y = await) {} }) {}`);
shouldThrowSyntaxError(`async function f(x = function () { async (await) => 1; }) {}`);
shouldThrowSyntaxError(`async function f(x = function () { async await => 1; }) {}`);
shouldThrowSyntaxError(`async function f(x = function () { return async () => { var await; }; }) {}`);
shouldThrowSyntaxError(`async function f(x = class { static { var await; } }) {}`);
shouldThrowSyntaxError(`(class { static { var await; } });`);

// The nested functions work. Calling an async function evaluates its parameter defaults synchronously, so
// the values are observable right after the call.
shouldBe((0, eval)(`
    (() => {
        let result;
        async function f(x = (function () { var await = "variable"; return await; })()) { result = x; }
        f();
        return result;
    })();
`), "variable");

shouldBe((0, eval)(`
    (() => {
        let result;
        async function* g(x = (() => { let n = 0; await: for (;;) { n++; if (n === 3) break await; } return n; })()) { result = x; }
        g().next();
        return result;
    })();
`), 3);

shouldBe((0, eval)(`
    (() => {
        let result;
        ({ async m(x = class { f() { var await = "method"; return await; } }) { result = new x().f(); } }).m();
        return result;
    })();
`), "method");

shouldBe((0, eval)(`
    (() => {
        let result;
        (async (x = function(){var await = "arrow"; return await}) => { result = x(); })();
        return result;
    })();
`), "arrow");

{
    const AsyncFunction = (async function () {}).constructor;
    let result;
    AsyncFunction("report", "x = (function () { var await = \"constructor\"; return await; })()", "report(x);")(value => { result = value; });
    shouldBe(result, "constructor");
}
