//@ runDefault("--maxRegExpStackSize=1048576", "--useRegExpJIT=0")
//@ runDefault("--maxRegExpStackSize=1048576")

// Each iteration of a quantified group keeps a backtracking context until the search ends. A search
// that needs more of them than Options::maxRegExpStackSize holds is abandoned, and throws the stack
// overflow RangeError instead of reporting no match. With the RegExp JIT the contexts go on the
// machine stack first: when they do not fit there the search starts again in the interpreter, so
// both runs end in the interpreter's pool.

const overflowMessage = "Maximum call stack size exceeded.";

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": expected " + JSON.stringify(expected) + " but got " + JSON.stringify(actual));
}

function shouldAbandon(run, what) {
    let result;
    try {
        result = run();
    } catch (error) {
        if (!(error instanceof RangeError) || error.message !== overflowMessage)
            throw new Error(what + ": expected the RangeError, got " + error);
        return;
    }
    throw new Error(what + ": expected the RangeError, got the result " + JSON.stringify(result && result.length));
}

const pattern = "^(?:a|b)+$";
const big = "a".repeat(2000000);
const small = "a".repeat(5000);

shouldAbandon(() => new RegExp(pattern).test(big), "test");
shouldAbandon(() => new RegExp(pattern).exec(big), "exec");
shouldAbandon(() => big.match(new RegExp(pattern)), "match");
shouldAbandon(() => [...big.matchAll(new RegExp(pattern, "g"))], "matchAll");
shouldAbandon(() => big.search(new RegExp(pattern)), "search");
shouldAbandon(() => big.replace(new RegExp(pattern), "X"), "replace");
shouldAbandon(() => big.split(new RegExp(pattern)), "split");

shouldBe(new RegExp(pattern).test(small), true, "small test");
shouldBe(new RegExp(pattern).exec(small)[0].length, 5000, "small exec");

// No group, no contexts: the length of the subject alone does not abandon a search.
shouldBe(/^[ab]+$/.test(big), true, "character class");

// A throw is not a failed match: lastIndex stays where it was, and the RegExp still works.
{
    const regExp = /(?:a|b)+$/g;
    regExp.lastIndex = 3;
    shouldAbandon(() => regExp.test(big), "test /g");
    shouldBe(regExp.lastIndex, 3, "lastIndex after the throw");
    shouldBe(regExp.test(small), true, "reuse after the throw");
    shouldBe(regExp.lastIndex, 5000, "lastIndex after the match");
}
