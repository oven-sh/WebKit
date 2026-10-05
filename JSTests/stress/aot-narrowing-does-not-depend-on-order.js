//@ runDefault("--compileMainScriptAheadOfTime=1", "--useDollarVM=1", "--numberOfAOTCompilerThreads=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useDollarVM=1", "--numberOfAOTCompilerThreads=1", "--useAOTInlining=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useDollarVM=1", "--numberOfAOTCompilerThreads=1", "--useAOTDataStubs=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useDollarVM=1", "--validateGraphAtEachPhase=1", "--validateAOTInferredTypes=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function has(name, remark) {
    return aotRemarks(name).includes(remark);
}
function readsFromString(name) {
    return !aotRemarks(name).some(remark => /^untyped-access-|GetById$|GetLength$/.test(remark));
}
function remarksWithout(name, suffix) {
    return aotRemarks(name).map(remark => remark.replaceAll(suffix, "")).sort().join(" ");
}

(function () {
    function earlyBehind() { return middleBehind("abc") + middleBehind({ a: 1 }); }
    function calleeBehind(s) { return s.length + s.charCodeAt(0); }
    function middleBehind(e) { if (typeof e === "string") return calleeBehind(e); return 0; }
    function lateBehind() { return middleBehind(7); }
    check(earlyBehind() + lateBehind(), 100, "a string, the number comes behind");
})();

(function () {
    function lateInFront() { return middleInFront(7); }
    function earlyInFront() { return middleInFront("abc") + middleInFront({ a: 1 }); }
    function calleeInFront(s) { return s.length + s.charCodeAt(0); }
    function middleInFront(e) { if (typeof e === "string") return calleeInFront(e); return 0; }
    check(earlyInFront() + lateInFront(), 100, "a string, the number comes in front");
})();

(function () {
    function earlyOtherEdgeBehind() { return middleOtherEdgeBehind("abc") + middleOtherEdgeBehind({ a: 1 }); }
    function calleeOtherEdgeBehind(s) { return s.length + s.charCodeAt(0); }
    function middleOtherEdgeBehind(e) { if (typeof e === "object") return 0; return calleeOtherEdgeBehind(e); }
    function lateOtherEdgeBehind() { return middleOtherEdgeBehind(null); }
    check(earlyOtherEdgeBehind() + lateOtherEdgeBehind(), 100, "no object, null comes behind");
})();

(function () {
    function lateOtherEdgeInFront() { return middleOtherEdgeInFront(null); }
    function earlyOtherEdgeInFront() { return middleOtherEdgeInFront("abc") + middleOtherEdgeInFront({ a: 1 }); }
    function calleeOtherEdgeInFront(s) { return s.length + s.charCodeAt(0); }
    function middleOtherEdgeInFront(e) { if (typeof e === "object") return 0; return calleeOtherEdgeInFront(e); }
    check(earlyOtherEdgeInFront() + lateOtherEdgeInFront(), 100, "no object, null comes in front");
})();

(function () {
    function earlyCells() { return middleCells("abc") + middleCells({ a: 1 }); }
    function calleeCells(s) { return s.length + s.charCodeAt(0); }
    function middleCells(e) { if (typeof e === "string") return calleeCells(e); return 0; }
    check(earlyCells(), 100, "cells only");
})();

(function () {
    function earlyNested() { return middleNested("abc") + middleNested({ a: 1 }) + middleNested(null); }
    function calleeNested(s) { return s.length + s.charCodeAt(0); }
    function middleNested(e) { if (e !== null) { if (typeof e === "string") return calleeNested(e); } return 0; }
    check(earlyNested(), 100, "a test inside a test");
})();

(function () {
    function earlyObjects() { return middleObjects(undefined) + middleObjects(null) + middleObjects({ a: 1 }); }
    function calleeObjects(o) { if (o === undefined) return 1; if (o === null) return 2; return 3; }
    function middleObjects(e) { if (typeof e === "object" && e !== null) return calleeObjects(e); return 0; }
    check(earlyObjects(), 3, "an object that is not null");
})();

(function () {
    function callsWithObjects() { return passesObjects({ a: 1 }); }
    function givenObjects(o) { if (o === undefined) return 1; if (o === null) return 2; return 3; }
    function passesObjects(e) { return givenObjects(e); }
    check(callsWithObjects(), 3, "only objects");
})();

(function () {
    function callsWithObjectsOrUndefined() { return passesObjectsOrUndefined({ a: 1 }) + passesObjectsOrUndefined(undefined); }
    function givenObjectsOrUndefined(o) { if (o === undefined) return 1; if (o === null) return 2; return 3; }
    function passesObjectsOrUndefined(e) { return givenObjectsOrUndefined(e); }
    check(callsWithObjectsOrUndefined(), 4, "objects or undefined");
})();

(function () {
    function earlyArrays() { return middleArrays("abc") + middleArrays([1]); }
    function calleeArrays(s) { return s.length + s.charCodeAt(0); }
    function middleArrays(e) { if (!Array.isArray(e)) return calleeArrays(e); return 0; }
    check(earlyArrays(), 100, "not an array");
})();

for (const kind of ["", "OtherEdge"]) {
    for (const name of ["callee", "middle"])
        check(remarksWithout(name + kind + "Behind", "Behind"), remarksWithout(name + kind + "InFront", "InFront"), name + kind + " in the two orders");
}
for (const suffix of ["Behind", "InFront", "OtherEdgeBehind", "OtherEdgeInFront", "Cells", "Nested"]) {
    check(has("middle" + suffix, "narrowed-tested-value"), true, "middle" + suffix + " narrows");
    check(readsFromString("callee" + suffix), true, "callee" + suffix + " reads from a string");
}
check(has("middleObjects", "narrowed-tested-value"), true, "middleObjects narrows");
check(remarksWithout("givenObjects", "givenObjects") !== remarksWithout("givenObjectsOrUndefined", "givenObjectsOrUndefined"), true, "code for objects differs from code for objects or undefined");
check(remarksWithout("calleeObjects", "calleeObjects"), remarksWithout("givenObjects", "givenObjects"), "calleeObjects and a function that is only given objects");
check(has("middleArrays", "narrowed-tested-value"), false, "middleArrays narrows");
check(readsFromString("calleeArrays"), false, "calleeArrays reads from a string");
