//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuessedPlaces=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--validateGraphAtEachPhase=1", "--validateAOTInferredTypes=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}

function makesWithReturn(i) { return { return: i, besideReturn: i }; }
function readsReturn(o) { return o.return + o.besideReturn; }
function readsReturnAlone(o) { return o.return; }
function closesIterator(list) {
    for (let element of list) {
        if (element)
            return element;
    }
    return 0;
}

function makesWithNamesOfObjectPrototype(i) {
    return { constructor: 1, hasOwnProperty: 2, isPrototypeOf: 3, propertyIsEnumerable: 4, toLocaleString: 5, toString: 6, valueOf: 7, __defineGetter__: 8, __defineSetter__: 9, __lookupGetter__: 10, __lookupSetter__: 11, behindEleven: i };
}
function readsNamesOfObjectPrototype(o) {
    return o.constructor + o.hasOwnProperty + o.isPrototypeOf + o.propertyIsEnumerable + o.toLocaleString + o.toString + o.valueOf + o.__defineGetter__ + o.__defineSetter__ + o.__lookupGetter__ + o.__lookupSetter__;
}
function readsBehindEleven(o) { return o.behindEleven; }

for (let i = 0; i < 100; i++) {
    check(readsReturn(makesWithReturn(i)), 2 * i, "a field named return");
    check(readsReturnAlone(makesWithReturn(i)), i, "a field named return, alone");
    check(closesIterator([0, i, 7]), i || 7, "a loop that is left early");
    check(readsNamesOfObjectPrototype(makesWithNamesOfObjectPrototype(i)), 66, "own properties with the names of Object.prototype");
    check(readsBehindEleven(makesWithNamesOfObjectPrototype(i)), i, "a property behind them");
    check(typeof readsNamesOfObjectPrototype({ }), "string", "the properties of Object.prototype");
}
{
    let closed = 0;
    let iterable = { [Symbol.iterator]() { return { return() { closed++; return { }; }, next() { return { done: false, value: 1 }; } }; } };
    check(closesIterator(iterable), 1, "an iterator of the program");
    check(closed, 1, "the iterator is closed once");
}
{
    let countOf = detail => typeof aotOperationCount === "function" && aotOperationCount("operationAOTCountGuessedPlace:" + detail) || 0;
    let before = [countOf("hit"), countOf("another-slot")];
    for (let i = 0; i < 100; i++)
        readsBehindEleven(makesWithNamesOfObjectPrototype(i));
    if (countOf("hit") > before[0]) {
        check(countOf("hit") - before[0] >= 100, true, "guessed reads of a property behind names that get no place hit");
        check(countOf("another-slot") - before[1], 0, "guessed reads of a property behind names that get no place that find it in another slot");
    }
}
{
    let countOf = detail => typeof aotOperationCount === "function" && aotOperationCount("propertyNameIDIfKnown:" + detail) || 0;
    let before = [countOf("known"), countOf("unknown")];
    let parsed = JSON.parse('{"valueOf":1,"toString":2}');
    if (countOf("known") + countOf("unknown") > before[0] + before[1])
        check(countOf("known") - before[0], 0, "names of Object.prototype that have an ID");
    check(parsed.valueOf + parsed.toString, 3, "parsed properties with the names of Object.prototype");
}
if (aotRemarks("readsReturn") && aotRemarks("readsReturn").some(remark => remark.startsWith("guessed-place") || remark.startsWith("no-guess"))) {
    let has = (name, remark) => aotRemarks(name).includes(remark);
    let applies = (name, remark) => {
        if (!has(name, remark))
            throw new Error(remark + " does not apply to " + name + ": " + aotRemarks(name).join(" | "));
    };
    let doesNotApply = (name, remark) => {
        if (has(name, remark))
            throw new Error(remark + " applies to " + name + ": " + aotRemarks(name).join(" | "));
    };
    applies("readsReturn", "guessed-place:return");
    applies("readsReturnAlone", "guessed-place:return");
    doesNotApply("closesIterator", "guessed-place:return");
    doesNotApply("closesIterator", "no-guess:disagree");
    for (let name of ["constructor", "hasOwnProperty", "isPrototypeOf", "propertyIsEnumerable", "toLocaleString", "toString", "valueOf", "__defineGetter__", "__defineSetter__", "__lookupGetter__", "__lookupSetter__"])
        doesNotApply("readsNamesOfObjectPrototype", "guessed-place:" + name);
    applies("readsNamesOfObjectPrototype", "no-guess:no-shape");
    applies("readsBehindEleven", "guessed-place:behindEleven");
}
