// The parser has several places that parse the same text a second time as another production when the first parse
// fails: an object or array literal as a destructuring pattern, a pattern element as a member expression, a
// parenthesized expression as arrow function parameters. A stack overflow used to count as such a failure, so each
// nesting level on the way out parsed everything below it again, and each of those parses did the same. These
// programs never finished parsing. Now the first overflow ends the parse.

function shouldThrowStackOverflow(name, run) {
    let error;
    try {
        run();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof RangeError) || error.message !== "Maximum call stack size exceeded.")
        throw new Error(name + ": expected a stack overflow RangeError, got " + error);
}

function nest(open, inner, close, depth) {
    return open.repeat(depth) + inner + close.repeat(depth);
}

// Far past what any stack size allows, so that every configuration overflows.
const depth = 100000;

const programs = {
    "object literal": "var x = " + nest("{v:", "1", "}", depth) + ";",
    "array literal": "var x = " + nest("[", "1", "]", depth) + ";",
    "object and array literal": "var x = " + nest("{v:[", "1", "]}", depth) + ";",
    "array assignment pattern": "var a; " + nest("[", "a", "]", depth) + " = [];",
    "object assignment pattern": "var a; (" + nest("{v:", "a", "}", depth) + " = {});",
    "arrow function parameter default": "var x = " + nest("(a = ", "1", ")", depth) + ";",
    "arrow function with parameter default": "var x = " + nest("(a = ", "1", ") => 1", depth) + ";",
    "async arrow function with parameter default": "var x = " + nest("async (a = ", "1", ") => 1", depth) + ";",
};

// eval builds the AST as it parses (ASTBuilder). The Function constructor only checks the syntax of the body, which is
// the parser's other instantiation (SyntaxChecker).
for (const [name, program] of Object.entries(programs)) {
    shouldThrowStackOverflow(name + " in eval", () => (0, eval)(program));
    shouldThrowStackOverflow(name + " in a function body", () => new Function(program));
}

// Nesting that fits the stack still parses, and still gets the second parse where it needs one.
function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("expected " + expected + ", got " + actual);
}

const shallow = 50;

// Not a bare literal: eval gives a program that is only a literal to LiteralParser, and this parser never sees it.
let literal = (0, eval)("var nested = " + nest("{v:[", "42", "]}", shallow) + "; nested");
for (let i = 0; i < shallow; i++)
    literal = literal.v[0];
shouldBe(literal, 42);

shouldBe((0, eval)("var a; " + nest("[", "a", "]", shallow) + " = " + nest("[", "7", "]", shallow) + "; a"), 7);
shouldBe((0, eval)("var b; (" + nest("{v:", "b", "}", shallow) + " = " + nest("{v:", "5", "}", shallow) + "); b"), 5);

// Each level of this one is parsed more than once with or without a stack overflow, so it stays small.
const shallowArrows = 8;
let arrow = (0, eval)(nest("(a = ", "9", ") => a", shallowArrows));
for (let i = 0; i < shallowArrows; i++)
    arrow = arrow();
shouldBe(arrow, 9);
