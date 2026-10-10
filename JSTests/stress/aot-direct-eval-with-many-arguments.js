//@ runDefault("--compileMainScriptAheadOfTime=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function identity(x) { return x; }
noInline(identity);

function evaluatesWithTen() { return eval("40 + 2", 2, 3, 4, 5, 6, 7, 8, 9, 10); }
check(evaluatesWithTen(), 42, "ten arguments, no other call");

function evaluatesAfterCall() {
    const one = identity(1);
    return one + eval("64", 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
        33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64);
}
check(evaluatesAfterCall(), 65, "sixty-four arguments after a call with one");

function callsAnotherEval(eval) {
    const one = identity(1);
    return one + eval(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
        33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64);
}
check(callsAnotherEval(function () { return arguments.length + arguments[63]; }), 129, "a function named eval that is not eval");
check(callsAnotherEval((...all) => all.reduce((sum, one) => sum + one, 0)), 2081, "every argument arrives");

function seesLocals(a, b) { let local = a * b; return eval("local + a", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12); }
check(seesLocals(3, 4), 15, "locals are visible");
