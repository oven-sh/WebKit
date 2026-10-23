//@ runDefault("--regExpMatchLimit=0")
//@ runDefault("--regExpMatchLimit=0", "--useRegExpJIT=0")

// Both RegExp engines count the steps of a search down to zero. A limit of 0 is taken as 1, the
// lowest limit there is, so the first step abandons the search. Counted down from 0, the steps
// would wrap around and the search would have 2^32 of them.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": expected " + JSON.stringify(expected) + " but got " + JSON.stringify(actual));
}

// The first iteration of the group is a step in JIT code and in the interpreter.
const regExp = /(?:a|b)+c/;
for (let i = 0; i < 3; i++) {
    let error = null;
    try {
        regExp.test("abc");
    } catch (e) {
        error = e;
    }
    shouldBe(error instanceof RangeError, true, "use " + i);
    shouldBe(error.message, "Regular expression backtracking limit exceeded", "use " + i);
}
