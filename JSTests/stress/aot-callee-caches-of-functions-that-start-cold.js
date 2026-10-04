//@ skip if $architecture != "arm64"
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--useAOTOperationCounters=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function double(x) { return x * 2; }
function triple(x) { return x * 3; }
function callsOnly(f, x) { const result = f(x); return result + 1; }
function callsHostFunctionOnly(f, x) { const result = f(x); return !result; }
function callsTwice(f, g, x) { const first = f(x); const second = g(x); return first + second; }
function callsLast(f, x) { "use strict"; return f(x); }
function callsFunctionThatCallsLast(f, x) { const result = callsLast(f, x); return result + 1; }
function neverReads(f, x, o) {
    if (o !== undefined)
        o.value;
    const result = f(x);
    return result + 1;
}

const countsOperations = typeof aotOperationCount === "function" && aotOperationCount("operationAOTCacheCallee") !== null && isAOTCompiled(callsOnly) && (aotRemarks("callsOnly") || []).includes("cached-call");
function countsDuring(f) {
    const names = ["operationAOTCountMissOfCalleeCache", "operationAOTCacheCallee", "operationAOTCacheHostCallee"];
    const before = names.map(name => countsOperations ? aotOperationCount(name) : 0);
    f();
    const [missesOnSharedData, fills, hostFills] = names.map((name, i) => (countsOperations ? aotOperationCount(name) : 0) - before[i]);
    return { missesOnSharedData, fills: fills + hostFills };
}
function checkRange(actual, atLeast, atMost, what) {
    if (countsOperations && (actual < atLeast || actual > atMost))
        throw new Error(what + ": " + actual + ", not " + atLeast + " to " + atMost);
}
function checkGetsItsOwnCaches(what, call) {
    const first = countsDuring(() => {
        for (let i = 0; i < 100; ++i)
            call(i);
    });
    checkRange(first.missesOnSharedData, 1, 40, "misses that are counted while " + what + " has no caches of its own");
    checkRange(first.fills, 1, 8, "caches that are filled once " + what + " has its own");
    const later = countsDuring(() => {
        for (let i = 0; i < 2000; ++i)
            call(i);
    });
    checkRange(later.missesOnSharedData, 0, 0, "later misses of " + what);
    checkRange(later.fills, 0, 0, "later fills of " + what);
}

checkGetsItsOwnCaches("a function that only calls", i => check(callsOnly(double, i), i * 2 + 1, "a call"));
checkGetsItsOwnCaches("a function that only calls a host function", i => check(callsHostFunctionOnly(Number.isSafeInteger, i), false, "a call of a host function"));
checkGetsItsOwnCaches("a function that calls twice", i => check(callsTwice(double, triple, i), i * 5, "two calls"));
checkGetsItsOwnCaches("a function whose property read never runs", i => check(neverReads(double, i, undefined), i * 2 + 1, "a call behind a read that is skipped"));

{
    const all = countsDuring(() => {
        for (let i = 0; i < 3000; ++i)
            check(callsFunctionThatCallsLast(double, i), i * 2 + 1, "a call in tail position");
    });
    checkRange(all.missesOnSharedData, 0, 40, "misses that are counted for a call in tail position, whose return address is not in the function");
}
