//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTDataStubs=false")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useMiniVMModeWithoutJIT=0", "--scribbleFreeCells=1", "--sweepSynchronously=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
(function () {
    function lengthOf(a) { return a.length; }
    function lengthInLoop(a) { let n = 0; for (let i = 0; i < a.length; i++) n++; return n; }
    function markOf(a) { return a.mark; }
    function checkLength(array, expected, what) {
        for (let i = 0; i < 3; i++) {
            check(lengthOf(array), expected, what);
            check(lengthInLoop(array), typeof expected === "number" ? expected : 0, what + ", in a loop");
        }
    }
    class Once extends Uint8Array { }
    class Twice extends Once { }
    class Thrice extends Twice { }

    const viewPrototype = Object.getPrototypeOf(Uint8Array.prototype);
    const builtinLength = Object.getOwnPropertyDescriptor(viewPrototype, "length");
    const arrays = [new Uint8Array(8), new Float64Array(8), new Once(8), new Twice(8), new Thrice(8)];

    viewPrototype[0] = 0;
    for (const array of arrays)
        checkLength(array, 8, "the length while the prototype has one element");

    let garbage = [];
    for (let round = 0; round < 6; round++) {
        for (let i = 1 + round * 500; i <= (round + 1) * 500; i++)
            viewPrototype[i] = i;
        fullGC();
        for (let i = 0; i < 2000; i++)
            garbage.push(new Array(1 + (i & 63)).fill(i));
        garbage = [];
        for (const array of arrays)
            checkLength(array, 8, "the length after the elements of the prototype moved, round " + round);
    }

    const marked = new Uint8Array(3);
    Object.defineProperty(marked, "mark", { get: builtinLength.get, configurable: true });
    for (let i = 0; i < 3; i++)
        check(markOf(marked), 3, "the built-in getter under another name");
    Object.defineProperty(marked, "mark", { get() { return "mine"; }, configurable: true });
    for (let i = 0; i < 3; i++)
        check(markOf(marked), "mine", "a getter of the program on a typed array");

    Object.defineProperty(viewPrototype, "length", { get() { return "redefined"; }, configurable: true });
    for (const array of arrays)
        checkLength(array, "redefined", "the length after it was redefined");

    Object.defineProperty(viewPrototype, "length", builtinLength);
    for (const array of arrays)
        checkLength(array, 8, "the length after it was restored");

    check(delete viewPrototype.length, true, "deleting the length");
    fullGC();
    for (const array of arrays)
        checkLength(array, undefined, "the length after it was deleted");
    for (let i = 0; i < 2000; i++) {
        const other = new Uint8Array(3);
        Object.defineProperty(other, "mark", { get: function () { return i; }, configurable: true });
        check(markOf(other), i, "a new getter after the built-in one was dropped");
    }
})();
