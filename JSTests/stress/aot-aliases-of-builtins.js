//@ runDefault("-m", "--compileMainScriptAheadOfTime=1")
//@ runDefault("-m", "--compileMainScriptAheadOfTime=1", "--useAOTInlining=0")
//@ runDefault("-m", "--compileMainScriptAheadOfTime=1", "--useAOTDataStubs=0")
//@ runDefault("-m", "--compileMainScriptAheadOfTime=1", "--validateAOTInferredTypes=1", "--validateGraphAtEachPhase=1")
//@ runDefault("-m")
import { exportedFloor as importedFloor, exportedClz32 as importedClz32 } from "./aot-aliases-of-builtins.js";

function check(actual, expected, what)
{
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}

function remarksOf(name)
{
    return typeof aotRemarks === "function" ? aotRemarks(name) : null;
}

function lowers(name, path)
{
    const remarks = remarksOf(name);
    if (remarks && !remarks.includes("lowered-builtin:" + path))
        throw new Error(name + " does not lower " + path);
}

function checksAlias(name, path)
{
    const remarks = remarksOf(name);
    if (remarks && !remarks.includes("checks-alias-of-builtin:" + path))
        throw new Error(name + " does not check that it calls " + path);
}

function lowersNothing(name)
{
    for (const remark of remarksOf(name) || []) {
        if (remark.startsWith("lowered-builtin:"))
            throw new Error(name + " has " + remark);
    }
}

function errorOf(run)
{
    try {
        run();
    } catch (error) {
        return error.constructor;
    }
    return null;
}

export const exportedFloor = Math.floor;
export var exportedClz32 = Math.clz32 ? Math.clz32 : function () { return -1; };
function callsImported(x) { return importedFloor(x) + importedClz32(x); }
check(callsImported(5.5), 34, "aliases imported from a module");

function aliases()
{
    check(errorOf(() => callsVariable([])), TypeError, "a variable called before it has a value");
    check(errorOf(() => callsConstant(1.5)), ReferenceError, "a constant called before it has a value");
    check(errorOf(() => callsSliceOnArray([1, 2, 3])), TypeError, "a method read of a variable before it has a value");

    const floor = Math.floor;
    var isArray = Array.isArray;
    var again = floor;
    var byTruthiness = Math.clz32 ? Math.clz32 : clz32Fallback;
    var byOr = Math.sqrt || clz32Fallback;
    var byTypeof = typeof Object.is === "function" ? Object.is : function (a, b) { return a === b; };
    var byTypeofUndefined = typeof Math.abs !== "undefined" ? Math.abs : clz32Fallback;
    var byLooseNull = Math.ceil != null ? Math.ceil : clz32Fallback;
    var byNullish = Math.trunc ?? clz32Fallback;
    var slice = Array.prototype.slice;
    var hasOwnProperty = Object.prototype.hasOwnProperty;

    function clz32Fallback() { return -1; }
    function callsConstant(x) { return floor(x); }
    function callsVariable(x) { return isArray(x); }
    function callsAliasOfAlias(x) { return again(x); }
    function callsByTruthiness(x) { return byTruthiness(x); }
    function callsByOr(x) { return byOr(x); }
    function callsByTypeof(a, b) { return byTypeof(a, b); }
    function callsByTypeofUndefined(x) { return byTypeofUndefined(x); }
    function callsByLooseNull(x) { return byLooseNull(x); }
    function callsByNullish(x) { return byNullish(x); }
    function callsSliceOnArray(array) { return slice.call(array, 1); }
    function callsSliceOnString() { return slice.call("abc", 1); }
    function callsSliceOnAnything(anything) { return slice.call(anything, 1); }
    function callsHasOwnProperty(object, key) { return hasOwnProperty.call(object, key); }
    function appliesFloor(list) { return floor.apply(null, list); }

    for (let i = 0; i < 100; ++i) {
        check(callsConstant(i + 0.5), i, "a constant");
        check(callsConstant(-0.5), -1, "a constant");
        check(callsConstant("2.5"), 2, "a constant, with a string");
        check(callsVariable([i]), true, "a variable");
        check(callsVariable({ length: 0 }), false, "a variable");
        check(callsAliasOfAlias(i + 0.25), i, "an alias of an alias");
        check(callsByTruthiness(1), 31, "chosen by truthiness");
        check(callsByTruthiness(0), 32, "chosen by truthiness");
        check(callsByOr(i * i), i, "chosen by ||");
        check(callsByTypeof(NaN, NaN), true, "chosen by typeof");
        check(callsByTypeof(0, -0), false, "chosen by typeof");
        check(callsByTypeofUndefined(-i), i, "chosen by typeof undefined");
        check(callsByLooseNull(i + 0.5), i + 1, "chosen by != null");
        check(callsByNullish(i + 0.5), i, "chosen by ??");
        check(callsSliceOnArray([i, i + 1, i + 2]).join(), (i + 1) + "," + (i + 2), "slice.call on an array");
        check(callsSliceOnString().join(), "b,c", "slice.call on a string");
        check(callsSliceOnAnything([i, 7]).join(), "7", "slice.call on anything");
        check(callsSliceOnAnything({ length: 2, 0: "a", 1: "b" }).join(), "b", "slice.call on anything");
        check(callsHasOwnProperty({ own: i }, "own"), true, "hasOwnProperty.call");
        check(callsHasOwnProperty({ own: i }, "toString"), false, "hasOwnProperty.call");
        check(appliesFloor([i + 0.5]), i, "apply");
    }
    check(errorOf(() => callsHasOwnProperty(null, "own")), TypeError, "hasOwnProperty.call on null");
    check(errorOf(() => callsSliceOnAnything(undefined)), TypeError, "slice.call on undefined");

    lowers("callsConstant", "Math.floor");
    lowers("callsVariable", "Array.isArray");
    lowers("callsAliasOfAlias", "Math.floor");
    lowers("callsByTruthiness", "Math.clz32");
    lowers("callsByOr", "Math.sqrt");
    lowers("callsByTypeof", "Object.is");
    lowers("callsByTypeofUndefined", "Math.abs");
    lowers("callsByLooseNull", "Math.ceil");
    lowers("callsByNullish", "Math.trunc");
    lowers("callsSliceOnArray", "Array.prototype.slice");
    lowersNothing("callsSliceOnString");
    checksAlias("callsConstant", "Math.floor");
    checksAlias("callsVariable", "Array.isArray");
}
aliases();

