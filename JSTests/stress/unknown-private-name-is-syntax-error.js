//@ runDefault("--exposePrivateIdentifiers=1")

// A private name (@name), a well-known symbol name (@@name), or a private name that an intrinsic takes as a string,
// is looked up in the VM's BuiltinNames. A name the VM does not have is a SyntaxError. exposePrivateIdentifiers gives
// the syntax to any script. The lookup result used to be dereferenced without a test.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("bad value: " + actual + ", expected " + expected);
}

function syntaxErrorOf(run) {
    try {
        run();
    } catch (error) {
        if (!(error instanceof SyntaxError))
            throw error;
        return error.message;
    }
    throw new Error("no error was thrown");
}

// Names the VM has.
shouldBe((0, eval)("@isCallable(function () { })"), true);
shouldBe((0, eval)("typeof [].@@iterator"), "function");
shouldBe((0, eval)('var object = { }; @putByIdDirectPrivate(object, "iteratedObject", 5); @getByIdDirectPrivate(object, "iteratedObject")'), 5);

// Names it does not have, and names with an escape in them, in a script.
const scripts = [
    ["@nope", "Invalid private name '@nope'"],
    ["a.@nope", "Invalid private name '@nope'"],
    ["@@nope", "Invalid private name '@@nope'"],
    ["a.@@nope", "Invalid private name '@@nope'"],
    ["@", "Invalid private name '@'"],
    ["@;", "Invalid private name '@'"],
    ["@@;", "Invalid private name '@@'"],
    ["@1", "Invalid private name '@1'"],
    ["@\\u0069sCallable", "Invalid private name '@\\u0069sCallable'"],
    ["@is\\u0043allable", "Invalid private name '@is\\u0043allable'"],
    ["(function () { return @nope; })", "Invalid private name '@nope'"],
    ['@getByIdDirectPrivate({ }, "nope")', "Invalid private name 'nope'"],
    ['@putByIdDirectPrivate({ }, "nope", 1)', "Invalid private name 'nope'"],
];
for (const [source, message] of scripts) {
    shouldBe(syntaxErrorOf(() => (0, eval)(source)), message);
    shouldBe(syntaxErrorOf(() => new Function(source)()), message);
}

// The name an intrinsic takes as a string is looked up when the function that has it is compiled: at its first call,
// and again at the next one.
const compiledLater = (0, eval)('(function () { return @getByIdDirectPrivate({ }, "nope"); })');
shouldBe(syntaxErrorOf(compiledLater), "Invalid private name 'nope'");
shouldBe(syntaxErrorOf(compiledLater), "Invalid private name 'nope'");

// A function reports its first failure. This chain is too deep for the bytecode generator, which recurses for each
// operand. The parser does not recurse for it, so no size of stack makes the parser fail first.
const tooDeep = "var a = 1; " + Array(400000).fill("a").join("*") + ";";
shouldBe(syntaxErrorOf(() => (0, eval)('@getByIdDirectPrivate({ }, "nope"); ' + tooDeep)), "Invalid private name 'nope'");
