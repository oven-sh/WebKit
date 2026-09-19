// The parameters of an arrow function in an async function are parsed with [+Await]: `await` is not an identifier in them,
// and an AwaitExpression is an early error. That holds for an arrow function in the parameters of another arrow function
// too.
//
// The parser first reads "( ... )" as an expression. To see that it is a parameter list it parses it again, in a scope
// that is never async, and an arrow function in there goes to the source provider cache with `await` as an identifier.
// The parse that knows about the async function then skipped that arrow function from the cache, so the code below that
// must throw was accepted when the text is parsed once more after that, as an assignment pattern is. It was always
// rejected with --useSourceProviderCache=false, and the messages here are the ones that run gives.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${String(expected)} but got ${String(actual)}`);
}

function shouldThrowSyntaxError(source, message) {
    let error;
    try {
        (0, eval)(source);
    } catch (e) {
        error = e;
    }
    if (!(error instanceof SyntaxError))
        throw new Error(`${source}: expected a SyntaxError but got ${String(error)}`);
    if (message !== undefined)
        shouldBe(error.message, message, source);
}

// The value of a promise, after the microtasks ran. A rejection is an error.
function settled(promise, what) {
    let result;
    let error;
    promise.then((value) => { result = value; }, (reason) => { error = reason; });
    drainMicrotasks();
    if (error !== undefined)
        throw new Error(`${what}: ${String(error)}`);
    return result;
}

function shouldParse(source) {
    try {
        (0, eval)(source);
    } catch (e) {
        if (e instanceof SyntaxError)
            throw new Error(`${source}: ${String(e)}`);
    }
}

const parameterName = "Cannot use 'await' as a parameter name in an async function.";
const defaultExpression = "Cannot use 'await' within a parameter default expression.";
const shorthand = "Cannot use 'await' as a shorthand property name in an async function.";

// [text before the expression, text after it]. The messages below are those of the first one.
const asyncFunctions = [
    ["(async function () { (", "); })"],
    ["(async function () { return ", "; })"],
    ["(async function (p = ", ") { })"],
    ["async function f() { (", "); }"],
    ["({ async g() { (", "); } })"],
    ["({ async g(p = ", ") { } })"],
    ["(class { async g() { (", "); } })"],
    ["(class { static async g(p = ", ") { } })"],
    ["(async function* () { (", "); })"],
    ["(async function* (p = ", ") { })"],
    ["(async () => { (", "); })"],
    ["(async (p = ", ") => { })"],
    ["(async function () { { let q; (", "); } })"],
    ["(async function (p = async function () { (", "); }) { })"],
    ["(function () { async function f() { (", "); } })"],
];

// [expression, message, true if the expression is not valid where `await` is an identifier either]
const invalid = [
    // The arrow function is in the parameters of another arrow function.
    ["(a = (await) => 1) => a", defaultExpression],
    ["(a = (b = await) => b) => a", defaultExpression],
    ["(a = (await = 1) => 1) => a", defaultExpression],
    ["(a = (...await) => 1) => a", "Unexpected token '...'"],
    ["(a = ([await]) => 1) => a", defaultExpression],
    ["(a = ({ await }) => 1) => a", shorthand],
    ["(a = ({ b = await }) => 1) => a", "Unexpected token '='. Expected a ':' following the property name 'b'."],
    ["(a = async (await) => 1) => a", parameterName, true],
    ["(a = async (b = await) => b) => a", defaultExpression, true],
    ["(b = (await) => 1, c) => b", defaultExpression],
    ["(c, b = (await) => 1) => b", defaultExpression],
    ["({ a = (await) => 1 }) => a", defaultExpression],
    ["({ k: a = (b = await) => b }) => a", defaultExpression],
    ["([a = (await) => 1]) => a", defaultExpression],
    ["(...[a = (await) => 1]) => a", defaultExpression],
    ["async (a = (await) => 1) => a", defaultExpression, true],
    ["async ({ a = (b = await) => b }) => a", defaultExpression, true],
    // Deeper.
    ["(a = (b = (await) => 1) => b) => a", defaultExpression],
    ["(a = (b = (c = await) => c) => b) => a", defaultExpression],
    ["(a = (b = (c = (d = (await) => 1) => d) => c) => b) => a", defaultExpression],
    ["({ x = 1 }, a = (b = (c = await) => c) => b) => a", defaultExpression],
    // "( ... )" is not a parameter list, but the parser has to try that to find out. The enclosing pattern is parsed twice.
    ["[a = (b = (await) => 1)] = []", parameterName],
    ["[a = ({ [(await) => 1]: 1 })] = []", parameterName],
    ["[a = (b = (c = await) => c)] = []", defaultExpression],
    ["[a = (b = ({ await }) => 1)] = []", parameterName],
    ["({ a = (b = (await) => 1) } = {})", "Unexpected token '='. Expected a ':' following the property name 'a'."],
    ["[a = (b = (c = (await) => 1) => c)] = []", defaultExpression],
    // One arrow function. These were rejected before, and the messages are the same.
    ["(await) => 1", parameterName],
    ["(a = await) => a", defaultExpression],
    ["({ a = await }) => a", defaultExpression],
    ["(...[a = await]) => a", defaultExpression],
];

