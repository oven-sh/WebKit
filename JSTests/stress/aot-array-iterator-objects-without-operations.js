//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTDataStubs=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--validateAOTInferredTypes=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function joined(iterable) {
    let s = "";
    for (const x of iterable)
        s += String(x) + ";";
    return s;
}
function joinedPairs(iterable) {
    let s = "";
    for (const [index, value] of iterable)
        s += String(index) + "=" + String(value) + ";";
    return s;
}
function joinedWhile(iterable, during) {
    let s = "";
    for (const x of iterable) {
        s += String(x) + ";";
        during(x);
    }
    return s;
}
function firstOf(iterable) {
    for (const x of iterable)
        return x;
    return "nothing";
}

const operation = "operationAOTIteratorNextTryFast";
const countsOperations = typeof aotOperationCount === "function" && isAOTCompiled(joined) && (aotRemarks("joined") || []).includes("calls:IteratorNext") && aotOperationCount(operation) !== null;
function operationsDuring(f) {
    if (!countsOperations) {
        f();
        return -1;
    }
    const before = aotOperationCount(operation);
    f();
    return aotOperationCount(operation) - before;
}
function checkOperations(actual, atLeast, atMost, what) {
    if (actual >= 0 && (actual < atLeast || actual > atMost))
        throw new Error(what + ": " + actual + " calls of the operation, not " + atLeast + " to " + atMost);
}

{
    const integers = [10, 11, 12, 13];
    const written = [10, 11, 12, 13];
    written[0] = 10;
    const anything = ["a", null, undefined, 3];
    checkOperations(operationsDuring(() => {
        for (let round = 0; round < 50; round++) {
            check(joined(integers.keys()), "0;1;2;3;", "the keys of a literal of integers");
            check(joined(integers.values()), "10;11;12;13;", "the values of a literal of integers");
            check(joined(written.values()), "10;11;12;13;", "the values of an array of integers");
            check(joined(anything.keys()), "0;1;2;3;", "the keys of an array of anything");
            check(joined(anything.values()), "a;null;undefined;3;", "the values of an array of anything");
            check(joined([].values()), "", "the values of an empty array");
        }
    }), 0, 0, "keys and values of arrays");
    checkOperations(operationsDuring(() => {
        for (let round = 0; round < 50; round++) {
            check(joinedPairs(integers.entries()), "0=10;1=11;2=12;3=13;", "the entries of an array of integers");
            check(joinedPairs(anything.entries()), "0=a;1=null;2=undefined;3=3;", "the entries of an array of anything");
        }
    }), 0, 30, "entries of arrays");
    const pairs = [...function* () { for (const pair of anything.entries()) yield pair; }()];
    check(pairs.length === 4 && Array.isArray(pairs[0]) && pairs[0].length === 2 && pairs[0] !== pairs[1], true, "each pair is its own array of two");
    check(Object.getPrototypeOf(pairs[0]), Array.prototype, "the prototype of a pair");
}
{
    checkOperations(operationsDuring(() => check(joined([0.5, 1.5].values()), "0.5;1.5;", "the values of doubles")), 3, 3, "doubles");
    checkOperations(operationsDuring(() => check(joined([1, , 3].values()), "1;undefined;3;", "the values of an array with a hole")), 1, 4, "a hole");
    check(joined([1, , 3].keys()), "0;1;2;", "the keys of an array with a hole");
    check(joinedPairs([1, , 3].entries()), "0=1;1=undefined;2=3;", "the entries of an array with a hole");
    Array.prototype[1] = "inherited";
    check(joined([1, , 3].values()), "1;inherited;3;", "a hole over an element of Array.prototype");
    delete Array.prototype[1];
    const sparse = [1];
    sparse[100000] = 2;
    sparse.length = 2;
    check(joined(sparse.values()), "1;undefined;", "an array that was sparse");
    check(joined(Object.freeze([1, 2]).values()), "1;2;", "a frozen array");
}
{
    let a = [1];
    check(joinedWhile(a.values(), x => { if (x < 20) a.push(x + 1); }), Array.from({ length: 20 }, (_, i) => i + 1).join(";") + ";", "an array that grows");
    a = [1, 2, 3, 4];
    check(joinedWhile(a.values(), () => { a.length = 2; }), "1;2;", "an array that is truncated");
    a = [1, 2, 3];
    check(joinedWhile(a.values(), () => { a[2] = 0.5; }), "1;2;0.5;", "integers that become doubles");
    a = [1, 2, 3];
    check(joinedWhile(a.keys(), () => { a.length = 0; }), "0;", "an array that is emptied");
}
{
    const a = [1, 2, 3, 4, 5];
    const iterator = a.values();
    check(firstOf(iterator), 1, "a loop that is left early");
    check(iterator.next().value, 2, "a call of next afterwards");
    check(firstOf(iterator), 3, "a loop after a call of next");
    check(joined(iterator), "4;5;", "the rest");
    check(joined(iterator), "", "an iterator that reached the end");
    a.push(6);
    check(joined(iterator), "", "an iterator that reached the end, after a push");
    check(iterator.next().done, true, "an iterator that reached the end, asked directly");
    const direct = a.entries();
    while (!direct.next().done);
    check(joined(direct), "", "an iterator that was exhausted by calls of next");
}
{
    check(joined(new Uint8Array([1, 2]).values()), "1;2;", "the values of a typed array");
    check(joinedPairs(new Float64Array([0.5]).entries()), "0=0.5;", "the entries of a typed array");
    check(joined(Array.prototype.keys.call({ length: 2 })), "0;1;", "the keys of something like an array");
    check(joined(Array.prototype.values.call("ab")), "a;b;", "the values of a string as an array");
    (function () { check(joined(Array.prototype.values.call(arguments)), "7;8;", "the values of arguments"); })(7, 8);
    class Sub extends Array { }
    check(joined(Sub.from([1, 2]).values()), "1;2;", "the values of an instance of a subclass");
    const own = [1, 2].values();
    own.next = function () { return { done: true }; };
    check(joined(own), "", "an iterator with its own next");
}
