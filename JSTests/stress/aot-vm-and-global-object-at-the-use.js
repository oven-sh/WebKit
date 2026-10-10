//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--validateAOTInferredTypes=1", "--validateGraphAtEachPhase=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
let declaredLater = 1;
globalThis.propertyOfGlobal = 10;
let calls = [];
function opaque(x) { calls.push(x); return calls.length > 1000 ? null : x; }
let keepsOpaqueOpen = [opaque];

function isTruthyAfterCall(x) { keepsOpaqueOpen[0](1); return x ? 1 : 0; }
function isNullishAfterCall(x) { keepsOpaqueOpen[0](2); return x == null ? 1 : 0; }
function isNotNullishAfterCall(x) { keepsOpaqueOpen[0](3); return x != undefined ? 1 : 0; }
function readsGlobalsAfterCall() { keepsOpaqueOpen[0](4); return declaredLater + propertyOfGlobal; }
function writesGlobalAfterCall(x) { keepsOpaqueOpen[0](5); declaredLater = x; return declaredLater; }
function isGlobalAfterCall(x) { keepsOpaqueOpen[0](6); return x === globalThis ? 1 : 0; }
function makesObjectAfterCall(a, b) { keepsOpaqueOpen[0](7); return { a, b, c: a + b, d: [a, b] }; }
function makesObjectOnOnePath(a, b) { if (a < 0) return null; keepsOpaqueOpen[0](8); return { a, b }; }
function Made() { this.a = 1; }
function isInstanceAfterCall(x) { keepsOpaqueOpen[0](9); return x instanceof Made ? 1 : 0; }
function leavesLoopEarly(list) { let sum = 0; for (let x of list) { keepsOpaqueOpen[0](10); if (x > 2) break; sum += x; } return sum; }
function loopsWithCalls(n) { let sum = 0; for (let i = 0; i < n; i++) sum += keepsOpaqueOpen[0](i); return sum; }
function loopsWithoutCalls(n) { let sum = 0; for (let i = 0; i < n; i++) sum += i & 3; return sum; }
function catches(x) { try { keepsOpaqueOpen[0](11); if (x) throw new Error("thrown"); return 0; } catch (e) { return e.message.length; } }

let masquerader = makeMasquerader();
for (let round = 0; round < 3; round++) {
    check(isTruthyAfterCall({ }), 1, "an object is truthy");
    check(isTruthyAfterCall(masquerader), 0, "an object that masquerades as undefined is not");
    check(isTruthyAfterCall(""), 0, "the empty string is not");
    check(isNullishAfterCall(null), 1, "null is nullish");
    check(isNullishAfterCall({ }), 0, "an object is not");
    check(isNullishAfterCall(masquerader), 1, "an object that masquerades as undefined is");
    check(isNotNullishAfterCall(masquerader), 0, "an object that masquerades as undefined, negated");
    check(isNotNullishAfterCall(0), 1, "zero is not nullish");
    check(writesGlobalAfterCall(round + 1), round + 1, "a global lexical variable is written");
    check(readsGlobalsAfterCall(), round + 11, "a global lexical variable and a property of the global object are read");
    check(isGlobalAfterCall(globalThis), 1, "the global object is itself");
    check(isGlobalAfterCall({ }), 0, "another object is not the global object");
    let made = makesObjectAfterCall(round, 2);
    check(made.a + made.b + made.c + made.d.length, round * 2 + 6, "an object and an array are made");
    check(makesObjectOnOnePath(-1, 2), null, "the path that makes no object");
    check(makesObjectOnOnePath(1, 2).b, 2, "the path that makes an object");
    check(isInstanceAfterCall(new Made), 1, "an instance");
    check(isInstanceAfterCall({ }), 0, "not an instance");
    check(leavesLoopEarly([1, 2, 3, 4]), 3, "a loop over an array that is left early");
    check(loopsWithCalls(5), 10, "a loop with calls");
    check(loopsWithoutCalls(9), 12, "a loop without calls");
    check(catches(false), 0, "nothing is thrown");
    check(catches(true), 6, "an exception is caught");
}
