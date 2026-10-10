//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--validateAOTInferredTypes=1", "--validateGraphAtEachPhase=1")
// Typed bodies are switched off (AOT::mayHaveGeneralBody admits no function) until every exact call site is checked against the types its callee's typed body was compiled for. The assertions that a function has one are skipped meanwhile. Every value must hold either way.
const typedBodiesAreSwitchedOff = true;
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function has(name, remark) {
    let remarks = aotRemarks(name);
    return remarks && remarks.length ? remarks.some(other => other === remark || other.startsWith(remark + ":")) : undefined;
}
function hasTwoBodies(name) { check([has(name, "is-typed-body") !== false, has(name, "is-general-body") !== false].join(), "true,true", name + " has two bodies"); }
function hasOneBody(name) { check([has(name, "is-typed-body") !== true, has(name, "is-general-body") !== true].join(), "true,true", name + " has one body"); }
(function () {
let kept = [];

const describe = function passesText(x) { return typeof x === "number" ? passesText("n" + x) : x + 1; };
function describes(count) { let last; for (let i = 0; i < count; i++) last = describe(i); return last; }
check(describes(3), "n21", "the function passes itself a string, its caller only numbers");

const halve = function passesDouble(x) { return x > 2 ? passesDouble(x / 2) : x; };
function halves(count) { let sum = 0; for (let i = 0; i < count; i++) sum += halve(i * 3); return sum; }
check(halves(4), 0 + 1.5 + 1.5 + 1.125, "the function passes itself a double, its caller only integers");

const kindOfThis = function readsThis(n) { return n ? readsThis(n - 1) : typeof this; };
function kindsOfThis(count) { let last; for (let i = 0; i < count; i++) last = kindOfThis(i); return last; }
check(kindsOfThis(3), "object", "this in a sloppy function that calls itself");

const strictKindOfThis = function readsStrictThis(n) { "use strict"; return n ? readsStrictThis(n - 1) : typeof this; };
function strictKindsOfThis(count) { let last; for (let i = 0; i < count; i++) last = strictKindOfThis.call(kept, i); return last; }
check(strictKindsOfThis(3), "undefined", "this in a strict function that calls itself");

const dropsArgument = function passesNothing(x) { return x === undefined ? "none" : passesNothing(); };
function dropsArguments(count) { let last; for (let i = 0; i < count; i++) last = dropsArgument(i); return last; }
check(dropsArguments(3), "none", "the function passes itself nothing");

const factorial = function multiplies(n) { return n ? n * multiplies(n - 1) : 1; };
function sumsFactorials(count) { let sum = 0; for (let i = 0; i < count; i++) sum += factorial(i); return sum; }
check(sumsFactorials(5), 34, "the function passes itself what its caller passes");

const increments = function doesNotNameItself(x) { return x + 1; };
kept.push(increments);
function sumsIncrements(count) { let sum = 0; for (let i = 0; i < count; i++) sum += increments(i); return sum; }
check(sumsIncrements(4), 10, "a function that leaves and does not name itself");
check(kept[0]("s"), "s1", "and is called from outside with something else");

for (let name of ["passesText", "passesDouble", "readsThis", "readsStrictThis", "passesNothing", "multiplies"])
    hasOneBody(name);
if (!typedBodiesAreSwitchedOff)
    hasTwoBodies("doesNotNameItself");
})();
