//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--validateAOTInferredTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--validateAOTInferredTypes=1", "--useAOTInlining=0")
//@ runDefault
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function has(name, remark) {
    let remarks = aotRemarks(name);
    return remarks && remarks.length ? remarks.some(other => other === remark || other.startsWith(remark + ":")) : undefined;
}
function says(name, remark) { check(has(name, remark) !== false, true, name + " has " + remark); }
function doesNotSay(name, remark) { check(has(name, remark) !== true, true, name + " has no " + remark); }

(function () {
function first() { return 2; }
function second() { return 3; }
function readsTooEarly() { keepsOfLet(mayBeEmpty); }
let thrown = "nothing";
try {
    readsTooEarly();
} catch (error) {
    thrown = error.constructor.name;
}
check(thrown, "ReferenceError", "a variable that is read before it is initialized");

let mayBeEmpty;
var isNeverEmpty;
let keptOfLet = 0;
let keptOfVar = 0;
let keptUntested = 0;
function keepsOfLet(e) { keptOfLet = e ?? 0; }
function keepsOfVar(e) { keptOfVar = e ?? 0; }
function keepsUntested(e) { keptUntested = e; }
function passesLet() { keepsOfLet(mayBeEmpty); }
function passesVar() { keepsOfVar(isNeverEmpty); }
function passesLetUntested() { keepsUntested(mayBeEmpty); }
function callsKept() { return (keptOfLet === 0 ? 1 : keptOfLet()) * 100 + (keptOfVar === 0 ? 1 : keptOfVar()) * 10 + (keptUntested === undefined ? 1 : keptUntested()); }
let letAlwaysUndefined;
var varAlwaysUndefined;
function passesLetAlwaysUndefined() { keepsOfLet(letAlwaysUndefined); }
function passesVarAlwaysUndefined() { keepsOfVar(varAlwaysUndefined); }
let results = [];
for (let i = 0; i < 3; i++) {
    mayBeEmpty = i === 0 ? undefined : i === 1 ? first : second;
    isNeverEmpty = i === 0 ? undefined : i === 1 ? first : second;
    passesLet();
    passesVar();
    passesLetUntested();
    results.push(callsKept());
}
check(results.join(), "111,222,333", "what the inlined callees stored");
passesLetAlwaysUndefined();
passesVarAlwaysUndefined();
check(keptOfLet, 0, "what is stored for a variable that is always undefined");
check(keptOfVar, 0, "what is stored for a variable that is always undefined and never empty");

if (has("passesLet", "inlined-call:keepsOfLet")) {
    says("passesVar", "inlined-call:keepsOfVar");
    says("passesLetUntested", "inlined-call:keepsUntested");
    says("passesVar", "narrowed-tested-value");
    says("passesLet", "narrowed-tested-value");
    doesNotSay("passesLetUntested", "narrowed-tested-value");
} else if (has("passesLet", "direct-call:keepsOfLet")) {
    doesNotSay("passesLet", "narrowed-tested-value");
    doesNotSay("passesVar", "narrowed-tested-value");
}
})();