for (const [before, after] of asyncFunctions) {
    for (const [expression, message] of invalid)
        shouldThrowSyntaxError(before + expression + after, before === asyncFunctions[0][0] ? message : undefined);
}

// `await` is an identifier in the body of an arrow function, in a function that is not async, and in an arrow function
// that is in one of those.
const valid = [
    "(a = (b) => await) => a",
    "(a = (b) => { var await; }) => a",
    "(a = (b) => { (await) => 1; }) => a",
    "(a = (b) => (c = (await) => 1) => c) => a",
    "(a = function (await) { await; }) => a",
    "(a = function (b = await) { }) => a",
    "(a = function () { (b = (await) => 1) => b; }) => a",
    "(a = async function () { await 1; }) => a",
    "(a = async () => await 1) => a",
    "(a = { await: 1 }.await) => a",
    "({ x = 1 }, a = (b = (c = 1) => c) => b) => a",
    "[a = (b = (c = 1) => c)] = []",
    // The parser has to parse these again for the async function, and then the nested body is not in its parameters.
    "[a = (b = (c = function () { var await; }) => c)] = []",
    "[a = (b = (c = () => { var await; }) => c)] = []",
    "[a = (b = (c = () => await) => c)] = []",
    "[a = (b = (c = class { m() { await: 1; } }) => c)] = []",
];

for (const [before, after] of asyncFunctions) {
    for (const expression of valid)
        shouldParse(before + expression + after);
}

// Outside an async function `await` is an identifier at every level, also in strict mode code.
for (const [expression, , neverValid] of invalid) {
    if (neverValid)
        continue;
    shouldParse(`(function () { (${expression}); })`);
    shouldParse(`(function () { "use strict"; (${expression}); })`);
    shouldParse(`(function* () { (${expression}); })`);
    shouldParse(`(async function () { () => { (${expression}); }; })`);
    shouldParse(`(async function () { function f() { (${expression}); } })`);
    shouldParse(`(async function () { ({ m() { (${expression}); } }); })`);
}

// The arrow functions do what they say, also the ones that the parser had to parse again for the async function.
{
    const f = (a = (await) => await + 1) => a;
    shouldBe(f()(41), 42, "await as a parameter name");

    async function inArrowBody() {
        return () => (a = (await) => await * 2) => a;
    }
    shouldBe(settled(inArrowBody(), "inArrowBody")()()(21), 42, "await as a parameter name in the body of an arrow function in an async function");

    // "{ x = 1 }" is not an expression, so the nested arrow functions are first parsed in the scope that is not async.
    const captures = `(async function (p) {
        let local = 10;
        const first = await p;
        return ({ x = 1 }, a = (b = (c = x + local + p + first) => c) => b) => a;
    })`;
    for (const dropCache of [false, true]) {
        const arrow = settled((0, eval)(captures)(100), "captures");
        // A full collection drops the source provider cache. An arrow function is compiled when it is first called, and
        // parses its text again then.
        if (dropCache)
            fullGC();
        shouldBe(arrow({ })()(), 211, "captured variables");
        shouldBe(arrow({ x: 2 })()(), 212, "captured variables");
    }

    // The array is parsed as an expression, then tried as parameters, then parsed as an assignment pattern.
    const pattern = `(async function () {
        let a, b;
        [a = (b = (c = function () { var await = "nested"; return await; }) => c)] = [];
        return a()();
    })`;
    shouldBe(settled((0, eval)(pattern)(), "pattern"), "nested", "await as an identifier in a function nested in a pattern");

    class Base {
        name() { return "base"; }
    }
    class Derived extends Base {
        async method() {
            return ({ x = 1 }, a = (b = (c = [this.field, arguments[0], super.name(), x]) => c) => b) => a;
        }
        name() { return "derived"; }
    }
    const derived = new Derived;
    derived.field = "field";
    shouldBe(JSON.stringify(settled(derived.method("argument"), "method")({ })()()), `["field","argument","base",1]`, "this, arguments and super");

    async function withEval(p) {
        return ({ x = 1 }, a = (b = (c = eval("x + p")) => c) => b) => a;
    }
    shouldBe(settled(withEval(41), "withEval")({ })()(), 42, "eval");
}
