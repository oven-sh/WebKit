// CatchParameter is BindingIdentifier[?Yield, ?Await], so a catch parameter named `await` is a
// SyntaxError wherever `await` is not an identifier: in async functions, in class static blocks and
// in module code. The simple (non-destructuring) form used to skip that check.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: ${String(actual)}, expected: ${String(expected)}`);
}

function shouldThrowSyntaxError(script, message) {
    let error;
    try {
        (0, eval)(script);
    } catch (e) {
        error = e;
    }
    if (!(error instanceof SyntaxError))
        throw new Error(`Expected SyntaxError for: ${script}` + (error ? `, but got ${error}` : ", but nothing was thrown"));
    if (error.message !== message)
        throw new Error(`Expected "${message}" for: ${script}, but got "${error.message}"`);
}

function shouldNotThrowSyntaxError(script) {
    try {
        (0, eval)(script);
    } catch (e) {
        if (e instanceof SyntaxError)
            throw new Error(`Unexpected SyntaxError for: ${script}: ${e.message}`);
        throw e;
    }
}

function shouldThrowSyntaxErrorWhenCalled(func, description, message) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof SyntaxError))
        throw new Error(`Expected SyntaxError for: ${description}` + (error ? `, but got ${error}` : ", but nothing was thrown"));
    if (error.message !== message)
        throw new Error(`Expected "${message}" for: ${description}, but got "${error.message}"`);
}

// checkModuleSyntax() throws the string "SyntaxError: <message>:<line>"; every source below is one line long.
function checkModuleSyntaxError(source, message) {
    let thrown = false;
    let error;
    try {
        checkModuleSyntax(source);
    } catch (e) {
        thrown = true;
        error = e;
    }
    if (!thrown)
        throw new Error(`Expected a syntax error for module: ${source}`);
    shouldBe(String(error), `SyntaxError: ${message}:1`);
}

const inAsyncFunction = "Cannot use 'await' as a catch parameter name in an async function.";
const inStaticBlock = "Cannot use 'await' as a catch parameter name in a static block.";
const inModule = "Cannot use 'await' as a catch parameter name in a module.";

// Every shape of catch parameter that binds `await`. The destructuring ones were already rejected;
// they are here so the simple form keeps producing the same error as they do.
const awaitCatchStatements = [
    String.raw`try { } catch (await) { }`,
    String.raw`try { } catch (\u0061wait) { }`,
    String.raw`try { } catch (aw\u0061it) { }`,
    String.raw`{ try { } catch (await) { } }`,
    String.raw`if (true) try { } catch (await) { }`,
    String.raw`try { } catch (e) { try { } catch (await) { } }`,
    String.raw`try { } finally { try { } catch (await) { } }`,
    String.raw`try { } catch ([await]) { }`,
    String.raw`try { } catch ([...await]) { }`,
    String.raw`try { } catch ({ await }) { }`,
    String.raw`try { } catch ({ a: await }) { }`,
    String.raw`try { } catch ({ ...await }) { }`,
];

const asyncBodies = [
    body => `async function f() { ${body} }`,
    body => `(async function () { ${body} });`,
    body => `(async function* () { ${body} });`,
    body => `async () => { ${body} };`,
    body => `({ async m() { ${body} } });`,
    body => `({ async* m() { ${body} } });`,
    body => `(class { async m() { ${body} } });`,
    body => `(class { static async m() { ${body} } });`,
    body => `(class { async* m() { ${body} } });`,
    body => `(class { async #m() { ${body} } });`,
    body => `function outer() { async function f() { ${body} } }`,
    body => `"use strict"; async function f() { ${body} }`,
];

for (const statement of awaitCatchStatements) {
    for (const wrap of asyncBodies)
        shouldThrowSyntaxError(wrap(statement), inAsyncFunction);

    shouldThrowSyntaxError(`class C { static { ${statement} } }`, inStaticBlock);
    shouldThrowSyntaxError(`class C { static { { ${statement} } } }`, inStaticBlock);
    shouldThrowSyntaxError(`async function outer() { class C { static { ${statement} } } }`, inStaticBlock);
    shouldThrowSyntaxError(`function outer() { class C { static { ${statement} } } }`, inStaticBlock);

    checkModuleSyntaxError(statement, inModule);
    checkModuleSyntaxError(`function f() { ${statement} }`, inModule);
    checkModuleSyntaxError(`() => { ${statement} };`, inModule);
}

