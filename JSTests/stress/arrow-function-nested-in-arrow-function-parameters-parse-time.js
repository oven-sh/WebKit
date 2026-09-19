//@ requireOptions("--useSourceProviderCache=true")

// The parser reads "(" as the start of an expression, and parses the text again as the parameters of an arrow function
// when it finds "=>" after the ")". An arrow function in those parameters, say in a default value, was parsed once
// per pass, and parsed what its own parameters held as often again: a nest of depth 24 took 16 seconds, and each level
// more than doubled that. None of the nests below finish if that comes back.
//
// The source provider cache is what lets the parser step over an arrow function it already parsed. The mode that runs
// without it (no-cjit-validate-phases) still parses a nested arrow function twice per level, so it cannot run this.

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

// A nest of this depth did not finish before.
const depth = 32;
// A call compiles one level with a new parser, which parses the levels below it again. A walk down the whole nest
// costs depth^3, so it gets a nest of its own.
const callDepth = 12;

// Each shape is [text before the innermost value, text after it, how to get from one level's function to the next].
const call = f => f();
const shapes = {
    "default value": ["(a = ", ") => a", call],
    "second parameter": ["(b, a = ", ") => a", call],
    "before a rest parameter": ["(a = ", ", ...rest) => a", call],
    "block body": ["(a = ", ") => { return a; }", call],
    "object pattern": ["({ a = ", " }) => a", f => f({ })],
    "object pattern with a key": ["({ k: a = ", " }) => a", f => f({ })],
    "array pattern": ["([a = ", "]) => a", f => f([])],
    "parenthesized": ["((a = ", ") => a)", call],
    "call argument": ["id((a = ", ") => a)", call],
    "array element": ["[(a = ", ") => a][0]", call],
    "conditional": ["true ? (a = ", ") => a : null", call],
    "async": ["async (a = ", ") => a", null],
    "default value of a function expression": ["(function (a = ", ") { return a; })", call],
    "default value of a method": ["({ m(a = ", ") { return a; } }).m", call],
};

globalThis.id = value => value;

for (const [name, [before, after, next]] of Object.entries(shapes)) {
    const nest = levels => before.repeat(levels) + "'innermost'" + after.repeat(levels);
    shouldBe(typeof (0, eval)(nest(depth)), "function", name);

    // The same text where a syntax check reads it first, and the call parses it again to compile the function.
    shouldBe(typeof new Function(`return ${nest(depth)};`)(), "function", name + " in a function");

    if (!next)
        continue;
    let value = (0, eval)(nest(callDepth));
    for (let i = 0; i < callDepth; i++)
        value = next(value);
    shouldBe(value, "innermost", name);
}

// An async arrow function returns a promise at every level, so walk down with await.
{
    const [before, after] = shapes["async"];
    let value = (0, eval)(before.repeat(callDepth) + "'innermost'" + after.repeat(callDepth));
    let result;
    (async () => {
        for (let i = 0; i < callDepth; i++)
            value = await value();
        result = value;
    })();
    drainMicrotasks();
    shouldBe(result, "innermost", "async");
}

// Two candidates side by side at every level.
{
    const levels = 8;
    const nest = level => level ? `(a = ${nest(level - 1)}, b = ${nest(level - 1)}) => [a, b]` : "1";
    let value = (0, eval)(nest(levels));
    let calls = 0;
    while (typeof value === "function") {
        value = value()[1];
        calls++;
    }
    shouldBe(calls, levels, "two candidates per level");
    shouldBe(value, 1, "two candidates per level");
}

