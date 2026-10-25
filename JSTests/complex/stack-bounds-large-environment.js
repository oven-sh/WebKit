// complex.yaml runs this on Linux under glibc's loader, with 700 KB of environment and an 8 MB stack limit. The kernel counts the strings of execve() against the limit.
// The RegExp parser checks the native stack against the thread's bound: a bound that leaves the strings out lets it overrun the stack.

let error;
try {
    new RegExp("(?:".repeat(100000) + "a" + ")".repeat(100000));
} catch (e) {
    error = e;
}

const expected = "RangeError: Out of memory: Invalid regular expression: too many nested disjunctions";
if (String(error) !== expected)
    throw new Error("expected " + expected + ", got " + error);
