// The parameters of an arrow function in a generator are parsed with [+Yield]: `yield` is not an identifier in them, and a
// YieldExpression is an early error. That holds for an arrow function in the parameters of another arrow function too.
//
// The parser first reads "( ... )" as an expression. To see that it is a parameter list it parses it again in a scope
// that it throws away. That scope was never a generator, so an arrow function in there went to the source provider cache
// with `yield` as an identifier. The parse that knows about the generator then skipped that arrow function from the
// cache, so all of the code below that must throw was accepted. It was always rejected with
// --useSourceProviderCache=false, and the messages here are the ones that run gives.

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

function shouldParse(source) {
    try {
        (0, eval)(source);
    } catch (e) {
        if (e instanceof SyntaxError)
            throw new Error(`${source}: ${String(e)}`);
    }
}

const parameterName = "Cannot use 'yield' as a parameter name in a generator function.";
const yieldExpression = "Unexpected keyword 'yield'. Cannot use yield expression out of generator.";
const shorthand = "Cannot use abbreviated destructuring syntax for keyword 'yield'.";
const shorthandProperty = "Cannot use 'yield' as a shorthand property name in a generator function.";
const escaped = "Unexpected escaped characters in keyword token: 'yi\\u0065ld'";

// [text before the expression, text after it]. The messages below are those of the first one.
const generators = [
    ["(function* () { (", "); })"],
    ["(function* () { return ", "; })"],
    ["(function* (p = ", ") { })"],
    ["function* g() { (", "); }"],
    ["({ *g() { (", "); } })"],
    ["({ *g(p = ", ") { } })"],
    ["(class { *g() { (", "); } })", "strict"],
    ["(class { static *g(p = ", ") { } })", "strict"],
    ["(async function* () { (", "); })"],
    ["(async function* (p = ", ") { })"],
    ["(function* () { { let q; (", "); } })"],
    ["(function* (p = function* () { (", "); }) { })"],
];

// [expression, message, true if the expression is not valid where `yield` is an identifier either]
const invalid = [
    // The arrow function is in the parameters of another arrow function.
    ["(a = (yield) => 1) => a", parameterName],
    ["(a = (b = yield) => b) => a", yieldExpression],
    ["(a = (b = yield 1) => b) => a", "Unexpected number '1'. Expected ')' to end a compound expression.", true],
    ["(a = (yield = 1) => 1) => a", parameterName],
    ["(a = (...yield) => 1) => a", parameterName],
    ["(a = ([yield]) => 1) => a", parameterName],
    ["(a = ({ yield }) => 1) => a", shorthand],
    ["(a = ({ b = yield }) => 1) => a", yieldExpression],
    ["(a = async (yield) => 1) => a", parameterName],
    ["(a = async (b = yield) => b) => a", yieldExpression],
    ["(b = (yield) => 1, c) => b", parameterName],
    ["(c, b = (yield) => 1) => b", parameterName],
    ["({ a = (yield) => 1 }) => a", parameterName],
    ["({ k: a = (b = yield) => b }) => a", yieldExpression],
    ["([a = (yield) => 1]) => a", parameterName],
    ["(...[a = (yield) => 1]) => a", parameterName],
    ["async (a = (yield) => 1) => a", parameterName],
    ["async ({ a = (b = yield) => b }) => a", yieldExpression],
    // Other ways to use `yield` as an identifier.
    ["(a = (b = { yield }) => b) => a", shorthandProperty],
    ["(a = (b = ({ yield } = { })) => b) => a", shorthandProperty],
    ["(a = (b = [yield] = []) => b) => a", yieldExpression],
    ["(a = (yi\\u0065ld) => 1) => a", escaped],
    ["(a = (b = yi\\u0065ld) => b) => a", escaped],
    // The name of a generator expression. With the escape it was an error only where the code around it is a generator.
    ["(a = (b = function* yi\\u0065ld() { }) => b) => a", escaped, true],
    // An error before and after, but the message is another one now: the parameters are parsed as in a generator earlier.
    ["(a = async yield => 1) => a", undefined],
    ["(a = class { [yield]() { } }) => a", undefined, true],
    // Deeper.
    ["(a = (b = (yield) => 1) => b) => a", parameterName],
    ["(a = (b = (c = yield) => c) => b) => a", yieldExpression],
    ["(a = (b = (c = (d = (yield) => 1) => d) => c) => b) => a", parameterName],
    ["({ x = 1 }, a = (b = (c = yield) => c) => b) => a", yieldExpression],
    // "( ... )" is not a parameter list, but the parser has to try that to find out. The enclosing pattern is parsed twice.
    ["[a = (b = (yield) => 1)] = []", parameterName],
    ["[a = ({ [(yield) => 1]: 1 })] = []", parameterName],
    ["[a = (b = (c = yield) => c)] = []", yieldExpression],
    // One arrow function. These were rejected before, and the messages are the same.
    ["(yield) => 1", parameterName],
    ["(a = yield) => a", yieldExpression],
    ["({ a = yield }) => a", yieldExpression],
    ["(...[a = yield]) => a", yieldExpression],
];

