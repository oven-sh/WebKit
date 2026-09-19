// The parser reads "(" as the start of an expression, and parses the text again as the parameters of an arrow
// function when the expression does not parse. It does the same with "[" and "{", which it parses again as a
// destructuring pattern. A syntax error deep in such a nest was found once per pass, and every level doubled the
// passes: "(a = ".repeat(18) + "9 9" + ")".repeat(18) took 0.2 seconds to report, and depth 30 took minutes.
//
// The parser now steps over the text with the lexer first. Without a "=>" after the ")" that closes the
// parentheses, the text holds no parameters, and without a "=" after the "]" or "}" it holds no pattern.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${String(expected)} but got ${String(actual)}`);
}

function shouldThrowSyntaxError(source, message, what) {
    let error;
    try {
        (0, eval)(source);
    } catch (e) {
        error = e;
    }
    if (!(error instanceof SyntaxError))
        throw new Error(`${what}: expected a SyntaxError but got ${String(error)}`);
    if (message !== undefined)
        shouldBe(error.message, message, what);
}

// A nest of this depth did not finish before.
const depth = 32;

// Each shape is [text before the error, text after it, the message]. The first seven took 2^depth passes to report it.
// The last two never did, because what follows their "(" is no parameter, but they take the same new path.
const shapes = {
    "parenthesized assignment": ["(a = ", ")", "Unexpected number '9'. Expected ')' to end a compound expression."],
    "array literal": ["[", "]", "Unexpected number '9'. Expected either a closing ']' or a ',' following an array element."],
    "array literal with a default value": ["[a = ", "]", "Unexpected number '9'. Expected either a closing ']' or a ',' following an array element."],
    "object literal": ["({ k: ", " })", "Unexpected number '9'. Expected '}' to end an object literal."],
    "call of a function named async": ["async (a = ", ")", "Unexpected number '9'. Expected ')' to end an argument list."],
    "array literal in parentheses": ["([", "])", "Unexpected number '9'. Expected either a closing ']' or a ',' following an array element."],
    "parentheses in an array literal": ["[(a = ", ")]", "Unexpected number '9'. Expected ')' to end a compound expression."],
    "parentheses": ["(", ")", "Unexpected number '9'. Expected ')' to end a compound expression."],
    "call argument": ["f(", ")", "Unexpected number '9'. Expected ')' to end an argument list."],
};

for (const [name, [before, after, message]] of Object.entries(shapes)) {
    shouldThrowSyntaxError(before.repeat(depth) + "9 9" + after.repeat(depth), message, name);

    // The same text in the places where an assignment expression is parsed again.
    shouldThrowSyntaxError(`(p = ${before.repeat(depth)}9 9${after.repeat(depth)}) => p`, message, name + " in a default value");
    shouldThrowSyntaxError(`[p = ${before.repeat(depth)}9 9${after.repeat(depth)}] = []`, message, name + " in a pattern");
    shouldThrowSyntaxError(`(function () { 'use strict'; ${before.repeat(depth)}9 9${after.repeat(depth)}; })`, message, name + " in strict code");
}

// The source ends before the brackets close, so nothing follows them.
for (const [name, [before]] of Object.entries(shapes))
    shouldThrowSyntaxError(before.repeat(depth), "Unexpected end of script", name + " truncated");

// What the text can still be is parsed as before. Each of these needs the pass that the check above skips when it
// finds no "=>" or "=", because the text does not parse as an expression.
{
    shouldBe(((a, b,) => a)(1, 2), 1, "trailing comma in parameters");
    shouldBe(((...a) => a.length)(1, 2), 2, "rest parameter");
    shouldBe((() => 1)(), 1, "no parameters");
    shouldBe(((a, ...b) => b.length)(1, 2, 3), 2, "rest parameter after a parameter");
    shouldBe((({ a = 1 }) => a)({}), 1, "object pattern with a default value");
    shouldBe((({ a: { b = 2 } }) => b)({ a: {} }), 2, "nested object pattern with a default value");
    shouldBe((([a = 3]) => a)([]), 3, "array pattern with a default value");
    shouldBe((async (a, b,) => a)(4, 5) instanceof Promise, true, "trailing comma in async parameters");

    let a, b;
    ({ a = 6 } = {});
    shouldBe(a, 6, "object pattern with a default value in an assignment");
    [a = 7, b = 8] = [];
    shouldBe(a + b, 15, "array pattern with default values in an assignment");
    ({ a: [b = 9] = [] } = {});
    shouldBe(b, 9, "nested pattern in an assignment");
}

// The lexer alone does not see a template literal or a regular expression the way the parser does, so the check
// answers "it can be" for them and the parser does the work it did before.
{
    shouldBe(((a = `)`) => a)(), ")", "template literal in a default value");
    shouldBe(((a = `${")"}`) => a)(), ")", "template substitution in a default value");
    shouldBe(((a = /\)/.source) => a)(), "\\)", "regular expression in a default value");
    shouldBe(((a = 6 / 2) => a)(), 3, "division in a default value");
    shouldBe(((a = ")") => a)(), ")", "string in a default value");
    shouldBe(((a = ")" /* ) */) => a)(), ")", "comment in a default value");

    let a;
    [a = `)`] = [];
    shouldBe(a, ")", "template literal in a pattern");
    [a = /]/.source] = [];
    shouldBe(a, "]", "regular expression in a pattern");
    [a = 8 / 4] = [];
    shouldBe(a, 2, "division in a pattern");
    shouldThrowSyntaxError("[a = `]`] = 9 9", undefined, "template literal in a pattern with an error after it");
    shouldThrowSyntaxError("(a = `)`) => 9 9", undefined, "template literal in parameters with an error after it");
}

// Brackets that do not nest, where the check cannot say which one closes the first.
shouldThrowSyntaxError("(a = [b) => a", undefined, "a bracket that closes nothing");
shouldThrowSyntaxError("[a = (b] = []", undefined, "a bracket that closes nothing in a pattern");
shouldThrowSyntaxError("({ a = [b) } = {})", undefined, "a bracket that closes nothing in an object literal");

// The error of the first pass is the one to report, and it does not depend on the level.
shouldThrowSyntaxError("(a = (b = 9 9))", "Unexpected number '9'. Expected ')' to end a compound expression.", "nested parentheses");
shouldThrowSyntaxError("(a = (b = 9 9)) => a", "Unexpected number '9'. Expected ')' to end a compound expression.", "nested parentheses in parameters");
shouldThrowSyntaxError("[a = [b = 9 9]]", "Unexpected number '9'. Expected either a closing ']' or a ',' following an array element.", "nested array literals");
shouldThrowSyntaxError("[a = [b = 9 9]] = []", "Unexpected number '9'. Expected either a closing ']' or a ',' following an array element.", "nested array literals in a pattern");
shouldThrowSyntaxError("({ a: { b: 9 9 } })", "Unexpected number '9'. Expected '}' to end an object literal.", "nested object literals");
shouldThrowSyntaxError("({ a: { b: 9 9 } } = {})", "Unexpected number '9'. Expected '}' to end an object literal.", "nested object literals in a pattern");
shouldThrowSyntaxError("'use strict'; (a = (eval = 1) => eval) => a", "Cannot modify 'eval' in strict mode.", "an error that only strict code has");
shouldThrowSyntaxError("async (a = (await) => 1) => a", "Cannot use 'await' within a parameter default expression.", "an error that only an async function has");
