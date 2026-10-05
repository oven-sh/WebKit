//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTTypeCoverageCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTTypeCoverageCounters=1", "--validateAOTInferredTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function has(name, remark) {
    let remarks = typeof aotRemarks === "function" && aotRemarks(name);
    return remarks && remarks.length ? remarks.some(other => other === remark || other.startsWith(remark + ":")) : undefined;
}
function says(name, remark) { check(has(name, remark) !== false, true, name + " has " + remark); }

(function () {
function callee(a) { return a; }
let getter = { get x() { return callee(1); } };
function readsGetterInLoop() {
    let sum = 0;
    for (let i = 0; i < 10; ++i)
        sum += getter.x;
    return sum;
}
check(readsGetterInLoop(), 10, "what a loop that leaves its guarded copy returns");
let options = typeof jscOptions === "function" ? jscOptions() : { };
let isCompiled = typeof aotRemarks === "function" && !!(aotRemarks("check") || []).length;
if (isCompiled)
    check((aotRemarks("readsGetterInLoop") || []).length > 0, true, "readsGetterInLoop has remarks");
if (isCompiled && options.useAOTLoopSplitting && options.useAOTDataStubs)
    says("readsGetterInLoop", "split-loop");
})();