const AsyncFunction = (async function () { }).constructor;
const AsyncGeneratorFunction = (async function* () { }).constructor;
for (const statement of awaitCatchStatements) {
    shouldThrowSyntaxErrorWhenCalled(() => AsyncFunction(statement), `AsyncFunction(${JSON.stringify(statement)})`, inAsyncFunction);
    shouldThrowSyntaxErrorWhenCalled(() => AsyncGeneratorFunction(statement), `AsyncGeneratorFunction(${JSON.stringify(statement)})`, inAsyncFunction);
}

// `await` is an identifier everywhere else, including in non-async functions and arrow functions
// nested in an async function or a static block, and in eval code called from an async function.
const identifierCatchStatements = [
    String.raw`try { } catch (await) { }`,
    String.raw`try { } catch (\u0061wait) { }`,
    String.raw`try { } catch ([await]) { }`,
    String.raw`try { } catch ({ await }) { }`,
];

const identifierBodies = [
    body => body,
    body => `"use strict"; ${body}`,
    body => `function f() { ${body} }`,
    body => `function* g() { ${body} }`,
    body => `() => { ${body} };`,
    body => `({ m() { ${body} } });`,
    body => `(class { m() { ${body} } });`,
    body => `(class { static m() { ${body} } });`,
    body => `(class { x = () => { ${body} }; });`,
    body => `async function outer() { function f() { ${body} } }`,
    body => `async function outer() { () => { ${body} }; }`,
    body => `async function outer() { class C { m() { ${body} } } }`,
    body => `async function* outer() { function f() { ${body} } }`,
    body => `class C { static { function f() { ${body} } } }`,
    body => `class C { static { () => { ${body} }; } }`,
    body => `class C { static { class D { m() { ${body} } } } }`,
];

for (const statement of identifierCatchStatements) {
    for (const wrap of identifierBodies)
        shouldNotThrowSyntaxError(wrap(statement));
    shouldNotThrowSyntaxError(`Function(${JSON.stringify(statement)})`);
    shouldNotThrowSyntaxError(`(function () { eval(${JSON.stringify(statement)}); })()`);
}

// Eval code is parsed as a Script, so `await` is an identifier in it even when the eval is called
// from an async function.
{
    let error;
    (0, eval)(`(async function () { eval("try { } catch (await) { }"); })`)().catch(e => { error = e; });
    drainMicrotasks();
    if (error)
        throw error;
}

// Other catch parameters in async functions and static blocks are unaffected.
shouldNotThrowSyntaxError(`async function f() { try { } catch (e) { } try { } catch ({ e }) { } try { } catch ([e]) { } try { } catch { } }`);
shouldNotThrowSyntaxError(`async function f() { try { } catch (yield) { } }`);
shouldNotThrowSyntaxError(`async function f() { try { } catch (async) { } }`);
shouldNotThrowSyntaxError(`class C { static { try { } catch (e) { } try { } catch ({ e }) { } try { } catch { } } }`);

// `yield` and `let` as catch parameters keep their own errors.
shouldThrowSyntaxError(`function* g() { try { } catch (yield) { } }`, "Cannot use 'yield' as a catch parameter name in a generator function.");
shouldThrowSyntaxError(`"use strict"; try { } catch (yield) { }`, "Cannot use 'yield' as a catch parameter name in strict mode.");
shouldThrowSyntaxError(`"use strict"; try { } catch (let) { }`, "Cannot use 'let' as a catch parameter name in strict mode.");
shouldNotThrowSyntaxError(`try { } catch (yield) { } try { } catch (let) { }`);

// Where it is allowed, the catch parameter named `await` is a working binding.
shouldBe((0, eval)(`(function () { try { throw 42; } catch (await) { return await; } })()`), 42);
shouldBe((0, eval)(`(function () { try { throw 42; } catch (\\u0061wait) { return await; } })()`), 42);
shouldBe((0, eval)(`(function () { try { throw { await: 42 }; } catch ({ await }) { return await; } })()`), 42);
shouldBe((0, eval)(`class C { static value; static { (() => { try { throw 42; } catch (await) { C.value = await; } })(); } } C.value`), 42);
