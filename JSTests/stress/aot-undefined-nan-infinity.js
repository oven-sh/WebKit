//@ runDefault("--compileMainScriptAheadOfTime=1")
function same(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function readsUndefined() { return undefined; }
function readsNaN() { return NaN; }
function readsInfinity() { return Infinity; }
function negatesInfinity() { return -Infinity; }
function comparesWithUndefined(x) { return x === undefined; }
function typeofUndefined() { return typeof undefined; }
same(readsUndefined(), void 0, "undefined");
same(readsNaN(), 0 / 0, "NaN");
same(readsInfinity(), 1 / 0, "Infinity");
same(negatesInfinity(), -1 / 0, "-Infinity");
same(comparesWithUndefined(void 0), true, "undefined is undefined");
same(comparesWithUndefined(null), false, "null is not undefined");
same(comparesWithUndefined(0), false, "0 is not undefined");
same(typeofUndefined(), "undefined", "typeof undefined");

function shadowedByVar() { var undefined = 5; return undefined; }
function shadowedByParameter(undefined) { return undefined; }
function shadowedByLet() { let NaN = 1; { let Infinity = 2; return NaN + Infinity; } }
function shadowedInOuterFunction() { var undefined = 6; return (() => undefined)(); }
function shadowedByWith(object) { with (object) return undefined; }
function shadowedByEval(code) { eval(code); return undefined; }
function shadowedByCatch() { try { throw 7; } catch (undefined) { return undefined; } }
same(shadowedByVar(), 5, "a var");
same(shadowedByParameter(4), 4, "a parameter");
same(shadowedByLet(), 3, "a let");
same(shadowedInOuterFunction(), 6, "a var of an outer function");
same(shadowedByWith({ undefined: 8 }), 8, "a with that has it");
same(shadowedByWith({ }), void 0, "a with that does not");
same(shadowedByEval("var undefined = 9"), 9, "an eval that declares it");
same(shadowedByEval(""), void 0, "an eval that does not");
same(shadowedByCatch(), 7, "a catch parameter");

function assignsSloppily() { undefined = 1; NaN = 2; Infinity = 3; return [undefined, NaN, Infinity]; }
function assignsStrictly() { "use strict"; undefined = 1; }
let assigned = assignsSloppily();
same(assigned[0], void 0, "undefined after an assignment");
same(assigned[1], 0 / 0, "NaN after an assignment");
same(assigned[2], 1 / 0, "Infinity after an assignment");
let threw = null;
try { assignsStrictly(); } catch (error) { threw = error; }
same(threw instanceof TypeError, true, "an assignment in strict code");
same(readsUndefined(), void 0, "undefined after all that");
same(Object.getOwnPropertyDescriptor(globalThis, "undefined").writable, false, "the property of the global object");
same(globalThis.undefined, void 0, "read as a property");

if (aotRemarks("readsUndefined")) {
    let readsGlobal = name => aotRemarks(name).some(remark => remark === "calls:GetGlobal" || remark === "calls:ResolveScope" || remark === "calls:operationAOTGetFromScope" || remark === "calls:operationAOTResolveScope");
    for (let name of ["readsUndefined", "readsNaN", "readsInfinity", "negatesInfinity", "comparesWithUndefined", "typeofUndefined"]) {
        if (readsGlobal(name))
            throw new Error(name + " reads a global: " + aotRemarks(name).join(" "));
    }
    if (aotRemarks("comparesWithUndefined").some(remark => remark === "calls:StrictEqual" || remark === "calls:operationAOTCompareStrictEq"))
        throw new Error("the comparison with undefined is a call");
}
