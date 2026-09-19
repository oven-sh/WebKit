// The parser reads "[" and "{" as the start of an array or object literal, and parses the text again as a pattern when
// it finds "=" after the closing bracket. A destructuring assignment in that pattern, say in a default value, was
// parsed once per pass, and parsed what its own pattern held as often again: a nest of depth 18 took 0.3 seconds, and
// each level doubled that. None of the nests below finish if that comes back.

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
// To run a nest, the engine compiles it, and every default value runs. A shallow nest is enough to see that it works.
const runDepth = 6;

// Each shape is [text before the innermost value, text after it]. Every right side is empty, so every default value
// runs, down to the innermost one.
const shapes = {
    "array pattern": ["[a = ", "] = []"],
    "array pattern, second element": ["[b, a = ", "] = []"],
    "array pattern before a rest element": ["[a = ", ", ...rest] = []"],
    "object pattern": ["({ a = ", " } = {})"],
    "object pattern with a key": ["({ k: a = ", " } = {})"],
    "object pattern with a computed key": ["({ ['k']: a = ", " } = {})"],
    "object pattern before a rest property": ["({ a = ", ", ...rest } = {})"],
    "array pattern in an object pattern": ["({ k: [a = ", "] = [] } = {})"],
    "object pattern in an array pattern": ["[{ a = ", " } = {}] = []"],
    "member target": ["[o.a = ", "] = []"],
    "parenthesized": ["([a = ", "] = [])"],
    "call argument": ["id([a = ", "] = [])"],
    "conditional": ["true ? [a = ", "] = [] : null"],
    "assignment chain": ["[a = ", "] = b = []"],
    "body of an arrow function": ["(() => [a = ", "] = [])()"],
    "body of a function expression": ["(function () { return [a = ", "] = []; })()"],
};

// An arrow function is parsed again at every level too. The parser steps over the ones it already parsed, which
// takes the source provider cache: see arrow-function-nested-in-arrow-function-parameters-parse-time.js.
if (jscOptions().useSourceProviderCache)
    shapes["default value of an arrow function"] = ["((p = [a = ", "] = []) => p)()"];

globalThis.id = value => value;

const declarations = "var a, b, rest, reached, o = {};";
for (const [name, [before, after]] of Object.entries(shapes)) {
    const nest = levels => before.repeat(levels) + "(reached = 'innermost')" + after.repeat(levels);

    // Nothing calls these functions, so the deep nest is only parsed: once as it is, and once more by the parse that
    // an async function without await gets.
    shouldBe(typeof (0, eval)(`(function () { ${declarations} ${nest(depth)}; })`), "function", name);
    shouldBe(typeof (0, eval)(`(async function () { ${declarations} ${nest(depth)}; })`), "function", name + " in an async function");

    shouldBe(new Function(`${declarations} ${nest(runDepth)}; return reached;`)(), "innermost", name);
}

// The same in a generator and in an async function that runs.
{
    const [before, after] = shapes["array pattern in an object pattern"];
    const source = before.repeat(runDepth) + "(reached = 'innermost')" + after.repeat(runDepth);
    shouldBe((0, eval)(`(function* () { ${declarations} ${source}; return reached; })`)().next().value, "innermost", "generator");
    let result;
    (0, eval)(`(async function () { ${declarations} ${source}; return reached; })`)().then(value => { result = value; });
    drainMicrotasks();
    shouldBe(result, "innermost", "async function");
}

// Two candidates side by side at every level.
{
    const levels = 6;
    const nest = level => level ? `[a = ${nest(level - 1)}, b = ${nest(level - 1)}] = []` : "count++";
    let a, b, count = 0;
    eval(nest(levels));
    shouldBe(count, 2 ** levels, "two candidates per level");
}

// A value that is there keeps the default value from running, at every level.
{
    let a, log = [];
    const tag = value => (log.push(value), value);
    [a = tag("outer")] = [[a = tag("inner")] = ["given"]];
    shouldBe(a.length, 1, "the default value does not run");
    shouldBe(a[0], "given", "the default value does not run");
    shouldBe(log.length, 0, "the default value does not run");

    [a = ([a = tag("inner")] = [], tag("outer"))] = [];
    shouldBe(a, "outer", "the default value runs");
    shouldBe(log.join(), "inner,outer", "the default value runs");
}

