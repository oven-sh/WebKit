//@ runDefault("--compileMainScriptAheadOfTime=1", "--validateAOTInferredTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--validateAOTInferredTypes=1", "--useAOTInlining=0")
//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function remarksOf(name) { return (typeof aotRemarks === "function" && aotRemarks(name)) || []; }
function numberOf(name, remark) { return remarksOf(name).filter(other => other === remark || other.startsWith(remark + ":")).length; }
function says(name, remark) { check(!remarksOf(name).length || numberOf(name, remark) > 0, true, name + " has " + remark); }
function doesNotSay(name, remark) { check(numberOf(name, remark), 0, "how often " + name + " has " + remark); }

(function () {
"use strict";
function joins(x) { return x ?? 0; }
function joinsTwice(x, y) { return [x ?? 0, y ?? 1]; }
function joinsInLoop(x) { let r = 0; for (let i = 0; i < 2; i++) r = x ?? r; return r; }
function joinsAfterOr(x) { return x || 0; }
function orZero(x) { return x ?? 0; }
function callsInlined(x) { let a = orZero(x); return a === 0 ? 1 : a; }
function guardsAfterTest(x, n) { let s = 0; for (let i = 0; i < n; i++) { if (x != null) s += x.v; } return s; }
function hasBytecodeAfter(x) { if (x != null) return x.v; return 0; }
function testsNothing(x) { return x; }

let kinds = [];
for (let round = 0; round < 3; round++) {
    let value = round ? { v: round } : undefined;
    kinds.push(typeof joins(value), joinsTwice(value, value).map(one => typeof one).join("+"), typeof joinsInLoop(value), typeof joinsAfterOr(value), typeof callsInlined(value), guardsAfterTest(value, 3), hasBytecodeAfter(value), typeof testsNothing(value));
}
check(kinds.join(), "number,number+number,number,number,number,0,0,undefined,object,object+object,object,object,object,3,1,object,object,object+object,object,object,object,6,2,object", "what the functions return");

let options = typeof jscOptions === "function" ? jscOptions() : { };
let isCompiled = remarksOf("check").length > 0;
let validates = isCompiled && !!options.validateAOTInferredTypes;
let tested = ["joins", "joinsTwice", "joinsInLoop", "joinsAfterOr", "orZero", "callsInlined", "guardsAfterTest", "hasBytecodeAfter"];
for (let name of [...tested, "testsNothing"]) {
    if (isCompiled)
        check(remarksOf(name).length > 0, true, name + " has remarks");
    doesNotSay(name, "cannot-verify-value");
    if (!validates)
        doesNotSay(name, "verifies-value-after-bytecode");
}
for (let name of tested) {
    if (name !== "callsInlined" || options.useAOTInlining)
        says(name, "narrowed-tested-value");
}
doesNotSay("hasBytecodeAfter", "verifies-value-after-bytecode");
doesNotSay("testsNothing", "verifies-value-after-bytecode");
if (isCompiled && options.useAOTInlining)
    says("callsInlined", "inlined-call:orZero");
let splitsLoops = isCompiled && !!options.useAOTLoopSplitting && !!options.useAOTDataStubs;
if (splitsLoops)
    says("guardsAfterTest", "split-loop");
if (validates) {
    for (let name of ["joins", "joinsAfterOr", "orZero"])
        check(numberOf(name, "verifies-value-after-bytecode"), 1, "how many values " + name + " verifies after the last bytecode of their blocks");
    check(numberOf("joinsTwice", "verifies-value-after-bytecode"), 2, "how many values joinsTwice verifies after the last bytecode of their blocks");
    says("joinsInLoop", "verifies-value-after-bytecode");
    check(numberOf("callsInlined", "verifies-value-after-bytecode"), options.useAOTInlining ? 1 : 0, "how many values callsInlined verifies after the last bytecode of their blocks");
    check(numberOf("guardsAfterTest", "verifies-value-after-bytecode") > 0, splitsLoops, "whether guardsAfterTest verifies a value in front of a guard");
}
})();
