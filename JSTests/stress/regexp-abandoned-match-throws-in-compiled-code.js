//@ runDefault("--regExpMatchLimit=100", "--useConcurrentJIT=0", "--useFTLJIT=0")
//@ runDefault("--regExpMatchLimit=100", "--useConcurrentJIT=0", "--useFTLJIT=0", "--useRegExpJIT=0")
//@ runDefault("--regExpMatchLimit=100", "--useConcurrentJIT=0", "--testTheFTL=1") if $isFTLPlatform

// DFG and FTL code run a search through their own operations (operationRegExpTestString and the
// like). Each function here is compiled on a subject that matches, so that its RegExp node is
// live, and then gets a subject whose search is abandoned at Options::regExpMatchLimit: every call
// throws, from the compiled code. With --testTheFTL, numberOfDFGCompiles() counts FTL compiles.

const limitMessage = "Regular expression backtracking limit exceeded";
const regExp = /(?:a|aa)+b|c/;
const globalRegExp = /(?:a|aa)+b|c/g;
const stickyRegExp = /(?:a|aa)+b|a+c/y;

function testLiteral(s) { return /(?:a|aa)+b|c/.test(s); }
function test(s) { return regExp.test(s); }
function exec(s) { return regExp.exec(s)[0]; }
function execSticky(s) {
    stickyRegExp.lastIndex = 1;
    return stickyRegExp.exec(s)[0];
}
function match(s) { return s.match(regExp)[0]; }
function matchGlobal(s) { return s.match(globalRegExp).length; }
function matchAll(s) { return [...s.matchAll(globalRegExp)].length; }
function search(s) { return s.search(regExp); }
function replace(s) { return s.replace(regExp, "X"); }
function replaceGlobal(s) { return s.replace(globalRegExp, ""); }
function replaceFunction(s) { return s.replace(globalRegExp, () => "Y"); }
function split(s) { return s.split(regExp).length; }
function caught(s) {
    try {
        return regExp.test(s);
    } catch (error) {
        return error.message;
    }
}

const small = "aaac";
const abandoned = "a".repeat(35) + "c";
const throwCount = Math.min(testLoopCount, 200);
const sites = [
    [testLiteral, true], [test, true], [exec, "c"], [execSticky, "aac"], [match, "c"], [matchGlobal, 1], [matchAll, 1],
    [search, 3], [replace, "aaaX"], [replaceGlobal, "aaa"], [replaceFunction, "aaaY"], [split, 2], [caught, true],
];

for (const [run, expected] of sites) {
    noInline(run);

    // Until the optimizing compiler has compiled it. numberOfDFGCompiles() is large when that compiler is off.
    for (let i = 0; i < testLoopCount && !numberOfDFGCompiles(run); i++) {
        const result = run(small);
        if (result !== expected)
            throw new Error(run.name + " on the small subject: expected " + JSON.stringify(expected) + " but got " + JSON.stringify(result));
    }
    const compiles = numberOfDFGCompiles(run);
    if (!compiles)
        throw new Error(run.name + " was not compiled");

    for (let i = 0; i < throwCount; i++) {
        let result;
        let error = null;
        try {
            result = run(abandoned);
        } catch (e) {
            error = e;
        }
        if (run === caught) {
            if (result !== limitMessage)
                throw new Error("caught at iteration " + i + ": expected the message, got " + (error || JSON.stringify(result)));
        } else if (!(error instanceof RangeError) || error.message !== limitMessage)
            throw new Error(run.name + " at iteration " + i + ": expected the RangeError, got " + (error || JSON.stringify(result)));
        // A throw is not a failed match: lastIndex stays where the search started.
        if (run === execSticky && stickyRegExp.lastIndex !== 1)
            throw new Error("execSticky at iteration " + i + ": lastIndex is " + stickyRegExp.lastIndex);
    }

    // Code that left for a lower tier at each of these calls is thrown away and compiled again.
    if (numberOfDFGCompiles(run) !== compiles)
        throw new Error(run.name + " was compiled again: " + compiles + " then " + numberOfDFGCompiles(run));
    if (run(small) !== expected)
        throw new Error(run.name + " does not answer after the throws");
}
