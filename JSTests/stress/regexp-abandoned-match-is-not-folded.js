//@ runDefault("--regExpMatchLimit=100", "--useConcurrentJIT=0", "--useFTLJIT=0")
//@ runDefault("--regExpMatchLimit=100", "--useConcurrentJIT=0", "--useFTLJIT=0", "--useRegExpJIT=0")

// DFG strength reduction runs a constant RegExp on a constant string while it compiles, and puts
// the result in place of the search. A search that is abandoned has no result: the compiled code
// must run it, and throw, like the lower tiers. Folded, each function below would return.
//
// The result of each search is not used, or the call is a tail call. A call whose result is used
// and that has never returned has no value profile, and the DFG leaves compiled code before it:
// such a call never reaches a folded search.

const limitMessage = "Regular expression backtracking limit exceeded";
const regExp = /(?:a|aa)+b|c/;

function test() { regExp.test("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaac"); return "returned"; }
function testLiteral() { /(?:a|aa)+b|c/.test("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaac"); return "returned"; }
function exec() { regExp.exec("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaac"); return "returned"; }
function search() { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaac".search(regExp); return "returned"; }
function match() { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaac".match(regExp); return "returned"; }
function replace() { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaac".replace(regExp, "X"); return "returned"; }
function testTailCall() { "use strict"; return regExp.test("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaac"); }
function execTailCall() { "use strict"; return regExp.exec("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaac"); }
// A validator that throws for what it matches, called as a statement after it is warm.
function deny(s) { if (regExp.test(s)) throw new TypeError("denied"); }
function validator() { deny("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaac"); return "returned"; }
// Enough calls for the DFG to compile each function, with calls after that.
const loopCount = Math.min(testLoopCount, 3000);
for (let i = 0; i < loopCount; i++)
    deny("xyz");

for (const run of [test, testLiteral, exec, search, match, replace, testTailCall, execTailCall, validator]) {
    noInline(run);
    for (let i = 0; i < loopCount; i++) {
        let result;
        let error = null;
        try {
            result = run();
        } catch (e) {
            error = e;
        }
        if (!(error instanceof RangeError) || error.message !== limitMessage)
            throw new Error(run.name + " at iteration " + i + ": expected the RangeError, got " + (error || JSON.stringify(result)));
    }
    if (!numberOfDFGCompiles(run))
        throw new Error(run.name + " was not compiled");
}