function variablesThatAreNoAliases()
{
    var reassigned = Math.floor;
    var reassignedToFunction = Math.floor;
    var absent = Math.notThere ? Math.notThere : function () { return -1; };
    var oneOfTwo = Date.now() > 0 ? Math.floor : Math.ceil;
    var floor = Math.floor;

    function reassigns() { reassigned = Math.ceil; }
    function reassignsToFunction() { reassignedToFunction = function (x) { return x + 100; }; }
    function callsReassigned(x) { return reassigned(x); }
    function callsReassignedToFunction(x) { return reassignedToFunction(x); }
    function callsAbsent(x) { return absent(x); }
    function callsOneOfTwo(x) { return oneOfTwo(x); }
    function callsParameter(floor, x) { return floor(x); }
    function callsLocal(x) { let floor = x > 1 ? Math.ceil : Math.round; return floor(x); }

    for (let i = 0; i < 100; ++i) {
        check(callsReassigned(1.5), i < 50 ? 1 : 2, "a variable that is assigned again");
        check(callsReassignedToFunction(1.5), i < 50 ? 1 : 101.5, "a variable that is assigned a function of the program");
        if (i == 49) {
            reassigns();
            reassignsToFunction();
        }
        check(callsAbsent(1.5), -1, "a built-in that does not exist");
        check(callsOneOfTwo(1.5), 1, "one of two built-ins");
        check(callsParameter(i & 1 ? floor : x => x + 7, 1.5), i & 1 ? 1 : 8.5, "a parameter named like the variable");
        check(callsLocal(1.5), 2, "a local variable named like the variable");
        check(callsLocal(0.4), 0, "a local variable named like the variable");
    }
    lowersNothing("callsReassigned");
    lowersNothing("callsReassignedToFunction");
    lowersNothing("callsAbsent");
    lowersNothing("callsOneOfTwo");
    lowersNothing("callsParameter");
    lowersNothing("callsLocal");
}
variablesThatAreNoAliases();

function variableThatEvalWrites()
{
    var writtenByEval = Math.round;
    function callsWrittenByEval(x) { return writtenByEval(x); }

    check(callsWrittenByEval(1.4), 1, "a variable that eval may write");
    eval("writtenByEval = Math.ceil");
    check(callsWrittenByEval(1.4), 2, "a variable that eval has written");
    lowersNothing("callsWrittenByEval");
}
variableThatEvalWrites();

function builtinsThatGetTheirOwnCall()
{
    var sign = Math.sign;
    var cbrt = Math.cbrt;
    var log2 = Math.log2;
    var log10 = Math.log10;
    var isPrototypeOf = Object.prototype.isPrototypeOf;

    function callsSign(x) { return sign.call(null, x); }
    function callsSignByPath(x) { return Math.sign.call(null, x); }
    function callsCbrt(x) { return cbrt.call(null, x); }
    function callsCbrtByPath(x) { return Math.cbrt.call(null, x); }
    function callsLog2(x) { return log2.call(null, x); }
    function callsLog2ByPath(x) { return Math.log2.call(null, x); }
    function readsCallOfLog10() { return typeof log10.call; }
    function appliesIsPrototypeOf(object, other) { return isPrototypeOf.apply(object, [other]); }

    for (let i = 0; i < 100; ++i) {
        check(callsSign(-3), -1, "call, inherited from Function.prototype");
        check(callsSignByPath(-3), -1, "call, inherited from Function.prototype");
        check(callsCbrt(27), 3, "call, inherited from Function.prototype");
        check(callsCbrtByPath(27), 3, "call, inherited from Function.prototype");
        check(callsLog2(8), 3, "call, inherited from Function.prototype");
        check(callsLog2ByPath(8), 3, "call, inherited from Function.prototype");
        check(readsCallOfLog10(), "function", "call, inherited from Function.prototype");
        check(appliesIsPrototypeOf(Object.prototype, {}), true, "apply, inherited from Function.prototype");
    }

    sign.call = function () { return "assigned"; };
    Object.defineProperty(cbrt, "call", { value: function () { return "defined"; } });
    Object.setPrototypeOf(log2, { call() { return "inherited from another prototype"; } });
    Object.setPrototypeOf(log10, null);
    isPrototypeOf.apply = function () { return "assigned"; };

    for (let i = 0; i < 100; ++i) {
        check(callsSign(-3), "assigned", "a call of its own");
        check(callsSignByPath(-3), "assigned", "a call of its own");
        check(callsCbrt(27), "defined", "a call of its own");
        check(callsCbrtByPath(27), "defined", "a call of its own");
        check(callsLog2(8), "inherited from another prototype", "another prototype");
        check(callsLog2ByPath(8), "inherited from another prototype", "another prototype");
        check(readsCallOfLog10(), "undefined", "no prototype");
        check(appliesIsPrototypeOf(Object.prototype, {}), "assigned", "an apply of its own");
    }
    check(sign(-3), -1, "the function itself is what it was");
    check(Math.cbrt(27), 3, "the function itself is what it was");
}
builtinsThatGetTheirOwnCall();