// What a pattern uses does not depend on how often the parser went over it. Only the pattern names these variables,
// so a closure that does not capture them writes to a global variable, or throws in strict code.
{
    function shorthand() {
        let v = "init";
        ((p = ({ v } = { v: "new" })) => p)();
        return v;
    }
    shouldBe(shorthand(), "new", "shorthand property in the default value of an arrow function");

    function shorthandInNestedPatterns() {
        let v = "init", w;
        (() => { [w = [w = ({ v } = { v: "new" })] = []] = []; })();
        return v;
    }
    shouldBe(shorthandInNestedPatterns(), "new", "shorthand property in nested patterns");

    function shorthandStrict() {
        "use strict";
        let v = "init", w;
        (() => { [w = ({ v } = { v: "new" })] = []; })();
        return v;
    }
    shouldBe(shorthandStrict(), "new", "shorthand property in strict code");

    // An async function that never awaits is parsed twice when it is compiled: the destructuring assignments in it are
    // all known by the second time.
    async function neverAwaits(x) {
        let v = "init", w;
        (() => { [w = [w = ({ v } = { v: [x, this.field, arguments.length] })] = []] = []; })();
        return v;
    }
    let result;
    neverAwaits.call({ field: "field" }, "x", "y").then(value => { result = JSON.stringify(value); });
    drainMicrotasks();
    shouldBe(result, `["x","field",2]`, "async function without await");

    // Nothing but the pattern names arguments here, so only the pattern can say that the function uses it.
    async function neverAwaitsAndAssignsArguments() {
        let w;
        [w = ({ arguments } = { arguments: "new" })] = [];
    }
    neverAwaitsAndAssignsArguments("old");
    drainMicrotasks();
    shouldBe("arguments" in globalThis, false, "arguments as a shorthand property");

    class Base {
        name() { return "base"; }
    }
    class Derived extends Base {
        method() {
            let a, b;
            [a = [b = [this.field, arguments[0], super.name(), new.target]] = []] = [];
            return b;
        }
        name() { return "derived"; }
    }
    const derived = new Derived;
    derived.field = "field";
    shouldBe(JSON.stringify(derived.method("argument")), `["field","argument","base",null]`, "this, arguments, super and new.target");

    function withEval(x) {
        let a, b;
        [a = [b = eval("x + 1")] = []] = [];
        return b;
    }
    shouldBe(withEval(41), 42, "eval");

    function* withYield() {
        let a, b;
        [a = [b = yield "yielded"] = []] = [];
        return b;
    }
    const generator = withYield();
    shouldBe(generator.next().value, "yielded", "yield");
    shouldBe(generator.next("sent").value, "sent", "yield");

    let target;
    for ([target = [target = "b"] = []] of [[]])
        shouldBe(target[0], undefined, "pattern of a for-of statement");
    shouldBe(target.length, 0, "pattern of a for-of statement");
}

// An error at any level is still an error, and the message does not depend on the level. What a default value can
// hold depends on what encloses it, so the pattern is parsed again each time, even when the parser knows that a
// destructuring assignment starts there.
shouldThrowSyntaxError("[a = [b = [c = 1 1] = []] = []] = []", "Unexpected number '1'. Expected either a closing ']' or a ',' following an array element.");
shouldThrowSyntaxError("[a = [b = [c = 1] = [] []] = []] = []", "Unexpected token ']'");
shouldThrowSyntaxError("[a = [b = [1] = []] = []] = []", "Invalid destructuring assignment target.");
shouldThrowSyntaxError("[a = [b = [c()] = []] = []] = []", "Invalid destructuring assignment target.");
shouldThrowSyntaxError("[a = [b = [...c, d] = []] = []] = []", "Unexpected token ','. Expected a closing ']' following a rest element destructuring pattern.");
shouldThrowSyntaxError("({ a = ({ b = ({ c: 1 } = {}) } = {}) } = {})", "Unexpected token '='. Expected a ':' following the property name 'a'.");
shouldThrowSyntaxError("({ a = ({ b = ({ c = 1 }) } = {}) } = {})", "Unexpected token '='. Expected a ':' following the property name 'a'.");
shouldThrowSyntaxError("'use strict'; [a = [b = [eval] = []] = []] = []", "Cannot modify 'eval' in strict mode.");
shouldThrowSyntaxError("'use strict'; [a = [b = ({ arguments } = {})] = []] = []", "Cannot modify 'arguments' in strict mode.");
shouldThrowSyntaxError("(async function () { (p = [a = [b = await 1] = []] = []) => p; })", "Unexpected number '1'. Expected either a closing ']' or a ',' following an array element.");
shouldThrowSyntaxError("(function* () { (p = [a = [b = yield] = []] = []) => p; })", "Unexpected keyword 'yield'. Cannot use yield expression out of generator.");
shouldThrowSyntaxError("(class { field = [a = [b = arguments] = []] = []; })", "Unexpected identifier 'arguments'. Cannot reference 'arguments' in class field initializer.");
shouldThrowSyntaxError("async (p = [a = [b = (await) => 1] = []] = []) => p", "Cannot use 'await' within a parameter default expression.");

// These are not destructuring assignments, and stay what they are when an enclosing candidate is parsed again.
{
    let a, b, c;
    [a = [b = 1][0]] = [];
    shouldBe(a, 1, "member of an array literal in a default value");
    [a = [b = 2] == 0] = [];
    shouldBe(a, false, "comparison in a default value");
    [a = { b: c = 3 }.b] = [];
    shouldBe(a, 3, "member of an object literal in a default value");
    shouldBe(c, 3, "member of an object literal in a default value");
    [a = [[b = 4]][0][0]] = [];
    shouldBe(a, 4, "nested array literals in a default value");
    [a = ([b] = [5], [c] = [b + 1])] = [];
    shouldBe(a[0], 6, "two destructuring assignments in a default value");
}
