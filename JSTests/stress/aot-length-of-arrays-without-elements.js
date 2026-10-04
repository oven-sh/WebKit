//@ runDefault("--compileMainScriptAheadOfTime=1", "--useSoundTypes=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useSoundTypes=1", "--useDollarVM=1", "--useAOTDataStubs=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useSoundTypes=1", "--useDollarVM=1", "--validateAOTInferredTypes=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function remarksOf(f) {
    let remarks = aotRemarks(f.name);
    if (!remarks && isAOTCompiled(f))
        throw new Error("no remarks for " + f.name);
    return remarks;
}
function matches(remark, pattern) { return remark === pattern || remark.startsWith(pattern + ":"); }
function applies(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (!remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " does not apply to " + f.name + ": " + remarks.join(" "));
    }
}
function doesNotApply(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " applies to " + f.name + ": " + remarks.join(" "));
    }
}

(function () {
    function lengthOf(x) { return $$t(x, 256).length; }
    function lengthOfArrayPrototype() { return Array.prototype.length; }
    function counted(x) {
        const a = $$t(x, 256);
        let count = 0;
        for (let i = 0; i < a.length; i++) {
            if (a[i] === "stop")
                return -1;
            count++;
        }
        return count;
    }
    function found(x, wanted) {
        const a = $$t(x, 256);
        for (let i = 0; i < a.length; i++) {
            if (a[i] === wanted)
                return i;
        }
        return -1;
    }
    function both(x) { return lengthOf(x) + " " + counted(x); }

    const kept = [];
    for (let i = 0; i < 6000; i++) {
        const filler = new Array(300).fill(7);
        if (!(i % 3))
            kept.push(filler);
    }
    fullGC();
    for (let i = 0; i < 100; i++)
        Array.prototype["extra" + i] = i;
    check(lengthOfArrayPrototype(), 0, "Array.prototype with storage for properties only");
    check(both(Array.prototype), "0 0", "Array.prototype with storage for properties only, as a value");
    check(kept.length, 2000, "what was kept");

    check(both([]), "0 0", "an empty array");
    check(both([1, 2, 3]), "3 3", "an array of integers");
    check(both([1.5, 2.5]), "2 2", "an array of doubles");
    check(both(["a", , "c"]), "3 3", "an array with a hole");
    check(both(new Array(5)), "5 5", "an array of holes");
    check(both(Object.freeze([1, 2])), "2 2", "a frozen array");
    const sparse = [];
    sparse[100000] = 1;
    check(both(sparse), "100001 100001", "a sparse array");
    const longest = [];
    longest.length = 4294967295;
    check(lengthOf(longest), 4294967295, "the longest array");
    class Sub extends Array { }
    check(both(new Sub(1, 2, 3)), "3 3", "an instance of a subclass");
    check(both(createGlobalObject().Array(1, 2)), "2 2", "an array of another realm");

    check(both(Array.prototype), "0 0", "Array.prototype");
    check(both(createGlobalObject().Array.prototype), "0 0", "Array.prototype of another realm");
    check(both($vm.createRuntimeArray()), "0 0", "an empty array that keeps its length elsewhere");
    check(both($vm.createRuntimeArray(1, 2, 3)), "3 3", "an array that keeps its length elsewhere");
    check(found($vm.createRuntimeArray(1, 2, 3), 3), 2, "an element of an array that keeps its elements elsewhere");
    check(found($vm.createRuntimeArray(1, 2, 3), 4), -1, "no element of an array that keeps its elements elsewhere");
    check(found([1, 2, 3], 3), 2, "an element");

    doesNotApply(lengthOf, "calls:GetLength", "calls:operationAOTGetByIdWellKnown");
    doesNotApply(lengthOfArrayPrototype, "calls:GetLength", "calls:operationAOTGetByIdWellKnown");
    applies(counted, "array-view");
    applies(found, "array-view");

    Array.prototype[2] = "element";
    check(lengthOfArrayPrototype(), 3, "Array.prototype with an element");
    check(both(Array.prototype), "3 3", "Array.prototype with an element, as a value");
    Array.prototype.length = 0;
    check(lengthOfArrayPrototype(), 0, "Array.prototype without its element");
})();
