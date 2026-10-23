//@ requireOptions("--maximumRegExpJITCodeSize=1024")

// An inlined RegExp#test is emitted into the buffer that already holds the code of the
// DFG/FTL function it is inlined into. Comparing the size of that buffer with
// maximumRegExpJITCodeSize stopped code generation after the first term and left a
// matcher that accepted everything.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + expected + " but got " + actual);
}

function opaque(x) {
    return x;
}
noInline(opaque);

// The calls take more than maximumRegExpJITCodeSize bytes of code.
const test = new Function("opaque", "s", "k", `
    let sum = 0;
    ${"sum += opaque(k);".repeat(128)}
    return /^a/.test(s) ? sum : -sum;
`);
noInline(test);

for (let i = 0; i < testLoopCount; ++i) {
    const k = (i & 0xff) + 1;
    shouldBe(test(opaque, "x", k), -128 * k, "no match for i=" + i);
    shouldBe(test(opaque, "a", k), 128 * k, "match for i=" + i);
}
