// complex.yaml runs this on Linux with 700 KB of environment and an 8 MB stack limit. The kernel counts the
// argument and environment strings, which are at the end of the main thread's stack, against that limit. The RegExp
// parser checks the native stack against the bound of the thread's stack. If the bound leaves the strings out, it is
// below the lowest address the kernel allows, and the parser overruns the stack before its check trips.

let error;
try {
    new RegExp("(?:".repeat(100000) + "a" + ")".repeat(100000));
} catch (e) {
    error = e;
}

const expected = "RangeError: Out of memory: Invalid regular expression: too many nested disjunctions";
if (String(error) !== expected)
    throw new Error("expected " + expected + ", got " + error);
