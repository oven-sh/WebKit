//@ runDefault("--maxRegExpStackSize=65536", "--useRegExpJIT=0")

// The interpreter takes one backtracking context for the pattern's body before it starts. 5000
// optional terms need two frame slots each, 80000 bytes, which Options::maxRegExpStackSize does not
// hold here. The search is abandoned before it starts, and throws like one abandoned later on.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": expected " + JSON.stringify(expected) + " but got " + JSON.stringify(actual));
}

const tooWide = new RegExp("^" + "a?".repeat(5000) + "$");
for (let i = 0; i < 3; i++) {
    let error = null;
    try {
        tooWide.test("a");
    } catch (e) {
        error = e;
    }
    shouldBe(error instanceof RangeError, true, "a body that does not fit, use " + i);
    shouldBe(error.message, "Maximum call stack size exceeded.", "a body that does not fit, use " + i);
}

// The pool is usable after the throw.
shouldBe(new RegExp("^" + "a?".repeat(2000) + "$").test("a"), true, "a body that fits");
shouldBe(/^(?:a|b)+$/.test("abab"), true, "a small pattern");
