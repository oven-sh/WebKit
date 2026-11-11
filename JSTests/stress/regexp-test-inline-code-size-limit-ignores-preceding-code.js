//@ requireOptions("--maximumRegExpJITCodeSize=1024")

// An inlined RegExp#test is emitted into the buffer that already holds the code of the
// DFG/FTL function it is inlined into. Comparing the size of that buffer with
// maximumRegExpJITCodeSize stopped code generation part way through the matcher.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("expected " + expected + " but got " + actual);
}

function opaque() { }
noInline(opaque);

// The calls take more than maximumRegExpJITCodeSize bytes of code.
const test = new Function("s", `${"opaque();".repeat(128)} return /^a/.test(s);`);
noInline(test);

for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(test("x"), false);
    shouldBe(test("a"), true);
}
