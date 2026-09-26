//@ runDefault("--useDollarVM=1")
//@ runBytecodeCache
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1")

// A source's line start table is made when its code is compiled, and the code has it too, and so does its bytecode: in
// the second run of the last two configurations this file's text is not read. Either way, asking for a line or column
// later does not read the source.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

function declared(a, b) { return a + b; }

class Klass {
    method() { return 1; }
}

shouldBe($vm.lineStartTableIsBuilt(declared), true, "a function of this file");
shouldBe($vm.lineStartTableIsBuilt(Klass.prototype.method), true, "a method of this file");
shouldBe(new Error().line, 22, "a line of this file");

// Each of these has a source of its own.
const long = `/* ${"x".repeat(1024)} */\n`;
shouldBe($vm.lineStartTableIsBuilt((0, eval)(long + "(function () { })")), true, "indirect eval");
shouldBe($vm.lineStartTableIsBuilt(eval(long + "(function () { })")), true, "direct eval");

// The same text again is not compiled again: the code is shared, and the new source gets its table from the code.
shouldBe($vm.lineStartTableIsBuilt((0, eval)(long + "(function () { })")), true, "indirect eval of the same text");

// A table is not worth its cost to a short source, which has none until it is asked for a position.
const short = (0, eval)("(function () {\n    return new Error().line;\n})");
shouldBe($vm.lineStartTableIsBuilt(short), false, "a short source");
shouldBe(short(), 2, "a line of a short source");
shouldBe($vm.lineStartTableIsBuilt(short), true, "a short source that was asked");

// The builtins need no table.
shouldBe($vm.lineStartTableIsBuilt(Array.prototype.map), false, "a builtin");
