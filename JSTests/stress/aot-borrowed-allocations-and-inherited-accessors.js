//@ runDefault("--compileMainScriptAheadOfTime=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function staysWith(name, kind) {
    let all = aotRemarks(name);
    if (all && !all.includes("allocation-is-only-borrowed:" + kind) && !all.includes("allocation-stays-here:" + kind))
        throw new Error("the " + kind + " of " + name + " should stay: " + all.filter(remark => remark.startsWith("allocation-")).join("; "));
}
function doesNotStayWith(name, kind) {
    let all = aotRemarks(name);
    if (all && (all.includes("allocation-is-only-borrowed:" + kind) || all.includes("allocation-stays-here:" + kind)))
        throw new Error("the " + kind + " of " + name + " is said to stay");
}
(function () {
    "use strict";
    let leaked;

    function readsName(a, again) { return again ? readsName(a, again - 1) : a.first; }
    function readsElement(a, again) { return again ? readsElement(a, again - 1) : a[7]; }
    function writesName(a, again) { if (again) writesName(a, again - 1); else a.last = 1; }
    function writesElement(a, again) { if (again) writesElement(a, again - 1); else a[7] = 1; }
    function passesOn(a, again) { return again ? passesOn(a, again - 1) : readsName(a, 1); }
    function readsLength(a, again) { return again ? readsLength(a, again - 1) : a.length; }
    function calls(f, again) { return again ? calls(f, again - 1) : f(); }

    function lendsArrayForName(x) { return readsName([x, 2], 1) + 0; }
    function lendsArrayForElement(x) { return readsElement([x, 2], 1) + ""; }
    function lendsArrayForStoreToName(x) { writesName([x, 2], 1); return x; }
    function lendsArrayForStoreToElement(x) { writesElement([x, 2], 1); return x; }
    function lendsArrayToBePassedOn(x) { return passesOn([x, 2], 1) + 0; }
    function lendsFunctionForName(x) { const read = readsName(() => x, 1); return read; }
    function readsNameOfItsOwnArray(x) { const a = [x, 2]; return a.first + 0; }
    function readsBeyondItsOwnArray(x) { const a = [x, 2]; return a[2]; }
    function readsItsOwnArrayAt(x, i) { const a = [x, 2]; return a[i]; }
    function deletesAndSpreads(x) { const a = [x, 2]; delete a[0]; return Math.max(...a); }

    function readsAndWritesWithinItsOwnArray(x) { const a = [x, 2]; a[0] = a[1]; return a[0] + a.length; }
    function lendsArrayForLength(x) { return readsLength([x, 2], 1) + 0; }
    function lendsObjectForName(x) { return readsName({ first: x, second: 2 }, 1) + 0; }
    function lendsObjectForElement(x) { const read = readsElement({ first: x, second: 2 }, 1); return read; }
    function lendsObjectForStore(x) { writesName({ first: x, second: 2 }, 1); return x; }
    function lendsObjectToBePassedOn(x) { return passesOn({ first: x, second: 2 }, 1) + 0; }
    function lendsFunctionToBeCalled(x) { return calls(() => x, 1) + 0; }

    check(readsAndWritesWithinItsOwnArray(1), 4, "elements that are there");
    check(lendsArrayForLength(1), 2, "the length");
    check(lendsObjectForName(1), 1, "a property of an object");
    check(lendsObjectForElement(1), undefined, "an element of an object");
    check(lendsObjectForStore(1), 1, "a store to an object");
    check(lendsObjectToBePassedOn(1), 1, "an object that is passed on");
    check(lendsFunctionToBeCalled(1), 1, "a function that is called");

    doesNotStayWith("lendsArrayForName", "array");
    doesNotStayWith("lendsArrayForElement", "array");
    doesNotStayWith("lendsArrayForStoreToName", "array");
    doesNotStayWith("lendsArrayForStoreToElement", "array");
    doesNotStayWith("lendsArrayToBePassedOn", "array");
    doesNotStayWith("lendsFunctionForName", "closure");
    doesNotStayWith("readsNameOfItsOwnArray", "array");
    doesNotStayWith("readsBeyondItsOwnArray", "array");
    doesNotStayWith("readsItsOwnArrayAt", "array");
    doesNotStayWith("deletesAndSpreads", "array");
    staysWith("readsAndWritesWithinItsOwnArray", "array");
    staysWith("lendsArrayForLength", "array");
    staysWith("lendsObjectForName", "object");
    staysWith("lendsObjectForElement", "object");
    staysWith("lendsObjectForStore", "object");
    staysWith("lendsObjectToBePassedOn", "object");
    staysWith("lendsFunctionToBeCalled", "closure");

    const takes = { get() { leaked = this; return 5; }, set(value) { leaked = this; }, configurable: true };
    Object.defineProperty(Array.prototype, "first", takes);
    Object.defineProperty(Array.prototype, "last", takes);
    Object.defineProperty(Array.prototype, 7, takes);
    Object.defineProperty(Array.prototype, 0, takes);
    Object.defineProperty(Array.prototype, 2, takes);
    Object.defineProperty(Function.prototype, "first", takes);
    function gotOut(what, run, expected) {
        leaked = undefined;
        run();
        check(typeof leaked === "function" ? leaked() : JSON.stringify(leaked), expected, what);
    }
    gotOut("an array, by a getter under a name", () => lendsArrayForName(1), "[1,2]");
    gotOut("an array, by a getter under an index", () => lendsArrayForElement(2), "[2,2]");
    gotOut("an array, by a setter under a name", () => lendsArrayForStoreToName(3), "[3,2]");
    gotOut("an array, by a setter under an index", () => lendsArrayForStoreToElement(4), "[4,2]");
    gotOut("an array that is passed on", () => lendsArrayToBePassedOn(5), "[5,2]");
    gotOut("a function, by a getter", () => lendsFunctionForName(6), 6);
    gotOut("an array, in the function that made it", () => readsNameOfItsOwnArray(7), "[7,2]");
    gotOut("an array, read beyond its end", () => readsBeyondItsOwnArray(9), "[9,2]");
    gotOut("an array, read at an index that is not known", () => readsItsOwnArrayAt(10, 7), "[10,2]");
    gotOut("an array with a hole, by spreading it", () => deletesAndSpreads(8), "[5,2]");
})();
