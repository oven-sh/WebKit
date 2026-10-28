// complex.yaml runs this on Linux with an 8 MB stack limit, under a seccomp filter that makes prlimit64() fail: the limit cannot be read.
// The RegExp parser checks the native stack against the bound of the main thread. 8 MB hold about 34,000 of these groups.

function compile(depth) {
    try {
        new RegExp("(?:".repeat(depth) + "a" + ")".repeat(depth));
        return "compiled";
    } catch (e) {
        return String(e);
    }
}

// A bound above the stack pointer, or close below it, refuses these.
const shallow = compile(10000);
if (shallow !== "compiled")
    throw new Error("10000 groups: expected compiled, got " + shallow);

// A bound below the end of the stack lets these run past it.
const deep = compile(100000);
const expected = "RangeError: Out of memory: Invalid regular expression: too many nested disjunctions";
if (deep !== expected)
    throw new Error("100000 groups: expected " + expected + ", got " + deep);
