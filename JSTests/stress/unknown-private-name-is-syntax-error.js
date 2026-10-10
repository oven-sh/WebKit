//@ runDefault("--exposePrivateIdentifiers=1")
//@ run("validate-bytecode", "--exposePrivateIdentifiers=1", "--validateBytecode=true")

// A private name (@name), a well-known symbol name (@@name), or a private name that an intrinsic takes as a string,
// is looked up in the VM's BuiltinNames. A name the VM does not have is a SyntaxError. Builtin source is not always
// this build's: an embedder that generates bytecode for another executable compiles that executable's builtins, and
// exposePrivateIdentifiers gives the syntax to any script. The lookup result used to be dereferenced without a test.

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

// Names it does not have, in a script.
const scripts = [
    ["@nope", "Invalid private name '@nope'"],
    ["a.@nope", "Invalid private name '@nope'"],
    ["@@nope", "Invalid private name '@@nope'"],
    ["a.@@nope", "Invalid private name '@@nope'"],
    ["@", "Invalid private name '@'"],
    ["@;", "Invalid private name '@'"],
    ["@@;", "Invalid private name '@@'"],
    ["@1", "Invalid private name '@1'"],
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

// What an embedder does with builtin source: make the executable, generate all of its code, encode it, decode it.
// The second run validates a builtin when its executable is made, which parses it there.
const builtins = [
    ["(function (a) { return @nope; })", "Invalid private name '@nope'"],
    ["(function (a) { return a.@@nope; })", "Invalid private name '@@nope'"],
    ["(function (a) { return 1 +; })", "Unexpected token ';'"],
    ['(function (a) { return @getByIdDirectPrivate(a, "nope"); })', "Invalid private name 'nope'"],
    // A nested function that fails is the error of the whole builtin, whichever function is generated after it.
    ['(function (a) { function first() { return @getByIdDirectPrivate(a, "nope"); } function second() { return 1; } return 2; })', "Invalid private name 'nope'"],
    ['(function (a) { function first() { return 1; } function second() { return @putByIdDirectPrivate(a, "nope", 1); } return 2; })', "Invalid private name 'nope'"],
    ['(function (a) { function first() { function deep() { return @getByIdDirectPrivate(a, "nope"); } return deep; } function second() { return 1; } var third = function () { return 3; }; return 2; })', "Invalid private name 'nope'"],
];
const good = "(function (a) { function check() { return @isCallable(a); } return check(); })";
shouldBe(builtinFromBytecodeCache(good, true)(function () { }), true);
for (const [source, message] of builtins) {
    shouldBe(syntaxErrorOf(() => builtinFromBytecodeCache(source, true)), message);
    shouldBe(builtinFromBytecodeCache(good, true)(function () { }), true);
}