for (const [before, after] of generators) {
    for (const [expression, message] of invalid)
        shouldThrowSyntaxError(before + expression + after, before === generators[0][0] ? message : undefined);
}

// `yield` is an identifier in the body of an arrow function, in a function that is not an arrow function, and in an arrow
// function that is in one of those.
const valid = [
    "(a = (b) => yield) => a",
    "(a = (b) => { var yield; }) => a",
    "(a = (b) => { (yield) => 1; }) => a",
    "(a = (b) => (c = (yield) => 1) => c) => a",
    "(a = function (yield) { yield; }) => a",
    "(a = function (b = yield) { }) => a",
    "(a = function () { (b = (yield) => 1) => b; }) => a",
    "(a = function* () { yield 1; }) => a",
    "(a = { yield: 1 }.yield) => a",
    // `yield` as a property name is not an identifier.
    "(a = ({ yield: b }) => b) => a",
    "(a = ({ yield: b = 1 }, c = (d = b) => d) => c) => a",
    "(a = (b = { yield: 1, get yield() { return 1; }, set yield(v) { }, yield() { } }) => b) => a",
    "(a = (b = ({ yield: c } = { })) => b) => a",
    "(a = (b = c.yield) => b) => a",
    "(a = (b = function yield() { }) => b) => a",
    "({ x = 1 }, a = (b = (c = 1) => c) => b) => a",
    "[a = (b = (c = 1) => c)] = []",
];

for (const [before, after, strict] of generators) {
    // `yield` is a reserved word in strict mode code.
    if (strict)
        continue;
    for (const expression of valid)
        shouldParse(before + expression + after);
}

// Outside a generator `yield` is an identifier in sloppy mode, at every level.
for (const [expression, , neverValid] of invalid) {
    if (neverValid)
        continue;
    shouldParse(`(function () { (${expression}); })`);
    shouldParse(`(function* () { () => { (${expression}); }; })`);
    shouldParse(`(function* () { function f() { (${expression}); } })`);
    shouldThrowSyntaxError(`(function () { "use strict"; (${expression}); })`);
}

// The arrow functions do what they say.
{
    const f = (a = (yield) => yield + 1) => a;
    shouldBe(f()(41), 42, "yield as a parameter name");

    function* inArrowBody() {
        return () => (a = (yield) => yield * 2) => a;
    }
    shouldBe(inArrowBody().next().value()()(21), 42, "yield as a parameter name in the body of an arrow function in a generator");

    const nest = (a = (yield = 1) => (b = (c = yield + 1) => c) => b) => a;
    shouldBe(nest()()()(), 2, "yield as a parameter name and in a default value");

    function yieldAsVariable() {
        var yield = 5;
        return ((a = (b = yield) => b * 2) => a)()();
    }
    shouldBe(yieldAsVariable(), 10, "yield as a variable in a default value");

    // "{ x = 1 }" is not an expression, so the nested arrow functions are first parsed in the scope that is not a generator.
    const captures = `(function* (p) {
        let local = 10;
        const first = yield;
        return ({ x = 1 }, a = (b = (c = x + local + p + first) => c) => b) => a;
    })`;
    for (const dropCache of [false, true]) {
        const generator = (0, eval)(captures)(100);
        generator.next();
        const arrow = generator.next(1000).value;
        // A full collection drops the source provider cache. An arrow function is compiled when it is first called, and
        // parses its text again then.
        if (dropCache)
            fullGC();
        shouldBe(arrow({ })()(), 1111, "captured variables");
        shouldBe(arrow({ x: 2 })()(), 1112, "captured variables");
    }

    // The parser still takes a valid arrow function in a generator from the cache: one with `yield` as a property name only,
    // and one with no `yield` at all. This shows in what the default value reads. It reads N of the generator, and the body
    // declares an N of its own. With the item the generator knows that its N is captured. When the parser has to parse the
    // arrow function to get there, the generator does not know it (a bug of that parse), and N is not defined.
    if (jscOptions().useSourceProviderCache) {
        function* yieldAsPropertyName() {
            let N = 40;
            return ({ x = 1 }, a = ({ yield: b } = { yield: N }) => { var N = 7; return b; }) => a;
        }
        shouldBe(yieldAsPropertyName().next().value({ })(), 40, "yield as a property name in a pattern");

        function* noYield() {
            let N = 40;
            return ({ x = 1 }, a = (b = N + x) => { var N = 7; return b; }) => a;
        }
        shouldBe(noYield().next().value({ })(), 41, "a default value that reads a variable that the body declares again");
    }

    class Base {
        name() { return "base"; }
    }
    class Derived extends Base {
        *method() {
            return ({ x = 1 }, a = (b = (c = [this.field, arguments[0], super.name(), x]) => c) => b) => a;
        }
        name() { return "derived"; }
    }
    const derived = new Derived;
    derived.field = "field";
    shouldBe(JSON.stringify(derived.method("argument").next().value({ })()()), `["field","argument","base",1]`, "this, arguments and super");

    function* withEval(p) {
        return ({ x = 1 }, a = (b = (c = eval("x + p")) => c) => b) => a;
    }
    shouldBe(withEval(41).next().value({ })()(), 42, "eval");
}