// What an arrow function in a default value captures does not depend on how often the parser went over it.
{
    function capture(x) {
        let y = 10;
        return (a = (b = (c = (d = x + y) => d + y) => c) => b) => a;
    }
    shouldBe(capture(1)()()()(), 21, "captured variables");

    class Base {
        name() { return "base"; }
    }
    class Derived extends Base {
        constructor() {
            super();
            this.field = "field";
        }
        method() {
            return (a = (b = (c = [this.field, arguments[0], super.name(), new.target]) => c) => b) => a;
        }
        name() { return "derived"; }
    }
    shouldBe(JSON.stringify(new Derived().method("argument")()()()), `["field","argument","base",null]`, "this, arguments, super and new.target");

    function withEval(x) {
        return (a = (b = (c = eval("x + 1")) => c) => b) => a;
    }
    shouldBe(withEval(41)()()(), 42, "eval");

    // An async function that never awaits is parsed twice when it is compiled: the arrow functions in it are all known
    // by the second time.
    async function neverAwaits(x) {
        return ((a = (b = (c = [x, this.field, arguments.length]) => c) => b) => a)()()();
    }
    let result;
    neverAwaits.call({ field: "field" }, "x", "y").then(value => { result = JSON.stringify(value); });
    drainMicrotasks();
    shouldBe(result, `["x","field",2]`, "async function without await");

    // A destructuring assignment is parsed as an expression first and as a pattern after that.
    let target;
    [target = (a = (b = (c = "c") => c) => b) => a] = [];
    shouldBe(target()()(), "c", "default value in an array assignment pattern");
    ({ target = (a = (b = (c = "c") => c) => b) => a } = { });
    shouldBe(target()()(), "c", "default value in an object assignment pattern");
    for ([target = (a = (b = "b") => b) => a] of [[]])
        shouldBe(target()(), "b", "default value in the pattern of a for-of statement");
}

// An error at any level is still an error. What is allowed in a default value depends on what encloses it, so the
// parameters are checked again each time, even when the parser knows an arrow function starts there.
shouldThrowSyntaxError("(a = (b = (c = 1 1) => c) => b) => a");
shouldThrowSyntaxError("(a = (b = (c = 1) => c c) => b) => a");
shouldThrowSyntaxError("(a = (b = (c, c) => c) => b) => a", "Duplicate parameter 'c' not allowed in an arrow function.");
shouldThrowSyntaxError("(a = (b = (c = 1) => { 'use strict'; }) => b) => a");
shouldThrowSyntaxError("'use strict'; (a = (b = (eval = 1) => eval) => b) => a");
shouldThrowSyntaxError("(a = (b = (c = 1)\n=> c) => b) => a");
shouldThrowSyntaxError("(a = (b = (...c, d) => c) => b) => a");
shouldThrowSyntaxError("(a = (b = (c.d) => c) => b) => a");
shouldThrowSyntaxError("async (a = (await) => 1) => a", "Cannot use 'await' within a parameter default expression.");
shouldThrowSyntaxError("async (a = (b = (await) => 1) => b) => a", "Cannot use 'await' within a parameter default expression.");
shouldThrowSyntaxError("async (a = (b = await 1) => b) => a");
shouldThrowSyntaxError("(async function () { (a = (await) => 1) => a; })", "Cannot use 'await' within a parameter default expression.");
shouldThrowSyntaxError("(async function () { (a = (b = (c = await) => c) => b) => a; })", "Cannot use 'await' within a parameter default expression.");
shouldThrowSyntaxError("(class { static { (a = (b = (c = await) => c) => b) => a; } })");
shouldThrowSyntaxError("(class { field = (a = (b = (c = arguments) => c) => b) => a; })");

// These are not arrow functions, and stay what they are when an enclosing candidate is parsed again.
{
    let a, b;
    shouldBe(((x = (a = (b = 2)) + 1) => x)(), 3, "parenthesized assignments in a default value");
    shouldBe(a, 2, "parenthesized assignments in a default value");
    shouldBe(b, 2, "parenthesized assignments in a default value");
    let async = value => value * 2;
    shouldBe(((x = async(a = 4)) => x)(), 8, "call of a function named async in a default value");
    shouldBe(((x = async (y = 5) => y) => x)() instanceof Function, true, "async arrow function in a default value");
}
